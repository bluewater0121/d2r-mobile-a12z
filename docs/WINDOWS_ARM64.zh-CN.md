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
| 帧率（营地，1600x900） | **约 42 帧**；修掉栅栏等待前是 6 帧，见下面「性能」 |
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

**标题栏**显示当前渲染分辨率与实时帧率（`1600x900 · 43 fps`，每秒刷新一次，数字来自呈现层真正上屏的帧数）。

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

### 常用环境变量

| 变量 | 作用 |
|---|---|
| `DM_WINDOW=1` | 开窗口、走 Vulkan 呈现（必需；`0` 目前还没接好） |
| `DM_ROMFS` / `DM_SAVE` / `DM_TEMP` | RomFS、存档目录、临时目录 |
| `DM_SHADER_DIR` | 着色器包目录（缺省 `./shaders`） |
| `DM_TACT_PREFETCH=0` | 关掉战术预取（Windows 上建议关） |
| `DM_KBD=<文本>` | `swkbd` 起名面板直接返回这段文本，跳过宿主输入框 |
| `DM_PAD=0` / `DM_PAD_AB=switch` / `DM_PAD_LOG=1` / `DM_PAD_TEST=<秒>` | 手柄开关 / Switch 面键语义 / 手柄日志 / 无手柄时注入一次 A 验通路 |
| `DM_WIN_SHOT=<首帧>[+<步长>]:<路径模板>` | 在呈现线程把上屏前的图读回成 32 位 BMP，例如 `200+100:C:\...\shot%d.bmp` |
| `DM_WIN_SIZE=宽x高` | 窗口客户区尺寸（缺省 1600x900，按屏幕工作区不超过 90%） |
| `DM_GAME_RES=宽x高` | 游戏渲染分辨率（缺省按原版 16:9：1600x900） |
| `DM_GFX_FENCE_SPIN=0` | 退回栅栏阻塞等待（默认开自旋，见下） |
| `DM_GFX_FENCE_LOG=1` | 每次栅栏等待打一行 `wait_ms` / `since_submit_ms` |
| `DM_NVN_LOG` / `DM_FS` / `DM_IMPORTS` / `DM_FAULT_LOG` / `DM_VEH_LOG` / `DM_VEH_TRACE` | 排障日志开关 |

## 性能

局内曾经只有 **6 帧**。`NVN PERF` 显示每帧 10~11 次 `vkQueueSubmit`、11 次 `vkWaitForFences`，栅栏等待累计约 120 ms，而 GPU 实际只干了 16 ms。

单个栅栏的 `since_submit` 是 **13~14 ms**，但同一次提交的 GPU 工作量只有约 1.5 ms：耗掉的时间不在 GPU 干活上，而在这块 Windows ARM 驱动上 **`vkWaitForFences` 阻塞等待的唤醒延迟**。改成先轮询 `vkGetFenceStatus`（自旋预算 8 ms，用完了再退回阻塞等待）以后，等待从 13~14 ms 降到 1 ms 上下，营地帧率从 6 变成 **41~44**，`gpu_ms` 稳定在 9.7 ms 左右。默认只在 Windows 上开这个自旋（`_WIN32`），安卓 / iOS 仍是阻塞等待，别白烧手机的电。

还有余量的地方（没做，按风险从低到高）：

1. 每帧仍然有 10 次「提交完就等」的串行点（写回 6 次、计数报告 4 次），GPU 与 CPU 基本完全串起来。要再快就得让 GPU 和 CPU 重叠，比如把写回延后到帧末或允许两帧在飞。
2. 引擎的 slot 环只有 2 槽（`xbg_game.cpp` 的 `Slot slots[2]` + `slot_index^=1`），所有 `slot_index^1` 的语义都建立在这个假设上，加深环是一次中等风险改动。

## 取图颜色

`DM_WIN_SHOT` 读回的是引擎源图（日志里的 `源格式`，这块机器上是 37 = `VK_FORMAT_R8G8B8A8_UNORM`）。写 BMP 前要按 `source_format` 判断通道序：RGBA 源必须交换 0/2 字节再写，否则存出来的图红蓝互换（红药水变蓝、火把火焰变蓝、紫色法力球变红）。窗口本身不经过这条路径（交换链格式与源一致，直接上屏），所以「截图偏色」不等于「画面偏色」——先看 `DM_WIN_SHOT：已写 …（源格式 %d，交换红蓝 %d）` 这一行再判断。

## 已知问题

- `DM_WINDOW=0` 走到 `真实Vulkan后端未初始化` 就停，尚未接上。
- 音频（WASAPI）已实现但没实际听过；`hle_lifecycle.c` 的 SIGTERM/GCD、`hle_movie.c` 的解码在 Windows 上没验过。
- 「掉落过滤」规则还没能写进 `save/Settings/*.fltr`（日志有 `规则文件没写成`）。
- 角色列表页用键盘 A 直接「选择」在这台机器上没能触发（职业选择页和模式页没问题），自动化时走「新建角色」那条路更稳。
- 窗口拉伸 / 全屏切换、原生手柄都没实测。

相关文档：[DEPENDENCIES.zh-CN.md](DEPENDENCIES.zh-CN.md)、[GAME_FILES.zh-CN.md](GAME_FILES.zh-CN.md)、[ANDROID.zh-CN.md](ANDROID.zh-CN.md)（安卓参照实现）。
