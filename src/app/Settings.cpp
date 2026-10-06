#include "app/Settings.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

bool Settings::Hotkey::valid() const {
    // 0x10-0x12 Shift/Ctrl/Alt, 0x5B-0x5C Win, 0xA0-0xA5 left/right Shift/Ctrl/Alt.
    const bool modifierKey = (vk >= 0x10 && vk <= 0x12) || vk == 0x5B || vk == 0x5C ||
                             (vk >= 0xA0 && vk <= 0xA5);
    // 0x01 left mouse would fire on every normal click; 0x03 is Ctrl+Break, not a button.
    const bool unusable = vk == 0x01 || vk == 0x03;
    return vk >= 0x01 && vk <= 0xFE && !modifierKey && !unusable &&
           (mods & ~(kModAlt | kModCtrl | kModShift | kModWin)) == 0;
}

void Settings::clamp() {
    if (!toggleKey.valid()) toggleKey = Hotkey{};
    radius = std::clamp(radius, kMinRadius, kMaxRadius);
    if (!std::isfinite(strength)) strength = kDefaultStrength;
    strength = std::clamp(strength, kMinStrength, kMaxStrength);
}

std::string settingsToJson(const Settings& s) {
    nlohmann::json j;
    j["enabled"] = s.enabled;
    j["radius"] = s.radius;
    j["strength"] = s.strength;
    j["gradual"] = s.gradual;
    j["toggleKey"] = {{"mods", s.toggleKey.mods}, {"vk", s.toggleKey.vk}};
    j["holdMode"] = s.holdMode;
    return j.dump(2);
}

bool settingsFromJson(const std::string& json, Settings& out) {
    auto j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;

    Settings s;
    try {
        s.enabled = j.value("enabled", s.enabled);
        s.radius = j.value("radius", s.radius);
        s.strength = j.value("strength", s.strength);
        s.gradual = j.value("gradual", s.gradual);
        s.holdMode = j.value("holdMode", s.holdMode);
        if (auto it = j.find("toggleKey"); it != j.end()) {
            if (!it->is_object()) return false;
            s.toggleKey.mods = it->value("mods", s.toggleKey.mods);
            s.toggleKey.vk = it->value("vk", s.toggleKey.vk);
        }
    } catch (const nlohmann::json::exception&) {
        return false;  // wrong field type
    }
    s.clamp();
    out = s;
    return true;
}

LoadResult loadSettings(const std::filesystem::path& file, Settings& out) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        out = Settings{};
        return LoadResult::Missing;
    }

    std::ifstream in(file, std::ios::binary);
    std::stringstream buf;
    buf << in.rdbuf();
    if (in && settingsFromJson(buf.str(), out)) return LoadResult::Ok;

    in.close();
    auto bad = file;
    bad += ".bad";
    std::filesystem::rename(file, bad, ec);
    out = Settings{};
    return LoadResult::Corrupt;
}

bool saveSettings(const std::filesystem::path& file, const Settings& s) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);

    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << settingsToJson(s);
        if (!out) return false;
    }
    std::filesystem::rename(tmp, file, ec);  // replaces existing file
    return !ec;
}
