# dm/gfx/tools/tier1_verify — iPad Pro 2020（A12Z）可行性验证脚本

一次性验证工具，不参与出包链路。用途：验证「去掉参数缓冲（退回逐槽绑定）」能否让本工程的
着色器在 Metal 2 / 参数缓冲 Tier 1（Apple GPU family 5，即 iPad Pro 2020 的 A12Z）的资源
上限内编译通过。结论与完整数据见 `docs/IPAD_PRO_2020_SUPPORT.md`。

不改仓库任何生产文件，只读现有翻译链（Ryujinx → SPIR-V → spirv-cross），只替换 spirv-cross
的 MSL 生成选项。

## 用法

前置：`./setup.sh` 已跑过（需要 `~/dm-work/build/cache/dm/dmsh` 的着色器包、
`~/dm-work/build/gfx/dmsh_xlate/dmsh_xlate` 翻译器、`spirv-cross` 在 PATH）。

```bash
PY=~/dm-work/build/cache/dm/venv/bin/python     # 需 lz4（解 main NSO 用）

# 抽样验证：3 个最简单的片元着色器 + 全部含可写贴图的着色器（Tier 1 的真门槛）
$PY verify.py

# 全量验证：映射表6927 个着色器，12 片并行，约 30 秒
$PY verify_all.py
```

两者都会打印每个着色器的资源数与是否超限，并写出明细：

- `~/dm-work/out/tier1_verify/result.json`（抽样）
- `~/dm-work/out/tier1_verify/result_all.json`（全量）

## 判据

family 5 每函数上限（Metal Feature Set Tables）：缓冲 31、贴图 96、采样器 16。

## writekeys.txt

含可写贴图（`access::write`）的 15 个着色器键，从当前链版本的 metallib缓存 grep 提取：

```bash
grep -l "access::write\|access::read_write" \
  ~/dm-work/build/cache/metal_shader_cache/b-<链版本>/*_b.metal | sed 's/_b\.metal//'
```

换翻译链版本后需重新生成。

## 注意

两个脚本都读 `../../nvn/shader_facts.tsv`（作答表）。**必须载入**——缺了它翻译器对每个查询
取缺省值，贴图类型认错会导致大批翻译失败（实测失败率从 3% 升到 44%）。`verify_all.py` 靠
模块级 `_init_facts()` 在导入时载入，因为分片子进程不经过 `load_map()`。