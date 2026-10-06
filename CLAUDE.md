# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this project is

A Windows desktop app that draws a **magnifying lens at the center of the screen on top of fullscreen apps, mainly games**. The lens is a smooth radial "bulge": strongest zoom at the exact center, fading to 1× at the lens edge, so there's no hard seam. A **Settings window** lets the user tune the lens live.

Primary use case: aiming or spotting detail at the crosshair without changing the game's FOV or zoom.

## Hard requirements

1. **Center lens over fullscreen games.** Must work over borderless and windowed fullscreen, and over "exclusive" fullscreen games that Windows runs with fullscreen optimizations (flip model through DWM). That covers most modern DX11/DX12/Vulkan games on Windows 10/11.
2. **Settings page** with at least:
   - **Radius** (px): size of the magnified region, measured from the screen center. Range `0 … min(screenW, screenH) / 2`. `0` means the lens is off. Larger values magnify a bigger area.
   - **Strength** (×): zoom factor at the exact center. Range `1.0 … 10.0`, step `0.1`. `1.0` means no zoom.
   - Changes apply **live** while dragging the sliders, with no restart.
   - Settings persist between runs.
3. **Never touch the game process.** No DLL injection, no `Present`/swap-chain hooking, no memory reads or writes. Anti-cheat (EAC, BattlEye, Vanguard, …) bans for that. Only capture the screen and draw a separate overlay window.
4. **Low latency, low overhead.** Keep the whole pipeline on the GPU and never read frames back to the CPU. Target under 1 frame of added latency and under 1–2% GPU at 1440p.
5. **Click-through overlay.** The lens must never steal focus, input, or the mouse from the game.

## Tech stack (decided; don't change without asking)

| Concern | Choice | Why |
|---|---|---|
| Language | C++20, MSVC (VS 2019 16.11+ Build Tools; Windows SDK ≥ 10.0.19041) | Direct access to D3D11, DXGI, and Win32 with minimal latency |
| Build | CMake ≥ 3.20 (the copy bundled with VS works) + Ninja, presets in `CMakePresets.json` (schema v2) | `build.bat` sets up the MSVC environment |
| Screen capture | **DXGI Desktop Duplication** (`IDXGIOutput1::DuplicateOutput`, or `IDXGIOutput5::DuplicateOutput1` for HDR) | Fast, GPU-resident, full monitor, no yellow capture border |
| Fallback capture | Windows.Graphics.Capture (monitor item, `IsBorderRequired = false`) | **Not implemented yet.** Use only if Desktop Duplication fails, e.g. on some hybrid-GPU laptops |
| Rendering | Direct3D 11 + HLSL pixel shader | Simple and fast enough |
| Overlay presentation | DirectComposition + `CreateSwapChainForComposition` (premultiplied alpha, flip-sequential) | Per-pixel transparency with no `UpdateLayeredWindow` CPU cost |
| Settings UI | Dear ImGui (Win32 + DX11 backends) in a **separate normal window** | Quick to build; live sliders |
| Config | JSON at `%APPDATA%\Magnifier\settings.json` via nlohmann/json | Human-editable |
| Shaders | HLSL embedded in `render/LensShader.h`, compiled at startup with `D3DCompile` | No build-time fxc step |
| Tests | Small self-contained runner in `tests/test_main.cpp`, run through CTest | No test framework dependency |
| Deps | Get them with CMake `FetchContent` (imgui v1.91.8, nlohmann_json v3.11.3). Don't commit binaries. Static CRT (`/MT`). | |

## Architecture

```
src/
  main.cpp                WinMain: DPI awareness, single-instance mutex, tray icon + menu, hotkeys, UI message loop
  app/Settings.h/.cpp     Settings struct, load/save JSON, clamping and validation (no Win32; unit tested)
  app/SharedSettings.h    Mutex-guarded snapshot + atomic version counter (UI thread writes, render thread reads)
  app/LensMath.h          CPU mirror of the lens shader math (used by tests)
  app/Log.h/.cpp          logf() + HR_CHECK macro
  capture/DesktopDup.*    Desktop Duplication: owns the D3D11 device; acquire → full-frame GPU copy
  render/LensShader.h     Fullscreen-triangle VS + radial-magnify PS (HLSL source string)
  render/Overlay.*        Lens-sized topmost click-through window + DComp swap chain + draw
  render/LensRenderer.*   Render thread: settings polling, monitor following, capture → render loop, recovery
  ui/SettingsWindow.*     ImGui window (own D3D device, UI thread): sliders, toggles, key binder, save/reset
  ui/HotkeyName.*         Hotkey → display text ("Ctrl+Alt+Z", "F8", "Right Mouse")
  input/BindingWatcher.*  Raw-input thread: mouse-button bindings, and every binding in hold mode (press + release)
tests/test_main.cpp       Lens math, clamping, JSON round-trip, corrupt-file handling
tools/make_demo.py        Renders docs/ media (demo.gif, settings.gif, modes.png, zoom-profile.svg) by
                          applying the lens math offline. The live lens can't be recorded because it's
                          capture-excluded. Re-run it after any lens-formula change.
                          `python tools/make_demo.py clip.gif` uses real game footage recorded with the lens OFF.
```

### Threads
- **UI thread** (`main.cpp`): tray window, hotkeys, and the settings window. When the settings window is hidden it blocks in `GetMessage`; when it's shown it renders each frame.
- **Render thread** (`LensRenderer`): owns the overlay window, the capture device and every D3D object it uses. It pumps its own messages. When the lens is disabled it hides and sleeps, but stays warm (see "Instant show/hide"). Radius 0 or strength 1 tears everything down. `LensRenderer::wake()` wakes it.

### Overlay window rules
- Extended styles: `WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP`. Style: `WS_POPUP`.
- Size it to the **lens bounding box only** (`2R × 2R`), centered on the **target monitor**. The target is the monitor the foreground window is on, ignoring our own windows; default is the primary monitor. A small window keeps DWM composition cheap.
- Call **`SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)`** on the overlay. Otherwise Desktop Duplication captures our own lens and creates a feedback loop. This needs Windows 10 2004+; check for it at startup and show an error if it's missing.
- Re-assert `HWND_TOPMOST` when the foreground window changes (`SetWinEventHook(EVENT_SYSTEM_FOREGROUND)`), because games often push themselves to the top.
- Only the lens circle has alpha > 0. Everything else is fully transparent (alpha 0), so the game shows through.

### Capture rules
- Create the D3D11 device on the **adapter that owns the target output**. Otherwise `DuplicateOutput` returns `DXGI_ERROR_UNSUPPORTED` on hybrid-GPU laptops.
- Handle `DXGI_ERROR_ACCESS_LOST` (mode change, UAC, fullscreen switch) by recreating the duplication. Never crash on it.
- Handle `DXGI_ERROR_WAIT_TIMEOUT` as "no new frame": re-present the last frame or skip.
- Copy **only the lens source box** (`Overlay::sourceBox`: `2R+2` square around the center, clamped to the monitor) with `CopySubresourceRegion` into the top-left of a persistent shader-readable texture, then call `ReleaseFrame()` right away. Full-frame copies at 4K/HDR cost GB/s of bandwidth that a GPU-bound game needs.
- Skip frames that only moved the pointer (`LastPresentTime == 0`). The exception is when the box changed: the acquired surface always holds the whole desktop, so re-copy from it.
- Resize the overlay and render in the same step, and only once pixels for the new box exist (`hasFrameFor(box)`). That way the lens never flashes an empty or wrongly sized frame.

### Performance rules (the lens must keep up with high-refresh games)
- **Frame-rate ceiling = monitor refresh rate.** Desktop Duplication delivers one composed frame per DWM composition, and the monitor can't show more. Don't add "uncapped" rendering: redrawing the same captured frame is invisible and steals GPU from the game. The user asked for 1000 fps; that was explained and declined for this reason.
- **Low-latency loop order:** `Overlay::waitUntilReady()` (frame-latency waitable, max latency 1: DWM has taken our previous frame) → `AcquireNextFrame` (newest desktop frame) → copy box → draw → `Present(0, 0)`. Never `Present(1)`: on top of the acquire wait it made each iteration span two vblanks.
- Swap chain: `FLIP_DISCARD` (fall back to `FLIP_SEQUENTIAL`), 2 buffers, `FRAME_LATENCY_WAITABLE_OBJECT`. Pass the same flag to `ResizeBuffers`. Consume the waitable's initial signal right after creation, otherwise every frame runs one frame behind.
- Remaining latency is inherent to capture: the lens shows a frame that was already composed, so it appears about one refresh after the game's own pixels. Removing that would require hooking the game's swap chain, which is forbidden (see Hard requirements #3). Lower-latency game settings help: NVIDIA Reflex or AMD Anti-Lag, or an fps cap slightly below refresh so the GPU isn't 100% busy.
- The settings window shows live stats (`app/RenderStats.h`): lens fps, monitor Hz, and capture-to-lens ms. Use them to verify any performance change.
- `SetMaximumFrameLatency(1)` and `SetGPUThreadPriority(7)` on the device. Set the process GPU scheduling class to HIGH (`D3DKMTSetProcessSchedulingPriorityClass` through `GetProcAddress`; don't use REALTIME). Otherwise a GPU-bound game starves the lens.
- Render thread at `THREAD_PRIORITY_HIGHEST`, and opt out of EcoQoS power throttling (`ProcessPowerThrottling`), because the lens process is never in the foreground.
- No per-frame allocations, logging or CPU readback. The lens can't run faster than the monitor's refresh rate, because Desktop Duplication delivers composed frames.
- HDR monitors: use `DuplicateOutput1` with `DXGI_FORMAT_R16G16B16A16_FLOAT` and a matching swap chain format. SDR: `B8G8R8A8_UNORM`.
- Per-monitor-v2 DPI aware (`SetProcessDpiAwarenessContext`), so all coordinates are physical pixels.

### Lens math (implement exactly; it's monotonic and seamless)
Let `c` = lens center (screen center), `R` = radius, `S` = strength, `d` = pixel − c, `r = |d|`.

```
if R <= 0 or S <= 1 or r >= R:  discard / alpha = 0          // outside the lens
t    = 1 - r / R                                              // 1 at center, 0 at edge
z    = gradual ? 1 + (S - 1) * t * t                          // zoom: S at center, gradually → 1 at edge
               : S                                            // fixed: S across the whole lens (scope look)
uv   = c + d / z                                              // source sample position
color = sample(capturedFrame, uv)  (linear filtering)
alpha = saturate(R - r)                                       // 1px anti-aliased edge
```
- **Why quadratic, not smoothstep** (user-requested "gradual" falloff): smoothstep is flat near the center, so it produced a plateau of near-full zoom followed by a sharp squeeze ring near ~0.8R. With `t²`, the zoom starts falling immediately from the center (at R/2 a quarter of the extra zoom remains) and has zero slope at the edge, so it blends seamlessly. Some radial squeeze is unavoidable in any lens that magnifies its center and matches 1× at the edge; this profile spreads it over the whole lens. Tests lock these properties in.
- In gradual mode, the mapping `r → r / z(r)` is strictly increasing because `z` decreases with `r`, so the image never folds over itself. Keep that property if you change the falloff.
- Fixed mode (`gradual = false`, user-requested toggle) intentionally has a hard edge at `R`: the image jumps from `S×` to `1×`. That's expected, not a bug.
- Pass `R`, `S`, `c`, the source box origin and the inverse texture size in a constant buffer, and update it whenever settings change.
- Optional settings for later (not required now): falloff-curve slider, thin border ring, crosshair dot.

## Settings window

- Opens from the tray icon, the hotkey (default `Ctrl+Alt+M`), or the first launch.
- Controls:
  - `Radius` slider: int px, `0 … maxRadius`, also editable as a number. Show "Off" at 0.
  - `Strength` slider: float `1.0× … 10.0×`, format `%.1fx`. This is the zoom at the exact center; it falls off gradually to 1× at the radius.
  - `Gradual zoom` checkbox (default on). On: the zoom builds from 1× at the edge to Strength at the center. Off: the zoom is a fixed Strength across the whole radius. Saved as `"gradual"` in settings.json; files without the field default to on.
  - Enabled checkbox, mirroring the toggle hotkey.
  - **Toggle key binding** (user-requested): click the button, then press a combo. Lone modifiers are ignored, Esc cancels, and losing focus or hiding the window cancels too.
    - Stored as `"toggleKey": {"mods", "vk"}`. The mods bits equal the Win32 `MOD_*` values, so `app/Settings` stays Win32-free. Default is `Ctrl+Alt+Z`; invalid values fall back to the default.
    - While capturing, unregister the current hotkey, otherwise `RegisterHotKey` swallows that keypress. If the new key can't be registered, keep the old one and show an error.
    - All hotkeys are registered on the tray window (UI thread) in `main.cpp`. The names shown come from `ui/HotkeyName.cpp`.
    - **Mouse buttons** (right, middle, Mouse 4/5; never left) are stored with their VK codes (0x02/0x04/0x05/0x06). `RegisterHotKey` can't bind them, so `input/BindingWatcher` uses **Raw Input with `RIDEV_INPUTSINK`** on its own thread and message-only window.
      - Don't use `WH_MOUSE_LL`: a low-level hook adds latency to all system mouse input when its thread is slow, and anti-cheat sometimes flags it. Raw input observes without hooking or swallowing, so the game still gets the click.
      - Raw input is registered only while a mouse button is bound, so 1–8 kHz mouse movement isn't delivered for nothing.
      - Modifiers must match exactly (`GetAsyncKeyState`). The binding is disabled while the binder is capturing.
  - **Hold mode** checkbox (user-requested, `"holdMode"`, default off). The lens is on only while the binding is held.
    - In hold mode, keyboard keys also go through raw input instead of `RegisterHotKey`, because key-up events are needed. A side effect is that the key isn't swallowed from the game.
    - Hold presses and releases change `enabled` without saving to disk. Startup in hold mode forces `enabled = false`.
    - `input/BindingWatcher` reports both pressed and released. In toggle mode, `main.cpp` acts only on presses. Release always ends a hold, whatever the modifiers.
  - Reset to defaults keeps the binding, hold mode and the on/off state.

### Instant show/hide
When the lens is disabled but configured (radius > 0, strength > 1), the renderer **stays warm**. It keeps the device, the duplication and the overlay, hides the window, invalidates the copied frame and sleeps on its event. Only radius 0 or strength 1 tears everything down. A full rebuild takes about 100 ms, which is far too slow for hold-to-zoom. On resume it waits for a fresh capture (`invalidateFrame`), so a stale image never flashes.
  - Buttons: `Save`, `Reset to defaults` (radius 200, strength 2.0). Also autosave on close.
- Push slider changes to the overlay **immediately** through a shared, mutex-guarded or atomic `Settings` snapshot. Overlay rendering must never block on the UI.
- The settings window is a normal window. While it's open the user is alt-tabbed, so it doesn't need to draw over the game.

## Coding conventions

- Use RAII for COM with `Microsoft::WRL::ComPtr`. No raw `Release()` calls.
- Check every `HRESULT`. Use a `HR_CHECK(expr)` macro that logs `__FILE__:__LINE__` and the hex code. Don't ignore errors silently.
- Log to `%APPDATA%\Magnifier\log.txt` and `OutputDebugString`.
- No allocations or blocking calls in the per-frame render path.
- Clamp every setting at load time and at UI input. Treat the JSON file as untrusted: if it's corrupt, use defaults and back up the bad file.
- Keep `app/Settings`, `app/SharedSettings.h` and `app/LensMath.h` free of Win32 so the `magnifier_core` library and its tests stay platform-independent.
- If you change the lens formula, update `render/LensShader.h` and `app/LensMath.h` together and keep the tests passing.

## Build & run

```powershell
.\build.bat              # configure + build + run tests (x64-release); finds VS through vswhere
.\build.bat x64-debug
.\build\x64-release\Magnifier.exe
```
From a VS Developer Prompt you can also run `cmake --preset x64-release`, `cmake --build --preset x64-release` and `ctest --preset x64-release` directly.

Runtime files are in `%APPDATA%\Magnifier\`: `settings.json` and `log.txt`. Check `log.txt` first when something doesn't work.

## Verifying changes

- Lens math: unit test the CPU version of the formula. Check that `z(0)=S`, `z(R)=1`, monotonic `r/z(r)`, and that `R=0` or `S=1` means a no-op.
- Manual check, every time: (1) borderless-windowed game, (2) a fullscreen game or a fullscreen video in a browser, (3) drag the sliders and confirm live update, (4) alt-tab and toggle the hotkey, (5) change resolution or plug in a monitor and confirm recovery from `ACCESS_LOST`.
- Confirm the game still gets all mouse and keyboard input while the lens is visible.

## Known limitations (tell the user; don't try to "fix" them with injection)

- True exclusive fullscreen (no fullscreen optimizations, some older DX9 titles) bypasses DWM, so no overlay can draw over it. Tell the user to switch the game to borderless fullscreen.
- Games with anti-cheat that block screen capture of their window will appear black inside the lens.
- Protected/DRM content (some video players) appears black.
