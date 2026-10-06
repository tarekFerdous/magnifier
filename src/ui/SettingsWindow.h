#pragma once

#include <functional>
#include <string>

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include "app/RenderStats.h"
#include "app/SharedSettings.h"

// Normal (non-overlay) window with live sliders, drawn with Dear ImGui on the UI thread.
// Uses its own D3D device so it never contends with the render thread.
class SettingsWindow {
public:
    struct Callbacks {
        std::function<void()> changed;  // settings were modified (wake the renderer)
        std::function<void()> save;     // persist to disk
        // Toggle-key binding. Hotkeys are registered by the owner of the tray window.
        std::function<void(bool)> setToggleHotkeyActive;        // false while capturing a key
        std::function<bool(Settings::Hotkey)> rebindToggle;     // false if the key is taken
        std::function<bool()> toggleHotkeyWorking;              // current key registered OK
    };

    bool create(HINSTANCE inst, SharedSettings& settings, const RenderStats& stats,
                Callbacks callbacks);
    void destroy();

    void show();
    void hide();
    bool visible() const;  // shown and not minimized: the main loop should call frame()

    void frame();

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    bool createDevice();
    void createRenderTarget();
    void applyDpiScale(UINT dpi);
    void drawUi();
    void startCapture();
    void stopCapture();
    void onCaptureKey(UINT vk);

    HWND hwnd_ = nullptr;
    SharedSettings* settings_ = nullptr;
    const RenderStats* stats_ = nullptr;
    Callbacks callbacks_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;

    UINT pendingDpi_ = 0;  // nonzero: rebuild fonts/style before the next frame
    bool occluded_ = false;
    ULONGLONG savedAt_ = 0;
    bool capturing_ = false;  // waiting for the user to press a new toggle key
    std::string bindError_;
};
