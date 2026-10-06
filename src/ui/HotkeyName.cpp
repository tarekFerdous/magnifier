#include "ui/HotkeyName.h"

#include <windows.h>

static_assert(Settings::kModAlt == MOD_ALT && Settings::kModCtrl == MOD_CONTROL &&
              Settings::kModShift == MOD_SHIFT && Settings::kModWin == MOD_WIN);

namespace {

std::wstring keyName(UINT vk) {
    switch (vk) {
        case VK_RBUTTON: return L"Right Mouse";
        case VK_MBUTTON: return L"Middle Mouse";
        case VK_XBUTTON1: return L"Mouse 4";
        case VK_XBUTTON2: return L"Mouse 5";
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, wchar_t(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);

    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    switch (vk) {  // these share scan codes with numpad keys unless marked extended
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
            scan |= 0x100;
    }
    wchar_t buf[64];
    if (scan && GetKeyNameTextW(static_cast<LONG>(scan << 16), buf, ARRAYSIZE(buf)) > 0) return buf;

    wchar_t hex[16];
    swprintf_s(hex, L"Key 0x%02X", vk);
    return hex;
}

}  // namespace

std::wstring hotkeyToWide(const Settings::Hotkey& key) {
    std::wstring s;
    if (key.mods & Settings::kModCtrl) s += L"Ctrl+";
    if (key.mods & Settings::kModAlt) s += L"Alt+";
    if (key.mods & Settings::kModShift) s += L"Shift+";
    if (key.mods & Settings::kModWin) s += L"Win+";
    return s + keyName(key.vk);
}

std::string hotkeyToString(const Settings::Hotkey& key) {
    const std::wstring w = hotkeyToWide(key);
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                  nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), len,
                        nullptr, nullptr);
    return out;
}
