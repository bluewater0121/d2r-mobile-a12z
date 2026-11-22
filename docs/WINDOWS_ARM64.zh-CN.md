# 在 Windows ARM64 上玩（开发版）

Windows 版和安卓版的做法一样：游戏程序用你自己的 Switch 游戏文件在电脑上改写一遍，着色器现翻一遍，然后用本仓库的 `dmhost.exe` 直接跑。目前**没有做安装包**，只有命令行宿主，给开发和验证用。

English: [WINDOWS_ARM64.md](WINDOWS_ARM64.md)

**先看现状：**

| 这一步 | 现状 |
|---|---|
| 启动、片头、标题 | 通过（1600x900 窗口，Vulkan 呈现） |
| 主菜单 → 职业 → 模式 → 起名 → 进局 | 通过（起名走宿主输入框 / `DM_KBD` 通道） |
| 营地局内画面 | 通过：HUD 生命 55/55、法力 10/10、小地图、NPC 名字都对 |
| 走路、镜头跟随 | 通过（WASD 当左摇杆，鼠标左键当落手指） |
| 帧率（营地，1600x900） | **55~60 帧**（多数窗口贴着 60，偶发掉一个垂直同步时 53~55） |
| 音频 | 已接 WASAPI，但没实际听过 |
| 全屏切换、窗口拉伸 | 没实测 |
| 过场动画（movie） | 没实测（Windows 上的解码路径没验过） |

这台验证机：Windows 11 ARM64，骁龙 X2 Plus（Adreno X2-45），Vulkan 1.4。

## 机器要求

- Windows 11 ARM64（骁龙 X 系列等），显卡驱动带 Vulkan 1.1 以上
- 内存 8 GB 以上（局内进程常驻约 2.6 GB）
- 约 10 GB 磁盘：改写后的程序约 78 MB，着色器包约 700 MB，运行期存档与临时目录若干

## 你需要准备的

- 你自己购买的游戏文件：Switch 版 1.0.34.0（对应 PC 版 3.3.94012）的 `ExeFS`（`main`/`sdk`/`subsdk0`）与 `RomFS`，见 [GAME_FILES.zh-CN.md](GAME_FILES.zh-CN.md)
- llvm-mingw 的 aarch64-w64-mingw32 工具链（`aarch64-w64-mingw32-clang` / `-clang++` / `llvm-nm` / `llvm-ar`）
- Vulkan SDK（只要头文件与 `glslc`）
- CMake 3.22+、Ninja、Python 3.9+（`pillow` 用于把 BMP 截图转 PNG，`numpy`/`lz4`/`capstone` 给改写与着色器工具用）

## 第一步：改写游戏程序

在仓库根目录，对三个 NSO 各跑一次（目标选 `win`）：

```
set DM_TARGET=win
set DM_RULESET=touch-run-v4
python dm/tools/patch_nso.py <ExeFS>\main      build\win\dmdata main
python dm/tools/patch_nso.py <ExeFS>\sdk       build\win\dmdata sdk
python dm/tools/patch_nso.py <ExeFS>\subsdk0   build\win\dmdata subsdk0
```

产出 `build\win\dmdata\` 里的 `main.patched.bin`（约 54 MB）、`sdk.patched.bin`（约 15 MB）、`subsdk0.patched.bin`（约 8 MB），以及配套的 `.fixups.tsv` / `.gates.tsv` / `.sites.tsv` 等表。`win` 目标改的是 Windows 上的线程本地存储与系统调用落点，和安卓那份不是同一套。

## 第二步：取并翻译着色器

```
python dm/win/tools/extract_casc.py <RomFS> build\win\switch-pkg
python dm/gfx/tools/dmsh_vk.py build\win\switch-pkg build\win\shaders
```

第一条从 `RomFS/data` 的静态 CASC 里把着色器包（162 个 `.bin`）原样取出来；第二条翻成引擎要的 XGS1 包，一共 **6,931 个 `.xgs`**（6,927 个游戏着色器 + 4 个内置辅助着色器）。这一步最慢，几十分钟量级。

## 第三步：构建 `dmhost.exe`

```
cmake -G Ninja ^
  -DCMAKE_TOOLCHAIN_FILE=dm/win/toolchain-aarch64-w64-mingw32.cmake ^
  -DDM_GEN_DIR=build/win/gen ^
  -DCMAKE_BUILD_TYPE=Release ^
  -S dm/win -B build/win/obj
ninja -C build/win/obj
```

`-DDM_GEN_DIR` 指向由游戏文件生成的宿主表头目录（`dm_vault_tab.h` / `dm_hud_exp.h` / `dm_bright_levels.inc`）。Vulkan 没有 ARM64 导入库，链接时直接用 `C:\Windows\System32\vulkan-1.dll`（`-DDM_VULKAN_DLL` 可改），头文件路径用 `-DDM_VULKAN_INCLUDE` 指定。

产物 `build/win/obj/dmhost.exe` 是自带的：C++ 运行库静态链进来（`-static`），只剩 `libwinpthread-1.dll` 是导入项，构建时从工具链的 `bin` 拷到 exe 旁边（`CMakeLists.txt` 的 POST_BUILD）。少这一步时双击 `start-win-arm64.bat` 会以 `0xC0000135`（找不到 DLL）秒退、日志一个字都没有。

## 运行

```
cd build\win
set DM_ROMFS=<RomFS 目录>
set DM_SAVE=C:\work\prj\d2r-mobile\build\win\save
set DM_TEMP=C:\work\prj\d2r-mobile\build\win\temp
set DM_WINDOW=1
obj\dmhost.exe dmdata main sdk subsdk0
```

`shader_dir` 取 `DM_SHADER_DIR`，缺省是当前目录下的 `shaders`，所以工作目录放在 `build\win` 就能命中第二步的产物。

省事的办法是双击工程根目录的 `start-win-arm64.bat`：它把上面这些环境变量都设好（RomFS 缺省取 `..\Diablo\RomFS`），顺手建好 `save` / `temp`，并在启动前关掉旧实例。`--rebuild` 先重编译，`--res 1280x720` 换渲染分辨率。

**标题栏**显示当前渲染分辨率与实时帧率（`1600x900 · 52 fps`，每秒刷新一次，数字来自呈现层真正上屏的帧数）。

### 键鼠

| 键 | 对应 |
|---|---|
| W / A / S / D | 左摇杆 |
| J / K / U / I | A / B / X / Y |
| F / G | 按下左 / 右摇杆 |
| Q / E / Z / C | L / R / ZL / ZR |
| Enter | ＋ |
| Tab | − |
| Space | A |
| 方向键 | 十字键 |
| 鼠标左键 | 画面上落一根手指（触屏点击） |
| 鼠标右键 | B |
| 滚轮 | 右摇杆上下推 |

注意：客体是**每帧采样一次按键状态**，按下和松开落在同一帧里等于没按。自己写脚本注入按键时，按下与松开之间至少留 100 ms（本仓库的 `dm/win/tools/keys_win.py` 默认 140 ms 就是为这个）。

### 手柄

接上手柄（XInput：Xbox 手柄、以及 PC 上以 XInput 模式出现的第三方手柄）可以直接玩，判据与 iOS / 安卓一致：手柄一有输入就换成手柄布局，敲键盘或点画面换回触屏布局，摇杆要先回中才重新接管。

面键默认按位置（下 = A 确认、右 = B 返回、左 = Y、上 = X），与 PC / Xbox 版同位置同功能；按 Switch 字母报键的手柄（或习惯「右 A 确认」）加 `DM_PAD_AB=switch` 只换 A/B。细节与验证方法见 [../dm/win/README.zh-CN.md](../dm/win/README.zh-CN.md) 的「手柄」一节。

### 前端流程：开机停在主菜单

iOS / 安卓是触屏，前端流程由宿主「替玩家按键」理顺（`dm/host/dm_cursor.c` 的「前端路径理顺」）：有存档就在主菜单替按 X 打开选角页、选角列表一收起又替按 X 重开。键盘 / 手柄能自己走原版菜单，这两下会跟玩家抢：开机第一屏直接成了人物列表，选角页按 A 选定后列表被替按的 X 重开、要再按一次才进得去。

所以 Windows 上 `tuning_defaults()` 把 `DM_FE_AUTO` 设成 `0`，走原版流程：**开机是主菜单 → A 进选角页 → A 开始游戏**，和原版 Switch 一致。要看触屏那套行为加 `DM_FE_AUTO=1`（例如以后接上真实触屏）。

### 存档：关窗口前会先请游戏存盘

游戏自己的定时自动存盘约 5 分钟一次，直接关窗口会丢掉这之间的进度。关窗口（右上角 ×、Alt+F4、`taskkill /im dmhost.exe`）时前端照 Mac 壳 `app_quit` 的做法，先 `dm_save_wait(3, …)` 请游戏走一次它自己的存盘、等 `存档提交 #N … 原子换入` 落盘再收尾，日志是 `窗口：退出前等游戏存盘 388 ms，期间存档提交 2 次`；3 秒内没等到会如实打一行 `【红】窗口：未存盘退出`。

`start-win-arm64.bat` 关旧实例也是先温和关（发 WM_CLOSE）等最多 8 秒，实在不走才 `taskkill /f`。

### 常用环境变量

| 变量 | 作用 |
|---|---|
| `DM_WINDOW=1` | 开窗口、走 Vulkan 呈现（必需；`0` 目前还没接好） |
| `DM_ROMFS` / `DM_SAVE` / `DM_TEMP` | RomFS、存档目录、临时目录 |
| `DM_SHADER_DIR` | 着色器包目录（缺省 `./shaders`） |
| `DM_TACT_PREFETCH=0` | 关掉战术预取（Windows 上建议关） |
| `DM_KBD=<文本>` | `swkbd` 起名面板直接返回这段文本，跳过宿主输入框 |
| `DM_PAD=0` / `DM_PAD_AB=switch` / `DM_PAD_LOG=1` / `DM_PAD_TEST=<秒>` | 手柄开关 / Switch 面键语义 / 手柄日志 / 无手柄时注入一次 A 验通路 |
| `DM_FE_AUTO=0` | 关掉触屏那套「替玩家按键」的前端流程（Windows 缺省关，开机停在主菜单） |
| `DM_WIN_SHOT=<首帧>[+<步长>]:<路径模板>` | 在呈现线程把上屏前的图读回成 32 位 BMP，例如 `200+100:C:\...\shot%d.bmp` |
| `DM_WIN_SIZE=宽x高` | 窗口客户区尺寸（缺省 1600x900，按屏幕工作区不超过 90%） |
| `DM_GAME_RES=宽x高` | 游戏渲染分辨率（缺省按原版 16:9：1600x900） |
| `DM_GFX_FENCE_SPIN=0` | 退回栅栏阻塞等待（默认开自旋，见下） |
| `DM_PRESENT_MODE=fifo\|relaxed\|mailbox\|immediate` | 出图模式（缺省 `relaxed`，见「性能」；改回 `fifo` 就是严格的垂直同步） |
| `DM_GFX_PERF_SAMPLE=1` | 打开逐帧分项计时（缺省关，开了会掉 1~2 帧） |
| `DM_PRESENT_IMAGES=N` | 交换链张数（缺省 3；4 张实测更差，排障可调） |
| `DM_PRESENT_LEAD_MS=N` | 出图提前量（缺省 0 = 关；4 ms 实测无效，见「性能」第五步） |
| `DM_STALL_PROBE=1` | 长帧取栈：每 1 ms 采出图线程 PC，超过 22 ms 的帧打一行命中函数（会拖慢一点，只在排障时开） |
| `DM_HID_LOG=1` | 两份输入通道（宿主键盘 `dm_input_set` / 手柄 `dm_pad_set`）按键变化各打一行，带调用者地址 |
| `DM_GFX_FENCE_LOG=1` | 每次栅栏等待打一行 `wait_ms` / `since_submit_ms` |
| `DM_NVN_LOG` / `DM_FS` / `DM_IMPORTS` / `DM_FAULT_LOG` / `DM_VEH_LOG` / `DM_VEH_TRACE` | 排障日志开关 |

## 性能

局内曾经只有 **6 帧**。`NVN PERF` 显示每帧 10~11 次 `vkQueueSubmit`、11 次 `vkWaitForFences`，栅栏等待累计约 120 ms，而 GPU 实际只干了 16 ms。

单个栅栏的 `since_submit` 是 **13~14 ms**，但同一次提交的 GPU 工作量只有约 1.5 ms：耗掉的时间不在 GPU 干活上，而在这块 Windows ARM 驱动上 **`vkWaitForFences` 阻塞等待的唤醒延迟**。改成先轮询 `vkGetFenceStatus`（自旋预算 8 ms，用完了再退回阻塞等待）以后，等待从 13~14 ms 降到 1 ms 上下，营地帧率从 6 变成 **41~44**，`gpu_ms` 稳定在 9.7 ms 左右。默认只在 Windows 上开这个自旋（`_WIN32`），安卓 / iOS 仍是阻塞等待，别白烧手机的电。

第二步是**把安卓那套实测过的引擎开关搬到 Windows**（`dm/win/front_win.c` 的 `tuning_defaults()`，逐项注释见 `dm/android/jni_android.c` 245-337 行）。同一套 Adreno + Vulkan + 同一份引擎，那批默认值就是手机上营地 60 帧的来源；Windows 按同一套开，玩家自己设了同名环境变量仍以他的为准。实测每帧的提交与栅栏等待从 **10 次提交 / 10 次等**塌成 **1 次提交、fence=0**：角色列表 25 → **53 帧**，职业选择页 21.6 → **48~52 帧**，营地 43 → **46 帧**。其中 `DM_GFX_NATIVE_BC=1` 让 BC 贴图按原格式采样（引擎直接吃 BC7，不解码成 RGBA），图像内存 `live_images` 从 826 MB 降到 311 MB。

第三步是**把长帧的尾巴削掉**。此时 `[nvn] 计时` 显示中位帧时间已经只有 **13~15 ms（约 70 帧）**，但 `p95 37 ms / p99 70 ms`、300 帧里 130 多帧超过 16.7 ms，均值被拖到 46。三项改动：

- 出图模式从 `FIFO` 换成 `FIFO_RELAXED`（`DM_PRESENT_MODE` 可选 `fifo|relaxed|mailbox|immediate`，缺省 `relaxed`）。FIFO 在「差一点赶不上一个垂直同步周期」时会把整帧压到下一个周期，实测约三成帧就是这样被压成两个周期；relaxed 只在赶得上时才等。营地 46 → **50~55 帧**。
- 交换链改成 **3 张图**（两张交替在途 + 一张给呈现引擎读），`vkAcquireNextImageKHR` 不再卡在垂直同步上。
- `DM_GFX_PERF_SAMPLE=0`（与安卓一致）：逐帧分项计时会让被计时的那一帧多花约 4 ms，关掉再涨 1~2 帧；要看分项时用 `DM_GFX_PERF_SAMPLE=1` 打开。

第四步是**把尾部尖峰按掉**。前三步之后中位帧时间已经只有 13~15 ms（约 70 帧），但 300 帧里仍有 130 多帧超过 16.7 ms、`p95` 约 37 ms，均值被拖到 50 出头。`[长帧]` 的明细全部是「新建管线 0 / 上传贴图 0」，尖峰既不在编译也不在上传；`DM_3D_SCALE=0.5` 和 `DM_PRESENT_MODE=immediate` 也都不改变帧率——不是 GPU 受限，也不是垂直同步压的。

真正的原因是 **Windows 的定时器粒度**：缺省是 15.6 ms，客体里每一次 `pthread_cond_timedwait` / `nanosleep` 超时都被按这个粒度取整，于是「差一点等到」的那一帧整块多出 15~16 ms（基线 13~15 ms、尖峰 24~33 ms，正好一个粒度）。安卓 / iOS 的定时器是 µs 级，所以手机上没这个问题。`raise_timer_resolution()`（`dm/win/front_win.c`）在 `front_run()` 里先用 `NtSetTimerResolution(5000, ...)` 要到 **0.5 ms**（日志打 `计时器精度：请求 0.5 ms，系统给的是 0.50 ms`），失败再退到 winmm 的 `timeBeginPeriod(1)`。效果：

- 营地 **55~60 帧**（多数窗口 57~60，只掉一个垂直同步的那种窗口 53~55），改之前同场景 50~55、且 `掉帧 86/300` 变成 **51/300**。
- 职业选择页 48~55 → **55~60**，角色列表同样贴着 60。
- `submit_cpu_ms` 从 6~7 降到 **4~4.5**（等待变短，翻译线程少被掐）。

第五步（进行中）是**把 9 帧一次的尖峰按住**。定时器修好之后剩下的抖动是周期性的：营地随机走动（`build\win\enter-town-v3.ps1`，W/A/S/D 各 0.3~1.2 s）里约 **9 帧（≈150 ms）出现一次 30~42 ms 的尖峰**，其余帧 16.6~16.9 ms。最近 70 s 窗口（约 3900 帧）的量法：整帧 **≥20 ms 占 9.7%、≥30 ms 占 7.5%、≥50 ms 占 0.84%**，窗口帧率 55~60（多数 58~60）。注意 `[nvn] 帧间隔 >16.7ms` 那一列在 59 Hz 节拍下几乎恒为全部，不能当长帧比例用。

已经排除的方向（各自实测，数据都比基线差或不变）：出图提前量 `DM_PRESENT_LEAD_MS=4`（≥30 ms 7.5% → 9.0%）、交换链 4 张（`DM_PRESENT_IMAGES=4`，9.5%，且掉帧变多）、`DM_PRESENT_HZ=50` 锁 50 帧（10.7%，部分窗口掉到 45）、`DM_3D_SCALE=0.7`（12.1%，说明不是 GPU/填充率受限）、`DM_PRESENT_MODE=fifo` / `mailbox`、`DM_PAD=0`+`DM_AUDIO=0`（9 帧周期都不变）。

`DM_STALL_PROBE=1`（`dm/win/stall_win.c`）是这一轮加的取栈器：每 1 ms 把出图线程 SuspendThread + GetThreadContext，一帧超过 22 ms 就把整帧的命中按次数排序打一行，符号用模块自带 symtab 反查，系统 DLL 里的地址换成 `dll+偏移`。它给出的结论是：尖峰帧里出图线程绝大部分采样都停在 `ntdll`/`KERNELBASE` 的等待上（条件变量/唤醒），而出图线程自己的栅栏与取图从未单次阻塞 ≥8 ms，GPU 7.8 ms、翻译+出图 4.5 ms 都不尖峰——所以尖峰不是呈现、GPU 或分辨率造成的，而是某条协作线程每 ~150 ms 多花 20~30 ms，把这一帧顶出了预算，多买一个垂直同步。

`DM_PRESENT_LEAD_MS` 缺省已改回 0（开关保留），`DM_PRESENT_IMAGES` 缺省 3。

游戏本身按屏幕刷新（本机上报 59 Hz，日志 `[nvn] 出图节拍 59.00 Hz`）出图，所以这套配置下 60 就是上限，再高要把显示刷新报到更高档位，没有实际意义。

剩下的余量（没做，按风险从低到高）：

1. 逐帧 CPU 里 `descriptor` 约 1.5~2 ms（每帧 1300~1800 个描述符）、`draw` 约 1.2~1.7 ms，想再往下压要动翻译层。
2. 引擎的 slot 环只有 2 槽（`xbg_game.cpp` 的 `Slot slots[2]` + `slot_index^=1`），所有 `slot_index^1` 的语义都建立在这个假设上，加深环是一次中等风险改动。

## 取图颜色

`DM_WIN_SHOT` 读回的是引擎源图（日志里的 `源格式`，这块机器上是 37 = `VK_FORMAT_R8G8B8A8_UNORM`）。写 BMP 前要按 `source_format` 判断通道序：RGBA 源必须交换 0/2 字节再写，否则存出来的图红蓝互换（红药水变蓝、火把火焰变蓝、紫色法力球变红）。窗口本身不经过这条路径（交换链格式与源一致，直接上屏），所以「截图偏色」不等于「画面偏色」——先看 `DM_WIN_SHOT：已写 …（源格式 %d，交换红蓝 %d）` 这一行再判断。

## 已知问题

- `DM_WINDOW=0` 走到 `真实Vulkan后端未初始化` 就停，尚未接上。
- 音频（WASAPI）已实现但没实际听过；`hle_lifecycle.c` 的 SIGTERM/GCD、`hle_movie.c` 的解码在 Windows 上没验过。
- 「掉落过滤」规则还没能写进 `save/Settings/*.fltr`（日志有 `规则文件没写成`）。
- 掉帧还在：300 帧里约 50 帧会掉一个垂直同步（`[nvn] 上屏` 的「掉帧」一列），来源是帧内偶尔的几毫秒抖动，暂时只能靠余量吃。
- 窗口拉伸 / 全屏切换没实测；实体手柄这台机器上没有，通路靠 `DM_PAD_TEST` 注入 A 验过。

相关文档：[DEPENDENCIES.zh-CN.md](DEPENDENCIES.zh-CN.md)、[GAME_FILES.zh-CN.md](GAME_FILES.zh-CN.md)、[ANDROID.zh-CN.md](ANDROID.zh-CN.md)（安卓参照实现）。
