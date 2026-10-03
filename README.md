# Saints Reborn (formerly SaintsRowPC)

Saints Reborn is the new name of the SaintsRowPC project. Nothing else has
changed: old links to the SaintsRowPC repository lead here, and existing
installs keep updating as before.

An unofficial native Windows port of **Saints Row** (Xbox 360, 2006). The game's
PowerPC code is statically recompiled to x86-64 with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), so it runs as a normal
Windows program rather than under an emulator.

**This repository contains no game code or data.** You build the game yourself,
on your own PC, from your own disc.

> Saints Reborn is a fan project. It is not affiliated with, endorsed by or
> sponsored by Volition, THQ Nordic, Deep Silver, Plaion or Microsoft.

## Status

Fully playable from start to finish. The intro videos, character creator, the
whole story, side missions and free roam all work, with sound. Co-op is built
in: play the story or free roam together over LAN, by direct IP, or online with
a join code. The game runs at 60 FPS and above with the bundled 60 FPS mod (or
at its original 30), renders at 2x its original resolution by default, and has
its own Pause > Options menu for resolution scale, shadows and mouse
sensitivity. Keyboard and mouse are fully supported, the on-screen button
prompts switch between controller and keyboard pictures to match what you use,
and there is a first-person view (V). Older PCs are covered too: CPUs without
AVX2 automatically get a compatible build, and laptops with two graphics chips
use the faster one.

### Known issues

- Some textures on the character flicker slightly while rotating them in the
  character creator.
- Only the disc version this port was made with is supported. Setup warns if
  your `default.xex` is different.

## Easy install

Download **SaintsReborn-Setup.exe** from the
[latest release](https://github.com/whompay/SaintsReborn/releases/latest), run
it, choose your `.iso` or a folder with your game files (the one with
`default.xex` and `packfiles` in it), and press **Install**.

Setup downloads the build tools it needs: clang, CMake, Ninja and Microsoft's
C++ headers and libraries. That is about 1.5 GB, all kept in one folder, and
no Visual Studio is needed. Windows only asks for permission if the Visual C++
runtime is missing. Setup then builds the game and adds a Saints Reborn
shortcut. The first build takes 10–60 minutes, depending on your CPU.

To update, run Setup again or use the **Update Saints Reborn** shortcut. Only
what changed is rebuilt, and your saves and mod list are kept. Installs made
with the old Setup can be updated the same way.

Co-op and multiplayer are built in, so they don't appear in the mod loader's
list. They are always on.

### Linux (Steam Deck, desktop Linux)

Coming soon: Setup will build the game on Linux, and you'll play it with
Proton.

## Requirements (manual build with setup.bat)

- Your own Saints Row (Xbox 360) disc, dumped to an `.iso` file.
- Windows 10 or 11 (64-bit) and a GPU with Direct3D 12 support.
- [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/)
  (Community or Build Tools, both free) with the **Desktop development with
  C++** workload and these individual components:
  - C++ Clang Compiler for Windows
  - C++ CMake tools for Windows
- [Git for Windows](https://git-scm.com/download/win).
- About 15 GB of free disk space and 16 GB of RAM. The first build takes
  30–90 minutes depending on your CPU.

## Building manually

1. Download this repository to a short path, for example `C:\SaintsReborn`.
2. Run `setup.bat`.
3. Select your Saints Row `.iso` when asked.

When it finishes, the game is in the `dist` folder. If a step fails, fix the
cause and run `setup.bat` again; finished steps are skipped. See
[docs/BUILDING.md](docs/BUILDING.md) for what each step does and for
troubleshooting.

## Playing

Run `dist\WhompaysModLoader.exe` to pick mods and play, or `dist\saintsrow.exe`
to play directly.

An Xbox controller works as on the console. Keyboard and mouse controls follow
Saints Row 2 on PC:

| On foot | | In a vehicle | |
|---|---|---|---|
| Move | W A S D | Accelerate / brake | W / S |
| Camera | Mouse | Steer | A / D |
| Attack / secondary | Left / right mouse button | Drive-by | Left mouse button |
| Jump / sprint | Space / Shift | Handbrake / nitrous | Space / Shift |
| Action, enter vehicle | E | Exit vehicle | E |
| Reload, pick up weapon | R | Look left / right / back | Z / C / X |
| Kick / crouch | F / C | Hydraulics | Ctrl |

Everywhere: hold Q for the weapon wheel (point with the mouse), Esc or M for
the pause menu and map, Tab for back, the arrow keys for the D-pad, Enter and
Backspace for A and B in menus. In the pause menu the mouse pans the map, the
wheel zooms and the left button sets a waypoint; Q and E switch tabs. A mouse
click skips cutscenes. When tagging, move the mouse in circles the way the
arrows show.

| Key | Effect |
|---|---|
| F11 | Fullscreen / window |
| F10 | Frame rate cap: 30, 60, 90, 120 or off (above 30 needs the 60 FPS mod) |
| F1 | Frame rate counter |
| V | First person view on foot, swimming and in vehicles (switches back by itself in shops, cutscenes and scripted scenes). Settings: `dist\core\FirstPerson\mod.ini` |

Input is ignored while the game window is not focused. Saves and profile data
are stored in `dist\game`. Setup makes the keyboard button pictures from your
own game files (see [tools/glyphgen](tools/glyphgen/README.md)); if that step
fails, the game shows controller buttons.

Options, set by creating a file next to `saintsrow.exe`:

| File | Effect |
|---|---|
| `res_scale.txt` | Internal resolution scale: `1` (720p), `2` (default) or `3`. |
| `start_windowed` | Start in a window instead of fullscreen. The file can be empty. |
| `mouse_sensitivity.txt` | Mouse sensitivity, `1.0` by default. |
| `fps_cap.txt` | Frame rate cap. F10 writes it for you. |
| `ram_cache_mb.txt` | Memory for caching reads from the game's packfiles, in MB (`0` turns it off). The default depends on your RAM. |
| `gpu_queue.txt` | How many GPU command buffers may be queued, `4` by default. `0` waits for every buffer (slower, for troubleshooting). |
| `gpu_max_lag.txt` | How far the GPU thread may fall behind the game, in microseconds, `4000` by default. `0` means no limit. |
| `gpu_timing` | An empty file with this name logs the GPU time per frame by kind of work (draws, render target copies, texture loads, resolves, uploads) every 2 seconds. |
| `replay_thread.off`, `upload_batching.off`, `upload_check.off`, `bg_verify.off`, `direct_resolve_off`, `direct_resolve_skip_memory.off`, `rt_fast.off`, `texture_lookup_cache.off`, `keep_cross_transfers`, `msaa_tiling` | Troubleshooting: each empty file turns one performance optimization off. Only useful when looking for the cause of a graphics problem. |

Press **F5** in game to open the display settings menu. It lists every
resolution your monitor supports and offers:

| Setting | Effect |
|---|---|
| Window mode | `Windowed`, `Borderless Fullscreen` (desktop resolution) or `Fullscreen` (changes the monitor's display mode). |
| Resolution | Window size in windowed mode, display mode in fullscreen. |
| Widescreen | `Keep aspect (black bars)`, `Fill screen (crop edges)` (zooms in, crops top/bottom, no distortion) or `Stretch to fill (distorts)`. Applies immediately. |
| Aspect ratio | `Auto (game)`, `4:3`, `16:9`, `16:10` or `21:9` — the shape the image is presented at. Combined with `Keep aspect` you get bars around the chosen ratio; with `Fill screen` it zooms to cover. Has no effect in `Stretch` mode. |
| Internal resolution | Render scale `1x` (720p) to `4x` (2880p). Applies after a restart. `res_scale.txt` overrides it if present. |

**Apply** switches immediately and asks you to confirm within 30 seconds,
otherwise it reverts. Settings are saved to `saintsrow.toml` next to
the exe. F11 toggles fullscreen.

## Mods

Saints Reborn comes with **Whompay's Mod Loader**. Run
`dist\WhompaysModLoader.exe` to turn mods on or off and change their load
order, then press Play. Mods can replace game files, run Lua scripts, or load
C/C++ code that hooks the game's functions. See
[modding/README.md](modding/README.md) to use or make mods.

## How it works

The ReXGlue SDK translates every PowerPC function in the game's executable to
C++ and reimplements the Xbox 360 kernel, GPU (on Direct3D 12), audio and
input. This repository adds the Saints Row-specific parts:

| Path | Contents |
|---|---|
| `config/saintsrow_manifest.toml` | Recompiler configuration: ABI helpers, functions static analysis misses, mid-function hooks |
| `project/src/stubs.cpp` | Game-specific replacements for recompiled functions and kernel calls |
| `project/src/main.cpp` | Program entry: memory setup, window, runtime |
| `project/src/render_fixes.cpp` | Rendering setup changes for PC (single-pass rendering) |
| `project/src/kbm.cpp`, `project/src/glyphs.cpp` | Keyboard and mouse controls; controller / keyboard button prompts |
| `tools/glyphgen` | Makes the keyboard button pictures from your game files during setup |
| `project/src/wml`, `project/launcher` | Whompay's Mod Loader and its launcher |
| `modding` | Mod API header, examples and documentation |
| `patches/rexglue-sdk.patch` | Changes to the SDK that the game needs |
| `tools/xiso_extract` | Xbox disc image (XDVDFS) extractor |
| `scripts/setup.ps1` | The build script behind `setup.bat` |

[docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md) explains the individual fixes.

## Contributing

Bug reports and fixes are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Legal

This project distributes only original source code, configuration files and a
patch to the BSD-licensed ReXGlue SDK. It does not include, and must not be
used to distribute, any part of Saints Row: no disc images, game files,
recompiled code or built executables. Dump your own disc. Requests for or links
to game files will be removed.

The project's own code is released under the [MIT License](LICENSE). The SDK
and the libraries it downloads during the build are covered by their own
licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). "Saints Row" is
a trademark of its owner and is used here only to name the game this project
works with.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay and
  contributors.
- [Xenia](https://xenia.jp) by Ben Vanik and contributors, which the SDK is
  derived from.
- Volition, for the game.
