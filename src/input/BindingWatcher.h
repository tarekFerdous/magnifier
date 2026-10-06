#pragma once

#include <atomic>
#include <thread>

#include <windows.h>

#include "app/Settings.h"

// Detects presses and releases of the toggle binding globally via Raw Input
// (RIDEV_INPUTSINK) on its own thread. Nothing is hooked or swallowed: the game still
// receives the input, and system input latency is unaffected.
//   - Mouse buttons are always watched here (RegisterHotKey can't bind them).
//   - Keyboard keys are watched here only in hold mode, which needs key-up events;
//     in toggle mode they use RegisterHotKey instead.
class BindingWatcher {
public:
    enum : WPARAM { kReleased = 0, kPressed = 1 };  // wParam of the notification

    ~BindingWatcher() { stop(); }

    bool start(HWND notifyWindow, UINT message);
    void stop();

    // Pass {} to stop watching (e.g. while the user is binding a new key).
    void setBinding(const Settings::Hotkey& key, bool holdMode);

private:
    void run(HANDLE ready);
    void applyRegistration();
    void onRawInput(HRAWINPUT input);
    void press();
    void release();
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND notifyWindow_ = nullptr;
    UINT message_ = 0;
    HWND hwnd_ = nullptr;
    std::thread thread_;
    std::atomic<unsigned> mods_{0};
    std::atomic<unsigned> vk_{0};  // 0 = not watching
    std::atomic<bool> watchKeyboard_{false};

    // Input thread only.
    bool mouseRegistered_ = false;
    bool keyboardRegistered_ = false;
    bool holding_ = false;  // a press was reported and its release not yet
};
