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

**17. 输入源接管照 iOS，但「切过去的那一下」会被输入闸吃掉——要补发。**
iOS 的判据（`dm_gamepad.m` `apply` / `front_ios.m` `padMode:`）只有两条：手柄有**明确输入**（按任何一颗键，或摇杆推过 25%，回 15% 内算回中）→ 手柄模式、收起浮层、`dm_tc_cancel_all`；碰画面 / 敲键盘 → 触屏模式、露出浮层。Windows 原先没有这层概念（进局就亮、一直亮），现在按同一套做（`touch_win.cpp` 的 `g_pad_in` + `dm_win_touch_pad_input/_screen_input`）：
- 浮层显隐 = `!in_run || pad_in || dm_padmode_busy()`；最后一条是 iOS `front_ios.m:497` 每帧也算进去的——切换事务那 12 帧整块界面在重建，浮层压在上面等于盖住半成品。
- 布局请求跟着输入源走（`scene_apply`：`(in_run && !pad_in) ? 0 : 1`），`pad_step` 在手柄接管时早退（触控按键不再合进游戏读的那一拍）。

坑在这里：`dm_padmode` 切布局时输入闸（`dm_modetx.h` 的 `hold`）会把**当时按着的键全扣下，且要松开一次才再交**——设计如此（「按 A 切界面，这一下 A 不能落到刚建好的界面上」）。结果是从触屏版按手柄第一下：镜头不转、技能不放，玩家只会说「手柄没反应」，而这一下其实全花在切换上了。iOS 也吃这一下，但它那边手柄模式一旦成立就基本不再碰屏幕，所以碰不到；Windows 是鼠标点面板 → 再用手柄，来回切，每切一次丢一下。
现在 `pad_win.c` 的 `publish` 把**触发切换的那一下**记进 `g_replay`，等事务结束（`!dm_padmode_busy()`）、玩家**也松开**之后再补发 8 拍（64 ms，`dm_pad_set` 有按下沿锁存，一拍就够游戏看见）。只在**局内**补：菜单里补发等于替玩家「确认」刚重建出来的界面，正是输入闸要避免的。日志 `手柄：补发触发切换的那一下 %#llx`。

同一处还有个自己挖的坑：圆钮「差一点没点到就吸附」最初写成 `cancel_finger` 后在同一 slot 再 `begin_at` 钮心，实测按下那一拍发布的是 `瞄准 -32767,32767`（旧日志全是 `0,0`）。`dm_tc_begin_at` 自带早退、`cancel_finger` 会 `++gesture_epoch` 并 `update` 一次，两次状态迁移把瞄准轴带偏了。改成**落点先吸附再单次 `begin_at`**（`dm_tc_hit == NONE` 时找 ≤1.25r 的圆钮、改写 x/y、摇杆不吸），aim 回到 `0,0`。触控灵敏度另外两项：tick 30 ms → **16 ms**（发布延迟 ≤1 帧），圆钮命中放宽到 1.25r。

**18. 「手柄按下去浮层还在」：浮层通知被塞在 `set_mode()` 里，而 `set_mode` 被桌面锁卡死在 1。**
`pad_win.c` 原先在 `set_mode()` 里调 `dm_win_touch_pad_input()`，但 `set_mode` 开头有两道早退：
```c
if (g_mode == pad) return;
if (!pad && getenv("DM_APP")) { g_need_center = 1; return; }   // ← 只在 DM_APP 下
```
`DM_APP`（Windows 桌面形态，探测脚本也带）配合第二道早退，`set_mode(0)` 直接 return、**不改 `g_mode`**，于是 `g_mode` 永远是 1；之后手柄一按下走 `set_mode(1)`，第一道早退又把它挡掉——**浮层的「手柄接管」通知压根没被调到**。碰画面能露浮层是因为那条走 `dm_win_touch_screen_input()`（另一个调用），所以症状恰好是「鼠标点了浮层回来了，手柄一按却不收回去」。注入 A 的探针之所以测得通，是因为那次运行全程只有键盘（`on_key_msg` → `dm_win_pad_touch`），`g_mode` 还是 0，第一次 `set_mode(1)` 真的发生了——真机手柄在菜单里就用过，`g_mode` 早就是 1 了，撞不上这条。

修法是把「布局换没换」和「输入源归谁」彻底拆开：
- `set_mode()` 只改 `g_mode` + `dm_padmode_request()`；桌面布局锁交给 `dm_padmode_request()` 里的 `g_lock`（`DM_WIN_TOUCH=1` 时它是放开的），不在这里再判一次 `DM_APP`。
- 浮层通知挪到**语义源头**，与模式变没变无关：`publish()` 里「手柄有明确输入」分支调 `dm_win_touch_pad_input()`；`dm_win_pad_touch()` 一进来就调 `dm_win_touch_screen_input()`。
- 顺带补 iOS `drop()` 的语义：最后一个手柄断开 → 交回触屏侧。
- `dm_win_touch_tick()` 显隐变化时打一行 `触屏浮层：露出/收起（运行 d 手柄接管 d 切换中 d）`，真机再出问题能直接看出是哪个判据没成立。

实测（本机接着一支 XInput 手柄，`DM_PAD_TEST=62,81` + 80 s 鼠标点画面）：
```
触屏浮层：露出（运行 1 手柄接管 0 切换中 0）
触屏浮层：手柄有输入 → 收起浮层（手柄布局）
触屏浮层：收起（运行 1 手柄接管 1 切换中 1）
触屏浮层：碰了画面 / 敲键盘 → 露出浮层（触屏布局）
触屏浮层：露出（运行 1 手柄接管 0 切换中 0）
```
一趟里 3 次布局切换、0 次超时；`fps` 均值 54.5（这趟有实体手柄 125 Hz 轮询 + 3 次重建）。

**19. 收起之后再露出，浮层「贴图成功、可见=1、桌面上什么都没有」——z 序插到游戏窗口下面了。**
症状是玩家点一下画面浮层不回来，非得改一下窗口大小或最小化再恢复。旧代码露出时调的是
```c
SetWindowPos(g_ov, g_game, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // 注释写「紧贴其上」，实际是插到 g_game 下面
```
z 序里 `hWndInsertAfter = g_game` 是**放到它后面**（below），所以露出后浮层被不透明的游戏窗口整个盖住；`UpdateLayeredWindow` 只改内容和位置、不改 z 序，因此「贴图成功」的日志照样打。而改变窗口大小 / 最小化再恢复时，系统会把 owned 窗口重新抬到 owner 上面，所以那两个动作能把它弄出来——这就是这个症状的指纹。

改法：
- 露出时改用 `SetWindowPos(g_ov, HWND_TOPMOST, …, SWP_NOACTIVATE | SWP_NOOWNERZORDER)`，放进 topmost 带的最顶；
- 只把浮层窗口也算进来还不够可靠（`GW_HWNDPREV` 只给紧邻上一号，中间夹着任务栏 / IME 时看不出真实位置），所以加了个 `z_rank()`：从 `GetTopWindow(NULL)` 顺着 `GW_HWNDNEXT` 数，数字越小越靠前；
- 露着的时候每 8 拍（约 128 ms）核一次 `z_rank`，掉到游戏窗口后面就重抬（点游戏窗口 / 切回前台都会把游戏窗口抬上来）；
- 只在游戏窗口在前台时抬，免得玩家切到别的程序时浮层压在别人上面。
实测露出那一拍：`z序 浮层=13 游戏=42（小 = 更靠前）`，`位图已存 …-ov2.bmp`，之后 0 次重抬。

顺带确认：布局刚重建完的头一帧技能钮还是键位字母（六槽快照要凑齐一组才有效，约 250 ms），随后自动换成图标。

**20. 「绑定的技能很容易丢，指针点了没反应」——丢的不是游戏槽位，是浮层自己的图标快照；没反应的点击落在手柄接管期间。**
逐项实测（`tap-seq.ps1` 连点 A/B/X/Y/R/回城 + 三处钮间空隙，`switch-test.ps1` 做手柄↔触屏往返，都开 `DM_BINDING_DIAG=1`）后，可以把三件事分开：

- **游戏槽位没变**：每轮切换打的 `技能格（…当前武器组 1）A=0/-1 B=-1/359 X=373/-1 Y=395/-1 R=-1/-1` 从头到尾一致，连点 10 下（含三处空隙误触）前后两张 dump 逐像素一致。所以「技能被清空」不成立。
- **点击也确实发布了按键**：每次 `手指按下 … 目标 0..5 → 触屏控件` 后面都跟着 `按键 0 → 0x1 / 0x2 / 0x4 / 0x8 / 0x80`。
- **真正会「丢」的是浮层画图标用的那份快照**：`dm_touch_game_bridge.c` 的六槽要等一整帧的六次 getter 调用才拼成一组，拼不齐整组作废；Windows 上它每两拍就抖一次 `DM_TA_UNKNOWN`，布局重建（切手柄版 / 切触屏版）之后更是连续 0.5 s 左右全是 UNKNOWN。老代码这时就退回键位字母，看起来就是「这些钮没绑技能」。

改法（`touch_win.cpp` 的 `glyph_resolve` / `glyph_warm`）：

- 每个圆钮存一份**上次有效图标** `g_glyph[]`，这一拍读到 UNKNOWN 就沿用它，绝不退回键位字母；读到 `DM_TA_EMPTY` 才画空槽十字。真换技能 / 真清槽才更新缓存并打一行（`换了技能` / `槽空了`）。
- 关键的一点：**收起的时候也要解析**。图标原先只在 `paint()` 里算，而手柄接管与布局切换期间浮层是收起的、根本没人 paint，于是切回触屏版露出的第一张全是字母。现在 `dm_win_touch_tick()` 每 4 拍（64 ms）调一次 `glyph_warm()`（只读共享表、不绘制），露出第一张就有图标。实测 `inP-ov2.bmp`（切换后露出的第一拍）已经是 A 箭头 / B 骷髅 / X 爪 / Y,R 空槽十字 / 回城漩涡。
- 日志只记「断档超过 0.5 s」和「真换技能」，不再逐拍刷（桥每两拍抖一次，逐拍照抄会淹掉 stderr）：`技能钮 N 的绑定读不到，持续了 547 ms（技能 373 还在，图标用上一次的）`。
- `pad_step` 去掉 `g_pad_in` 早退（现只剩 `!g_on || !g_in_run`）：手柄接管期间浮层虽是收起的，鼠标仍可能点中它的钮位；早退的话这一下直接没人管，等于「点了没反应」。现在照样按触摸路径发布按键——实测切换事务进行中也能打出 `按键 0 → 0x1`。

排查期间还确认了一个坑：`DM_PAD_TEST` 那轮 `switch-test` 里 70 s 的鼠标点击被判成 `目标 -2`（`DM_TC_JOYSTICK`），是当时有人在实时操作这台机器，不是坐标换算错——同一条命令在 `switch-test -Tag swC` 里正常判成 `目标 0 → 触屏控件`。

**21. 技能点一下之后就一直在放——「松手施放的脉冲」在 Windows 上没人收回。**
`dm_touch_controls.c` 有几条路不在松手时直接发键，而是把键落进一个脉冲（`s->pulse_button`），由前端自己收回：按下不足 2 帧的快速点按（`DM_TC_KEY_MIN_S` = 0.035）、第二组技能 ZL 没领先够的延后发、界面类点按（技能绑定框、腰带格、拖拽的拿起 / 放下）。iOS 由 `front_ios.m:744` 的 `pulseTick` 收回（判据：「游戏已取到这一次」+ 送达后再按 0.12 s + 脉冲自己要按住的 `pulse_hold_s`，2 秒兜底）。而 `dm/win` 里**一次 `dm_tc_pulse_end` 都没有**，于是脉冲落地就一直按着——每帧 `update()` 都把 `pulse_button` 并进 `s->buttons`，游戏看到的就是「按键一直按住」，技能自然一直放。

改法（`touch_win.cpp` 新增 `pulse_pump()`，在 `pad_step` 里推进状态机之后调）：
- 「游戏取到了没有」用 `pad_step` 被调的次数（那就是游戏读输入的采样）来数，判据照 iOS：`≥3 次采样` 且 `≥0.12 s`，`pulse_hold_s > 0` 时还要按住满它；
- 2 秒 + hold 兜底强制收回，并打一行 `脉冲 2 秒内没被取走（取了 N 次），强制收回`；
- 收回时打 `脉冲收回 0x4（取了 9 次，按住 158 ms）`，一眼能对上「按下 → 松开」；
- `dm_tc_pulse_pending`（拖拽的「放下」比「拿起」的收回先到）也补上了：等游戏看过 2 次松开、或过 0.5 s 再 `dm_tc_pulse_resume`。

实测：`-Hold 8`（8 ms 的超短点按，必定走脉冲）→ `按键 0 → 0x4` → `脉冲收回 0x4（取了 9 次，按住 158 ms）` → `按键 0x4 → 0`，不再永久按住；`tap-seq` 连点十下，每一下都是 `0 → 0x?` 紧接 `0x? → 0`。

**22. 浮层不在游戏窗口里（全局浮在最上面，游戏最小化还留在桌面上）——它原先是一个独立置顶弹窗。**
`dm_win_touch_start` 建的是 `WS_POPUP + WS_EX_TOPMOST` 的独立窗口，再用 `UpdateLayeredWindow` 的 `pptDst` 把它摆到游戏客户区原点。这样它和游戏窗口之间只有「碰巧对齐」的关系：不裁剪、不随窗口移动、最小化不跟着收，Alt-Tab 也能看到它，切到别的程序它照样压在人家上面。

改法是把它做成游戏窗口的**子窗口**。这里踩了三层：

- 直接带 `WS_EX_LAYERED` 建 `WS_CHILD`：本机（Windows 11 ARM64）`CreateWindowEx` 返回空、`GetLastError` 是 **0**；
- 先建普通子窗口再 `SetWindowLongPtr(GWL_EXSTYLE, … | WS_EX_LAYERED)`：窗口能建出来，但 `UpdateLayeredWindow` 一律 `ERROR_INVALID_PARAMETER (87)`，贴不上图（`pptDst` 传 NULL、传屏幕坐标都一样）；
- 能跑通的写法：**照常建分层弹窗（这条路径已经验证可用），再 `SetWindowLongPtr(GWL_STYLE, …) 去掉 WS_POPUP 加上 WS_CHILD` + `SetParent(g_ov, g_game)`**。分层表面保留，同时拿到子窗口的全部性质。

配套改动：

- 位置由 `dm_win_touch_layout` 用父客户区坐标摆（`SetWindowPos(g_ov, HWND_TOP, vx, vy, vw, vh, …)`），贴图那一步仍按屏幕坐标给 `pptDst`（`ClientToScreen(g_game) + 视图偏移`）——两者不冲突，且还原窗口时正是靠这一步把系统停在 −32000 的「停车位」摆回来；
- 显隐判据加上 `IsIconic(g_game)`：最小化就收起（日志 `收起（运行 1 手柄接管 0 切换中 0 最小化 1）`）；
- `dm_win_touch_layout` 忽略 0×0（最小化那一拍 WM_SIZE 给的就是 0×0，否则子窗口会被缩成 1×1）；还原后若视图仍为 0，就用 `front_game_res` 重算一次视图矩形；
- z 序只在**同一父窗口的兄弟之间**有意义了：露出时 `HWND_TOP`，露着时每 8 拍看 `GetWindow(g_ov, GW_HWNDPREV)` 有没有东西压上来（原来那套全桌面 `HWND_TOPMOST` + `z_rank` 排名不再需要）。

实测（`touch-probe` 里 68 s 最小化、8 s 后还原）：
```
覆盖层挂成子窗口（父=游戏窗口 1）
露出 可见=1 子窗口=480,254 1600x900 视图=0,0 1600x900 父=游戏窗口 最小化=0
贴图成功，子窗口 1600x900（可见 1）
收起（运行 1 手柄接管 0 切换中 0 最小化 1）          ← 最小化那一拍
露出 可见=1 子窗口=480,254 1600x900 … 最小化=0       ← 还原之后位置与尺寸都对
```
点按链路不受影响（`手指按下 … 目标 0/1/2 → 触屏控件` 后照常 `按键 0 → 0x1/0x2/0x4`）；`tap-seq` 帧率 `avg 52.5 / 末 20 51.5`，与之前同档。

**22 补记：子窗口的 `pptDst` 认父客户区坐标，给屏幕坐标会整体往右下挪。**
改成子窗口之后浮层跑到了窗口右下角：左边摇杆被裁掉看不见、右边技能钮挤在右下，**只有窗口最大化时看着正常**。原因是 `UpdateLayeredWindow` 的 `pptDst` 在子窗口上是**叠加到已由 `SetWindowPos` 摆好的子窗口位置**上的，而它还按屏幕坐标给（游戏客户区原点 `480,254`），于是整张浮层又被右下推了 `480,254`；最大化后客户区原点≈`0,0`，偏差才消失。把「窗口最大化才看得见」当成指纹，就能立刻认出是这类坐标系叠加错误。

改法：`paint()` 里分两条路给 `pptDst`——`g_child` 时给 `(g_vx, g_vy)`（父客户区坐标），顶层弹窗时才 `ClientToScreen(g_game) + 视图偏移`。贴图成功后顺手打一行 `屏幕矩形`，实测子窗口落在 `480,254`（= 游戏客户区原点），`tap-seq` 十下点击目标全对。

**23. 「按住方向键的同时按技能键没反应」——前端窗口原先根本没有触屏处理，只有一个鼠标。**
`front_win.c` 的 `wnd_proc` 里只有 `WM_MOUSEMOVE / WM_LBUTTONDOWN / WM_LBUTTONUP`。Windows 默认只把**第一根**手指合成成鼠标，第二根落下来时系统既不给第二条鼠标消息、也不给任何触点消息，于是「摇杆那根按住时再点技能」这条路上第二根手指直接不存在。浮层这边（`dm_win_touch_pointer` 的 `slot`、`g_claim[]`）本来就是一槽一根手指，缺的只是多点入口。

改法：

- `create_window` 里 `RegisterTouchWindow(g_hwnd, 0)`，日志 `输入：触屏消息注册 成功（可多点）`；
- 新增 `WM_TOUCH` 分支：`GetTouchInputInfo` 取触点数组（`TOUCHINPUT.x/y` 是 1/100 像素的屏幕坐标，`/100` 后 `ScreenToClient` → `mouse_to_pic`），逐根处理；
- 触点表 `g_tu[16]`（`id / slot / claim / front / nx,ny`），`tu_find` / `tu_alloc`：`slot` 就是浮层那根手指的槽位，`claim` = 归浮层、`front` = 前端页面、都不占 = 游戏触点；
- **关键一点**：喂给游戏的那一份手指表必须**一次提交全部手指**（`touch_publish()` 里凑齐再 `dm_touch_set(t, n)`）。原先 `set_finger` 是一根一根覆盖写，按住摇杆那根会被后按下去的那根冲掉，即使多点消息进来了也一样没方向；
- `WM_KILLFOCUS` 里把 `g_tu[]` 一起清掉（否则还原后按着的那根没人收）。

**取证的坑：本机是远程桌面会话，系统级触摸注入不可用。** 写了 `dm/win/tools/touch2_win.py`（`InitializeTouchInjection` + `InjectTouchInput`）想造两根手指，`InitializeTouchInjection` 返回成功、`InjectTouchInput` 无论怎么填（只剩 `DOWN|INRANGE|INCONTACT|PRIMARY` 一个字段集合）都恒返回 **`ERROR_INVALID_PARAMETER (87)`**；`touch_min.py` 逐步削减字段逐条试也全是 87。这台机器 `whoami /groups` 里有 `BUILTIN\Remote Desktop Users` + `NTLM Authentication`，是 RDP 会话——注入需要真正接了数字化仪的本地输入桌面，所以这条路在本机不可用（不是结构体写错：`POINTER_INFO` 96 / `POINTER_TOUCH_INFO` 144 字节都和 x64 的 ABI 对得上）。真机（本地控制台会话）上 `touch2_win.py` 仍可直接用。

因此补了一个**进程内的触摸模拟**：`DM_TOUCH_SIM="秒@相位:x,y[:x,y]|秒@相位:…"`（相位 0=按下 1=移动 2=抬起，段之间用 `|` 分隔——用 `;` 会被 `touch-probe.ps1` 的 `-Extra` 拆成两个环境变量），在 `touch_win.cpp` 的 tick 里按脚本造 `TOUCHINPUT`，走 `front_win.c` 的 `touch_dispatch()`——和 `WM_TOUCH` 完全同一条分发路径。摇杆是「浮动底盘」（圆心 = 落点），所以脚本要先用 0 相位把手指按在底盘上、再用 1 相位拖出去，才看得到方向。

实测（`DM_TOUCH_SIM="58@0:264,612:1221,547|58.3@1:170,430:1221,547|58.6@1:150,400:1221,547|60@2:264,612:1221,547"`，两根手指：左边摇杆 + 右边技能 X）：
```
输入：触屏消息注册 成功（可多点）
手指按下 点(110,255) 目标 -2 → 触屏控件        ← 摇杆
手指按下 点(508.75,227.917) 目标 2 → 触屏控件   ← 技能 X，同时按着
按键 0 → 0x4（摇杆 0,0 瞄准 0,0）
摇杆 0,0 → -8193,31726（同时按键 0x4）          ← 方向推出来了，技能还按着
按键 0x4 → 0（摇杆 0,0 瞄准 0,0）               ← 松手干净
```
即：两根手指同时被浮层各自认领、摇杆与技能键在同一拍上发布，且互不打断。`pad_step` 里顺手加了「摇杆出零 / 回零」一行，就是这条链路的直接证据。

**24. 多点还是不行（真机双指）——触点发给「手指下面那个窗口」，而浮层是压在客户区上的子窗口；另外同一根手指会同时给出 WM_TOUCH 和一条合成鼠标。**

对照移动端可以看清 Windows 独有的一段：iOS 把 `UITouch` 按对象存进 `_slot[16]`（按下取最小空槽，`dm_tc_begin_at/move_at/end_at` 按槽走，`pushFrom` 每次把 16 个槽里**全部**手指一次性 `dm_touch_set`，按键另走 `dm_input_set`）；安卓是 `pointerIds[16]` 把 `MotionEvent` 的 pointerId 映射成槽（`ACTION_DOWN/POINTER_DOWN` 取最小空槽、`ACTION_MOVE` 遍历全部 pointer 更新、`UP/POINTER_UP` 释放槽、`CANCEL` 全抬），`publishRaw()` 同样一次提交全部手指。**两边都没有「窗口」这一层**：触点只属于视图，不存在「这根手指落到哪个窗口去」的问题。Windows 上多出来的就是这个：WM_TOUCH 发给**手指正下方那个窗口**，而浮层正压在游戏客户区上；第一根手指又被系统顺手合成成鼠标消息（走 `HTTRANSPARENT` 穿透回游戏窗口），于是「单指能用、第二根没反应」看起来完全合理。

改法：

- 触屏消息抽成统一入口 `dm_win_front_touch_msg(HWND target, WPARAM, LPARAM)`（`front_win.c`）：`GetTouchInputInfo` → `touch_dispatch`，`target` 是要换算坐标的那个窗口（游戏窗口，`mouse_to_pic` 用它客户区）；
- **浮层子窗口也 `RegisterTouchWindow`**，`ov_proc` 收到 `WM_TOUCH` 就转给上面那个入口（日志 `触屏浮层：覆盖层触屏注册 成功`）——这样无论触点落在哪个窗口都进同一条路；
- **丢掉系统从触点合成出来的鼠标消息**：`GetMessageExtraInfo()` 带 `0xFF515700` 签名就是触点 / 笔合成的（真鼠标没有）。不丢的话同一根手指会被登记两次（一根当鼠标占槽、一根当触点另占一个槽）。只在 `RegisterTouchWindow` 成功后才启用这条过滤，免得万一注册失败就整机没输入；
- 新触点落进一个还挂着上次没抬起手指的槽时先 `dm_win_touch_free_slot()`：否则 `dm_tc_begin_at` 见 `finger[slot].active` 直接早退，按下去什么都不发生；
- 开机日志补数字化仪能力：`输入：触屏消息注册 成功（可多点）（数字化仪 197，触点上限 10）`。`触点上限 0` = 这个会话根本没有触摸。

排障开关：`DM_TOUCH_LOG=1` 会把每条 WM_TOUCH 的触点打出来（`输入：触点 1/2 id=3 flags=... 屏幕 x,y`），真机上「第二根手指到底有没有进来」一眼就能看出来。

进程内触摸模拟（`DM_TOUCH_SIM`）走的是同一条 `touch_dispatch`，所以它验证的是状态机与发布这一半；「触点能不能到窗口」这一半只能在真机上验（本机 RDP 会话注入恒 87，见坑 23）。

**25. 触屏点物品没有手势（长按 / 双击使用·鉴定 / 点世界 / 拖放）——Windows 只接了「圆钮 + 摇杆」，手势层那一半一直没人把请求发给宿主。**

`dm_touch_controls.c` 的手势层只定型、不干活，真正动手的是宿主的请求泵（`dm_ui_long_press` / `dm_ui_use_item` / `dm_ui_identify_hovered` / `dm_world_tap` / `dm_ui_drag_*`）。iOS 由 `front_ios.m` 在松手那一刻比较手势层的计数器（`long_press_seq` / `use_seq` / `ident_seq` / `item_tap_seq` / `world_tap_seq` / `mouse_click_seq` / `tab_tap_seq`）再调宿主，`touch_mac.c` 是同一套的桌面版（事件排队、在取手柄那一拍做）。`dm/win` 原先一处都没有：`grep long_press_seq dm/win` 为空——所以面板里的长按、双击（读卷轴 / 喝药 / 免鉴定）、点世界（拾取 / 对话 / 开箱）、拖放全都不存在，能用的只有圆钮与摇杆。

补齐的三件事（照 `touch_mac.c`）：

- **面板态**：`pad_step` 里读 `dm_ui_panel_state()` → `dm_tc_set_panel_state` + `dm_tc_set_navigation_mode(panel != 0)`。面板开着（物品栏 / 技能树 / 选项）= 手势层的导航态：手指变成「界面指针」，圆钮与摇杆停发（日志 `切面板态（手指当界面指针）` / `回游戏态（圆钮与摇杆）`）。
- **事件排队**：窗口线程不许读游戏内存，所以面板态的手指事件（`op 0/1/2` + 槽 + 点）只排进 `g_gq[]`，由游戏线程（`pad_step`）里的 `gesture_pump()` 消费：`dm_tc_begin_at` 之后立刻 `dm_tc_pointer_item(dm_ui_item_at_now(...))`（1 物品区 / 2 页签 / 3 面板外 / 其余面板内别处），拖动走 `dm_ui_drag_begin/move/end`。
- **松手发请求**：先交 `dm_tc_set_holding` / `set_tap_box` / `set_cur_stack` / `set_hover_unid`（判类要用），再 `dm_tc_end_at`，然后按计数器变化调宿主：`tab_tap_seq`→`dm_ui_tab_go`、`long_press_seq`→`dm_ui_long_press`（日志 `物品区长按 → 宿主界面长按`）、`item_tap_seq`→`dm_ui_item_tap`、`ident_seq`→`dm_ui_identify_hovered`、`use_seq`→`dm_ui_use_item`（日志 `双击物品 → 使用（喝药 / 读卷轴 / 开魔盒）`）、`world_tap_seq`→`dm_ui_world_tap`、`mouse_click_seq`→`dm_ui_touch(0)+dm_ui_touch(2)`。拖放的落点仍按 `DM_TC_POINTER_LIFT_PT` 抬起量算（`dm_ui_drop_at_now` / `dm_ui_drop_point` / `dm_ui_origin_point` / `dm_ui_shop_buy_at`）。点世界的纵向走 `ui_y_to_screen()`（界面归一化 → 出图归一化，同 iOS 的 world_norm）。

游戏态（没面板）一点没动：`面板位 0` → 直接走原来的 `dm_tc_begin_at`，`DM_TOUCH_SIM` 复跑仍是 `摇杆 0,0 → -8193,31726（同时按键 0x4）`。


## 排障

- 崩溃看 `dm/host/crash.c` 的 `FAULT` 行（`pcmod=` 会给客体模块归属）；`DM_VEH_LOG=1` / `DM_VEH_TRACE=1` 才打 VEH 阶段标记，平时不打（缺页每帧几十次会把 stderr 冲烂）。
- 画面取证用 `DM_WIN_SHOT`：`set DM_WIN_SHOT=200+100:C:\...\k%d.bmp`，在 `vkCmdBlitImage` 之后读回、上屏后落盘 32 位 BMP（BGRA，字节序和格式 0x25 一致）。转 PNG：`python -c "from PIL import Image; Image.open('k200.bmp').save('k200.png')"`。
- 退出码：`42` = 走了 Break 分支；`0xC0000028` = 坏栈（已修）；`0xC0000005` = 栈未提交页写（已修）；`0xC0000409` = fail-fast（已修）；`139` = `TerminateProcess(128+SIGSEGV)`。
