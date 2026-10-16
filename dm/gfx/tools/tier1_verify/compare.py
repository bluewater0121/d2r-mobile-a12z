#!/usr/bin/env python3
# 步骤 1、2 的回归验证：同一批着色器分别按 ab 档（参数块，现状）与 discrete 档（逐槽绑定）出包，
# 逐条比较反射 bindings 的「种类 + NVN 槽号 + 读写」是否完全一致。
# 一致 = 换绑定模型没有改变语义，步骤 3/4 只需改定位方式；不一致 = 反射解析有误，必须先修好。
# Regression check for steps 1 and 2: build the same set of shaders in mode ab (argument buffer, current) and
# mode discrete (per-slot), then compare the reflection bindings item by item on "kind + NVN slot + rw".
# Equal = switching the binding model did not change semantics, so steps 3/4 only need to change how things are located;
# unequal = the reflection parse is wrong and must be fixed first.
import os, sys, json, re, glob, shutil, random, subprocess, collections

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
GFX = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
DM_ROOT = os.environ.get('DM_ROOT', os.path.expanduser('~/dm-work'))
DM_DIR = os.path.join(DM_ROOT, 'build/cache/dm/dmsh')
MAP = os.path.join(GFX, 'nvn/shader_map.tsv')
HEADER = os.path.join(GFX, 'nvn/dmsh_chain.h')
PY = os.path.join(DM_ROOT, 'build/cache/dm/venv/bin/python')
BUILD = os.path.join(TOOLS, 'dmsh_build.py')
OUT = os.path.join(DM_ROOT, 'out/tier1_compare')
N = int(os.environ.get('N', '60'))   # 随机补充样本数 | number of extra random samples


def load_map():
    loc, stage_of = {}, {}
    for ln in open(MAP):
        if ln.startswith('#'):
            continue
        c = ln.rstrip('\n').split('\t')
        if len(c) > 4:
            loc.setdefault(c[3], (c[0], int(c[1]) if c[1].isdigit() else c[1]))
            if c[2] in ('顶点', '像素', '计算', '域', '外壳'):
                stage_of[c[3]] = c[2]
    return loc, stage_of


def pick_samples():
    """样本 = 覆盖全部绑定种类的代表 + 含可写贴图的 + 采样器超限的 + 随机补充。"""
    loc, stage_of = load_map()
    # 覆盖全部 kind 的代表（由已出反射 json 统计得来）
    kinds, rep = set(), {}
    for p in glob.glob(os.path.join(DM_ROOT, 'build/cache/metal_shader_cache/b-c2b35eaded5d/*_b.json')):
        key = os.path.basename(p)[:-7]
        try:
            d = json.load(open(p))
        except Exception:
            continue
        for b in d.get('bindings', []):
            k = b['kind']
            if k not in rep:
                rep[k] = key
            kinds.add(k)
    keys = [rep[k] for k in sorted(rep)]
    # 含可写贴图（Tier 1 曾判定为硬禁）的
    wk = os.path.join(HERE, 'writekeys.txt')
    if os.path.exists(wk):
        keys += [l.strip() for l in open(wk) if l.strip()]
    # 采样器超限的（family 5 上限 16）
    ra = os.path.join(DM_ROOT, 'out/tier1_verify/result_all.json')
    if os.path.exists(ra):
        over = [x['key'] for x in json.load(open(ra)) if x['st'] == 'ok' and x['over']]
        keys += over[:12]
    cand = [k for k in loc if k not in keys]
    random.seed(7)
    keys += random.sample(cand, min(N, len(cand)))
    return list(dict.fromkeys(k for k in keys if k in loc)), rep


def chain_versions():
    vs = {}
    for mode in ('ab', 'discrete'):
        env = dict(os.environ, DMSH_BIND=mode)
        vs[mode] = subprocess.run([PY, os.path.join(TOOLS, 'dmsh_chain.py')], capture_output=True, text=True, env=env).stdout.strip()
    return vs


def build(mode, keys, outroot):
    env = dict(os.environ, DMSH_BIND=mode, J=os.environ.get('J', '8'))
    env.pop('IOSOUT', None)
    os.makedirs(outroot, exist_ok=True)
    r = subprocess.run([PY, BUILD, DM_DIR, outroot, *keys], capture_output=True, text=True, env=env)
    return r


def _norm(v):
    """JSON 读回后数字可能是 int / str 混排，统一成可比较形态。"""
    return json.dumps(v, sort_keys=True, ensure_ascii=False) if v not in (None, {}, []) else ''


def main():
    keys, rep = pick_samples()
    print(f'样本 {len(keys)} 个；覆盖绑定种类：' + ' '.join(f'{k}={v[:8]}' for k, v in rep.items()))
    vs = chain_versions()
    print(f'链版本：ab={vs["ab"]}  discrete={vs["discrete"]}')
    if vs['ab'] == vs['discrete']:
        sys.exit('【红】两档链版本相同，产物会互相覆盖（dmsh_chain.py options() 没计入 BIND）')

    # dmsh_build.py 要求 header 里含当前 V；两档都写进去，跑完还原
    orig = open(HEADER).read()
    try:
        open(HEADER, 'w').write(orig + f'#define DMSH_CHAIN "{vs["ab"]}"\n#define DMSH_CHAIN "{vs["discrete"]}"\n')
        outs = {}
        for mode in ('ab', 'discrete'):
            root = os.path.join(OUT, mode)
            print(f'\n===== 出包 {mode} 档 =====')
            r = build(mode, keys, root)
            print('\n'.join(r.stdout.strip().splitlines()[-6:]) or '(无输出)')
            if r.returncode:
                print('stderr:', r.stderr[-600:])
            outs[mode] = os.path.join(root, 'b-' + vs[mode])
    finally:
        open(HEADER, 'w').write(orig)

    # 逐条比较
    print('\n===== 反射对比 =====')
    same, diff, miss = 0, [], 0
    kind_seen = collections.Counter()
    for key in keys:
        pa = os.path.join(outs['ab'], key + '_b.json')
        pc = os.path.join(outs['discrete'], key + '_b.json')
        if not (os.path.exists(pa) and os.path.exists(pc)):
            miss += 1
            continue
        a, c = json.load(open(pa)), json.load(open(pc))
        ba = {b['msl_name']: b for b in a['bindings']}
        bc = {b['msl_name']: b for b in c['bindings']}
        if set(ba) != set(bc):
            diff.append((key, '成员集合不同', f"ab-only={sorted(set(ba)-set(bc))[:4]} discrete-only={sorted(set(bc)-set(ba))[:4]}", None))
            continue
        for tgt in ('drv_reads', 'drv_unfillable'):   # 驱动段读取统计：两档必须一致，否则运行时会漏填 c0 / c2
            if _norm(a.get(tgt)) != _norm(c.get(tgt)):
                diff.append((key, tgt, a.get(tgt), c.get(tgt)))
        for nm, ea in ba.items():
            ec = bc[nm]
            ka = (ea['kind'], ea.get('reg'), ea.get('rw'), ea.get('bank'))
            kc = (ec['kind'], ec.get('reg'), ec.get('rw'), ec.get('bank'))
            kind_seen[ea['kind']] += 1
            if ka != kc:
                diff.append((key, nm, ka, kc))
            else:
                same += 1
    print(f'比对条目 {same + len(diff)}：一致 {same}，不一致 {len(diff)}，缺产物 {miss}')
    print('覆盖到的种类：' + ', '.join(f'{k}×{v}' for k, v in sorted(kind_seen.items())))
    if diff:
        print('\n不一致明细（前 20）：')
        for key, nm, ka, kc in diff[:20]:
            print(f'  {key[:16]}  {nm:<34} ab={ka}  discrete={kc}')
        sys.exit(1)
    print('\n【绿】两档反射完全一致：改绑定模型未改变任何资源的种类与槽号。')

    # 顺带核对 discrete 档的槽位分布（步骤 3 要用）
    mx = collections.Counter()
    for key in keys:
        pc = os.path.join(outs['discrete'], key + '_b.json')
        if not os.path.exists(pc):
            continue
        d = json.load(open(pc))
        for b in d['bindings']:
            mx[b['cls']] = max(mx[b['cls']], b['slot'] + 1)
        if len(d['bindings']) > 64:
            print(f'  注意 {key[:16]}：绑定总数 {len(d["bindings"])} 超 NVN_REFL_MAXB(64)')
    print('discrete 档槽位占用峰值：' + ', '.join(f'{k} {v}' for k, v in sorted(mx.items())))


if __name__ == '__main__':
    main()
