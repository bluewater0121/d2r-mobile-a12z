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
| Frame rate (encampment, 1600x900) | **55-60 fps** (mostly pinned at 60, 53-55 in the windows that drop a vblank) |
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

The skill-button icons need an atlas index generated from the game files first (the same `native_art` pipeline Android / iOS use):

```
python dm/ios/tools/native_art/generate.py <your game RomFS folder> --output-dir build/win/touchart
```

`clang` has to be on PATH (`romfs_art.py` uses it to compile deswizzle), and it has to be an **ARM64** python: an x64 one fails with `WinError 2` when spawning that helper. The run prints 12 PNGs and `dm_touch_native_art.c` (429 skill index entries) and takes a minute or two.

```
cmake -G Ninja ^
  -DCMAKE_TOOLCHAIN_FILE=dm/win/toolchain-aarch64-w64-mingw32.cmake ^
  -DDM_GEN_DIR=build/win/gen ^
  -DCMAKE_BUILD_TYPE=Release ^
  -S dm/win -B build/win/obj
ninja -C build/win/obj
```

`-DDM_GEN_DIR` points at the host tables generated from the game files (`dm_vault_tab.h` / `dm_hud_exp.h` / `dm_bright_levels.inc`). There is no ARM64 Vulkan import library, so the link goes straight to `C:\Windows\System32\vulkan-1.dll` (override with `-DDM_VULKAN_DLL`); headers come from `-DDM_VULKAN_INCLUDE`.

The build output `build/win/obj/dmhost.exe` is self-contained: the C++ runtime is linked statically (`-static`) and only `libwinpthread-1.dll` remains an import, which the POST_BUILD step in `CMakeLists.txt` copies next to the exe from the toolchain's `bin`. Without that step, double-clicking `start-win-arm64.bat` exits at once with `0xC0000135` (missing DLL) and not one line of log.

It is linked as a **Windows GUI subsystem** program (`add_executable(dmhost WIN32 …)`): no cmd window hangs around behind the game and the process takes no console. The price is that `stderr` has nowhere to go when it is started from Explorer or by a double-click, so the process points its own log at a file as soon as it enters — `DM_LOG_FILE` sets the path, the default is `dmhost.log` in the current directory (truncated on each start). Started from a terminal or with `Start-Process -RedirectStandardError`, the log goes to the usual standard handles and no file is created.

The exe also carries an icon and version info (`dmhost.rc` + `dmhost.ico`, compiled by windres into a COFF object that goes into the link): `tools/make_ico.py` turns the iOS `AppIcon` into a multi-size .ico, so the taskbar / title bar / Explorer show the same app icon as Android / iOS / HarmonyOS.

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

### The UI overlay (the Android / iOS interface changes live here)

The game's own interface is the original one. Our bottom HUD v4, panel changes, notice box, six mercenary slots, wide stash and keyboard key caps all live in the **UI overlay**, generated by the same scripts Android / iOS / the Mac App use. On Windows, generate it once:

```
powershell -ExecutionPolicy Bypass dm\win\tools\gen_overlay_win.ps1 -RomFS C:\work\prj\Diablo\RomFS
```

The output is `build\win\overlay`, and `start-win-arm64.bat` wires it into `DM_ROMFS_OVERLAY` by itself. Running without it works too, with the original interface.

A few notes:
- The script needs python (with PIL / numpy / scipy) and clang (without a system clang, point `-ToolchainBin` at llvm-mingw's `bin`).
- It compiles a texture conversion library that has to match the architecture of the python running it (x64 python -> x86_64 build, ARM64 python -> aarch64 build); a mismatch shows up as `WinError 193`, and the script picks by python's architecture itself.
- The overlay carries two layout sets: the touch version (its front-end button ribbon is drawn by the host) and the original controller version. Windows now brings that host-drawn layer along itself; see the next section.

### The touch control overlay (drawn by the Windows host)

The virtual stick, the skill buttons, the skill-set switch, town portal and map are not drawn by the game: on Android / iOS the host lays a separate layer over the picture. On Windows that layer is `dm/win/touch_win.cpp`: it runs the **same** touch state machine as the mobile ports (`dm/ios/Sources/dm_touch_controls.c`), feeds the mouse in as a finger, publishes the result as buttons / stick through the same `dm_pad_hook` the controller uses, and paints the result onto a layered window with GDI+.

- Geometry is in the same point space as iOS / Android (points on the 667×375 landscape base, scaled by screen height), so all three look like the same layout.
- The look follows iOS's vector recipe (`dm/ios/Sources/DmTouchControlArt.h`): iron-rimmed sockets with a charcoal well, an extra ring on the primary button, a silver lift while held, a feathered stick base with four directional etchings, and a matte thumb with a curved highlight.
- Skill buttons carry the **original skill icons**, the same set iOS uses: `dm/ios/tools/native_art/generate.py` builds the atlas index from the player's own game files, `dm_touch_game_state.c` / `dm_touch_game_bridge.c` work out which skill tile the current binding points at at runtime, and the icon is drawn the iOS way (clip radius inset 9% of the button rect, diameter .76 / .68, soft shadow, the pressed frame while held). An empty slot draws a cross, town portal uses original skill 359, map uses `dm_native_map`. Key letters remain only as the fallback when no icon can be had (outside a run, or the shared table has not identified the HUD yet). Off-hand-set skills get the "II" badge top right, charged skills the remaining count bottom right (`dm_swapskill_slot`).
- Without the atlas the build fails up front: run `generate.py` once (needs `clang` on PATH and an **ARM64** python — an x64 one dies with `WinError 2`), point `DM_WIN_TOUCHART` at `build\win\touchart`.
- The overlay is `WS_EX_TOPMOST` and `SetWindowPos`-ed directly above the game window: the game window is topmost itself (`CreateWindowExA` in `front_win.c`), so a plain owned popup ends up underneath it — `UpdateLayeredWindow` returns success and `IsWindowVisible` reads 1, yet nothing appears on screen.
- Input never goes through the overlay: it is `WS_EX_TRANSPARENT` and every mouse event lands on the game window (`wnd_proc` → `dm_win_touch_pointer`), so a transparent area stays clickable.
- The six bottom-bar cells (difficulty number, four belt columns, the function entry), the life / mana numbers and the experience bar are drawn by bottom-bar HUD v4 itself; the overlay keeps only their hit areas, exactly as on iOS.

Switches:

| Variable | Effect |
|---|---|
| default | entering a run (`dm_hud` has a character) switches to the touch layout and shows the overlay; leaving a run switches back |
| `DM_WIN_TOUCH=1` | master switch (also stops `dm_padmode.c` from locking the controller layout) |
| `DM_WIN_TOUCH_ALWAYS=1` | use the touch layout on the front-end pages too (their button ribbon is not drawn yet, so the menu comes up bare; debugging only) |
| `DM_WIN_TOUCH_DUMP=<path with %d>` | save the overlay's own bitmap as a 32-bit BMP on every repaint (first one, then every 5th) for review |
| `DM_WIN_TOUCH_ART=<dir>` | folder holding the skill-atlas PNGs (default `touchart\TouchArt`) |

Screen capture on this machine comes back black (`CopyFromScreen` and `PrintWindow` alike) and the `DM_WIN_SHOT` readback never contains a layered window, so **`DM_WIN_TOUCH_DUMP` is the only way to see the overlay**. Two gotchas when reading that BMP: its rows are bottom-up, and Pillow drops the alpha of a 32-bit BMP — read the raw BGRA and flip the rows to see the transparent background.

Whether a tap really becomes a key press shows up in the log as `触屏浮层：按键 0 → 0x1` — that is the button set the state machine published (`0x1` = A).

The easy way is to double-click `start-win-arm64.bat` at the checkout root: it sets all of the above (including `DM_WIN_TOUCH=1`; set it to `0` beforehand to play without the overlay), creates `save` / `temp`, and closes an older instance first. `--rebuild` rebuilds, `--res 1280x720` changes the render resolution.

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

### Front end: booting on the main menu

iOS / Android are touchscreens, so the host "presses keys on the player's behalf" to straighten the front-end flow (the "front-end path straightening" in `dm/host/dm_cursor.c`): with a save it presses X on the main menu to open the character select page, and presses X again as soon as the list collapses. A keyboard or controller can walk the original menu by itself and those presses fight the player: the first screen became the character list, and after pressing A to pick a character the list was reopened for the player so a second press was needed to get in.

So on Windows `tuning_defaults()` sets `DM_FE_AUTO` to `0` and the original flow is used: **main menu on boot → A opens the character select page → A starts the game**, exactly as on the original Switch. Set `DM_FE_AUTO=1` to get the touch behaviour back (e.g. once a real touchscreen is wired up).

### Saves: the game is asked to save before the window closes

The game's own autosave runs about once every 5 minutes, so closing the window straight away throws away everything since the last one. On close (the × button, Alt+F4, `taskkill /im dmhost.exe`) the front end does what the Mac shell's `app_quit` does: `dm_save_wait(3, …)` asks the game for one of its own saves and waits for the `存档提交 #N … 原子换入` commit to land before shutting down, logging `窗口：退出前等游戏存盘 388 ms，期间存档提交 2 次`; if nothing commits within 3 seconds it logs `【红】窗口：未存盘退出` as is.

`start-win-arm64.bat` closes an old instance politely first (WM_CLOSE) and waits up to 8 seconds before falling back to `taskkill /f`.

### Environment variables

| Variable | Effect |
|---|---|
| `DM_WINDOW=1` | open a window and present through Vulkan (required; `0` is not wired up yet) |
| `DM_ROMFS` / `DM_SAVE` / `DM_TEMP` | RomFS, save directory, temp directory |
| `DM_SHADER_DIR` | shader bundle directory (default `./shaders`) |
| `DM_TACT_PREFETCH=0` | turn tactical prefetch off (recommended on Windows) |
| `DM_KBD=<text>` | the `swkbd` naming panel returns this text directly, skipping the host input box |
| `DM_PAD=0` / `DM_PAD_AB=switch` / `DM_PAD_LOG=1` / `DM_PAD_TEST=<s>` | controllers off / Switch face-button semantics / controller log / one injected A press to check the path |
| `DM_FE_AUTO=0` | turn the touch-only "press keys for the player" front-end flow off (the Windows default: boot stays on the main menu) |
| `DM_ROMFS_OVERLAY=<dir>` | UI overlay (output of `gen_overlay_win.ps1`; the bat wires up `build\win\overlay` by itself) |
| `DM_APP=1` | desktop form: stay in the original controller layouts (Windows default on; the touch layouts' front-end ribbon is drawn by the host, which a desktop build does not have) |
| `DM_WIN_MMCSS=0` | turn off the MMCSS "Games" registration of the present / guest main thread (on by default) |
| `DM_WIN_MMCSS_PRIO=normal\|critical` | thread priority asked for after registration (default `high`) |
| `DM_WIN_PRIO=off\|normal\|above\|high` | process priority class (default `above`) |
| `DM_LOG_FILE=<path>` | log file used when there is no console under the GUI subsystem (default `dmhost.log` in the current directory; not created when started from a terminal or with redirection) |
| `DM_WIN_SHOT=<first>[+<step>]:<path template>` | read back the picture right before presentation into a 32-bit BMP, e.g. `200+100:C:\...\shot%d.bmp` |
| `DM_WIN_SIZE=WxH` | window client size (default 1600x900, capped at 90% of the work area) |
| `DM_GAME_RES=WxH` | game render resolution (default 1600x900, the original 16:9 docked size) |
| `DM_GFX_FENCE_SPIN=0` | go back to the blocking fence wait (spin is on by default, see below) |
| `DM_PRESENT_MODE=fifo\|relaxed\|mailbox\|immediate` | present mode (default `relaxed`, see "Performance"; `fifo` is strict vsync) |
| `DM_GFX_PERF_SAMPLE=1` | turn the per-part frame timing back on (off by default; it costs 1-2 fps) |
| `DM_HID_LOG=1` | log every button change on both input channels (host keyboard `dm_input_set` / controller `dm_pad_set`) with the caller address |
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

The fourth step is **removing the tail spikes**. After the three steps above the median frame time was already 13-15 ms (about 70 fps), yet more than 130 of 300 frames still ran past 16.7 ms with `p95` around 37 ms, holding the average in the low 50s. Every `[长帧]` entry reads "new pipelines 0 / texture uploads 0", so the spikes are neither compilation nor upload; `DM_3D_SCALE=0.5` and `DM_PRESENT_MODE=immediate` both leave the frame rate unchanged, so it is neither GPU-bound nor vsync-quantized.

The real cause is **Windows' timer granularity**: the default is 15.6 ms, and every `pthread_cond_timedwait` / `nanosleep` timeout in the guest is rounded up to it, so the frame that just missed its wait pays a whole 15-16 ms extra (baseline 13-15 ms, spikes 24-33 ms — exactly one quantum). Android and iOS timers are microsecond-grade, which is why phones never saw this. `raise_timer_resolution()` in `dm/win/front_win.c` (called from `front_run()`) asks for **0.5 ms** through `NtSetTimerResolution(5000, ...)` — the log prints `计时器精度：请求 0.5 ms，系统给的是 0.50 ms` — and falls back to winmm's `timeBeginPeriod(1)`. Results:

- Encampment **55-60 fps** (most windows 57-60; the windows that drop one vblank 53-55), against 50-55 before, and `掉帧` went from 86/300 to **51/300**.
- Class select 48-55 -> **55-60**; the character list likewise sits at 60.
- `submit_cpu_ms` from 6-7 down to **4-4.5** (shorter waits mean the translation thread gets squeezed less).

The game presents at the screen refresh (this machine reports 59 Hz; the log says `[nvn] 出图节拍 59.00 Hz`), so 60 is the ceiling for this configuration; going higher would mean reporting a higher refresh, which has no real benefit.

**The frame loss the touch overlay caused (fixed)**: with the overlay attached, idle camp dropped from **56.5 to 31.7 fps**. Measured per configuration (`NVN PERF` mean, idle camp, `-NoClick`):

| configuration | mean fps |
|---|---|
| bridge not attached (`DM_WIN_TOUCH_NOBIND=1`) | 56.6 |
| attached, callback returns immediately (`DM_BINDING_NOOP=1`) | 55.9 |
| attached, full callback (original) | 31.7 |
| pre-filter moved ahead of the lock only | 41.0 |
| pre-filter + stack-read page cache + per-fp negative cache (current) | **52.4** |
| plus a 15 ms per-slot throttle (`DM_BINDING_THROTTLE_MS=15`) | 54.1 (six-slot identity learning turns flaky and the icons fall back to key letters, so it is off by default) |

The gate itself (the guest → host jump) is cheap — the `NOOP` row proves it. The cost is the three words of the original getter's stack frame, which used to be preceded by a `VirtualQuery` on **every** call. The fix is a **page cache of validated spans** (unreadable pages are remembered too, since most `fp` values in the guest point at uncommitted memory) plus a **negative cache keyed on `fp`** (the `savedLR` stored at the same `fp` is structurally the same, so "not the HUD updater" can be remembered). Details and switches are in pitfall 14 of `dm/win/README.md`.

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
- Drops remain: about 50 of every 300 frames miss one vblank (the `掉帧` column of `[nvn] 上屏`), caused by a few milliseconds of jitter inside some frames; for now the headroom absorbs it.
- Window resize / fullscreen toggle is untested; with no hardware controller on this machine the pad path was only verified through `DM_PAD_TEST` injection.

See also: [DEPENDENCIES.md](DEPENDENCIES.md), [GAME_FILES.md](GAME_FILES.md), [ANDROID.md](ANDROID.md) (the Android implementation this port follows).


