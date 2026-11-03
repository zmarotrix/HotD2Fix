# HotD2Fix

A modern compatibility and enhancement layer for **The House of the Dead 2** on PC.

HotD2Fix is a single drop-in `ddraw.dll` made specifically for this game. It replaces generic
wrappers like dgVoodoo: it implements exactly the parts of DirectDraw 7, Direct3D 7 and
DirectSound that the game uses, and runs them on Direct3D 9Ex and WASAPI, both first-class APIs
on Windows 10 and 11.

It works with both PC releases:

| Release | `Hod2.exe` | Text language |
|---|---|---|
| Japanese release (2000) | built December 2000 | Japanese tutorials and subtitles |
| European release (2001, English / French / German / Spanish / Italian) | built May 2001 | English |

## Features

- **Runs properly on modern Windows.** No exclusive fullscreen and no display mode changes:
  the game's 640x480 mode is emulated and shown borderless fullscreen or in a window.
  **Alt+Enter** switches at any time.
- **High resolution.** The game renders internally at a multiple of 640x480 (automatically
  1920x1440 on a 1440p screen, 2560x1920 on 4K), anti-aliased by rendering at twice that and
  scaling down, kept at 4:3 with black bars.
  The HUD and text stay sharp.
- **No depth flicker.** A 32-bit floating-point depth buffer keeps surfaces that lie almost on
  top of each other (like the floor in the Magician fight) from flickering.
- **Steady 60 fps**, independent of your monitor's refresh rate.
- **Aiming lines up at any window size**, for mouse and lightguns that act as a mouse.
- **Visual fixes:**
  - Optional smooth (bilinear and anisotropic) textures on 3D scenes, where the PC port used
    point sampling. Off by default; the HUD always keeps its original crisp look. Toggle live
    with **F9**.
  - Transparency done properly: smoke, fire, sparks and text are drawn after the rest of the
    scene, back to front, so they blend with what's really behind them. No see-through
    "halos" around effects or text, and nothing pokes through smoke. Toggle live with **F11**.
  - Correct lighting on scaled models (for example the fish in the water boss fight no
    longer render solid white).
  - No dark fringes or stray lines along sprite edges, for example above the towers behind
    the first boss.
- **Modern audio.** A new low-latency mixer outputs straight to WASAPI (10 ms), follows your
  default output device when it changes, supports the game's 3D positional sound, and adds a
  limiter so overlapping sounds don't distort.
- **Screenshots.** **F12** saves a full-resolution PNG to a `screenshots` folder.
- **Fast forward.** Hold **F7** to run the game 8x faster, for example through cutscenes or the
  ending credits to see which ending you got.
- Everything is configurable in `hod2fix.ini`, and every fix can be turned off.

## Installing

You need your own copy of the game. HotD2Fix contains no game files.

1. **Install the game** from your disc as usual. Either release works.

   *Tip:* install outside `C:\Program Files`, e.g. `C:\Games\The House of the Dead 2`. Windows
   redirects files that old games write inside Program Files, which makes screenshots and logs
   hard to find.
2. **Remove any other wrapper** from the folder that contains `Hod2.exe`, if you used one
   before:
   `ddraw.dll`, `D3DImm.dll`, `D3D8.dll`, `dgVoodoo.conf`, `dgVoodooCpl.exe`.
   Also turn off any **compatibility mode** on `Hod2.exe` (right-click > Properties >
   Compatibility, untick everything).
3. **Download** the latest `HotD2Fix-*.zip` from the
   [Releases](../../releases) page.
4. **Copy `ddraw.dll` and `hod2fix.ini`** into the folder that contains `Hod2.exe`.
5. **Insert the disc** (or mount an image of your own disc). Both releases check for their CD
   at startup.
6. **Run `Hod2.exe`.**

That's it. There's nothing else to install: Direct3D 9 and WASAPI are part of Windows, and the
DLL has no other dependencies.

### Notes for each release

- **Japanese release:** tutorials and subtitles stay in Japanese.
- **European release:** works the same way. This build positions its window differently, and
  HotD2Fix handles both.
- **Controllers / lightguns:** set them up with the game's own `Config.exe` as usual.
- **Network play** uses Windows' DirectPlay, which is off by default on Windows 10/11: turn on
  *Windows Features > Legacy Components > DirectPlay*. Network play hasn't been tested with
  HotD2Fix yet.

### Uninstalling

Delete `ddraw.dll` and `hod2fix.ini` from the game folder.

## Settings

All settings are in `hod2fix.ini` next to `Hod2.exe`, and each one is explained there. The main
ones:

| Setting | Default | What it does |
|---|---|---|
| `[Display] Fullscreen` | `1` | Borderless fullscreen (`1`) or window (`0`) |
| `[Display] RenderScale` | `0` | Internal resolution multiplier; `0` picks the largest that fits your screen |
| `[Display] DepthBuffer` | `float` | `float` = no depth flicker, anti-aliased by 2x supersampling; `24` = classic depth with MSAA |
| `[Display] MSAA` | `4` | Anti-aliasing: 0 = off; 2, 4 or 8 = MSAA (only with `DepthBuffer=24`) |
| `[Display] SmoothTextures` | `0` | Smooth 3D textures (`1`), or the original sharp look (`0`) |
| `[Display] FixAlphaHalos` | `1` | Sorted transparency: no see-through halos around effects and text |
| `[Display] KeepAspect` | `1` | Keep 4:3 (`0` stretches) |
| `[Display] FPSLimit` | `60` | Keep at 60: the game logic runs at 60 Hz |
| `[Audio] Enabled` | `1` | New mixer (`0` = Windows' DirectSound) |
| `[Audio] LatencyMs` | `10` | Raise it if you ever hear crackling |
| `[Audio] Device` | *(empty)* | Part of a device name to force a specific output |
| `[System] PauseInBackground` | `1` | The game pauses when unfocused, as originally |
| `[System] SkipIntroLogos` | `1` | Skip the SEGA and Empire logo cards at startup |
| `[System] FastForwardKey` | `F7` | Hold to fast forward; `FastForwardSpeed` sets how fast (2-32) |

## Hotkeys

| Key | Action |
|---|---|
| Alt+Enter | Switch between fullscreen and window |
| F12 | Full-resolution PNG screenshot to `screenshots\` |
| F7 (hold) | Fast forward (8x) through cutscenes and credits |
| F11 | Toggle sorted transparency (compare with the original) |
| F9 | Toggle texture smoothing (compare with the original) |
| F8 | Debug probe: log every draw under the cursor to `hod2fix.log` |
| Esc | Quit (the game's own behaviour) |

All keys can be changed in `hod2fix.ini`.

## Troubleshooting

- **Check `hod2fix.log`** next to `Hod2.exe`. Set `[System] Log=2` for more detail.
- **Sound problems:** try `[Audio] LatencyMs=20`, or set `[Audio] Enabled=0` to compare with
  Windows' DirectSound.
- **The game closes right away:** make sure the disc (or your disc image) is mounted.
- **Debug tools:** `[Debug] DumpTextures=1` saves every texture the game loads as PNG.
  `DumpEvery=N` saves every Nth frame as a BMP.

When reporting a problem, include `hod2fix.log` and which release you have.

## How it works

The game is a Direct3D 7 title built around a statically linked D3DX 7. HotD2Fix exports
`DirectDrawCreateEx`, so the game loads it instead of the system `ddraw.dll`, and translates
each call:

- **Graphics** (`src/ddraw.cpp`, `src/d3d7.cpp`, `src/renderer.cpp`): DirectDraw surfaces become
  Direct3D 9Ex render targets, textures and depth buffers at the higher internal resolution.
  D3D7 render, texture-stage and transform states map onto D3D9. Pre-transformed (2D) vertices
  are rescaled with pixel centres preserved. Colour keys are emulated with alpha. Presentation
  goes through an additional swap chain, so resizing never resets the device.
- **Sorted transparency:** opaque draws go straight to the GPU; blended draws that test depth
  are queued with a copy of their vertices and the device state, and replayed back to front
  by distance once the opaque scene is done (end of scene, clear, render-target change,
  present, 2D work, or a draw that ignores depth such as a fade). Soft and additive effects
  replay without depth writes. Normals are always renormalized, as hardware of the time did.
- **Edge fixes:** sprites that use exactly one copy of their texture are clamped at the
  edges, wherever that copy sits. Smoothed draws with transparency sample a copy whose
  transparent texels carry their neighbours' colour.
- **Audio** (`src/audio.cpp`): `DirectSoundCreate` is redirected to a software mixer: float
  mixing with per-buffer resampling, DirectSound volume and pan laws, duplicated buffers,
  position notifications for streaming, DirectSound 3D (distance, rolloff, cones, Doppler), and
  a peak limiter. Output is event-driven shared-mode WASAPI, with a low-latency period through
  `IAudioClient3`.
- **Input and window** (`src/main.cpp`, `src/input.cpp`): the game's `GetCursorPos`,
  `GetClientRect` and window-sizing calls are redirected through its import table. Hotkeys are
  read from the game's own DirectInput keyboard data, because once DirectInput acquires the
  keyboard, Windows stops sending the game normal key messages.

## Building

Requires Visual Studio 2022 (or newer) with the x86 C++ build tools and a Windows 10/11 SDK.

```bat
build.bat
```

Output: `build\ddraw.dll` (32-bit, static runtime, no dependencies).

| File | Role |
|---|---|
| `src/main.cpp` | DLL entry, exports, ini, logging, window management, import hooks |
| `src/ddraw.cpp` | `IDirectDraw7`, `IDirectDrawSurface7`, clipper, palette, pixel conversion |
| `src/d3d7.cpp` | `IDirect3D7`, `IDirect3DDevice7`, state translation, state blocks, vertex buffers |
| `src/renderer.cpp` | Direct3D 9Ex backend: targets, textures, presentation, pacing, screenshots |
| `src/input.cpp` | Hotkeys from the game's DirectInput keyboard |
| `src/audio.cpp` | DirectSound replacement: software mixer with WASAPI output |

The DirectX 7 and DirectX 9 headers can't share a compile unit, so `renderer.h` is the only
boundary between them. It carries plain numbers and opaque handles.

## License

MIT, see [LICENSE](LICENSE).

The House of the Dead is a trademark of SEGA. This project is not affiliated with or endorsed by
SEGA and contains no game code or assets.
