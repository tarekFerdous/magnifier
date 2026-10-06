#include "ui/SettingsWindow.h"

#include <algorithm>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "app/Log.h"
#include "ui/HotkeyName.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
constexpr wchar_t kClassName[] = L"MagnifierSettingsWindow";
constexpr int kClientWidth = 440;   // at 96 DPI
constexpr int kClientHeight = 375;
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
constexpr float kBaseFontPx = 15.0f;

// Largest useful radius: half the short side of the primary monitor.
int maxRadius() {
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    return std::clamp(std::min(w, h) / 2, 1, Settings::kMaxRadius);
}
}  // namespace

bool SettingsWindow::create(HINSTANCE inst, SharedSettings& settings, const RenderStats& stats,
                            Callbacks callbacks) {
    settings_ = &settings;
    stats_ = &stats;
    callbacks_ = std::move(callbacks);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    hwnd_ = CreateWindowExW(0, kClassName, L"Magnifier Settings", kStyle, CW_USEDEFAULT,
                            CW_USEDEFAULT, kClientWidth, kClientHeight, nullptr, nullptr, inst,
                            this);
    if (!hwnd_) {
        logf("CreateWindowEx(settings) failed: %lu", GetLastError());
        return false;
    }

    // Size the client area for the window's actual DPI.
    UINT dpi = GetDpiForWindow(hwnd_);
    RECT rc{0, 0, MulDiv(kClientWidth, dpi, 96), MulDiv(kClientHeight, dpi, 96)};
    AdjustWindowRectExForDpi(&rc, kStyle, FALSE, 0, dpi);
    SetWindowPos(hwnd_, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    if (!createDevice()) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // our settings live in settings.json, not imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_.Get(), context_.Get());
    pendingDpi_ = dpi;
    return true;
}

void SettingsWindow::destroy() {
    if (!hwnd_) return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    rtv_.Reset();
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void SettingsWindow::show() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void SettingsWindow::hide() {
    stopCapture();
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

bool SettingsWindow::visible() const {
    return hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
}

bool SettingsWindow::createDevice() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd_;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, &sd, &swapChain_, &device_, nullptr, &context_);
    if (FAILED(hr))  // no usable GPU (e.g. remote session): fall back to software
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels,
                                           ARRAYSIZE(levels), D3D11_SDK_VERSION, &sd, &swapChain_,
                                           &device_, nullptr, &context_);
    if (FAILED(HR_CHECK(hr))) return false;
    createRenderTarget();
    return rtv_ != nullptr;
}

void SettingsWindow::createRenderTarget() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (SUCCEEDED(HR_CHECK(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))))
        HR_CHECK(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_));
}

void SettingsWindow::applyDpiScale(UINT dpi) {
    const float scale = dpi / 96.0f;

    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 0.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.ScaleAllSizes(scale);
    ImGui::GetStyle() = style;

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.SizePixels = kBaseFontPx * scale;
    io.Fonts->AddFontDefault(&cfg);
    ImGui_ImplDX11_InvalidateDeviceObjects();  // font atlas is rebuilt on the next NewFrame
}

void SettingsWindow::frame() {
    if (occluded_) {
        if (swapChain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(16);
            return;
        }
        occluded_ = false;
    }
    if (pendingDpi_) {
        applyDpiScale(pendingDpi_);
        pendingDpi_ = 0;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    drawUi();
    ImGui::Render();

    const float clear[4] = {0.06f, 0.06f, 0.07f, 1.0f};
    ID3D11RenderTargetView* rtvs[] = {rtv_.Get()};
    context_->OMSetRenderTargets(1, rtvs, nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    occluded_ = swapChain_->Present(1, 0) == DXGI_STATUS_OCCLUDED;
}

void SettingsWindow::drawUi() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##settings", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    Settings s = settings_->get();
    const Settings before = s;

    ImGui::Checkbox("Lens enabled", &s.enabled);

    // Toggle key binding: click the button, then press the new combination.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Toggle key");
    ImGui::SameLine();
    if (capturing_) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::Button("Press a key or mouse button...  (Esc cancels)");
        ImGui::PopStyleColor();
    } else {
        const std::string label = hotkeyToString(s.toggleKey) + "##bind";
        if (ImGui::Button(label.c_str())) startCapture();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click, then press a key or a mouse button (right, middle, side)\n"
                              "with optional Ctrl/Alt/Shift/Win.\n"
                              "Mouse buttons still reach the game (right-click still aims).\n"
                              "A keyboard key without modifiers is reserved system-wide,\n"
                              "so the game won't receive it - F-keys are a good choice.");
        if (!callbacks_.toggleHotkeyWorking()) {
            ImGui::SameLine();
            ImGui::TextColored({1.0f, 0.45f, 0.4f, 1.0f}, "in use by another app");
        }
    }
    if (!bindError_.empty()) ImGui::TextColored({1.0f, 0.45f, 0.4f, 1.0f}, "%s", bindError_.c_str());
    ImGui::Checkbox("Hold mode", &s.holdMode);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: the lens shows only while the key/button is held down\n"
                          "(e.g. hold right-click to aim). Keyboard keys are not blocked\n"
                          "from the game in this mode.\n"
                          "Off: each press toggles the lens on/off.");
    ImGui::Spacing();

    const float labelWidth = ImGui::CalcTextSize("Strength").x + ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::PushItemWidth(-labelWidth - ImGui::GetStyle().WindowPadding.x);
    ImGui::SliderInt("Radius", &s.radius, Settings::kMinRadius, maxRadius(),
                     s.radius == 0 ? "Off" : "%d px", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Size of the magnified circle around the screen center. 0 turns it off.");
    ImGui::SliderFloat("Strength", &s.strength, Settings::kMinStrength, Settings::kMaxStrength,
                       "%.1fx", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(s.gradual ? "Zoom at the exact center. Fades smoothly to 1x at the edge."
                                    : "Zoom across the whole lens.");
    ImGui::PopItemWidth();
    ImGui::Checkbox("Gradual zoom", &s.gradual);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: zoom builds up gradually from 1x at the edge to Strength at the center.\n"
                          "Off: the same fixed zoom everywhere inside the radius (scope look).");
    ImGui::TextDisabled("Ctrl+click a slider to type an exact value.");

    if (s != before) {
        settings_->set(s);
        callbacks_.changed();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Save")) {
        callbacks_.save();
        savedAt_ = GetTickCount64();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults")) {
        Settings defaults;
        defaults.toggleKey = s.toggleKey;  // the binding is a personal choice; keep it
        defaults.holdMode = s.holdMode;
        defaults.enabled = s.enabled;
        settings_->set(defaults);
        callbacks_.changed();
    }
    if (savedAt_ && GetTickCount64() - savedAt_ < 2000) {
        ImGui::SameLine();
        ImGui::TextColored({0.4f, 0.85f, 0.4f, 1.0f}, "Saved");
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (s.active()) {
        ImGui::TextDisabled("Lens %.0f fps  |  monitor %.0f Hz  |  capture-to-lens %.1f ms",
                            stats_->fps.load(), stats_->refreshHz.load(), stats_->delayMs.load());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "The lens redraws every time the screen changes, up to the monitor's refresh rate:\n"
                "that is the most frames a monitor can show. On a static desktop fps drops,\n"
                "which is expected. Capture-to-lens is the time from Windows updating the screen\n"
                "to the lens frame being submitted; it then appears on the next refresh.");
    } else {
        ImGui::TextDisabled("Lens off");
    }
    ImGui::TextWrapped(
        "Ctrl+Alt+M opens this window. The lens follows the monitor of the active app. "
        "Settings are saved automatically when you close this window.");
    ImGui::End();
}

LRESULT CALLBACK SettingsWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;  // messages arrive before CreateWindowEx returns
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // While binding, keystrokes and mouse buttons go to the binder, not ImGui
    // (and Alt doesn't open the system menu).
    if (self && self->capturing_) {
        switch (msg) {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN: self->onCaptureKey(static_cast<UINT>(wp)); return 0;
            case WM_RBUTTONDOWN: self->onCaptureKey(VK_RBUTTON); return 0;
            case WM_MBUTTONDOWN: self->onCaptureKey(VK_MBUTTON); return 0;
            case WM_XBUTTONDOWN:
                self->onCaptureKey(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2);
                return TRUE;
            case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR:
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_CONTEXTMENU: return 0;
            case WM_XBUTTONUP: return TRUE;
        }
    }

    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return 1;
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

void SettingsWindow::startCapture() {
    bindError_.clear();
    capturing_ = true;
    // The current hotkey would otherwise be swallowed by RegisterHotKey before we see it.
    callbacks_.setToggleHotkeyActive(false);
}

void SettingsWindow::stopCapture() {
    if (!capturing_) return;
    capturing_ = false;
    callbacks_.setToggleHotkeyActive(true);
}

void SettingsWindow::onCaptureKey(UINT vk) {
    auto down = [](int key) { return (GetKeyState(key) & 0x8000) != 0; };
    unsigned mods = 0;
    if (down(VK_CONTROL)) mods |= Settings::kModCtrl;
    if (down(VK_MENU)) mods |= Settings::kModAlt;
    if (down(VK_SHIFT)) mods |= Settings::kModShift;
    if (down(VK_LWIN) || down(VK_RWIN)) mods |= Settings::kModWin;

    if (vk == VK_ESCAPE && mods == 0) {
        stopCapture();
        return;
    }
    const Settings::Hotkey key{mods, vk};
    if (!key.valid()) return;  // a lone modifier: keep waiting for the actual key

    capturing_ = false;
    if (callbacks_.rebindToggle(key)) {
        Settings s = settings_->get();
        s.toggleKey = key;
        settings_->set(s);
        callbacks_.changed();
        callbacks_.save();
    } else {
        bindError_ = hotkeyToString(key) + " is taken by Windows or another app. Try another.";
    }
}

LRESULT SettingsWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (swapChain_ && wp != SIZE_MINIMIZED) {
                rtv_.Reset();
                HR_CHECK(swapChain_->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0));
                createRenderTarget();
            }
            return 0;
        case WM_DPICHANGED: {
            const auto* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            pendingDpi_ = HIWORD(wp);
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) stopCapture();  // clicked away: cancel binding
            break;
        case WM_CLOSE:  // closing only hides; the app keeps running in the tray
            hide();
            callbacks_.save();
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
