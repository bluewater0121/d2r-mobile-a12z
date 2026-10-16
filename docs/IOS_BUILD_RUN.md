# iOS 编译与运行操作说明

本文把「改代码 → 出包 → 装机 → 跑起来看日志」的完整链路写全，重点标出**会话开头那几步之外**还需要的操作
（着色器库出包、链版本、c1 清单、真机启动方式）。A12Z / 老机型相关的方案与坑见 `IPAD_PRO_2020_SUPPORT.md`。

约定：`DM_ROOT`（缺省 `~/dm-work`）是工作区根目录，游戏文件、产物、缓存、运行日志都在它下面；
仓库根目录记作 `DM`。

---

## 1. 只改了宿主 / App 代码（最常用的三步）

```bash
cd dm/ios
DM_FREE_ACCOUNT=1 DM_IOS_M3=1 DM_ASTC_PACK=$DM_ROOT/out/legacy_pack/pack ./run.sh build
./run.sh install
./run.sh boot 240          # 或解锁 iPad 后直接点图标
```

- `run.sh build`：生资源 → 生成 `Gen/` → `xcodebuild`。产物在 `$DM_ROOT/run/ios/dd/Build/Products/Release-iphoneos/DmIOS.app`。
- `run.sh install`：`devicectl device install app`，日志 `$DM_ROOT/run/ios/install.log`。
- `run.sh <模式> [上限秒]`：启动并把设备上的 `Documents/<模式>.txt` 拉回 `$DM_ROOT/run/ios/out/<模式>.txt`。

**模式**：`boot` = 起游戏（玩家点图标也是这个）；`main` / `sdk` 是**探针模式**
（屏显 `dm iOS · main`、日志里「未知模式」不是报错，是它在按探针跑）——想看游戏画面必须用 `boot`。

真机启动前：设备要**解锁**（锁屏状态下 `host_start()` 不会跑），且已在 `dm/ios/local.env` 里配好
`DM_DEVICE`（设备 UDID）与 `DM_BUNDLE_ID`（见 `local.env.example`，该文件不进仓）。

日志里值得先看的两行：

```
显卡 Apple A12Z GPU：参数缓冲 tier 1（着色器库走逐槽绑定档）
着色器库 b-<链版本>
```

---

## 2. 动到着色器库时的额外操作

着色器是**离线预编译**的（iOS 无 JIT），产物是 `<库目录>/b-<链版本>/<键>_b.metallib` + 反射 json。
改了下列任一文件，就必须重出库，否则宿主要么找不到库、要么逐个 json 核不过：

| 文件 | 影响 |
|---|---|
| `dm/gfx/tools/dmsh_build.py` | 出包主流程（翻译 → spirv-cross → metallib + 反射） |
| `dm/gfx/tools/dmsh_chain.py` | 链版本算法本身 |
| `dm/gfx/tools/dmsh_fetch.py` | 可读 tile（`_bf`）变体 |
| `dm/gfx/tools/dmsh_facts.py`、`dm/gfx/nvn/shader_facts.tsv` | 翻译查询的作答表 |
| `dm/gfx/tools/dmsh_xlate/Program.cs`、`dmsh_xlate.csproj` | 翻译器 |
| `dm/gfx/nvn/shader_map.tsv`（含 `.conflicts.tsv`） | 映射表 |
| `dm/ios/tools/ios_metal.py` | iOS 编译口径 |
| `dm/gfx/tools/dmsh_chain.lock` | 工具指纹 |

### 2.1 一键脚本（推荐）

```bash
J=10 ~/dm-work/run/ios/rebuild_a12z.sh           # 两档库 + c1 清单 + 编 App + 装机
J=10 ~/dm-work/run/ios/rebuild_a12z.sh libs      # 只出库 + 刷 c1
~/dm-work/run/ios/rebuild_a12z.sh app            # 库已齐，只编 App + 装机
BINDS=discrete J=10 ~/dm-work/run/ios/rebuild_a12z.sh libs   # 只出逐槽绑定档
```

脚本内部依次做：把 `/tmp` 里的产物搬回原位 → 按 `DMSH_BIND` 出两档库 → `c1_guard.py` 刷清单 → `run.sh build` → `run.sh install`。

### 2.2 手工等价命令

```bash
cd dm/gfx/tools
J=10 DMSH_BIND=discrete IOSOUT=$DM_ROOT/build/ios/dmsh_b DM_ROOT=$DM_ROOT \
  $DM_ROOT/build/cache/dm/venv/bin/python3 dmsh_build.py \
  $DM_ROOT/build/cache/dm/dmsh $DM_ROOT/build/cache/metal_shader_cache
```

末尾不给键 = 全部（6,927 个）。`DMSH_BIND` 两档：

- `ab` = 参数块档（无绑定，要 argument buffers tier 2，即 A14 / M1 起）→ 写进 `b-<DMSH_CHAIN>/`
- `discrete` = 逐槽绑定档（Metal 2 起可用，老显卡走这条）→ 写进 `b-<DMSH_CHAIN_T1>/`

### 2.3 链版本（最容易漏的一步）

链版本 `V` = 上表文件内容的哈希 + 绑定档位。**改了 `dmsh_build.py` 两档版本都会变**，
必须先重写头文件，否则出包直接报：

```
【红】nvn/dmsh_chain.h 不是当前翻译链版本 xxxxxxxx：先跑 dmsh_chain.py --header
```

```bash
python3 dm/gfx/tools/dmsh_chain.py --header        # 重写 nvn/dmsh_chain.h（两个宏都写）
python3 dm/gfx/tools/dmsh_chain.py                 # 打印当前缺省档的 V
python3 dm/gfx/tools/dmsh_chain.py --check-tools   # 核对翻译器 / spirv-cross / metal 与锁文件
python3 dm/gfx/tools/dmsh_chain.py --update-lock   # 换工具后重生成锁文件（随提交入仓）
```

`nvn/dmsh_chain.h` 是生成文件，随改动一起提交。

### 2.4 c1 清单

```bash
python3 dm/gfx/tools/c1_guard.py \
  $DM_ROOT/build/cache/metal_shader_cache/b-<DMSH_CHAIN> dm/gfx/nvn/c1_full.tsv
```

清单表头记着它出自哪档（`# chain b-…`），宿主两档版本都认，但**旧版本会被判过期**，所以链版本变了就要刷。

### 2.5 省事做法：参数块档改戳免重出

如果本次改动对参数块档是空操作（例如只对 `BIND=discrete` 生效的规则），参数块档产物其实**逐字节不变**
（可用单键对照验证：metallib `cmp` 一致、json 除 `chain` 字段外全等）。那就克隆目录 + 改戳，不必重出 6,927 个：

```bash
cp -cR .../b-<旧V> .../b-<新V>      # mac 缓存与 iOS 产物两个目录都要（APFS 克隆，不占空间）
python3 - <<'EOF'
import glob
for p in glob.glob('<新V 目录>/*_b.json'):
    s = open(p).read(); open(p, 'w').write(s.replace('"<旧V>"', '"<新V>"'))
EOF
python3 dm/gfx/tools/c1_guard.py <mac 缓存>/b-<新V> dm/gfx/nvn/c1_full.tsv
```

然后只重出离散档（`BINDS=discrete … libs`）。

---

## 3. 设备侧

| 事 | 命令 / 位置 |
|---|---|
| 装机 | `./run.sh install`（`devicectl device install app`） |
| 起游戏 | `./run.sh boot 240`，或解锁后点图标 |
| 日志 | 设备 `Documents/boot.txt` → 拉回 `$DM_ROOT/run/ios/out/boot.txt` |
| 进程是否活着 | `xcrun devicectl device info processes --device $DM_DEVICE \| grep DmIOS` |
| 卸载重装 | `xcrun devicectl device uninstall app --device $DM_DEVICE --bundle-id $DM_BUNDLE_ID` |

日志里要盯的几类红字：

- `建渲染管线失败：…`——着色器用了本机不支持的指令（如 `air.simd_shuffle` / `air.quad_shuffle` unlowered）。
  **编译期查不出来**，只有真机建管线时才暴露。
- `着色器 xxx 的翻译链版本 … 与本宿主期望的 … 不符`——库与宿主版本对不上（见 2.3）。
- `着色器库目录 … 不存在`——这一档还没出包。

---

## 4. 环境坑

- **沙箱内删文件有数量限制**：单轮累计删 ≥50 个会拦（`SAFE_DELETE_BULK_CONFIRM_REQUIRED`）。
  对策是产物目录放 `/tmp`（`/tmp` 不受限），跑完再拷回 `~/dm-work`；`Gen/` 常是 `/tmp/Gen_real` 的软链。
- **`sed -i ''` 在本机是 GNU 语义**（会把脚本当文件名，与 BSD 相反），批量改文件用 python 更稳。
- **zsh 不做变量分词**：`set -- $cfg` 拿不到两个词，要分开写或用 `${=cfg}`。
- **取链版本的 sed 必须锚定行首**：`sed -n 's/^#define DMSH_CHAIN "\(.*\)"/\1/p'`，
  不锚定会把 `DMSH_CHAIN_T1` 也匹配出来。
- **并发**：`J=10` 起步；偶发 `Brokered host rename` 就降并发续跑。
- **老机型资源包**：`DM_ASTC_PACK=$DM_ROOT/out/legacy_pack/pack`；日志里
  `boot: 用老机型补充包 …/Legacy Device Pack` 是正常的，不是被拦。

---

## 5. 排错速查

| 现象 | 先看 |
|---|---|
| 出包报「不是当前翻译链版本」 | 2.3：`dmsh_chain.py --header` |
| 黑屏 / 大面积错色 | 日志里 `建渲染管线失败：…unlowered…`；对照 `IPAD_PRO_2020_SUPPORT.md` 的坑 1~3 |
| 局部色斑（光照 / 阴影） | 离散档与参数块档的**语义差异**：先确认是不是 subgroup 降级（按作用域还原），再看可读 tile 变体缺失（它只会变慢、不会画错） |
| 启动就 SIGILL | `libdeflate`：构建口径要 `-mcpu=apple-a12` 且关掉 dotprod（见 `dm/ios/tools/project_m3.py`） |
| 装机失败 | `$DM_ROOT/run/ios/install.log`；设备是否解锁、UDID 是否对 |
| 起了但没画面 | 是不是用了 `main` 模式（探针）而不是 `boot`；设备是否锁屏 |
