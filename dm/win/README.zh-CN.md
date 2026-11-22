# dm/win/ — Windows ARM64 宿主

这里只放 Windows 独有的部分：窗口与 Vulkan 呈现、前端（键盘 / 鼠标 / 文本输入）、兼容层，以及这条平台线上用到的工具。宿主其余部分（`dm/host/`）、图形层（`dm/gfx/`）与安卓、iOS 共用同一份源码，构建时把源清单从 `dm/host/Makefile` 取出来，不手抄。

English: [README.md](README.md)

## 文件

| 文件 | 作用 |
|---|---|
| `CMakeLists.txt` | llvm-mingw（aarch64-w64-mingw32）构建。Vulkan 没有 ARM64 导入库，所以直接链 `C:\Windows\System32\vulkan-1.dll`；`log` 库用空的 INTERFACE 目标顶替，好让 `dm/gfx/vulkan/CMakeLists.txt` 原样复用 |
| `toolchain-aarch64-w64-mingw32.cmake` | 工具链文件（`clang` / `clang++` / `llvm-ar` / `llvm-nm`） |
| `front_win.c` | 窗口、键盘鼠标注入、`swkbd` 宿主输入框。客户区缺省 1600x900，`DM_WIN_SIZE` 可改；画面按游戏比例居中留边 |
| `pad_win.c` | 实体手柄：XInput 轮询线程 → 宿主 `dm_pad_set`，判据与 iOS `dm_gamepad.m` / 安卓 `PadInput.java` 一致（见下「手柄」） |
| `vulkan_present_win.cpp` | 交换链（1600x900、3 张、格式 0x25、`imageUsage` 只有 `TRANSFER_DST`）、blit 上屏、`DM_WIN_SHOT` 读回 |
| `touch_win.cpp` / `touch_win.h` | 触屏操控浮层：编进与安卓 / iOS 同一份 `dm/ios/Sources/dm_touch_controls.c` 状态机，鼠标当手指喂入，发布成按键 / 摇杆走 `dm_pad_hook`，并用 GDI+ 画到分层窗口（外观照 `DmTouchControlArt.h` 的矢量配方）。进局自动切触屏布局、出局切回手柄布局 |
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
| `tools/click_win.py` | `python click_win.py <x> <y> [--hold 毫秒] [--right] [--raw]`：往窗口客户区投鼠标，前进端会把左键当手指按在画面上（触屏 UI 只能这样点）。坐标是**窗口客户区像素**，不是游戏渲染分辨率。**125% 缩放的屏幕上 `PostMessage` 的坐标会被系统再乘一次 1.25**，脚本按 `GetDpiForWindow` 先除回去（`--raw` 不除） |
| `tools/dbg_win.py` | `python dbg_win.py <exe> <args…>`：起进程并挂调试器，能读线程上下文、TEB 栈区间、虚拟内存查询。必须和 `$env:DM_*` 写在同一条命令里，且 `cwd=build\win` |
| `tools/extract_casc.py` | `python extract_casc.py <RomFS> <输出目录>`：从静态 CASC 取出着色器包 |
| `tools/asm_coff.py` | 把 Mach-O 写法的 `.S` 转成 COFF（构建时自动调用） |
| `tools/sym_off.py` | 符号 → 偏移，配合崩溃回溯定位 |

## 手柄

`pad_win.c` 用 XInput（`xinput1_4` → `1_3` → `9_1_0` 依次动态加载，不引导入库），8 ms 轮询一次（125 Hz，比游戏取手柄的 60 Hz 快，短按不会漏），把状态交给宿主 `dm_pad_set`。与 iOS / 安卓同一套判据：

- 手柄有明确输入（按下任何键、或摇杆推出 25%）→ 切手柄布局（`dm_padmode_request(1)`），并先报键帽样式；
- 敲键盘 / 用鼠标点画面 → 切回触屏布局，摇杆要先回中、再推出来才算重新接管；
- 触屏布局下手柄的键和摇杆都不交给游戏；手柄布局下没推出来的摇杆给 0；
- 多只手柄按键取或、每根摇杆取推得最远的。

面键（**这个游戏是 Switch 版，键位含义按 Switch 键帽走**）：

| XInput 物理键 | 默认（按位置） | `DM_PAD_AB=switch` |
|---|---|---|
| 下 A | A 确认 | B 返回 |
| 右 B | B 返回 | A 确认 |
| 左 X | Y | Y |
| 上 Y | X（放进堆叠仓库） | X |

XInput 一律按键帽位置报告（下 = A、右 = B、左 = X、上 = Y），而 Xbox 手柄字母与位置一致，所以默认就是「下 = 确认」，与 PC / Xbox 版同位置同功能。**如果手柄是按 Switch 字母报键的**（Switch Pro 经转换器、或玩家习惯「右 A 确认」），设 `DM_PAD_AB=switch` 只换 A/B。键帽样式报 Xbox（覆盖层没有那套图时 `dm_padmode.c` 退回原版 Switch 键帽）。

肩键 L/R、扳机 ZL/ZR（按过 30/255）、摇杆按下 LS/RS、菜单键 → ＋、视图键 → －、十字键照位对。

| 环境变量 | 作用 |
|---|---|
| `DM_PAD=0` | 不接手柄 |
| `DM_PAD_AB=switch` | 面键换 Switch 语义（右 A 确认） |
| `DM_PAD_LOG=1` | 打连接与状态变化 |
| `DM_PAD_TEST=<秒>[,<秒>...]` | 没有实体手柄时，在启动后第 N 秒各注入一次 0.25 s 的 A 短按，验通路（可给多次，最多 8 个，如 `52,70,88`） |

没有实体手柄也能验：`DM_PAD_TEST=52` 启动，日志会依次出现 `手柄：XInput 已就绪`、`注入 A`、`有明确输入 → 手柄布局`，画面从标题页跳到角色列表即可确认「前端 → 宿主 → 游戏」这条通路通了；要往菜单更深处走就多给几个时刻（`DM_PAD_TEST=52,70,88`），每次都是 A 短按。

## 两个踩过的坑（改之前先看）

**1. `0xC0000409`（fail-fast，码 `0xd`）= `FAST_FAIL_INVALID_SET_OF_CONTEXT`。**
客体线程跑在客体自己的栈上，SP 不在宿主 TEB 的 `StackBase`/`StackLimit` 区间里。缺页异常被 VEH 认领并返回 `EXCEPTION_CONTINUE_EXECUTION` 时，Windows 会按 TEB 校验现场，区间外就 fail-fast —— 而且 `__fastfail` 绕开 VEH，日志里看不到任何 CRASH 行，只有几条 `claimed=0` 的缺页。

处理在 `compat/crash_win.c` 的 `widen_stack_range()`：读 `NtCurrentTeb()+8` 的两个 qword（`StackBase` / `StackLimit`），以 0x8000 余量**只放宽不收窄**，在 VEH 处理器返回「继续执行」之前调用。修完以后进程能长期稳定跑。

**2. `vkWaitForFences` 的唤醒延迟在这块 Windows ARM 驱动上是 12~14 ms。**
一次提交的 GPU 工作只有约 1.5 ms，但阻塞等待要等 13 ms 才回来；局内每帧 10~11 次等待，于是 6 帧。引擎侧的改法是在 `dm/gfx/vulkan/engine/xbg_game.cpp` 的 `retire_slot()` 先轮询 `vkGetFenceStatus`（自旋预算 8 ms，超了退回阻塞等待），Windows 上默认打开，安卓 / iOS 默认仍是阻塞等待（`DM_GFX_FENCE_SPIN` 可覆盖）。营地因此从 6 帧到 42 帧。

再往后到 50~55 帧两步在 `front_win.c` 的 `tuning_defaults()`（把安卓实测过的引擎开关按同一套设默认值，每帧的提交与栅栏等待从 10 次 / 10 次变成 1 次 / 0 次）和 `vulkan_present_win.cpp`（出图改用 `FIFO_RELAXED`、交换链 3 张图）。逐项数据见 `docs/WINDOWS_ARM64.zh-CN.md` 的「性能」。

**3. Windows 的定时器粒度是 15.6 ms，客体的每一次超时都被按它取整。**
帧时间中位数明明 13~15 ms，均值却只有 50 出头、`p95` 37 ms：`[长帧]` 明细里既没有新建管线也没有上传贴图，尖峰整块多出的正好是 15~16 ms，也就是一个系统定时器粒度——`pthread_cond_timedwait` / `nanosleep` 的超时全被它取整。安卓 / iOS 的定时器是 µs 级，所以手机上没这个坑。`front_run()` 里调 `raise_timer_resolution()`：先 `NtSetTimerResolution(5000, ...)` 要 0.5 ms（日志 `计时器精度：请求 0.5 ms，系统给的是 0.50 ms`），失败再退 winmm 的 `timeBeginPeriod(1)`。营地因此从 50 出头到 **55~60 帧**，`掉帧` 从 86/300 降到 51/300。

**4. 触屏那套「替玩家按键」的前端流程会把键盘 / 手柄玩家卡在选角页。**
`dm/host/dm_cursor.c` 的「前端路径理顺」是给 iOS / 安卓定的：有存档就在主菜单替按 X 开选角页（开机第一屏成了人物列表）、列表一收起又替按 X 重开（选了角色后又被重开，按一次 A 进不去）。键盘自带原版流程，`tuning_defaults()` 因此把 `DM_FE_AUTO` 设成 `0`：开机停在主菜单，A 进选角页，再按 A 开始游戏。

**5. 少了运行库 dll，`dmhost.exe` 会以 `0xC0000135` 秒退、日志一个字都没有。**
`CMakeLists.txt` 用 `-static` 静态链 libc++ / libunwind，只有 `libwinpthread-1.dll` 还是导入项，构建完由 POST_BUILD 从工具链的 `bin` 拷到 exe 旁边。双击 `start-win-arm64.bat` 时 PATH 里没有工具链目录，少了这份 dll 就静默退出（当时误以为是新代码崩了）。

**6. 覆盖层里触屏版布局把前端控件整排挪走，桌面形态切过去主菜单就只剩背景。**
界面覆盖层（`gen_overlay_win.ps1` 的产物）带两套布局：触屏版的前端按钮栏、键位提示由宿主的触屏层另画，原版手柄版保留游戏自己的。Windows 是键盘 / 鼠标，没有触屏那层绘制，一旦触发「敲键盘 / 碰画面 → 切触屏版」（`pad_win.c` 的 `dm_win_pad_touch`）就变成主菜单空屏、按 A 没反应。现在 `tuning_defaults()` 设 `DM_APP=1` 走桌面形态：开机锁在原版手柄版布局，`dm_padmode_request` 里的 `g_lock` 也把它锁死，不再切触屏版。

**7. 截图取证别用 `PrintWindow`，用 `DM_WIN_SHOT`。**
Vulkan 交换链上屏的窗口，`PrintWindow` + `PW_RENDERFULLCONTENT` 抓到的不是完整画面：局内会抓成放大的世界画面、把底栏 HUD 整条漏掉（一度以为 HUD 没出来，`DM_WIN_SHOT` 的读回里 HUD 好好的）。前端（主菜单 / 选角页）反而是准的。

**8. 触屏操控浮层（分层窗口）谁都拍不到，只能让它自己存位图。**
浮层是 `WS_EX_LAYERED` 的独立窗口，`DM_WIN_SHOT` 走 Vulkan 读回只含游戏自己的画面；`PrintWindow` 拿不到；本机连 `CopyFromScreen` 都是纯黑（`VirtualScreen 2048x1152` 抓出来全 0）。所以 `touch_win.cpp` 加了 `DM_WIN_TOUCH_DUMP=<路径带 %d>`：每次重画把自绘位图存一份 32 位 BMP（第 1 次 + 每 5 次）。看这份位图有两点要注意：
- BMP 行序是**自下而上**（`bitmap.bmp` 里 `biHeight` 为正），直接合成会看到整个浮层上下颠倒——按行翻一次再看。
- Pillow 读 32 位 BI_RGB BMP **会丢掉 alpha**（全读成 255）。按原始 BGRA 读：`np.frombuffer(d, dtype=np.uint8, offset=54).reshape(-1,4)`，才能看出背景 alpha=0、圆钮 alpha 0~235 的那几档。

**9. `gdiplus.h` 在 C++ 里缺 `PROPID`；`ColorBlend` 也没有。**
mingw 的 `gdiplus/gdiplusimaging.h` 用了 `PROPID` 却没带 `ole2.h`，所以 `#include <windows.h>` 之后要先 `#include <ole2.h>` 再 `#include <gdiplus.h>`（否则二十来处 `unknown type name 'PROPID'`）。另外这套头里**没有** `ColorBlend`，多色标渐变只能逐行插值填（`touch_win.cpp` 的 `grad_v()`）；`Pen::SetLineCap` 在这版头里要三个参数（起 / 止 / 虚线帽）。

**10. 重链接 `Permission denied` = 旧 `dmhost.exe` 还在跑。**
`ld.lld: error: failed to write output 'dmhost.exe': Permission denied` 不是权限问题，是上一次的进程没退。先 `Get-Process dmhost | Stop-Process -Force`（等 2~3 s 释放映像）再 ninja。

**11. 浮层「贴图成功、`IsWindowVisible`=1、屏幕上却什么都没有」= z-order，不是分层窗口的问题。**
`front_win.c` 建游戏窗口时带了 `WS_EX_TOPMOST`，而浮层当初只是个普通 owned popup：`UpdateLayeredWindow` 每次都返回成功、`GetWindowRect` / 可见性也都对，但窗口被 topmost 的游戏窗口整块压在下面。修法：浮层自己也加 `WS_EX_TOPMOST`，并在每次贴图时 `SetWindowPos(g_ov, g_game, …)` 把顺序钉成「浮层紧跟游戏窗口之上」。日志现在会打 `触屏浮层：贴图成功，窗口 <x>,<y> <w>x<h>（可见 1）`，坐标一开始是 `2099521032` 这种垃圾值——那是 `POINT dst` 没初始化 + `ClientToScreen` 走了失败分支，已置 `{0,0}` 并在失败时回退 `GetWindowRect`。

**12. 技能图标：GDI+ 的 `SetClip` 在这条路径上不生效，得自己按像素裁圆。**
原版图集每格是一块**方角**技能牌，iOS 靠 `CGContextAddEllipseInRect(...); CGContextClip(...)` 把它裁成圆的。照搬到 GDI+（`GraphicsPath::AddEllipse` + `SetClip(&path)` + `DrawImage`）之后，图还是方的、四角铆钉一个不少——裁切完全没起作用。现在做法是：先把这一格按目标尺寸画进一张临时 `Bitmap`，`LockBits` 后逐像素按圆半径（按钮的 .82，即 iOS 的「外接矩形内缩 9%」）乘 alpha，再画这张已经裁好的图，按 `(图集, 帧, 宽, 高)` 缓存。另外图集索引要 `clang` 在 PATH + **ARM64 的 python** 才能生成（x64 python 起子进程直接 `WinError 2`），缺产物 CMake 会 `FATAL_ERROR`。

**13. 触屏浮层一开，界面按钮全点不动：鼠标点击被浮层「吞掉」了。**
浮层开着时 `front_win.c` 把 `WM_LBUTTONDOWN/UP/MOUSEMOVE` 一律先喂给 `dm_win_touch_pointer`，而它当时无条件 `return`（等于吞掉），于是原版 UI 收不到任何点击——主菜单「进入游戏 / 选项」、局内的物品栏 / 技能树 / 选项这些页签全部失灵。现在分两步：
- `dm_win_touch_pointer` 改成**返回 int**：按下时只有真命中控件（`finger[slot].target != DM_TC_NONE || pointer_slot == slot || joy_slot == slot`）才算它处理，否则 `dm_tc_cancel_finger` 并返回 0；移动 / 抬起靠 `g_claim[]` 记住归属，未归属也返回 0（漏掉抬起会留下「一直按着」的手指）。
- 没被浮层认领的点击 = **界面点击**，走 `dm_ui_touch`（`dm/host/dm_cursor.c`，和 iOS 前端页同一条通道），**前端页和局内面板都走这条**：局内的物品栏 / 技能树 / 选项也是原版 UI，同样靠它按得动（实测「技能树」页签能切过去）。日志是 `前端：界面点击 (%.3f,%.3f)`。

**14. 开局 60 帧、进局掉到 30 帧：开销全在技能绑定的观测门回调里。**
`dm_touch_game_bridge.c` 的 `binding_getter` 是打在原版 getter 上的观测门回调，Windows 上被游戏叫到 **3~6 千次/秒**，每次都要离开客体进宿主函数。逐项对照（营地静置、`-NoClick`、`NVN PERF` 均值）：

| 配置 | 均值 fps |
|---|---|
| 不挂桥（`DM_WIN_TOUCH_NOBIND=1`） | 56.6 |
| 挂桥 + 回调立刻 return（`DM_BINDING_NOOP=1`） | 55.9 |
| 挂桥 + 完整回调（原状） | 31.7 |
| 只加「前置筛前移」 | 41.0 |
| 前置筛 + 栈读页缓存 + 按 fp 负缓存（现在） | **52.4** |
| 再加 15 ms 按槽位节流（`DM_BINDING_THROTTLE_MS=15`） | 54.1（但图标会时好时坏，故缺省关） |

`NOOP` 那条证明门本身很便宜，贵的是回调体里的栈读：它要读原版 getter 序言刚写下的 3 个字（`fp-8`/`fp`/`fp+8`），原先**每次**都先 `VirtualQuery` 查这段是不是已提交可读（`dm_touch_game_identity.c` 的 `dm_tg_vm_read`）。现在两处缓存：
- **按页缓存**已验证区间（`{page,end,ok,seen}`，4096 项，200 ms 过期）；读不出来的页也照记——客体里大半 `fp` 都指向没提交 / 带守卫的内存，那批要是每次重查等于没省。
- 按 `fp` 的**负缓存**：同一个 `fp` 存着的 `savedLR` 也是同一个，所以「这一帧不是 HUD updater」是结构性结论，记下来连栈都不用读。
- 前置筛 `regs[23] != regs[19]` 移到取锁之前（它原先在 `pthread_mutex_lock` 之后）。
- 排查开关：`DM_BINDING_STAGE=1..6`（走到第 n 段就 return，用来定位是哪一段花钱）、`DM_BINDING_DIAG=1`（每 3 s 打调用 / 各返回点计数 / 负缓存命中）。

**注意**：`regs[30]` 不能当「返回地址」用——进了回调它已经被门桩改写，拿它做筛会把所有调用筛光（试过，`成功 0`）。

**15. 图标退回键位字母，跟节流没关系——是「六槽一组」拼不齐 + 快照被半成品覆盖。**
`latest` 只在 `dm_tg_group_observe` 把**整帧六次** getter 调用凑成一组后才算齐（实测也就 4 组/秒）。所以：
- 任何把慢路径压到「每帧 1~2 次」的节流（100 ms、16 ms 按时间掐都算）都会让 group 永远差几位，`latest` 被 `ready=0` 的半成品覆盖，图标退回键位字母——先前的结论「节流太稀导致快照过期」只对了一半。
- 现在确认后改成**按帧预算**：每 16 ms 窗口最多放 16 次慢路径（`DM_BINDING_CONF_BUDGET`，0 = 关），够六槽（含副手组）拼齐，又比不节流省两个数量级的进门次数。
- 图标侧再放宽一步（`touch_win.cpp`）：查询用快照**自己的采集时刻`，组与组之间 200~400 ms 的空档不再被判过期（iOS 的 250 ms 门只适合它那边的刷新节奏）。
- 诊断：`DM_BINDING_DIAG=1` 现在多两行——`绑定快照：valid/ready/portal/group_seen/age_ms`、`绑定槽过：0..5 各通过几次`，一眼看出 group 拼到第几位、哪个槽没过。

**16. 剩下的那 2 帧不在浮层里，在客体主线程的等锁唤醒上。**
`DM_STALL_PROBE=1` 取栈（营地静置、浮层开着、`DM_LONGFRAME=0`）：长帧命中的是 `ntdll!NtWaitForAlertByThreadId <- ntdll`（`WaitOnAddress` 那类 futex 等待）和少量 `NtWriteFile <- WriteFile`，`host+0xd3xxx` 是我们的 `hle_thread.c`。也就是客体主线程每 9 帧左右等一次客体锁 / 条件变量，唤醒晚 12~15 ms。这条与浮层无关（不挂桥、无浮层同样存在），`DM_HLE_SPIN_US=800` 对它无效。现状：带浮层 avg 57.5 / 末 20 窗口 58.2 / 峰值 60.8；不挂浮层 56.5~57.6。


## 排障

- 崩溃看 `dm/host/crash.c` 的 `FAULT` 行（`pcmod=` 会给客体模块归属）；`DM_VEH_LOG=1` / `DM_VEH_TRACE=1` 才打 VEH 阶段标记，平时不打（缺页每帧几十次会把 stderr 冲烂）。
- 画面取证用 `DM_WIN_SHOT`：`set DM_WIN_SHOT=200+100:C:\...\k%d.bmp`，在 `vkCmdBlitImage` 之后读回、上屏后落盘 32 位 BMP（BGRA，字节序和格式 0x25 一致）。转 PNG：`python -c "from PIL import Image; Image.open('k200.bmp').save('k200.png')"`。
- 退出码：`42` = 走了 Break 分支；`0xC0000028` = 坏栈（已修）；`0xC0000005` = 栈未提交页写（已修）；`0xC0000409` = fail-fast（已修）；`139` = `TerminateProcess(128+SIGSEGV)`。
