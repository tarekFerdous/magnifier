// Minimal self-contained tests for the platform-independent core (settings + lens math).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>

#include "app/LensMath.h"
#include "app/Settings.h"

namespace {
int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

void lensMath() {
    const float R = 200.0f, S = 3.0f;
    CHECK(near(lens::zoomAt(0, R, S), S));          // full strength at the center
    CHECK(near(lens::zoomAt(R, R, S), 1.0f));       // identity at the edge
    CHECK(near(lens::zoomAt(R * 2, R, S), 1.0f));   // and outside
    CHECK(near(lens::zoomAt(50, 0, S), 1.0f));      // R = 0 is a no-op
    CHECK(near(lens::zoomAt(50, R, 1.0f), 1.0f));   // S = 1 is a no-op

    // r -> r / z(r) must be strictly increasing (no fold-over) for every strength.
    for (float strength : {1.5f, 2.0f, 5.0f, 10.0f}) {
        float prev = -1.0f;
        for (float r = 0; r <= R; r += 0.25f) {
            float s = lens::sourceRadius(r, R, strength);
            CHECK(s > prev || r == 0);
            prev = s;
        }
        CHECK(near(lens::sourceRadius(R, R, strength), R));  // seamless edge

        // Smooth edge: radial magnification -> 1 approaching the edge (no visible ring).
        const float h = 0.01f;
        const float slope = (lens::sourceRadius(R, R, strength) -
                             lens::sourceRadius(R - h, R, strength)) / h;
        CHECK(std::fabs(slope - 1.0f) < 0.02f);

        // Gradual: zoom keeps falling from the very center (no flat plateau);
        // at half radius exactly a quarter of the extra zoom remains.
        CHECK(lens::zoomAt(R * 0.1f, R, strength) < strength - 0.15f * (strength - 1));
        CHECK(near(lens::zoomAt(R * 0.5f, R, strength), 1 + (strength - 1) * 0.25f));

        // Zoom strictly decreases from center to edge.
        float prevZoom = strength + 1;
        for (float r = 0; r < R; r += 1.0f) {
            float z = lens::zoomAt(r, R, strength);
            CHECK(z < prevZoom);
            prevZoom = z;
        }
    }
}

void fixedZoom() {
    const float R = 200.0f, S = 3.0f;
    for (float r = 0; r < R; r += 10.0f) CHECK(near(lens::zoomAt(r, R, S, false), S));
    CHECK(near(lens::zoomAt(R, R, S, false), 1.0f));   // identity at/after the edge
    CHECK(near(lens::zoomAt(50, 0, S, false), 1.0f));  // R = 0 is a no-op
    CHECK(near(lens::zoomAt(50, R, 1.0f, false), 1.0f));
    CHECK(near(lens::sourceRadius(100, R, S, false), 100 / S));
}

void clamping() {
    Settings s;
    s.radius = -5;
    s.strength = 99.0f;
    s.clamp();
    CHECK(s.radius == 0);
    CHECK(s.strength == Settings::kMaxStrength);

    s.radius = 1'000'000;
    s.strength = std::numeric_limits<float>::quiet_NaN();
    s.clamp();
    CHECK(s.radius == Settings::kMaxRadius);
    CHECK(s.strength == Settings::kDefaultStrength);

    Settings off;
    off.radius = 0;
    CHECK(!off.active());
    off.radius = 100;
    off.strength = 1.0f;
    CHECK(!off.active());
    CHECK(Settings{}.active());
}

void json() {
    Settings s;
    s.enabled = false;
    s.radius = 321;
    s.strength = 4.5f;
    s.gradual = false;
    s.holdMode = true;
    Settings back;
    CHECK(settingsFromJson(settingsToJson(s), back));
    CHECK(back == s);

    Settings untouched;
    untouched.radius = 77;
    CHECK(!settingsFromJson("not json", untouched));
    CHECK(!settingsFromJson("[1,2]", untouched));
    CHECK(!settingsFromJson(R"({"radius":"big"})", untouched));
    CHECK(untouched.radius == 77);

    Settings partial;
    CHECK(settingsFromJson(R"({"radius":9999})", partial));
    CHECK(partial.radius == Settings::kMaxRadius);
    CHECK(partial.strength == Settings::kDefaultStrength);
    CHECK(partial.gradual);  // older settings files without the field keep gradual zoom
    CHECK(partial.toggleKey == Settings::Hotkey{});
    CHECK(!partial.holdMode);  // older files default to toggle mode
}

void hotkeys() {
    CHECK(Settings::Hotkey{}.valid());
    CHECK((Settings::Hotkey{0, 0x77}.valid()));                       // F8, no modifiers
    CHECK(!(Settings::Hotkey{Settings::kModCtrl, 0x11}.valid()));     // Ctrl alone
    CHECK(!(Settings::Hotkey{Settings::kModShift, 0xA0}.valid()));    // LShift alone
    CHECK(!(Settings::Hotkey{0, 0}.valid()));
    CHECK(!(Settings::Hotkey{0x40, 'Z'}.valid()));                    // unknown modifier bit

    // Mouse buttons: right/middle/side are bindable, left click never is.
    for (unsigned vk : {0x02u, 0x04u, 0x05u, 0x06u}) {
        CHECK((Settings::Hotkey{0, vk}.valid()));
        CHECK((Settings::Hotkey{0, vk}.isMouse()));
    }
    CHECK(!(Settings::Hotkey{0, 0x01}.valid()));
    CHECK(!(Settings::Hotkey{Settings::kModCtrl, 0x01}.valid()));
    CHECK(!(Settings::Hotkey{0, 'Z'}.isMouse()));
    Settings mouse;
    mouse.toggleKey = {0, 0x02};
    Settings mouseBack;
    CHECK(settingsFromJson(settingsToJson(mouse), mouseBack));
    CHECK(mouseBack.toggleKey == mouse.toggleKey);

    Settings s;
    s.toggleKey = {Settings::kModShift, 0x77};
    Settings back;
    CHECK(settingsFromJson(settingsToJson(s), back));
    CHECK(back.toggleKey == s.toggleKey);

    Settings bad;
    CHECK(settingsFromJson(R"({"toggleKey":{"mods":0,"vk":17}})", bad));  // invalid -> default
    CHECK(bad.toggleKey == Settings::Hotkey{});
    CHECK(!settingsFromJson(R"({"toggleKey":5})", bad));
}

void files() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "magnifier_tests";
    fs::remove_all(dir);
    const fs::path file = dir / "settings.json";

    Settings s;
    CHECK(loadSettings(file, s) == LoadResult::Missing);

    s.radius = 150;
    CHECK(saveSettings(file, s));
    Settings loaded;
    CHECK(loadSettings(file, loaded) == LoadResult::Ok);
    CHECK(loaded == s);

    { std::ofstream(file) << "{ broken"; }
    CHECK(loadSettings(file, loaded) == LoadResult::Corrupt);
    CHECK(loaded == Settings{});
    CHECK(fs::exists(dir / "settings.json.bad"));
    CHECK(!fs::exists(file));

    fs::remove_all(dir);
}
}  // namespace

int main() {
    lensMath();
    fixedZoom();
    clamping();
    json();
    hotkeys();
    files();
    if (g_failures == 0) std::printf("All tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
