# dm/win/ — Windows ARM64 宿主

这里只放 Windows 独有的部分：窗口与 Vulkan 呈现、前端（键盘 / 鼠标 / 文本输入）、兼容层，以及这条平台线上用到的工具。宿主其余部分（`dm/host/`）、图形层（`dm/gfx/`）与安卓、iOS 共用同一份源码，构建时把源清单从 `dm/host/Makefile` 取出来，不手抄。

English: [README.md](README.md)

## 文件

| 文件 | 作用 |
|---|---|
| `CMakeLists.txt` | llvm-mingw（aarch64-w64-mingw32）构建。Vulkan 没有 ARM64 导入库，所以直接链 `C:\Windows\System32\vulkan-1.dll`；`log` 库用空的 INTERFACE 目标顶替，好让 `dm/gfx/vulkan/CMakeLists.txt` 原样复用 |
| `toolchain-aarch64-w64-mingw32.cmake` | 工具链文件（`clang` / `clang++` / `llvm-ar` / `llvm-nm`） |
| `front_win.c` | 窗口、键盘鼠标注入、`swkbd` 宿主输入框。客户区缺省 1600x900，`DM_WIN_SIZE` 可改；画面按游戏比例居中留边 |
| `vulkan_present_win.cpp` | 交换链（1600x900、3 张、格式 0x25、`imageUsage` 只有 `TRANSFER_DST`）、blit 上屏、`DM_WIN_SHOT` 读回 |
| `compat/` | Windows 兼容层：PE 段表、自写 `setjmp`/`longjmp`、二进制读 `open`、arena 用的 `aligned_alloc`/`dm_free`、栈预提交、VEH 崩溃处理 |
| `tools/` | 平台工具，见下 |
| `scratch/` | 排障用的小探针（不参与构建） |

## 构建

```
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=dm/win/toolchain-aarch64-w64-mingw32.cmake ^
  -DDM_GEN_DIR=build/win/gen -DCMAKE_BUILD_TYPE=Release -S dm/win -B build/win/obj
ninja -C build/win/obj
```

产物 `build\win\obj\dmhost.exe`。运行方式、环境变量与键位见 [../../docs/WINDOWS_ARM64.zh-CN.md](../../docs/WINDOWS_ARM64.zh-CN.md)。

## 工具

| 工具 | 用法 |
|---|---|
| `tools/keys_win.py` | `python keys_win.py "j u enter" --delay 400`：`PostMessage` 往 `dmhost_win` 窗口投键盘，不需要前台焦点。`"w:1200"` 表示按住 1200 ms（走路用）。**默认按下与松开之间留 140 ms**：客体每帧只采样一次按键状态，同帧按下又松开会被当成没按 |
| `tools/click_win.py` | `python click_win.py <x> <y> [--hold 毫秒] [--right]`：往窗口客户区投鼠标，前进端会把左键当手指按在画面上（触屏 UI 只能这样点）。坐标是**窗口客户区像素**，不是游戏渲染分辨率 |
| `tools/dbg_win.py` | `python dbg_win.py <exe> <args…>`：起进程并挂调试器，能读线程上下文、TEB 栈区间、虚拟内存查询。必须和 `$env:DM_*` 写在同一条命令里，且 `cwd=build\win` |
| `tools/extract_casc.py` | `python extract_casc.py <RomFS> <输出目录>`：从静态 CASC 取出着色器包 |
| `tools/asm_coff.py` | 把 Mach-O 写法的 `.S` 转成 COFF（构建时自动调用） |
| `tools/sym_off.py` | 符号 → 偏移，配合崩溃回溯定位 |

## 两个踩过的坑（改之前先看）

**1. `0xC0000409`（fail-fast，码 `0xd`）= `FAST_FAIL_INVALID_SET_OF_CONTEXT`。**
客体线程跑在客体自己的栈上，SP 不在宿主 TEB 的 `StackBase`/`StackLimit` 区间里。缺页异常被 VEH 认领并返回 `EXCEPTION_CONTINUE_EXECUTION` 时，Windows 会按 TEB 校验现场，区间外就 fail-fast —— 而且 `__fastfail` 绕开 VEH，日志里看不到任何 CRASH 行，只有几条 `claimed=0` 的缺页。

处理在 `compat/crash_win.c` 的 `widen_stack_range()`：读 `NtCurrentTeb()+8` 的两个 qword（`StackBase` / `StackLimit`），以 0x8000 余量**只放宽不收窄**，在 VEH 处理器返回「继续执行」之前调用。修完以后进程能长期稳定跑。

**2. `vkWaitForFences` 的唤醒延迟在这块 Windows ARM 驱动上是 12~14 ms。**
一次提交的 GPU 工作只有约 1.5 ms，但阻塞等待要等 13 ms 才回来；局内每帧 10~11 次等待，于是 6 帧。引擎侧的改法是在 `dm/gfx/vulkan/engine/xbg_game.cpp` 的 `retire_slot()` 先轮询 `vkGetFenceStatus`（自旋预算 8 ms，超了退回阻塞等待），Windows 上默认打开，安卓 / iOS 默认仍是阻塞等待（`DM_GFX_FENCE_SPIN` 可覆盖）。营地因此从 6 帧到 42 帧。

## 排障

- 崩溃看 `dm/host/crash.c` 的 `FAULT` 行（`pcmod=` 会给客体模块归属）；`DM_VEH_LOG=1` / `DM_VEH_TRACE=1` 才打 VEH 阶段标记，平时不打（缺页每帧几十次会把 stderr 冲烂）。
- 画面取证用 `DM_WIN_SHOT`：`set DM_WIN_SHOT=200+100:C:\...\k%d.bmp`，在 `vkCmdBlitImage` 之后读回、上屏后落盘 32 位 BMP（BGRA，字节序和格式 0x25 一致）。转 PNG：`python -c "from PIL import Image; Image.open('k200.bmp').save('k200.png')"`。
- 退出码：`42` = 走了 Break 分支；`0xC0000028` = 坏栈（已修）；`0xC0000005` = 栈未提交页写（已修）；`0xC0000409` = fail-fast（已修）；`139` = `TerminateProcess(128+SIGSEGV)`。
