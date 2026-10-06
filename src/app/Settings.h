#pragma once

#include <filesystem>
#include <string>

// User-tunable lens settings. Deliberately free of Win32 so it can be unit tested.
struct Settings {
    static constexpr int kMinRadius = 0;
    static constexpr int kMaxRadius = 4096;  // hard cap; the renderer further clamps to the monitor
    static constexpr int kDefaultRadius = 200;
    static constexpr float kMinStrength = 1.0f;
    static constexpr float kMaxStrength = 10.0f;
    static constexpr float kDefaultStrength = 2.0f;

    // Global hotkey. Modifier bits equal Win32 MOD_ALT/MOD_CONTROL/MOD_SHIFT/MOD_WIN,
    // vk is a Win32 virtual-key code; kept as plain integers so this header stays Win32-free.
    static constexpr unsigned kModAlt = 0x1, kModCtrl = 0x2, kModShift = 0x4, kModWin = 0x8;
    struct Hotkey {
        unsigned mods = kModCtrl | kModAlt;
        unsigned vk = 'Z';
        bool valid() const;  // a real, non-modifier key and only known modifier bits
        // Right (0x02), middle (0x04), Mouse 4 (0x05), Mouse 5 (0x06). Left click is never valid.
        bool isMouse() const { return vk == 0x02 || vk == 0x04 || vk == 0x05 || vk == 0x06; }
        bool operator==(const Hotkey&) const = default;
    };

    bool enabled = true;
    Hotkey toggleKey;                    // turns the lens on/off
    bool holdMode = false;               // true: lens only while toggleKey is held down
    int radius = kDefaultRadius;         // px, 0 = lens off
    float strength = kDefaultStrength;   // zoom at the exact center, 1.0 = no zoom
    bool gradual = true;                 // true: zoom fades to 1x at the edge; false: fixed zoom

    // Forces every field into its valid range (NaN/inf strength -> default).
    void clamp();

    // True when the lens would actually change the image.
    bool active() const { return enabled && radius > 0 && strength > 1.0f; }

    bool operator==(const Settings&) const = default;
};

std::string settingsToJson(const Settings& s);

// Parses JSON into `out` (clamped). Returns false on malformed input or wrong field types;
// `out` is left untouched in that case. Missing fields keep their defaults.
bool settingsFromJson(const std::string& json, Settings& out);

enum class LoadResult { Ok, Missing, Corrupt };

// Corrupt files are renamed to "<name>.bad" and defaults are returned.
LoadResult loadSettings(const std::filesystem::path& file, Settings& out);

// Writes atomically (temp file + rename). Creates the parent directory if needed.
bool saveSettings(const std::filesystem::path& file, const Settings& s);
