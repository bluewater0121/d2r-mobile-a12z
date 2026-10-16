#!/usr/bin/env python3
# 采样器合并方案生成器（离线一半）：读真机 DM_NVN_SAMPDUMP 日志 → 算出每个超限着色器的采样器分组。
#
# 输入三样：
#   1. 真机 dump（DM_NVN_SAMPDUMP=<文件>）：每个「着色器 × 阶段 × 采样器槽」用到的采样器配置
#   2. 离散绑定档（DMSH_BIND=discrete）出的着色器库目录：<键>_b.json 里有 msl 变量名 ↔ NVN 槽号
#   3. 同一目录里的 <键>_b.metal（翻译产物保留的 MSL 源）
# 输出：<out>/plan.json（每个超限着色器的分组）+ <out>/report.txt（人类可读报告）。
# 之后由 samp_merge.py --plan 读 plan.json 生成合并版 MSL 并编 metallib。
#
# 分组判据（保守，宁可不并也不错并）：
#   · 同一 NVN 槽下的所有 sampler 变量永远同组：它们运行时读的是同一个句柄
#     （S.tex[stage][r] >> 20 / S.samp[stage][r] >> 20），任何一次绘制里配置都相同。
#     25 个超限着色器的 26 个变量其实只分属 5–6 个槽，仅靠这一条就够（samp_merge.py --by-reg）。
#   · 下面这些是「还想再往下并」（跨槽合并）时才需要的，依赖真机 dump：
#     - 一个槽在日志里只出现过一种配置，才可参与跨槽合并；
#     - 出现多种配置的槽（游戏按绘制改配置）单独成组，不与任何槽合并；
#     - 日志里没出现过的槽（这次没画到）也单独成组；
#   · 配置按「生成的 MTLSamplerState 是否相同」判定（与 nvn_mtl.m samp_by_id 同口径），
#     不是按原始字段逐字节比——字段不同但采样器对象相同的也要并。
# Sampler variables under the same NVN slot are always grouped together: at runtime they read the same handle
# (S.tex[stage][r] >> 20 / S.samp[stage][r] >> 20), so their config is identical in every draw.
# The 26 variables of the 25 over-limit shaders belong to only 5-6 slots, so this rule alone suffices (samp_merge.py --by-reg).
# The rules below are only needed to merge further across slots, and they need a device dump:
#
# 用法：samp_plan.py <dump.tsv> <discrete 库目录> [--out 目录] [--keys 键清单]
import os, re, sys, json, collections, argparse

LIM_SAMPLER = 16

# ---- 与 nvn_mtl.m 的 wrapm / samp_by_id 同口径：把原始字段折成「采样器对象的身份」 ----
def wrapm(w):
    return {0: 0, 7: 0, 1: 1, 3: 2, 2: 2, 5: 3, 4: 3, 6: 4}.get(w, 1)   # clamp / repeat / mirror-clamp / border / mirror-repeat

def canon(r):
    """r：dump 的一行（dict）。返回可比较的元组 = 这份配置生成的 MTLSamplerState 的身份"""
    minf, magf = int(r['minf']), int(r['magf'])
    lod_min, lod_max, aniso = float(r['lod_min']), float(r['lod_max']), float(r['aniso'])
    b3, b0 = float(r['b3']), float(r['b0'])
    a = int(aniso) if aniso >= 1 else 1
    a = 16 if a > 16 else a
    return (bool(magf),
            minf in (1, 3, 5),                                  # minFilter = Linear
            0 if minf <= 1 else (1 if minf in (2, 3) else 2),   # mipFilter
            wrapm(int(r['ws'])), wrapm(int(r['wt'])), wrapm(int(r['wr'])),
            round(max(lod_min, 0.0), 6), round(lod_max, 6), a,
            (int(r['cmp_func']) - 1) if int(r['cmp_mode']) else 0,
            (2 if b0 > 0.5 else 1) if b3 > 0.5 else 0)          # borderColor: 0 透明 / 1 黑 / 2 白

COLS = ['key', 'stage', 'reg', 'sid', 'minf', 'magf', 'ws', 'wt', 'wr', 'cmp_mode', 'cmp_func',
        'lod_min', 'lod_max', 'lod_bias', 'aniso', 'b0', 'b1', 'b2', 'b3', 'first_frame']

def load_dump(path):
    """{(键, 阶段, 槽): {配置元组: 首次帧}}"""
    seen = collections.defaultdict(dict)
    with open(path, encoding='utf-8') as f:
        head = f.readline().rstrip('\n').split('\t')
        idx = {c: head.index(c) for c in COLS if c in head}
        for ln in f:
            c = ln.rstrip('\n').split('\t')
            if len(c) < len(head):
                continue
            r = {k: c[i] for k, i in idx.items()}
            seen[(r['key'], int(r['stage']), int(r['reg']))].setdefault(canon(r), int(r['first_frame']))
    return seen

def json_path(lib, key):
    for p in (os.path.join(lib, key + '.json'), os.path.join(lib, key + '_b.json'),
              os.path.join(lib, key[:-2] + '_b.json') if key.endswith('_b') else None):
        if p and os.path.exists(p):
            return p
    return None

def sampler_names(lib, key):
    """离散档反射 → ({槽号: [msl 变量名...]}, 变量总数)。一个槽可以有多个变量（同一句柄被声明多次）|
    discrete-mode reflection -> ({slot: [msl name...]}, total variable count). One slot may hold several variables (the same handle declared more than once)"""
    p = json_path(lib, key)
    if not p:
        return None, 0
    d = json.load(open(p, encoding='utf-8'))
    out = collections.defaultdict(list)
    n = 0
    for b in d.get('bindings', []):
        if b.get('cls') == 'sampler' or (b.get('kind') or '').startswith('samp'):
            out[int(b['reg'])].append(b['msl_name'])
            n += 1
    return (dict(out) or None), n

def plan_one(key, regs, names, seen, nvars):
    """算一个着色器的分组。regs：槽号集合；names：{槽: [变量名]}；seen：dump"""
    single, multi, unseen = {}, [], []
    for r in sorted(regs):
        cfgs = None
        for st in range(6):   # dump 里记的是 nvn_stage（0 顶点 … 5 计算）| the dump records nvn_stage (0 vertex ... 5 compute)
            if (key, st, r) in seen:
                cfgs = seen[(key, st, r)]
                break
        if not cfgs:
            unseen.append(r)
        elif len(cfgs) == 1:
            single[r] = next(iter(cfgs))
        else:
            multi.append(r)
    by_cfg = collections.defaultdict(list)
    for r, c in single.items():
        by_cfg[c].append(r)
    groups = [sorted(v) for v in by_cfg.values()] + [[r] for r in multi] + [[r] for r in unseen]
    groups.sort(key=lambda g: g[0])
    return {'nsamp': nvars, 'nreg': len(regs), 'K': len(groups),
            'groups': [[nm for r in g for nm in names[r]] for g in groups],
            'groups_by_reg': groups, 'multi_cfg': multi, 'unseen': unseen,
            'names': {str(r): names[r] for r in regs}}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump')
    ap.add_argument('lib')
    ap.add_argument('--out', default='samp_plan_out')
    ap.add_argument('--keys', help='只处理这些键（每行一个）；不给 = 处理库里所有 sampler 超限的着色器')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    seen = load_dump(a.dump)
    print(f'dump：{len(seen)} 个（键, 阶段, 槽）组合，来自 {len({k[0] for k in seen})} 个着色器')

    keys = [l.strip() for l in open(a.keys, encoding='utf-8') if l.strip()] if a.keys else None
    if keys is None:
        keys = []
        for fn in sorted(os.listdir(a.lib)):
            if not fn.endswith('_b.json'):
                continue
            try:
                d = json.load(open(os.path.join(a.lib, fn), encoding='utf-8'))
            except ValueError:
                continue
            n = sum(1 for b in d.get('bindings', []) if b.get('cls') == 'sampler' or (b.get('kind') or '').startswith('samp'))
            if n > LIM_SAMPLER:
                keys.append(fn[:-7] if fn.endswith('_b.json') else fn[:-5])
    plan, rep = {}, []
    rep.append(f'{"着色器":<20}{"采样器":>7}{"并成":>7}  判定')
    rep.append('-' * 60)
    ok = over = nonames = 0
    for key in keys:
        names, nvars = sampler_names(a.lib, key)
        if not names:
            nonames += 1
            rep.append(f'{key[:18]:<20}{"?":>7}{"?":>7}  离散档反射里没有这个键或没有 sampler 绑定')
            continue
        p = plan_one(key, set(names), names, seen, nvars)
        plan[key] = p
        if p['K'] <= LIM_SAMPLER:
            ok += 1
            note = f'可并（多配置槽 {len(p["multi_cfg"])}，未见槽 {len(p["unseen"])}）'
        else:
            over += 1
            note = f'**仍超 {LIM_SAMPLER}**：配置种类太多，需另想办法'
        rep.append(f'{key[:18]:<20}{p["nsamp"]:>7}{p["K"]:>7}  {note}（变量 {p["nsamp"]} / 槽 {p["nreg"]}）')
    json.dump(plan, open(os.path.join(a.out, 'plan.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    open(os.path.join(a.out, 'report.txt'), 'w', encoding='utf-8').write('\n'.join(rep) + '\n')
    print('\n'.join(rep))
    print(f'\n可并 {ok}，仍超 {over}，缺反射 {nonames} → {os.path.join(a.out, "plan.json")}')
    return 0 if over == 0 else 1

if __name__ == '__main__':
    sys.exit(main())
