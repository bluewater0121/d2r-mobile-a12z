# 存档事务重做：快照代 + 无改名发布（Windows）

状态：设计稿，未实现。行号对应当前 `win-arm64` 分支。
范围：`dm/host/hle_fs.c` 的 `mount_ex` / `h_fs_Commit` / 文件与目录的 HLE、`dm/win/compat/mman_win.c` 的 `clonefile` / `renamex_np`。macOS / iOS / Android 一律走原路径，本次不动。

## 0. 一句话

Windows 没有「目录原子互换」。现在用三次改名去凑一个互换，又把「已提交目录」在窗口期里改名、在失败时用非原子覆盖去救——这两条都会丢档。新方案：**已提交版本 = 一个带编号的只读快照目录；发布 = 在快照里写一个 `DONE` 文件。整条提交路径上没有一次 rename、没有一次 delete。**

## 1. 现在到底在哪丢

**W1（致命，崩溃即全丢）** `hle_fs.c:585` 调 `renamex_np(new, commit, RENAME_SWAP)`，Windows 实现在 `mman_win.c:111`，是三次 `MoveFileEx`：

```
commit.new  →  commit.new.rtmp      # 第 1 步
commit      →  commit.new           # 第 2 步：$DM_SAVE 这个名字此刻不存在
commit.new.rtmp → commit            # 第 3 步
```

第 2 步成功后、第 3 步完成前被杀，盘上状态是：`$DM_SAVE` 不存在，**唯一一份完整旧档躺在 `<save>.new` 里，新档躺在 `<save>.new.rtmp` 里**。下次挂载 `mount_ex`（`hle_fs.c:117-119`）第一件事就是 `rm_tree(neu)` —— 把 `<save>.new` 删掉，然后 `mkdir(dir)` 造一个空目录再 clone 它。**角色列表全空。** 这段窗口只有几十毫秒，但它跟杀毒/索引器占用撞在一起时会被 `mv_retry` 的 8 次重试拉长到 1 秒级，概率不可忽略。

**W2（失败即静默丢本次进度）** 第 3 步失败（`mman_win.c:124`）→ `hle_fs.c:586` 回退到 `copy_over_tree(neu, commit)`。此时 `commit` 已经不存在，`neu` 是**旧**版本：于是它把旧档「恢复」进 `$DM_SAVE`，日志写「已逐个文件覆盖进已提交目录（0 个文件没写成）」，看起来像成功，本次进度全丢。而且 `copy_over_tree`（`hle_fs.c:462`）本身就是非原子的：`fopen(t,"wb")` 先截断再逐块写，中途被杀就是**半旧半新的混合存档**。

**W3（游戏自己那一套被吞掉）** D2R 存角色是「写临时文件 → 删旧的 → 改名」。`h_fs_DeleteFile`（`:802`）撞一次瞬时占用就返回失败；`h_fs_RenameFile`（`:831`）看到目标还在就直接返回 `RES_PATH_EXISTS`——于是改名必然失败。工作目录里角色档还是旧的，**而提交照常成功**（`tree_stamp` 只是把它记成一行红日志）。「保存并退出偶尔没存上」的根在这里，重试只是把概率压低。

**W4（次要但真实）**
- `clonefile` 在 Windows 上是 `CopyFileA` 逐文件（`mman_win.c:86`），**不复制备用数据流**。而 `guest-name` 别名正好存在 ADS 里（`mman_win.c:128` 的「扩展属性（NTFS 备用数据流）」），每次提交/重挂就把大小写冲突的物理别名丢了，枚举会漏文件。
- `h_fs_FlushFile`（`:755`）是空操作，提交时也没有 `FlushFileBuffers`：掉电时「已提交」的目录里可能有没落盘的文件。

## 2. 新模型的两条不变式

1. **编号最大的、带 `DONE` 的快照目录，永远是一份完整自洽的存档；它写成之后不再被任何路径改写。**
2. **`$DM_SAVE` 只是它的一个展开副本，可以随时扔掉重建。**

发布不改变任何目录的名字，只改变「哪个目录算数」——判据是一个文件存不存在。Windows 上「创建一个文件并 flush」是唯一不会被别人打开的句柄挡住的操作。

## 3. 盘上布局

```
build\win\save\                 ← 挂载点 = 本局工作目录（可抛弃）
build\win\save.txn\
    g000001\                    ← 只读快照，游戏看不见
       ...（与 save 同构的两层树）
       DONE                     ← 最后写；内容见下
    g000002\
    g000003\
    instance.lock               ← 独占打开，进程死即释放
    legacy-20261123-1204\       ← 迁移时隔离的旧 .work/.new，绝不直接删
```

`DONE` 是纯文本，几行就够，用来做完整性与「内容有没有变」的判定：

```
gen 000002
files 37
bytes 1843201
digest 3f9c1d2ab77e5014
created 1763930000
<相对路径>\t<字节数>\t<时间戳>      # 每行一个，TAB 分隔
```

时间戳用的是 Windows 自己的 **100ns FILETIME**（`GetFileAttributesEx` 的 `ftLastWriteTime`），不是 CRT 的秒级 `st_mtime`。原因很实际：同一秒里原地改写一个同样长度的文件（游戏存档里天天发生），「大小 + 秒」完全看不出区别，展开时会被当成「已经一致」跳过——那正好丢的就是最新进度。复制到快照 / 从快照展开时用 `SetFileTime` 把原值搬过去，比对的是同一个量。

`save.txn` 是 `save` 的**兄弟目录**而不是子目录：导出存档、用户手动拷 `save`、iOS 那边逐文件对拍，都还是原样的标准存档树。

## 4. 提交协议（`h_fs_Commit` 的 Windows 分支）

1. 毒化检查同现状（`perr` 非 0 直接走恢复，见 §5）。
2. 跑 `g_dm_pre_commit_hook[0..4]`（旁路文件仍写进 `$DM_SAVE`，因为那就是工作目录）。
3. `N = 最大已提交编号 + 1`；`mkdir save.txn\gNNNNNN`。若同名残留存在且没有 `DONE`，先 `rm_tree` 它并重试。
4. 把 `save` 整树复制进 `gN`（跳过 `save.txn` 自己、跳过 `~` 与 `.` 开头的宿主临时文件）。每个文件写完后 `FlushFileBuffers`。
5. 写 `gN\DONE`，用 `CREATE_NEW + FILE_FLAG_WRITE_THROUGH`，写完 flush。
6. 日志 + `g_save_commits++` + `g_dm_save_commit_hook("$DM_SAVE")`。
7. GC（见 §6）。

第 4/5 步被杀 → 盘上留一个没有 `DONE` 的 `gN`，**最大已提交编号不变**，下次挂载看到的是上一次的成功提交，本次进度丢但不伤档。第 6 步之后被杀 → 新档已完整，下次挂载直接用它。

## 5. 挂载与恢复（`mount_ex` 的 Windows 分支）

1. 拿 `save.txn\instance.lock` 独占句柄（`dwShareMode=0`），拿不到说明另一个 dmhost 在跑，报红退出。
2. 扫 `save.txn\g*`，取**有 `DONE` 且能解析**的最大编号。没有 `DONE` 的一律不算数。
3. 迁移（只在第一次、`save.txn` 为空时）：
   - 按优先级找来源：`save` 里有 `*.d2s` → 用 `save`；否则 `save.new` 里有 → 用 `save.new`；否则 `save.new.rtmp` → 用它；都没有就是全新安装。
   - 找到来源后先**快照成 `g000001`**，再决定要不要展开；旧 `.work` / `.new` / `.new.rtmp` 一律 `move` 进 `legacy-<时间>\`，**不删**。
4. 恢复：把 `gN` 展开回 `save`。**先比对 `DONE` 里的文件清单（路径 + 大小 + 100ns 时间戳）与 `save` 现状，完全一致就跳过**——正常退出时这一步是零成本；只有崩溃/强杀过才真的复制（几十毫秒，与现在的 clonefile 同量级）。
5. 展开方式：先对 `save` 里多出、`gN` 里没有的文件做删除（失败就留，只记日志）；再逐文件复制覆盖。顺序「先删多余、再覆盖内容」保证崩溃后重做的结果只会更接近目标版本。
6. 挂载指向 `save`（不是 `save.work`）。

毒化拒绝提交时走同一套「恢复」例程：`$DM_SAVE` 从 `gN` 重建，已提交版本一个字节不动。

## 6. 保留与 GC

- 保留最近 3 代（`DONE` 齐全的），外加当前这代。`keep` 可用环境变量调。
- 只在两处删：进程启动挂载前（此时没有任何存档句柄）、一次提交成功之后（删的是前 N 代，不是刚发布的）。
- 删不掉一律忽略并下一轮再试；GC 永远不能影响「最大 `DONE` 目录」。

## 7. 第二层：访客的文件操作永不被吞（`h_fs_DeleteFile` / `h_fs_RenameFile`）

Windows 分支下，存档挂载内：

- `DeleteFile`：先试 `unlink`（带现有重试）。仍失败则**记一条墓碑（逻辑删除）并返回 `RES_OK`**，不返回失败。
- `RenameFile`：目标在墓碑里 → 允许替换，用 `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)` 一步完成，成功后清墓碑。目标不在墓碑里且物理存在 → 仍按原语义返回 `RES_PATH_EXISTS`。
- `OpenFile` / `GetEntryType` / 目录枚举 / `CreateFile`：命中墓碑的一律当「不存在」（`CreateFile` 命中则清墓碑后照常建）。
- 墓碑表：固定 32 项、存**物理绝对路径**（不是客体相对路径）、只对 `in_txn_dir()` 命中的路径生效，提交时和挂载时清空。

这样游戏的「写临时文件 → 删旧的 → 改名」在宿主上塌缩成「写临时文件 → 原子替换」：两个可能失败的步骤变成一个，且这一步失败也不再有「提交照常成功」的假象——有墓碑说明这次替换没做成，直接 `save_poison`，走拒绝提交。

## 8. 改动清单

| 位置 | 改动 |
| --- | --- |
| 新增 `dm/win/compat/save_txn_win.c` + `include/save_txn_win.h` | `save_txn_lock` / `save_txn_prepare` / `save_txn_restore` / `save_txn_snapshot` / `save_txn_skip` / `save_txn_gc` / `save_txn_digest`。只用 Win32，不进 macOS 构建（已在 `dm/win/CMakeLists.txt` 的 `WIN_SRCS` 里登记） |
| `hle_fs.c:mount_ex` | Windows 分支改调 `save_txn_lock` + `save_txn_prepare` + `save_txn_restore`，挂载点就是 `$DM_SAVE`（不再有 `.work`） |
| `hle_fs.c:h_fs_Commit` | Windows 分支改调 `save_txn_snapshot`，`clonefile + renamex_np + copy_over_tree` 这条路在 Windows 上不再走 |
| `hle_fs.c` 毒化分支 | 用 `save_txn_prepare` + `save_txn_restore` 代替「改名重新克隆」 |
| `hle_fs.c` 六个 HLE 入口 | 接墓碑表（§7）：`OpenFile` / `CreateFile` / `DeleteFile` / `RenameFile` / `GetEntryType` / `ReadDirectory` |
| `hle_fs.c` 提交日志 | Windows 打「→ 快照 gNNNNNN（N 个文件 N 字节）」并用 `DONE` 摘要与上一代比较；mac 仍是 `tree_stamp` 指纹 |
| `mman_win.c:renamex_np` / `clonefile` | 存档不再调用（其它调用方不受影响），函数本身保留 |
| `start-win-arm64.bat` | 注释里「transaction log」改为「快照目录」；`instance.lock` 之后 bat 的 taskkill 可以保留当兜底 |

已知**没做**的一项：快照 / 展开仍用 `CopyFileA` 式的逐文件复制，**不搬 ADS 备用数据流**，而 `guest-name` 别名恰好藏在 ADS 里（`mman_win.c` 起的那套）。也就是说宿主名与客体名撞车、走了别名的文件，进快照后会丢掉别名。要解决得用 `FindFirstStreamW` / `FindNextStreamW` 逐流复制，做成独立一步再验。

## 9. 验证

已经跑过的（都是真跑的，命令和读数在下面）：

1. **快照模块自测** `build\win\save_txn_test.c`（临时文件，不进口）：18 项全过。覆盖首挂成 g000001、编号递增、无 `DONE` 的代不算数（且该编号会被复用）、展开、展开后内容逐字节正确、GC 只留最新 keep 代、从旧 `.new` 迁移、`.work` 被隔离而不是删除、`restore` 幂等。这轮它抓出两个真 bug：`DONE` 头里的 `created` 行没被解析（导致所有展开永远失败）、以及秒级 mtime 让同长度同秒改写被判成「已一致」。
2. **端到端** `dm/host/tests/t_save_win.c`（新增，`#include "../hle_fs.c"`，走真实 `MountSave` / `Commit`）：41 项全过。覆盖：首挂收成 g000001 且不生 `.work`；提交建新代、旧代不动；事务写到一半没提交 → 重挂载回到快照内容；被占用文件删不掉 → 逻辑删除 + 对客体不可见 + 不进快照；释放后物理残留被清掉；占用解除后「临时文件 → 改名」替换成功；**有文件被打开着也能提交**；占用一直不解除 → 改名失败、事务毒化、提交被拒、上一代一个字节没变；释放后重挂载回到上一代内容。
3. **旧布局迁移演练**：把现存的 `build\win\save`（55 个文件、8 个角色）连 `save.work` 拷成副本，跑 `lock → prepare → restore`：55 个文件 SHA256 与原件**逐个相同**，`.work` 整棵进了 `save.txn\legacy-<时间戳>`，一次都没删。

还没做、也不能算「已验证」的：

4. **杀进程矩阵**：在 `taskkill /f` 的不同时刻（快照 10%/50%/90%、`DONE` 写入前后、展开过程中）各跑若干次，每次重启后用 `dm/verify/savecmp.py` 比对上一次性提交的哈希。这一步要靠真实游戏在跑，属于下一轮。
5. **`hold.py` 实机按住**（`build\win\hold.py`）走一次「保存并退出」：预期日志是 `存档提交 #N：… → 快照 gNNNNNN（…）` 且无红行，不再出现 `原子互换失败 → 逐个文件覆盖`。
6. **ADS 别名**（见 §8 末尾）：`guest-name` 在快照 / 展开中是否保留，需要专门造一个宿主名撞车的存档来测。

## 10. 取舍与风险

- **盘占用**：`keep=3` 时是存档体积的 4 倍。D2R 存档（角色档 + 共享仓库 `.d2i`）通常几 MB，可接受；`keep` 可配。
- **游戏中 `$DM_SAVE` 是脏的**：这是从「提交目录常青」换来的代价。任何「导出/备份存档」的入口必须走最新快照（`save.txn\gN`），不能直接拷 `save`。Windows 前端目前没有导出入口，先记在文档里。
- **首次挂载多一次展开**：正常退出会被清单比对跳过，只有崩溃后才有这次几十毫秒。
- **掉电**：Windows 不能 fsync 目录。做法是「全部文件 `FlushFileBuffers` → 最后写 `DONE` 且 `WRITE_THROUGH`」，把「`DONE` 在、内容不在」的窗口压到最小而不是消掉；真要更强就得用 `FILE_FLAG_WRITE_THROUGH` 打开所有快照文件，代价是提交从 40 ms 涨到数百 ms。建议先按前者做，实测再决定。
- **不做的事**：不引入 SQLite / 单文件打包。存档目录必须保持可直接拷给 iOS 的标准布局，`savecmp.py` 的对拍闸门继续可用。
