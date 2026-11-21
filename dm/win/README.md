# dm/win/ — Windows ARM64 host

Only the Windows-specific pieces live here: the window and Vulkan presentation, the front end (keyboard / mouse / text input), the compatibility layer, and the tools used on this platform. The rest of the host (`dm/host/`) and the graphics layer (`dm/gfx/`) are shared with Android and iOS; the build pulls its source lists from `dm/host/Makefile` rather than copying them.

中文：[README.zh-CN.md](README.zh-CN.md)

## Files

| File | Purpose |
|---|---|
| `CMakeLists.txt` | llvm-mingw (aarch64-w64-mingw32) build. There is no ARM64 Vulkan import library, so it links `C:\Windows\System32\vulkan-1.dll` directly; the `log` library is an empty INTERFACE target so `dm/gfx/vulkan/CMakeLists.txt` can be reused unchanged |
| `toolchain-aarch64-w64-mingw32.cmake` | toolchain file (`clang` / `clang++` / `llvm-ar` / `llvm-nm`) |
| `front_win.c` | window, key and mouse injection, the `swkbd` host input box. Client area defaults to 1600x900 (`DM_WIN_SIZE` overrides); the picture is centered at the game's aspect with margins |
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

## Two things learned the hard way

**1. `0xC0000409` (fail-fast, code `0xd`) = `FAST_FAIL_INVALID_SET_OF_CONTEXT`.**
Guest threads run on the guest's own stacks, so their SP is outside the host TEB's `StackBase`/`StackLimit` range. When a page fault is claimed by the VEH and it returns `EXCEPTION_CONTINUE_EXECUTION`, Windows validates the context against the TEB and fail-fasts if the SP is outside it — and `__fastfail` bypasses the VEH, so the log shows no CRASH line at all, just a few `claimed=0` faults.

The fix lives in `compat/crash_win.c`'s `widen_stack_range()`: read the two qwords at `NtCurrentTeb()+8` (`StackBase` / `StackLimit`) and widen them with an 0x8000 margin, **never narrowing**, right before the handler returns "continue execution". The process now runs for as long as you like.

**2. `vkWaitForFences` wakes 12-14 ms late on this Windows ARM driver.**
A submission's GPU work is about 1.5 ms, but the blocking wait returns 13 ms later; with 10-11 waits per frame in a run, that meant 6 fps. The engine-side fix, in `dm/gfx/vulkan/engine/xbg_game.cpp`'s `retire_slot()`, polls `vkGetFenceStatus` first (8 ms spin budget, then falls back to the blocking wait). It is on by default on Windows only; Android and iOS keep the blocking wait (`DM_GFX_FENCE_SPIN` overrides). The encampment went from 6 fps to 42.

## Diagnostics

- For crashes, look at the `FAULT` line from `dm/host/crash.c` (`pcmod=` names the guest module); the VEH stage markers are only printed with `DM_VEH_LOG=1` / `DM_VEH_TRACE=1` — otherwise hundreds of page faults a frame would flood stderr.
- For the picture, use `DM_WIN_SHOT`: `set DM_WIN_SHOT=200+100:C:\...\k%d.bmp` reads back right after `vkCmdBlitImage` and writes a 32-bit BMP after present (BGRA, matching format 0x25's byte order). Convert with `python -c "from PIL import Image; Image.open('k200.bmp').save('k200.png')"`.
- Exit codes: `42` = took the Break branch; `0xC0000028` = bad stack (fixed); `0xC0000005` = write to an uncommitted stack page (fixed); `0xC0000409` = fail-fast (fixed); `139` = `TerminateProcess(128+SIGSEGV)`.
