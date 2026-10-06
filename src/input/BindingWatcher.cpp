#include "input/BindingWatcher.h"

#include "app/Log.h"

namespace {
constexpr wchar_t kClassName[] = L"McScopeBindingWatcher";
constexpr UINT WM_APP_REREGISTER = WM_APP + 1;
constexpr USHORT kUsageMouse = 0x02, kUsageKeyboard = 0x06;

struct MouseFlags { USHORT down, up; };

MouseFlags mouseFlagsFor(unsigned vk) {
    switch (vk) {
        case VK_RBUTTON: return {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP};
        case VK_MBUTTON: return {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP};
        case VK_XBUTTON1: return {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP};
        case VK_XBUTTON2: return {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP};
    }
    return {0, 0};
}

unsigned currentMods() {
    auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    unsigned mods = 0;
    if (down(VK_CONTROL)) mods |= Settings::kModCtrl;
    if (down(VK_MENU)) mods |= Settings::kModAlt;
    if (down(VK_SHIFT)) mods |= Settings::kModShift;
    if (down(VK_LWIN) || down(VK_RWIN)) mods |= Settings::kModWin;
    return mods;
}

bool registerRaw(USHORT usage, HWND target, bool add) {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;  // generic desktop
    rid.usUsage = usage;
    rid.dwFlags = add ? RIDEV_INPUTSINK : RIDEV_REMOVE;
    rid.hwndTarget = add ? target : nullptr;
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid))) return true;
    logf("RegisterRawInputDevices(usage %u, %s) failed: %lu", usage, add ? "add" : "remove",
         GetLastError());
    return false;
}
}  // namespace

bool BindingWatcher::start(HWND notifyWindow, UINT message) {
    notifyWindow_ = notifyWindow;
    message_ = message;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) return false;
    thread_ = std::thread([this, ready] { run(ready); });
    WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
    return hwnd_ != nullptr;
}

void BindingWatcher::stop() {
    if (!thread_.joinable()) return;
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    thread_.join();
}

void BindingWatcher::setBinding(const Settings::Hotkey& key, bool holdMode) {
    mods_ = key.mods;
    const bool watchable = key.valid() && (key.isMouse() || holdMode);
    vk_ = watchable ? key.vk : 0;
    watchKeyboard_ = watchable && !key.isMouse();
    if (hwnd_) PostMessageW(hwnd_, WM_APP_REREGISTER, 0, 0);  // registration is per-thread
}

void BindingWatcher::run(HANDLE ready) {
    // Press/release should feel instant even when the game keeps every core busy.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                            wc.hInstance, this);
    if (!hwnd_) logf("Binding watcher window failed: %lu", GetLastError());
    SetEvent(ready);
    if (!hwnd_) return;

    applyRegistration();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    hwnd_ = nullptr;
}

void BindingWatcher::applyRegistration() {
    release();  // the binding changed: never leave a hold stuck on

    // Each device class is registered only while needed, so e.g. 1-8 kHz mouse movement
    // isn't delivered to us when a keyboard key is bound.
    const bool wantMouse = vk_ != 0 && !watchKeyboard_;
    const bool wantKeyboard = vk_ != 0 && watchKeyboard_;
    if (wantMouse != mouseRegistered_ && registerRaw(kUsageMouse, hwnd_, wantMouse))
        mouseRegistered_ = wantMouse;
    if (wantKeyboard != keyboardRegistered_ && registerRaw(kUsageKeyboard, hwnd_, wantKeyboard))
        keyboardRegistered_ = wantKeyboard;
}

void BindingWatcher::press() {
    if (holding_) return;                 // keyboard auto-repeat
    if (currentMods() != mods_) return;   // modifiers must match exactly
    holding_ = true;
    PostMessageW(notifyWindow_, message_, kPressed, 0);
}

void BindingWatcher::release() {
    if (!holding_) return;  // releasing ends a hold regardless of modifiers
    holding_ = false;
    PostMessageW(notifyWindow_, message_, kReleased, 0);
}

void BindingWatcher::onRawInput(HRAWINPUT input) {
    RAWINPUT raw;
    UINT size = sizeof(raw);
    if (GetRawInputData(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1))
        return;

    const unsigned vk = vk_;
    if (vk == 0) return;
    if (raw.header.dwType == RIM_TYPEMOUSE) {
        const MouseFlags f = mouseFlagsFor(vk);
        const USHORT flags = raw.data.mouse.usButtonFlags;
        if (f.down && (flags & f.down)) press();
        if (f.up && (flags & f.up)) release();
    } else if (raw.header.dwType == RIM_TYPEKEYBOARD && raw.data.keyboard.VKey == vk) {
        if (raw.data.keyboard.Flags & RI_KEY_BREAK) release();
        else press();
    }
}

LRESULT CALLBACK BindingWatcher::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<BindingWatcher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_INPUT:
            if (self) self->onRawInput(reinterpret_cast<HRAWINPUT>(lp));
            break;  // DefWindowProc must still run for WM_INPUT cleanup
        case WM_APP_REREGISTER:
            if (self) self->applyRegistration();
            return 0;
        case WM_CLOSE:
            if (self) {
                self->vk_ = 0;
                self->applyRegistration();
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
