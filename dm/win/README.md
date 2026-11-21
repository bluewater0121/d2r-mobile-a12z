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

## Diagnostics

- For crashes, look at the `FAULT` line from `dm/host/crash.c` (`pcmod=` names the guest module); the VEH stage markers are only printed with `DM_VEH_LOG=1` / `DM_VEH_TRACE=1` — otherwise hundreds of page faults a frame would flood stderr.
- For the picture, use `DM_WIN_SHOT`: `set DM_WIN_SHOT=200+100:C:\...\k%d.bmp` reads back right after `vkCmdBlitImage` and writes a 32-bit BMP after present (BGRA, matching format 0x25's byte order). Convert with `python -c "from PIL import Image; Image.open('k200.bmp').save('k200.png')"`.
- Exit codes: `42` = took the Break branch; `0xC0000028` = bad stack (fixed); `0xC0000005` = write to an uncommitted stack page (fixed); `0xC0000409` = fail-fast (fixed); `139` = `TerminateProcess(128+SIGSEGV)`.
