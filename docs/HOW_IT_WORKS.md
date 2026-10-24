# How it works

## Static recompilation

The Xbox 360 runs PowerPC code. Instead of emulating that CPU at runtime, the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) translates every function
in the game's executable (`default.xex`) to C++ ahead of time. The result is
compiled with Clang into a normal x86-64 Windows program. The SDK also provides
the rest of the console: the kernel and system libraries, the Xenos GPU
(translated to Direct3D 12), XMA audio and controller input.

Saints Row needs some game-specific help on top of that. This document lists
what was changed and why.

## Recompiler configuration

`config/saintsrow_manifest.toml`:

- **ABI helpers.** Addresses of the compiler's register save/restore routines,
  which the recompiler needs in order to translate function prologues and
  epilogues.
- **Extra functions.** Nineteen small functions (mostly C++ adjustor thunks and
  virtual-call forwarders) are only reached through vtables, so static analysis
  does not find them. They are listed explicitly; without them, those virtual
  calls do nothing.
- **Mid-function hooks.** Two places inside functions get a short C++ hook:
  a voice-list loop in the audio code that can hit empty (null) slots, and a
  streaming routine whose byte-sized loop bounds are kept within 8 bits.

## Game code overrides

`project/src/stubs.cpp` replaces some recompiled functions with C++ versions
that call the original where appropriate:

- **Rendering.** Saints Row uses the Xbox 360's predicated tiling: it records
  commands once and replays them per screen tile through nested indirect
  buffers, synchronised with interrupts and CPU-written fences. The overrides
  keep the game's tiling executor, fence handling and command-buffer
  submission consistent with how the SDK processes GPU commands, and guard
  against command buffers whose memory has already been reused.
- **Audio.** The XAudio engine is created with a bounded number of worker
  threads, and the loader no longer waits forever for sounds whose playback
  state never reports completion.
- **Kernel objects.** A few semaphore, event and thread-creation calls are
  adjusted for differences between the SDK and real hardware.
- **Occlusion queries.** Lights, lens flares and similar effects are only
  drawn when the game's visibility queries report them as visible. The
  query-result function would skip the actual results while the device's
  "direct" mode is on, which this port needs for tiled rendering; the override
  reads the real results instead.
- **Null-pointer paths.** Several string and lookup routines are regularly
  called with null pointers. On the console those reads are harmless; on PC each
  one would raise an exception. Guards return the same result without the
  fault.
- **System.** Sign-in and content-licence queries report a signed-in local
  profile and the full game.

`project/src/kbm.cpp` turns the keyboard and mouse into controller input,
laid out like Saints Row 2 on PC. The mouse turns the gameplay camera directly
(bypassing the game's stick acceleration), points in the weapon wheel, pans the
pause map and, while tagging, turns the left stick in the direction it moves.
Keys mean different things on foot, in vehicles, in menus and in the character
creator; the game's own state tells which applies.

`project/src/glyphs.cpp` swaps the button prompts. The game draws them from
DXT textures (HUD sprite sheets, the `px_btn*` textures and button characters
in its fonts). `dist\kbm_ui.bin` holds, for each of these, the original blocks
and a keyboard/mouse version for each context. A background thread finds the
loaded textures in guest memory when the input device or context changes and
writes the matching version; the GPU texture cache sees the write and uploads
it. The file contains parts of the game's textures, so it is never distributed:
`tools/glyphgen` builds it during setup from the player's own packfiles and our
own pictures (`tools/glyphgen/art.png`).

Performance: the guest C library's `memset` and `memcpy` run natively; a
background copy of the game (for example a second window) is limited to 30 FPS;
the frame rate cap uses a waitable timer.

`project/src/main.cpp` sets up guest memory (including the low "null page" the
game expects to be readable), creates the window, loads the GPU backend and
starts the game.

## SDK changes

`patches/rexglue-sdk.patch`:

- **Nested indirect buffers.** Indirect buffers were read through a ring buffer
  exactly as large as the data, so its write position wrapped to zero and every
  nested buffer executed nothing. Saints Row draws most of its scene through
  them. The fix gives the reader one spare dword.
- **Waiting on memory.** When the GPU waits for a value written by a guest
  interrupt handler, the command processor now runs queued work while waiting,
  and gives up after a time limit instead of deadlocking.
- **Robustness.** Nested indirect buffers are limited in depth and skipped when
  they do not contain plausible GPU commands; unknown or broken packets are
  skipped instead of aborting the rest of a buffer.
- **Guest data source.** GPU data written only through the CPU's virtual
  mapping is read from there when the physical view is empty.
- **Rendering.** Unbound textures sample as white instead of black; draws
  without a finished pipeline are skipped; zero-scale viewports and 4-vertex
  resolve rectangles are handled.
- **Audio.** Audio client registration no longer holds a global lock while
  creating the audio driver (which could deadlock) and no longer hands out
  audio buffers before the client is fully registered.
- **Occlusion queries** are measured on the GPU with Direct3D 12 queries. The
  game uses separate memory blocks for the start and end of a query (one pair
  per screen tile) and reads the results a frame or two later; results are
  written back as soon as the GPU has finished the work, instead of reporting
  every query as visible.
- **Calls to unknown functions** return 0 instead of terminating the program.
- **Input** is ignored while the window is not focused.
- **Guest interrupt locks.** Recompiled code that turns interrupts off used to
  take one process-wide lock (and `mfmsr` locked and unlocked it every time).
  On the console this only stops the current hardware thread being preempted,
  and the code it protects already uses atomic reservations, so the lock is now
  per thread. `REX_GUEST_GLOBAL_LOCK=1` restores the old behaviour.
- **Packfile read cache.** Repeated reads from the game's read-only packfiles
  are served from a RAM cache (budget set with `ram_cache_mb.txt`).
- **Frame pacing.** Guest vertical blanks can follow the frame rate cap, so
  frame caps above 60 work with the 60 FPS mod.
- **Dot products.** The code generator emitted the VMX dot products
  (`vmsum3fp128`, `vmsum4fp128`) as `simde_mm_dp_ps`, which rounds every product
  and partial sum to single precision. They now multiply in double, add in
  double and round to single precision once, which is closer to the console
  (Xenia Canary does the same). Without this, thin background geometry in the
  opening cutscene (a ledge under a window, a phone booth) blinks on random
  frames. The native matrix multiply and shadow-volume builder
  (`native_math.inc`, `native_shadow.inc`) use the same arithmetic, bit for bit
  except the payload or sign of a NaN result.
