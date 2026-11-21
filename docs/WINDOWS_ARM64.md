# Playing on Windows ARM64 (development build)

The Windows build works the way the Android one does: rewrite the game program from your own Switch game files on the PC, translate the shaders once, then run it with this repository's `dmhost.exe`. There is **no installer** yet — only a command-line host, for development and verification.

中文：[WINDOWS_ARM64.zh-CN.md](WINDOWS_ARM64.zh-CN.md)

**Status at a glance:**

| Step | Status |
|---|---|
| Start-up, intro, title screen | Works (1600x900 window, Vulkan presentation) |
| Main menu → class → mode → name → into a run | Works (naming goes through the host input box or the `DM_KBD` channel) |
| In-run picture in the Rogue Encampment | Works: HUD life 55/55, mana 10/10, minimap, NPC names all correct |
| Walking, camera follow | Works (WASD as the left stick, left mouse button as a finger) |
| Frame rate (encampment, 1600x900) | **about 50-55 fps** (median frame time 13-15 ms, tail spikes still there, see "Performance") |
| Audio | WASAPI is wired up, but nobody has listened to it yet |
| Fullscreen toggle, window resize | Not tested |
| Cutscene movies | Not tested (the decode path has not been verified on Windows) |

The test machine: Windows 11 ARM64, Snapdragon X2 Plus (Adreno X2-45), Vulkan 1.4.

## Machine requirements

- Windows 11 ARM64 (Snapdragon X series and so on), a GPU driver with Vulkan 1.1 or newer
- 8 GB RAM or more (the process sits at about 2.6 GB in a run)
- About 10 GB of disk: the rewritten programs are about 78 MB, the shader bundles about 700 MB, plus saves and the temp directory

## What you need

- The game files you bought: Switch 1.0.34.0 (PC 3.3.94012) `ExeFS` (`main`/`sdk`/`subsdk0`) and `RomFS`, see [GAME_FILES.md](GAME_FILES.md)
- The llvm-mingw aarch64-w64-mingw32 toolchain (`aarch64-w64-mingw32-clang` / `-clang++` / `llvm-nm` / `llvm-ar`)
- The Vulkan SDK (only its headers and `glslc` are needed)
- CMake 3.22+, Ninja, Python 3.9+ (`pillow` for turning BMP screenshots into PNG; `numpy`/`lz4`/`capstone` for the rewrite and shader tools)

## Step 1: rewrite the game program

From the repository root, once per NSO, with the target set to `win`:

```
set DM_TARGET=win
set DM_RULESET=touch-run-v4
python dm/tools/patch_nso.py <ExeFS>\main      build\win\dmdata main
python dm/tools/patch_nso.py <ExeFS>\sdk       build\win\dmdata sdk
python dm/tools/patch_nso.py <ExeFS>\subsdk0   build\win\dmdata subsdk0
```

This writes `build\win\dmdata\`: `main.patched.bin` (about 54 MB), `sdk.patched.bin` (about 15 MB), `subsdk0.patched.bin` (about 8 MB), plus the `.fixups.tsv` / `.gates.tsv` / `.sites.tsv` tables. The `win` target rewrites thread-local storage and system-call landing sites for Windows; it is not the same set as the Android one.

## Step 2: extract and translate the shaders

```
python dm/win/tools/extract_casc.py <RomFS> build\win\switch-pkg
python dm/gfx/tools/dmsh_vk.py build\win\switch-pkg build\win\shaders
```

The first pulls the 162 shader packages (`.bin`) out of the static CASC under `RomFS/data`; the second translates them into the XGS1 bundles the engine reads — **6,931 `.xgs`** in total (6,927 game shaders plus 4 built-in helper shaders). This is the slow step, tens of minutes.

## Step 3: build `dmhost.exe`

```
cmake -G Ninja ^
  -DCMAKE_TOOLCHAIN_FILE=dm/win/toolchain-aarch64-w64-mingw32.cmake ^
  -DDM_GEN_DIR=build/win/gen ^
  -DCMAKE_BUILD_TYPE=Release ^
  -S dm/win -B build/win/obj
ninja -C build/win/obj
```

`-DDM_GEN_DIR` points at the host tables generated from the game files (`dm_vault_tab.h` / `dm_hud_exp.h` / `dm_bright_levels.inc`). There is no ARM64 Vulkan import library, so the link goes straight to `C:\Windows\System32\vulkan-1.dll` (override with `-DDM_VULKAN_DLL`); headers come from `-DDM_VULKAN_INCLUDE`.

## Running

```
cd build\win
set DM_ROMFS=<RomFS directory>
set DM_SAVE=C:\work\prj\d2r-mobile\build\win\save
set DM_TEMP=C:\work\prj\d2r-mobile\build\win\temp
set DM_WINDOW=1
obj\dmhost.exe dmdata main sdk subsdk0
```

`shader_dir` comes from `DM_SHADER_DIR` and defaults to `shaders` under the working directory, so running from `build\win` picks up step 2's output.

The easy way is to double-click `start-win-arm64.bat` at the checkout root: it sets all of the above (RomFS defaults to `..\Diablo\RomFS`), creates `save` / `temp`, and closes an older instance first. `--rebuild` rebuilds, `--res 1280x720` changes the render resolution.

The **title bar** shows the current render resolution and the live frame rate (`1600x900 · 52 fps`, refreshed once a second, counted from the frames the presentation layer actually put on screen).

### Keyboard and mouse

| Key | Maps to |
|---|---|
| W / A / S / D | left stick |
| J / K / U / I | A / B / X / Y |
| F / G | press left / right stick |
| Q / E / Z / C | L / R / ZL / ZR |
| Enter | + |
| Tab | − |
| Space | A |
| Arrow keys | d-pad |
| Left mouse button | one finger on the picture (touch tap) |
| Right mouse button | B |
| Wheel | small right-stick nudge |

Note that the guest **samples button state once per frame**: a press and release that land in the same frame read as no press. If you script key injection, leave at least 100 ms between down and up (this repo's `dm/win/tools/keys_win.py` defaults to 140 ms for that reason).

### Controllers

An XInput pad (Xbox pads, and third-party pads in XInput mode on PC) works right away, with the same rules as iOS / Android: controller input switches to the controller layout, the keyboard or a click on the picture switches back, and a stick must return to center before it takes over again.

Face buttons default to positions (bottom = A confirm, right = B back, left = Y, top = X), the same as the PC / Xbox version; for pads that report by Switch letters (or players used to "right A confirms"), `DM_PAD_AB=switch` swaps only A/B. Details and how to check it without hardware are in [../dm/win/README.md](../dm/win/README.md) under "Controllers".

### Environment variables

| Variable | Effect |
|---|---|
| `DM_WINDOW=1` | open a window and present through Vulkan (required; `0` is not wired up yet) |
| `DM_ROMFS` / `DM_SAVE` / `DM_TEMP` | RomFS, save directory, temp directory |
| `DM_SHADER_DIR` | shader bundle directory (default `./shaders`) |
| `DM_TACT_PREFETCH=0` | turn tactical prefetch off (recommended on Windows) |
| `DM_KBD=<text>` | the `swkbd` naming panel returns this text directly, skipping the host input box |
| `DM_PAD=0` / `DM_PAD_AB=switch` / `DM_PAD_LOG=1` / `DM_PAD_TEST=<s>` | controllers off / Switch face-button semantics / controller log / one injected A press to check the path |
| `DM_WIN_SHOT=<first>[+<step>]:<path template>` | read back the picture right before presentation into a 32-bit BMP, e.g. `200+100:C:\...\shot%d.bmp` |
| `DM_WIN_SIZE=WxH` | window client size (default 1600x900, capped at 90% of the work area) |
| `DM_GAME_RES=WxH` | game render resolution (default 1600x900, the original 16:9 docked size) |
| `DM_GFX_FENCE_SPIN=0` | go back to the blocking fence wait (spin is on by default, see below) |
| `DM_PRESENT_MODE=fifo\|relaxed\|mailbox\|immediate` | present mode (default `relaxed`, see "Performance"; `fifo` is strict vsync) |
| `DM_GFX_PERF_SAMPLE=1` | turn the per-part frame timing back on (off by default; it costs 1-2 fps) |
| `DM_GFX_FENCE_LOG=1` | log `wait_ms` / `since_submit_ms` for every fence wait |
| `DM_NVN_LOG` / `DM_FS` / `DM_IMPORTS` / `DM_FAULT_LOG` / `DM_VEH_LOG` / `DM_VEH_TRACE` | diagnostics |

## Performance

A run used to sit at **6 fps**. `NVN PERF` showed 10-11 `vkQueueSubmit` and 11 `vkWaitForFences` per frame, with about 120 ms spent waiting on fences while the GPU only did 16 ms of work.

Each fence reported `since_submit` of **13-14 ms** against about 1.5 ms of GPU work per submission: the time was not spent computing, but in the **wake-up latency of a blocking `vkWaitForFences` on this Windows ARM driver**. Polling `vkGetFenceStatus` first (with an 8 ms spin budget, then falling back to the blocking wait) brought the wait down to about 1 ms and the encampment from 6 fps to **41-44**, with `gpu_ms` steady around 9.7 ms. The spin defaults on only on Windows (`_WIN32`); Android and iOS keep the blocking wait rather than burning phone battery.

The second step is **bringing the engine switches Android measured over to Windows** (`tuning_defaults()` in `dm/win/front_win.c`; the per-switch notes are in `dm/android/jni_android.c` lines 245-337). Same Adreno + Vulkan + same engine code, so the set that holds 60 fps in the camp on phones applies here; a user-set environment variable of the same name still wins. Per frame it collapsed **10 submits with 10 fence waits** into **1 submit, fence=0**: character list 25 -> **53 fps**, class select 21.6 -> **48-52**, camp 43 -> **46**. `DM_GFX_NATIVE_BC=1` makes BC textures sample in their original format (the engine consumes BC7 directly, no RGBA decode), and `live_images` dropped from 826 MB to 311 MB.

The third step is **cutting the long-frame tail**. At that point `[nvn] 计时` already reported a median frame time of **13-15 ms (about 70 fps)** while `p95 37 ms / p99 70 ms` and more than 130 of 300 frames ran past 16.7 ms, dragging the average to 46. Three changes:

- Present mode from `FIFO` to `FIFO_RELAXED` (`DM_PRESENT_MODE` accepts `fifo|relaxed|mailbox|immediate`, default `relaxed`). FIFO pushes a frame that just misses one vblank period onto the next one, which in the camp test hit about 30% of frames, turning them into two periods; relaxed waits only when it can make the deadline. Camp 46 -> **50-55 fps**.
- Swapchain changed to **3 images** (two in flight alternately plus one the presentation engine is still reading), so `vkAcquireNextImageKHR` no longer blocks on vsync.
- `DM_GFX_PERF_SAMPLE=0` (same as Android): the per-part timing costs the timed frame about 4 ms more, so turning it off buys another 1-2 fps; `DM_GFX_PERF_SAMPLE=1` turns it back on when you want the breakdown.

Room left (not done, lowest risk first):

1. The tail spikes are not fully identified: the `[长帧]` detail shows "new pipelines 0 / texture uploads 0", so the spikes are neither compilation nor upload but guest-side work inside the frame; `DM_3D_SCALE=0.5` and `DM_PRESENT_MODE=immediate` both leave the frame rate unchanged, so it is neither GPU-bound nor vsync-quantized.
2. The engine's slot ring is only 2 deep (`Slot slots[2]` with `slot_index^=1` in `xbg_game.cpp`) and every `slot_index^1` use assumes that; deepening the ring is a medium-risk change.
3. Per-frame CPU shows `descriptor` at about 1.5-2 ms (1300-1800 descriptors per frame) and `draw` at 1.2-1.7 ms; going lower means touching the translation layer.

## Screenshot colour

`DM_WIN_SHOT` reads back the engine's source image (the `源格式` in the log, 37 = `VK_FORMAT_R8G8B8A8_UNORM` on this machine). The BMP writer has to respect that channel order: for an RGBA source it must swap bytes 0 and 2 before writing, otherwise saved frames come out with red and blue exchanged (red potions turn blue, torch flames turn blue, the purple mana orb turns red). The window itself does not go through that path (the swapchain format matches the source and is presented as-is), so a bad screenshot does not mean the picture is wrong — check the `DM_WIN_SHOT：已写 …（源格式 %d，交换红蓝 %d）` line before judging.

## Known gaps

- `DM_WINDOW=0` stops at `真实Vulkan后端未初始化` (the real Vulkan backend is not initialised); that path is not wired up yet.
- Audio (WASAPI) is implemented but untested by ear; `hle_lifecycle.c`'s SIGTERM/GCD and `hle_movie.c`'s decode path have not been verified on Windows.
- The drop-filter rules still fail to be written into `save/Settings/*.fltr` (the log says `规则文件没写成`).
- On the character-list screen a plain keyboard A does not trigger "select" on this machine (the class and mode screens are fine); the "new character" path is more reliable for automation.
- Window resize / fullscreen toggle and native gamepads are untested.

See also: [DEPENDENCIES.md](DEPENDENCIES.md), [GAME_FILES.md](GAME_FILES.md), [ANDROID.md](ANDROID.md) (the Android implementation this port follows).

