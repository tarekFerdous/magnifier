#include <filesystem>
#include <memory>
#include <string>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include "app/Log.h"
#include "app/Settings.h"
#include "app/SharedSettings.h"
#include "input/BindingWatcher.h"
#include "render/LensRenderer.h"
#include "ui/HotkeyName.h"
#include "ui/SettingsWindow.h"

namespace {

constexpr wchar_t kTrayClass[] = L"MagnifierTrayWindow";
constexpr wchar_t kMutexName[] = L"Local\\MagnifierSingleInstance";
constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_SHOW_SETTINGS = WM_APP + 2;  // also sent by a second instance
constexpr UINT WM_APP_BINDING = WM_APP + 3;       // from BindingWatcher: wParam pressed/released
enum : UINT { kHotkeyToggle = 1, kHotkeySettings = 2 };
enum : UINT { kMenuSettings = 100, kMenuToggle, kMenuExit };

struct App {
    HINSTANCE inst = nullptr;
    HWND tray = nullptr;
    UINT taskbarCreatedMsg = 0;
    std::filesystem::path settingsPath;
    std::unique_ptr<SharedSettings> settings;
    std::unique_ptr<LensRenderer> renderer;
    SettingsWindow settingsWindow;
    BindingWatcher bindingWatcher;

    void save() {
        if (!saveSettings(settingsPath, settings->get()))
            logf("Failed to save settings to %ls", settingsPath.c_str());
    }

    bool toggleHotkeyOk = false;
    Settings::Hotkey appliedKey{0, 0};  // what registerToggle last set up
    bool appliedHold = false;

    // (Re)registers the binding.
    //  - Toggle mode, keyboard key: RegisterHotKey (works while games have focus).
    //  - Mouse buttons, or hold mode: raw-input watcher (sees releases; can't conflict).
    bool registerToggle(const Settings::Hotkey& key, bool hold) {
        UnregisterHotKey(tray, kHotkeyToggle);
        bindingWatcher.setBinding(key, hold);
        appliedKey = key;
        appliedHold = hold;
        if (key.isMouse() || hold) {
            toggleHotkeyOk = true;
            return true;
        }
        toggleHotkeyOk =
            RegisterHotKey(tray, kHotkeyToggle, key.mods | MOD_NOREPEAT, key.vk) != FALSE;
        if (!toggleHotkeyOk) {
            const DWORD err = GetLastError();
            logf("Toggle hotkey %ls unavailable (error %lu)", hotkeyToWide(key).c_str(), err);
        }
        return toggleHotkeyOk;
    }

    // Binds a new key, keeping the old one if the new one is taken.
    bool rebindToggle(const Settings::Hotkey& key) {
        const Settings s = settings->get();
        if (registerToggle(key, s.holdMode)) return true;
        registerToggle(s.toggleKey, s.holdMode);
        return false;
    }

    // Called after any settings change: re-register only if the binding or mode changed.
    void syncBinding() {
        Settings s = settings->get();
        if (s.toggleKey == appliedKey && s.holdMode == appliedHold) return;
        const bool enteringHold = s.holdMode && !appliedHold;
        registerToggle(s.toggleKey, s.holdMode);
        if (enteringHold && s.enabled) setEnabled(false, true);  // hold mode rests with the lens off
    }

    // persist=false for hold presses/releases: no disk write on every click.
    void setEnabled(bool enabled, bool persist) {
        Settings s = settings->get();
        s.enabled = enabled;
        settings->set(s);
        renderer->wake();
        updateTrayTip();
        if (persist) save();
    }

    void onBinding(bool pressed) {
        const Settings s = settings->get();
        if (s.holdMode) setEnabled(pressed, false);
        else if (pressed) setEnabled(!s.enabled, true);
    }

    void fillTrayData(NOTIFYICONDATAW& nid) const {
        nid = {sizeof(nid)};
        nid.hWnd = tray;
        nid.uID = 1;
    }

    void addTrayIcon() {
        NOTIFYICONDATAW nid;
        fillTrayData(nid);
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = WM_APP_TRAY;
        nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(nid.szTip, settings->get().enabled ? L"Magnifier (on)" : L"Magnifier (off)");
        if (!Shell_NotifyIconW(NIM_ADD, &nid)) logf("Shell_NotifyIcon(NIM_ADD) failed");
    }

    void updateTrayTip() {
        NOTIFYICONDATAW nid;
        fillTrayData(nid);
        nid.uFlags = NIF_TIP;
        wcscpy_s(nid.szTip, settings->get().enabled ? L"Magnifier (on)" : L"Magnifier (off)");
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void removeTrayIcon() {
        NOTIFYICONDATAW nid;
        fillTrayData(nid);
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }

    void showTrayMenu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kMenuSettings, L"Settings...\tCtrl+Alt+M");
        const Settings s = settings->get();
        const std::wstring toggleLabel = L"Lens enabled\t" + hotkeyToWide(s.toggleKey);
        AppendMenuW(menu, MF_STRING | (s.enabled ? MF_CHECKED : 0), kMenuToggle,
                    toggleLabel.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(tray);  // required so the menu closes when clicking elsewhere
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, tray, nullptr);
        PostMessageW(tray, WM_NULL, 0, 0);
        DestroyMenu(menu);
    }
};

App g_app;

LRESULT CALLBACK trayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_app.taskbarCreatedMsg && msg == g_app.taskbarCreatedMsg) {  // Explorer restarted
        g_app.addTrayIcon();
        return 0;
    }
    switch (msg) {
        case WM_APP_TRAY:
            if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) g_app.settingsWindow.show();
            else if (lp == WM_RBUTTONUP) g_app.showTrayMenu();
            return 0;
        case WM_APP_SHOW_SETTINGS:
            g_app.settingsWindow.show();
            return 0;
        case WM_APP_BINDING:
            g_app.onBinding(wp == BindingWatcher::kPressed);
            return 0;
        case WM_HOTKEY:  // toggle-mode keyboard binding (press only)
            if (wp == kHotkeyToggle) g_app.onBinding(true);
            else if (wp == kHotkeySettings) g_app.settingsWindow.show();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kMenuSettings: g_app.settingsWindow.show(); break;
                case kMenuToggle: g_app.setEnabled(!g_app.settings->get().enabled, true); break;
                case kMenuExit: PostQuitMessage(0); break;
            }
            return 0;
        case WM_APP_RENDER_FATAL:
            MessageBoxW(nullptr,
                        L"Magnifier needs Windows 10 version 2004 or newer: it could not hide its "
                        L"lens from screen capture, which would cause a feedback loop.\n\n"
                        L"See log.txt in %APPDATA%\\Magnifier for details.",
                        L"Magnifier", MB_OK | MB_ICONERROR);
            PostQuitMessage(1);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::filesystem::path appDataDir() {
    PWSTR path = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path)))
        dir = std::filesystem::path(path) / L"Magnifier";
    CoTaskMemFree(path);
    return dir.empty() ? std::filesystem::current_path() : dir;
}


}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // Physical pixels everywhere: capture, overlay placement and the settings UI.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kTrayClass, nullptr))
            PostMessageW(existing, WM_APP_SHOW_SETTINGS, 0, 0);
        return 0;
    }

    const auto dir = appDataDir();
    logInit(dir / L"log.txt");
    logf("Magnifier starting");

    g_app.inst = inst;
    g_app.settingsPath = dir / L"settings.json";
    Settings initial;
    const LoadResult loaded = loadSettings(g_app.settingsPath, initial);
    if (loaded == LoadResult::Corrupt) logf("settings.json was corrupt; backed up as .bad, using defaults");
    if (initial.holdMode) initial.enabled = false;  // hold mode: lens only while held
    g_app.settings = std::make_unique<SharedSettings>(initial);
    g_app.renderer = std::make_unique<LensRenderer>(inst, *g_app.settings);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = trayProc;
    wc.hInstance = inst;
    wc.lpszClassName = kTrayClass;
    RegisterClassExW(&wc);
    g_app.tray = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayClass, L"Magnifier", WS_POPUP, 0, 0, 0, 0,
                                 nullptr, nullptr, inst, nullptr);
    if (!g_app.tray) {
        logf("Failed to create tray window: %lu", GetLastError());
        return 1;
    }
    g_app.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    SettingsWindow::Callbacks callbacks;
    callbacks.changed = [] {
        g_app.syncBinding();
        g_app.renderer->wake();
        g_app.updateTrayTip();
    };
    callbacks.save = [] { g_app.save(); };
    callbacks.setToggleHotkeyActive = [](bool active) {
        if (active) {
            const Settings s = g_app.settings->get();
            g_app.registerToggle(s.toggleKey, s.holdMode);
        } else {  // binding a new key: the old one must not fire meanwhile
            UnregisterHotKey(g_app.tray, kHotkeyToggle);
            g_app.bindingWatcher.setBinding({0, 0}, false);
        }
    };
    callbacks.rebindToggle = [](Settings::Hotkey key) { return g_app.rebindToggle(key); };
    callbacks.toggleHotkeyWorking = [] { return g_app.toggleHotkeyOk; };
    if (!g_app.settingsWindow.create(inst, *g_app.settings, g_app.renderer->stats(),
                                     std::move(callbacks)))
        logf("Settings window unavailable");

    if (!g_app.renderer->start(g_app.tray)) {
        logf("Failed to start render thread");
        return 1;
    }

    if (!g_app.bindingWatcher.start(g_app.tray, WM_APP_BINDING))
        logf("Binding watcher unavailable; mouse and hold-mode bindings won't work");
    g_app.registerToggle(initial.toggleKey, initial.holdMode);
    if (!RegisterHotKey(g_app.tray, kHotkeySettings, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M'))
        logf("Hotkey Ctrl+Alt+M is already in use by another app (error %lu)", GetLastError());
    g_app.addTrayIcon();
    if (loaded != LoadResult::Ok) g_app.settingsWindow.show();  // first run: show what the app does

    // Block on messages while the settings window is hidden; render it continuously while shown.
    MSG msg{};
    bool running = true;
    while (running) {
        if (g_app.settingsWindow.visible()) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) running = false;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (running && g_app.settingsWindow.visible()) g_app.settingsWindow.frame();
        } else {
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    logf("Magnifier exiting");
    g_app.renderer->stop();
    g_app.bindingWatcher.stop();
    g_app.save();
    g_app.settingsWindow.destroy();
    UnregisterHotKey(g_app.tray, kHotkeyToggle);
    UnregisterHotKey(g_app.tray, kHotkeySettings);
    g_app.removeTrayIcon();
    DestroyWindow(g_app.tray);
    if (mutex) CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
