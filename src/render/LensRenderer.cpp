#include "render/LensRenderer.h"

#include "app/Log.h"

namespace {
thread_local LensRenderer* t_renderer = nullptr;
constexpr UINT kAcquireTimeoutMs = 16;    // also bounds how fast settings changes show up
constexpr DWORD kRetryDelayMs = 500;      // after setup failures (UAC, lock screen, mode change)
constexpr ULONGLONG kTopmostIntervalMs = 1000;
}  // namespace

bool LensRenderer::start(HWND notifyWindow) {
    notifyWindow_ = notifyWindow;
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wakeEvent_) return false;
    quit_ = false;
    thread_ = std::thread([this] { run(); });
    return true;
}

void LensRenderer::stop() {
    if (thread_.joinable()) {
        quit_ = true;
        wake();
        thread_.join();
    }
    if (wakeEvent_) CloseHandle(wakeEvent_);
    wakeEvent_ = nullptr;
}

void CALLBACK LensRenderer::onForeground(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (!t_renderer) return;
    t_renderer->foregroundChanged_ = true;
    t_renderer->overlay_.bringToTop();  // games often push themselves on top when activated
}

HMONITOR LensRenderer::pickMonitor(HMONITOR current) const {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    // Our own windows (settings) never move the lens to another monitor.
    if (fg && pid != GetCurrentProcessId()) return MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);
    return current ? current : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

void LensRenderer::pumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void LensRenderer::idleWait(DWORD ms) {
    MsgWaitForMultipleObjects(1, &wakeEvent_, FALSE, ms, QS_ALLINPUT);
}

bool LensRenderer::setup(HMONITOR monitor, bool& fatal) {
    fatal = false;
    if (!dup_.init(monitor)) return false;

    switch (overlay_.init(inst_, dup_.device(), dup_.format())) {
        case Overlay::InitResult::Ok: return true;
        case Overlay::InitResult::CaptureExclusionUnsupported: fatal = true; [[fallthrough]];
        default: dup_.shutdown(); return false;
    }
}

void LensRenderer::teardown() {
    overlay_.shutdown();
    dup_.shutdown();
}

void LensRenderer::boostPriority() {
    // CPU: the loop does almost no work but must run the moment a frame arrives.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // Windows 11 throttles background processes (EcoQoS); the lens is never foreground.
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0;  // opt out
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling,
                          sizeof(throttling));

    // GPU: raise the process scheduling class so a GPU-bound game can't starve the lens.
    // D3DKMT lives in gdi32 but its header is WDK-only, hence GetProcAddress.
    // 4 = D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH (REALTIME needs admin and can starve DWM).
    using SetPriorityClassFn = LONG(APIENTRY*)(HANDLE, int);
    if (HMODULE gdi = GetModuleHandleW(L"gdi32.dll")) {
        auto fn = reinterpret_cast<SetPriorityClassFn>(
            GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass"));
        if (fn) {
            LONG status = fn(GetCurrentProcess(), 4);
            if (status != 0) logf("GPU scheduling priority not raised (NTSTATUS 0x%08X)",
                                  static_cast<unsigned>(status));
        }
    }
}

void LensRenderer::run() {
    t_renderer = this;
    boostPriority();
    HWINEVENTHOOK hook =
        SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, onForeground, 0,
                        0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    uint64_t seenVersion = 0;
    Settings s;
    HMONITOR target = nullptr;
    bool ready = false;
    bool geometryDirty = true;
    bool redraw = true;
    ULONGLONG lastTopmost = 0;

    LARGE_INTEGER qpcFreq;
    QueryPerformanceFrequency(&qpcFreq);
    ULONGLONG statsWindowStart = GetTickCount64();
    int framesInWindow = 0;
    float delayEma = 0;

    while (!quit_) {
        pumpMessages();

        if (uint64_t v = settings_.version(); v != seenVersion) {
            seenVersion = v;
            s = settings_.get();
            geometryDirty = redraw = true;
        }

        if (foregroundChanged_.exchange(false)) {
            HMONITOR m = pickMonitor(target);
            if (m != target) {
                target = m;
                if (ready) logf("Foreground moved to another monitor; following it");
                teardown();
                ready = false;
            }
        }

        // Radius 0 or strength 1: nothing to draw at all, release everything.
        if (s.radius <= 0 || s.strength <= 1.0f) {
            if (ready) teardown();
            ready = false;
            stats_.fps = 0;
            stats_.delayMs = 0;
            idleWait(INFINITE);
            continue;
        }

        if (!ready) {
            bool fatal = false;
            ready = setup(target, fatal);
            if (fatal) {
                PostMessageW(notifyWindow_, WM_APP_RENDER_FATAL, 0, 0);
                break;
            }
            if (!ready) {
                idleWait(kRetryDelayMs);
                continue;
            }
            geometryDirty = redraw = true;
        }

        // Disabled (toggled off or hold key released): stay warm, just hide and pause.
        // Rebuilding device + overlay + shaders takes ~100 ms, far too slow for hold-to-zoom.
        if (!s.enabled) {
            overlay_.show(false);
            dup_.invalidateFrame();  // on resume, wait for a fresh capture, never a stale one
            stats_.fps = 0;
            stats_.delayMs = 0;
            idleWait(INFINITE);
            continue;
        }

        // Low-latency order: first wait until DWM has taken our previous frame, *then* grab
        // the newest desktop frame, so what we draw is as fresh as it can be.
        const float hz = dup_.refreshHz() > 0 ? dup_.refreshHz() : 60.0f;
        overlay_.waitUntilReady(static_cast<DWORD>(1000.0f / hz) + 2);

        const RECT box = Overlay::sourceBox(dup_.monitorRect(), s.radius);
        auto result = dup_.acquire(kAcquireTimeoutMs, box);
        if (result == DesktopDup::Result::AccessLost || result == DesktopDup::Result::Error) {
            // Mode change, fullscreen switch, UAC prompt, device reset: rebuild everything.
            logf("Capture lost (%s); reinitializing",
                 result == DesktopDup::Result::AccessLost ? "access lost" : "error");
            teardown();
            ready = false;
            idleWait(kRetryDelayMs / 5);
            continue;
        }

        // Resize and redraw together, and only once pixels for the current box exist,
        // so the lens never flashes an empty or wrongly sized frame.
        if (dup_.hasFrameFor(box) &&
            (result == DesktopDup::Result::NewFrame || redraw || geometryDirty)) {
            if (geometryDirty) {
                if (!overlay_.setGeometry(dup_.monitorRect(), s.radius)) {
                    teardown();
                    ready = false;
                    continue;
                }
                geometryDirty = false;
            }
            const Overlay::Source source{dup_.frameSrv(), {box.left, box.top}, dup_.frameWidth(),
                                         dup_.frameHeight()};
            if (!overlay_.render(dup_.context(), source, s.strength, s.gradual)) {
                teardown();
                ready = false;
                continue;
            }
            overlay_.show(true);
            redraw = false;

            if (result == DesktopDup::Result::NewFrame) {
                ++framesInWindow;
                if (LONGLONG updated = dup_.lastUpdateQpc()) {
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    const float ms = static_cast<float>(now.QuadPart - updated) * 1000.0f /
                                     static_cast<float>(qpcFreq.QuadPart);
                    delayEma = delayEma == 0 ? ms : delayEma * 0.95f + ms * 0.05f;
                }
            }
        }

        if (ULONGLONG now = GetTickCount64(); now - statsWindowStart >= 1000) {
            stats_.fps = framesInWindow * 1000.0f / static_cast<float>(now - statsWindowStart);
            stats_.refreshHz = dup_.refreshHz();
            stats_.delayMs = delayEma;
            framesInWindow = 0;
            statsWindowStart = now;
        }

        if (ULONGLONG now = GetTickCount64(); now - lastTopmost > kTopmostIntervalMs) {
            overlay_.bringToTop();
            lastTopmost = now;
        }
    }

    teardown();
    if (hook) UnhookWinEvent(hook);
    t_renderer = nullptr;
}
