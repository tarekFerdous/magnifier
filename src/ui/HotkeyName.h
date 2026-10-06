#pragma once

#include <string>

#include "app/Settings.h"

// Human-readable name such as "Ctrl+Alt+Z" or "F8" (UTF-8).
std::string hotkeyToString(const Settings::Hotkey& key);
std::wstring hotkeyToWide(const Settings::Hotkey& key);
