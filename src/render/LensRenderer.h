#pragma once

#include <atomic>
#include <thread>

#include <windows.h>

#include "app/RenderStats.h"
#include "app/SharedSettings.h"
#include "capture/DesktopDup.h"
#include "render/Overlay.h"

// Posted to the notify window when the lens cannot work at all on this system.
constexpr UINT WM_APP_RENDER_FATAL = WM_APP + 10;

// Owns the render thread: capture -> lens shader -> overlay present.
// The overlay window and all D3D objects live on that thread only.
class LensRenderer {
public:
    LensRenderer(HINSTANCE inst, SharedSettings& settings) : inst_(inst), settings_(settings) {}
    ~LensRenderer() { stop(); }

    bool start(HWND notifyWindow);
    void stop();

    // Call after changing settings so a sleeping (disabled) renderer reacts immediately.
    void wake() { if (wakeEvent_) SetEvent(wakeEvent_); }

    const RenderStats& stats() const { return stats_; }

private:
    void run();
    static void boostPriority();
    bool setup(HMONITOR monitor, bool& fatal);
    void teardown();
    void pumpMessages();
    void idleWait(DWORD ms);
    HMONITOR pickMonitor(HMONITOR current) const;

    static void CALLBACK onForeground(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);

    HINSTANCE inst_;
    SharedSettings& settings_;
    HWND notifyWindow_ = nullptr;
    HANDLE wakeEvent_ = nullptr;
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> foregroundChanged_{true};

    RenderStats stats_;
    DesktopDup dup_;
    Overlay overlay_;
};
