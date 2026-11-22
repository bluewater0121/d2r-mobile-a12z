# dm/win/ — Windows ARM64 host

Only the Windows-specific pieces live here: the window and Vulkan presentation, the front end (keyboard / mouse / text input), the compatibility layer, and the tools used on this platform. The rest of the host (`dm/host/`) and the graphics layer (`dm/gfx/`) are shared with Android and iOS; the build pulls its source lists from `dm/host/Makefile` rather than copying them.

中文：[README.zh-CN.md](README.zh-CN.md)

## Files

| File | Purpose |
|---|---|
| `CMakeLists.txt` | llvm-mingw (aarch64-w64-mingw32) build. There is no ARM64 Vulkan import library, so it links `C:\Windows\System32\vulkan-1.dll` directly; the `log` library is an empty INTERFACE target so `dm/gfx/vulkan/CMakeLists.txt` can be reused unchanged |
| `toolchain-aarch64-w64-mingw32.cmake` | toolchain file (`clang` / `clang++` / `llvm-ar` / `llvm-nm`) |
| `front_win.c` | window, key and mouse injection, the `swkbd` host input box. Client area defaults to 1600x900 (`DM_WIN_SIZE` overrides); the picture is centered at the game's aspect with margins |
| `pad_win.c` | physical controllers: an XInput poll thread feeding the host `dm_pad_set`, with the same rules as iOS `dm_gamepad.m` / Android `PadInput.java` (see "Controllers" below) |
| `vulkan_present_win.cpp` | swapchain (1600x900, 3 images, format 0x25, `imageUsage` is only `TRANSFER_DST`), blit to the screen, `DM_WIN_SHOT` readback |
| `compat/` | Windows compatibility: PE section table, our own `setjmp`/`longjmp`, binary-read `open`, `aligned_alloc`/`dm_free` for the arena, stack pre-commit, VEH crash handling |
| `tools/` | platform tools, below |
| `scratch/` | small diagnostic probes (not part of the build) |

## Build

```
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=dm/win/toolchain-aarch64-w64-mingw32.cmake ^
  -DDM_GEN_DIR=build/win/gen -DCMAKE_BUILD_TYPE=Release -S dm/win -B build/win/obj
ninja -C build/win/obj
```

Output: `build\win\obj\dmhost.exe`. Running it, the environment variables and the key map are in [../../docs/WINDOWS_ARM64.md](../../docs/WINDOWS_ARM64.md).

## Tools

| Tool | Usage |
|---|---|
| `tools/keys_win.py` | `python keys_win.py "j u enter" --delay 400`: `PostMessage` keyboard into the `dmhost_win` window, no foreground focus needed. `"w:1200"` holds a key for 1200 ms (walking). **140 ms between down and up by default**: the guest samples button state once per frame, so a down+up inside one frame reads as no press |
| `tools/click_win.py` | `python click_win.py <x> <y> [--hold ms] [--right]`: posts a mouse event into the client area; the front end treats the left button as a finger on the picture (touch-only UI can only be driven this way). Coordinates are **client-area pixels**, not the game render size |
| `tools/dbg_win.py` | `python dbg_win.py <exe> <args…>`: launches the process under a debugger and can read thread contexts, TEB stack range and virtual-memory queries. It has to be in the same command as the `$env:DM_*` variables, with `cwd=build\win` |
| `tools/extract_casc.py` | `python extract_casc.py <RomFS> <output dir>`: pull the shader packages out of the static CASC |
| `tools/asm_coff.py` | converts Mach-O-syntax `.S` to COFF (called by the build) |
| `tools/sym_off.py` | symbol → offset, for crash backtraces |

## Controllers

`pad_win.c` uses XInput (`xinput1_4` -> `1_3` -> `9_1_0`, loaded dynamically in that order, no import library), polls every 8 ms (125 Hz, faster than the game's 60 Hz sampling so short presses are not missed) and hands the state to the host `dm_pad_set`. The rules are the same as iOS / Android:

- clear input on the controller (any button, or a stick pushed past 25%) switches to the controller layout (`dm_padmode_request(1)`) and reports the glyph style first;
- typing on the keyboard or clicking the picture switches back to the touch layout, and a stick has to return to center before it can take over again;
- in touch mode neither buttons nor sticks are delivered; in controller mode a stick that is not pushed out gives 0;
- several controllers OR their buttons, and each stick takes the farthest one.

Face buttons (the game is the Switch build, so the meanings follow the Switch keycaps):

| XInput physical | default (by position) | `DM_PAD_AB=switch` |
|---|---|---|
| bottom A | A confirm | B back |
| right B | B back | A confirm |
| left X | Y | Y |
| top Y | X (into the stack stash) | X |

XInput always reports by keycap position (bottom = A, right = B, left = X, top = Y) and Xbox pads print the letters at those positions, so the default is "bottom = confirm", the same position and function as the PC / Xbox version. Pads that report by Switch letters (a Switch Pro through an adapter, or a player used to "right A confirms") can set `DM_PAD_AB=switch` to swap only A/B. The glyph style reported is Xbox (when the overlay lacks that set, `dm_padmode.c` falls back to the original Switch glyphs).

Shoulders L/R, triggers ZL/ZR (past 30/255), stick clicks LS/RS, menu -> +, view -> -, d-pad by position.

| Env var | Effect |
|---|---|
| `DM_PAD=0` | disable controllers |
| `DM_PAD_AB=switch` | Switch face-button semantics (right A confirms) |
| `DM_PAD_LOG=1` | log connections and state changes |
| `DM_PAD_TEST=<seconds>[,<seconds>...]` | without hardware, inject a 0.25 s A press at each listed second (up to 8, e.g. `52,70,88`) |

No hardware needed to check the path: start with `DM_PAD_TEST=52` and the log walks through `手柄：XInput 已就绪`, the injection, and the layout switch; the picture moves from the title screen to the character list, which proves front end -> host -> game works end to end. To go further into the menus, list more moments (`DM_PAD_TEST=52,70,88`) — each is a short A press.

## Two things learned the hard way

**1. `0xC0000409` (fail-fast, code `0xd`) = `FAST_FAIL_INVALID_SET_OF_CONTEXT`.**
Guest threads run on the guest's own stacks, so their SP is outside the host TEB's `StackBase`/`StackLimit` range. When a page fault is claimed by the VEH and it returns `EXCEPTION_CONTINUE_EXECUTION`, Windows validates the context against the TEB and fail-fasts if the SP is outside it — and `__fastfail` bypasses the VEH, so the log shows no CRASH line at all, just a few `claimed=0` faults.

The fix lives in `compat/crash_win.c`'s `widen_stack_range()`: read the two qwords at `NtCurrentTeb()+8` (`StackBase` / `StackLimit`) and widen them with an 0x8000 margin, **never narrowing**, right before the handler returns "continue execution". The process now runs for as long as you like.

**2. `vkWaitForFences` wakes 12-14 ms late on this Windows ARM driver.**
A submission's GPU work is about 1.5 ms, but the blocking wait returns 13 ms later; with 10-11 waits per frame in a run, that meant 6 fps. The engine-side fix, in `dm/gfx/vulkan/engine/xbg_game.cpp`'s `retire_slot()`, polls `vkGetFenceStatus` first (8 ms spin budget, then falls back to the blocking wait). It is on by default on Windows only; Android and iOS keep the blocking wait (`DM_GFX_FENCE_SPIN` overrides). The encampment went from 6 fps to 42.

The two steps from there to 50-55 fps live in `tuning_defaults()` in `front_win.c` (the engine switches Android measured, as defaults — per frame the submits and fence waits drop from 10/10 to 1/0) and in `vulkan_present_win.cpp` (`FIFO_RELAXED` present and a 3-image swapchain). Numbers in `docs/WINDOWS_ARM64.md` under "Performance".

**3. Windows' timer granularity is 15.6 ms, and every guest timeout is rounded up to it.**
The median frame time was 13-15 ms but the average stayed in the low 50s with `p95` at 37 ms: the `[长帧]` entries showed no new pipelines and no texture uploads, and the extra block on each spike was 15-16 ms — exactly one system timer quantum, which every `pthread_cond_timedwait` / `nanosleep` timeout is rounded to. Android / iOS timers are microsecond-grade, so phones never saw it. `raise_timer_resolution()`, called from `front_run()`, asks `NtSetTimerResolution(5000, ...)` for 0.5 ms (the log prints `计时器精度：请求 0.5 ms，系统给的是 0.50 ms`), falling back to winmm's `timeBeginPeriod(1)`. The camp went from the low 50s to **55-60 fps**, and `掉帧` from 86/300 down to 51/300.

**4. The touch "press keys for the player" front-end flow traps keyboard / controller players on the character select page.**
The "front-end path straightening" in `dm/host/dm_cursor.c` is written for iOS / Android: with a save it presses X on the main menu to open the character select page (so the first screen became the character list) and presses X again whenever the list collapses (so after picking a character the list was reopened and one A press could not get in). A keyboard walks the original flow by itself, so `tuning_defaults()` sets `DM_FE_AUTO` to `0`: boot on the main menu, A opens the character select page, another A starts the game.

**5. Without the runtime dll, `dmhost.exe` exits at once with `0xC0000135` and writes nothing to the log.**
`CMakeLists.txt` links libc++ / libunwind statically (`-static`); only `libwinpthread-1.dll` remains an import, and a POST_BUILD step copies it from the toolchain's `bin` next to the exe. A double-clicked `start-win-arm64.bat` has no toolchain directory on PATH, so without that copy the process exits silently (which looked like a crash in the new code at first).

**6. The overlay's touch layouts move the front-end widgets off screen as a block, so switching to them leaves a desktop build with a bare main menu.**
The UI overlay (`gen_overlay_win.ps1` output) carries two layout sets: in the touch version the front-end button ribbon and key hints are drawn by the host's touch layer, while the original controller version keeps the game's own. Windows is keyboard / mouse and has no such drawing layer, so the moment "key typed / picture touched -> switch to the touch version" fires (`dm_win_pad_touch` in `pad_win.c`) the main menu becomes an empty background that ignores A. `tuning_defaults()` now sets `DM_APP=1` for the desktop form: boot straight into the original controller layouts, and `g_lock` inside `dm_padmode_request` keeps it there.

**7. Do not use `PrintWindow` for screenshots; use `DM_WIN_SHOT`.**
For a window presenting from a Vulkan swapchain, `PrintWindow` + `PW_RENDERFULLCONTENT` does not capture the complete picture: in game it grabs a zoomed-in world view with the whole bottom HUD missing (which briefly looked like the HUD was not drawn at all, while the `DM_WIN_SHOT` readback showed it fine). The front end (main menu / character select) is accurate though.

**8. Nothing can capture the touch overlay (a layered window), so it saves its own bitmap.**
The overlay is its own `WS_EX_LAYERED` window: `DM_WIN_SHOT` reads back the Vulkan picture only, `PrintWindow` misses it, and even `CopyFromScreen` comes back pure black on this machine (a 2048x1152 `VirtualScreen` grab is all zeros). `touch_win.cpp` therefore has `DM_WIN_TOUCH_DUMP=<path with %d>`: every repaint writes its own 32-bit BMP (the first one, then every 5th). Two things to know when looking at that bitmap:
- BMP rows are **bottom-up** (`biHeight` is positive), so a naive composite shows the whole overlay upside down — flip the rows first.
- Pillow **drops the alpha channel** when reading a 32-bit BI_RGB BMP (everything reads 255). Read the raw BGRA instead: `np.frombuffer(d, dtype=np.uint8, offset=54).reshape(-1,4)`, which is how the alpha 0 background and the 0-235 range on the buttons becomes visible.

**9. `gdiplus.h` is missing `PROPID` in C++, and has no `ColorBlend`.**
mingw's `gdiplus/gdiplusimaging.h` uses `PROPID` without pulling in `ole2.h`, so `#include <ole2.h>` has to come between `windows.h` and `gdiplus.h` (otherwise ~20 `unknown type name 'PROPID'` errors). These headers also have no `ColorBlend`, so multi-stop gradients are interpolated row by row (`grad_v()` in `touch_win.cpp`); and `Pen::SetLineCap` takes three arguments here (start / end / dash cap).

**10. A relink that fails with `Permission denied` means the old `dmhost.exe` is still running.**
`ld.lld: error: failed to write output 'dmhost.exe': Permission denied` is not a permission problem: the previous process has not exited yet. Stop it first (`Get-Process dmhost | Stop-Process -Force`), give it 2-3 s to release the image, then run ninja.

**11. "Blit succeeded, `IsWindowVisible` says 1, yet nothing is on screen" is z-order, not the layered window.**
`front_win.c` creates the game window with `WS_EX_TOPMOST`, while the overlay started life as an ordinary owned popup: `UpdateLayeredWindow` returned success every time, the rect and the visibility flag looked right, but the window sat entirely underneath the topmost game window. The fix is to give the overlay `WS_EX_TOPMOST` as well and `SetWindowPos(g_ov, g_game, …)` on every blit, pinning it directly above the game window. The log now reads `触屏浮层：贴图成功，窗口 <x>,<y> <w>x<h>（可见 1）`; the coordinates were garbage like `2099521032` at first because `POINT dst` was uninitialised and `ClientToScreen` took a failure path — it is now zero-initialised with a `GetWindowRect` fallback.

**12. Skill icons: GDI+ `SetClip` does nothing on this path, so the circle is cut per pixel.**
Each frame of the original atlas is a **square-cornered** skill tile and iOS makes it round with `CGContextAddEllipseInRect(...); CGContextClip(...)`. Porting that verbatim to GDI+ (`GraphicsPath::AddEllipse` + `SetClip(&path)` + `DrawImage`) left the tile square with every corner rivet intact — the clip simply never took effect. What works: draw the frame into a temporary `Bitmap` at the target size, `LockBits`, multiply alpha by the circle coverage (radius .82 of the button, i.e. iOS's "button rect inset by 9%"), then draw that already-clipped bitmap, cached by `(atlas, frame, width, height)`. Note also that the atlas index only builds with `clang` on PATH and an **ARM64** python (an x64 one dies with `WinError 2`); CMake raises `FATAL_ERROR` when the artefact is missing.

**13. With the touch overlay on, no UI button responds: the overlay swallowed every mouse click.**
While the overlay is active `front_win.c` fed `WM_LBUTTONDOWN/UP/MOUSEMOVE` to `dm_win_touch_pointer` first, and that function simply `return`ed (i.e. swallowed the event), so the original UI never saw a click — main-menu PLAY / OPTIONS and the in-run inventory / skill-tree / options tabs were all dead. Now in two steps:
- `dm_win_touch_pointer` returns **int**: on a press it only claims the event when a control is really hit (`finger[slot].target != DM_TC_NONE || pointer_slot == slot || joy_slot == slot`), otherwise it calls `dm_tc_cancel_finger` and returns 0; move/release use a `g_claim[]` table to remember ownership and return 0 when unowned (a lost release leaves a finger stuck down).
- A press the overlay did not claim is a **UI click** routed through `dm_ui_touch` (`dm/host/dm_cursor.c`, the same channel iOS front-end pages use) — for **both front-end pages and in-run panels**: the inventory / skill-tree / options panels are original UI on the same channel (verified: tapping the skill-tree tab switches pages). Log line: `前端：界面点击 (%.3f,%.3f)`.

**14. 60 fps at boot, 30 fps once in a run: the cost sits in the skill-binding observation gate.**
`binding_getter` in `dm_touch_game_bridge.c` is the observation gate patched onto the original getter; on Windows the game calls it **3-6k times a second**, each call leaving the guest for a host function. Measured in camp, idle, `-NoClick`, `NVN PERF` average:

| configuration | mean fps |
|---|---|
| bridge not attached (`DM_WIN_TOUCH_NOBIND=1`) | 56.6 |
| attached, callback returns immediately (`DM_BINDING_NOOP=1`) | 55.9 |
| attached, full callback (original) | 31.7 |
| pre-filter moved ahead of the lock only | 41.0 |
| pre-filter + stack-read page cache + per-fp negative cache (current) | **52.4** |
| plus a 15 ms per-slot throttle (`DM_BINDING_THROTTLE_MS=15`) | 54.1 (icon learning turns flaky, so it is off by default) |

The `NOOP` row shows the gate itself is cheap; the cost is the stack read inside the callback, which fetches the three words the original getter's prologue has just written (`fp-8`/`fp`/`fp+8`) and used to run a `VirtualQuery` on **every** call to check that the span is committed and readable (`dm_tg_vm_read` in `dm_touch_game_identity.c`). Two caches now:
- a **page cache** of validated spans (`{page,end,ok,seen}`, 4096 entries, 200 ms expiry); unreadable pages are remembered too, since in the guest most `fp` values point at uncommitted or guarded memory and re-checking that batch would save nothing;
- a **negative cache keyed on `fp`**: the same `fp` holds the same `savedLR`, so "this frame is not the HUD updater" is a structural verdict and can be remembered without reading the stack at all;
- the `regs[23] != regs[19]` pre-filter moved ahead of `pthread_mutex_lock` (it used to run after it);
- debug switches: `DM_BINDING_STAGE=1..6` (return at stage n, to find which stage costs), `DM_BINDING_DIAG=1` (per-3 s call and per-return-point counters plus negative-cache hits).

**Note**: `regs[30]` cannot be used as "the return address" — the gate stub has already rewritten it, and filtering on it drops every call (tried, `成功 0`).

## Diagnostics

- For crashes, look at the `FAULT` line from `dm/host/crash.c` (`pcmod=` names the guest module); the VEH stage markers are only printed with `DM_VEH_LOG=1` / `DM_VEH_TRACE=1` — otherwise hundreds of page faults a frame would flood stderr.
- For the picture, use `DM_WIN_SHOT`: `set DM_WIN_SHOT=200+100:C:\...\k%d.bmp` reads back right after `vkCmdBlitImage` and writes a 32-bit BMP after present (BGRA, matching format 0x25's byte order). Convert with `python -c "from PIL import Image; Image.open('k200.bmp').save('k200.png')"`.
- Exit codes: `42` = took the Break branch; `0xC0000028` = bad stack (fixed); `0xC0000005` = write to an uncommitted stack page (fixed); `0xC0000409` = fail-fast (fixed); `139` = `TerminateProcess(128+SIGSEGV)`.
