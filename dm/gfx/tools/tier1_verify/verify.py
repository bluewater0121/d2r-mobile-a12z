#!/usr/bin/env python3
# 一次性验证脚本（不进仓）：在不改动仓库任何文件的前提下，检验「去掉参数缓冲（退回离散绑定）」
# 能否让 MSL 在 Metal 2 / 参数缓冲 Tier 1 的资源上限内编译通过。
# 做法：复用 dmsh_build.py 的真实翻译链（Ryujinx→SPIR-V→spirv-cross），只把 spirv-cross 的
# --msl-argument-buffers 换成两种对照口径，再按 family 5 的上限逐个核对资源数。
#   A档：现状（--msl-argument-buffers），作为基线
#   B 档：--msl-argument-buffer-tier 1（保留参数块结构，但按 Tier1 布局生成）
#   C 档：完全去掉 --msl-argument-buffers（离散绑定，Metal 2 经典做法）
# 判据：C 档每个着色器都能编译，且每函数 缓冲<=31、贴图<=96、采样器<=16。
import os, sys, json, re, struct, subprocess, hashlib, collections, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = '/Users/zhcy/Documents/prj/d2r-mobile/dm/gfx/tools'
GFX = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
DM_ROOT = os.environ.get('DM_ROOT', os.path.expanduser('~/dm-work'))
DM_DIR = os.path.join(DM_ROOT, 'build/cache/dm/dmsh')
XLATE = os.path.join(DM_ROOT, 'build/gfx/dmsh_xlate/dmsh_xlate')
MAP = os.path.join(GFX, 'nvn/shader_map.tsv')
OUT = os.path.expanduser('~/dm-work/out/tier1_verify')
WORK = os.path.join(OUT, 'work')
os.makedirs(WORK, exist_ok=True)

# family 5（iPad Pro 2020 / A12Z）的每函数资源上限，见 Metal Feature Set Tables
LIM_BUF, LIM_TEX, LIM_SAMP = 31, 96, 16


def load_map():
    """读映射表：键(第4列) → (包名, 记录号, 阶段)。与 dmsh_build.py 同口径。
    包内行的阶段也在第 3 列（中文：顶点/像素/计算），dmsh_build.py 的 stage_of 只在内嵌行填，
    其余用 name[:2] 推断；这里统一按第 3 列取，避免两套规则。"""
    loc, stage_of = {}, {}
    zh = {'顶点': 'vs', '像素': 'ps', '计算': 'cs', '域': 'ds', '外壳': 'hs'}
    for ln in open(MAP):
        if ln.startswith('#'):
            continue
        c = ln.rstrip('\n').split('\t')
        if len(c) <= 4:
            continue
        key, st = c[3], zh.get(c[2])
        if c[0].endswith('.bin'):
            loc.setdefault(key, (c[0], int(c[1])))
            if st:
                stage_of.setdefault(key, st)
        elif c[0].startswith('main.rodata@') and st:
            loc.setdefault(key, (c[0], int(c[0].split('@')[1], 16)))
            stage_of[key] = st
    return loc, stage_of


def records(d):
    r = [m.start() for m in re.finditer(b'BTCDBTCD', d)]
    return [d[x:(r[k + 1] if k + 1 < len(r) else len(d))] for k, x in enumerate(r)]


def main_rec(mi, o):
    g = mi.rfind(bytes.fromhex('78563412'), o - 0x1000, o)
    if g < 0 or mi[o:o + 4] != bytes.fromhex('34127698'):
        raise ValueError('不是内嵌控制段')
    return bytes(0x38) + mi[g:o + 0x880]


def get_record(key, name, k, packs):
    if name.startswith('main.rodata@'):
        if 'main' not in packs:
            sys.path.insert(0, os.path.join(GFX, '..', 'verify'))
            import nsoimg
            packs['main'] = nsoimg.load(os.path.join(DM_ROOT, 'data/switch/3.3.94012/Update/ExeFS/main'))
        return main_rec(packs['main'], k)
    if name not in packs:
        packs[name] = records(open(os.path.join(DM_DIR, name), 'rb').read())
    return packs[name][k]


def job_for(key, rec, stage, c1_hex):
    ci = rec.find(bytes.fromhex('34127698'))
    code = rec[0x38:ci]
    if stage != 'cs':
        code = code[0x30:]
    c1o, c1t = struct.unpack_from('<2I', rec, ci + 0x700)
    c1 = rec[0x38 + c1o:0x38 + c1t]
    cp = os.path.join(WORK, key + '.bin')
    open(cp, 'wb').write(code)
    return {'key': key, 'stage': stage, 'code': cp, 'tex': {}, 'img': {}, 'attr': {}, 'fout': {},
            'c1': (c1 or c1_hex).hex() if c1 else c1_hex, 'debug': False, 'reduced': True}


def xlate(jobs):
    jp = os.path.join(WORK, 'job.json')
    json.dump(jobs, open(jp, 'w'))
    od = os.path.join(WORK, 'spv')
    os.makedirs(od, exist_ok=True)
    r = subprocess.run([XLATE, jp, od], capture_output=True, text=True)
    return od, r


def msl(spv, mode):
    """按三种口径生成 MSL。"""
    base = ['--msl', '--msl-version', '20300', '--msl-texture-buffer-native', '--msl-pad-fragment-output']
    if mode == 'A':       # 现状：Tier 2 无绑定
        opts = base + ['--msl-argument-buffers', '--msl-argument-buffer-tier', '2']
    elif mode == 'B':     # 保留参数块，但按 Tier 1 布局
        opts = base + ['--msl-argument-buffers', '--msl-argument-buffer-tier', '1']
    else:                 # C：离散绑定（Metal 2 经典）
        opts = base
    r = subprocess.run(['spirv-cross', *opts, spv], capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def entry_of(src):
    m = re.search(r'^(vertex|fragment|kernel)\s+(\w+)\s+(\w+)\s*\(', src, re.M)
    return (m.group(1), m.group(2)) if m else (None, None)


def counts(src):
    """数入口函数签名里的逐槽资源数（离散绑定口径）。"""
    kind, name = entry_of(src)
    if not kind:
        return None
    m = re.search(r'^(?:vertex|fragment|kernel)\s+\w+\s+\w+\s*\((.*?)\)\s*\{', src, re.S | re.M)
    if not m:
        return None
    args = m.group(1)
    b = len(re.findall(r'\[\[buffer\(\d+\)\]\]', args))
    t = len(re.findall(r'\[\[texture\(\d+\)\]\]', args))
    s = len(re.findall(r'\[\[sampler\(\d+\)\]\]', args))
    # 参数块口径下资源藏在 struct 里（A 档）
    ab_buf = ab_tex = ab_smp = 0
    for sm in re.finditer(r'struct\s+\w+\s*\{(.*?)\n\};', src, re.S):
        for l in sm.group(1).splitlines():
            if '[[id(' not in l:
                continue
            if re.search(r'\bconstant\s+\w+\s*\*', l):
                ab_buf += 1
            elif re.search(r'texture\w*<', l):
                ab_tex += 1
            elif re.match(r'\s*sampler\s', l):
                ab_smp += 1
    return {'kind': kind, 'entry': name, 'buf': b, 'tex': t, 'samp': s,
            'ab_buf': ab_buf, 'ab_tex': ab_tex, 'ab_samp': ab_smp,
            'ab_write': len(re.findall(r'access::(?:write|read_write)', src))}


def pick_targets():
    """挑验证目标：几个最简单的片元着色器 + 全部含可写贴图的库。"""
    loc, stage_of = load_map()
    # 含可写贴图（Tier1 硬禁）的键，从已出的 MSL 里读
    cache = os.path.join(DM_ROOT, 'build/cache/metal_shader_cache')
    write_keys = []
    for d in sorted(os.listdir(cache)):
        p = os.path.join(cache, d)
        if not (d.startswith('b-') and os.path.isdir(p)):
            continue
        for f in os.listdir(p):
            if f.endswith('_b.metal'):
                s = open(os.path.join(p, f), errors='ignore').read()
                if 'access::write' in s or 'access::read_write' in s:
                    write_keys.append(f[:-6])
    simple = sorted(k for k, (n, _) in loc.items()
                    if stage_of.get(k) == 'ps' and n.endswith('.bin'))
    return loc, stage_of, write_keys, simple


def main():
    if not os.path.isdir(DM_DIR):
        sys.exit(f'没有着色器包目录 {DM_DIR}（先跑 ./setup.sh）')
    loc, stage_of = load_map()
    cache = os.path.join(DM_ROOT, 'build/cache/metal_shader_cache')
    # 可写贴图着色器键：由 writekeys.txt 给（对当前链版本 b-c2b35eaded5d 用 grep 提取）
    kf = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'writekeys.txt')
    write_keys = [l.strip() for l in open(kf) if l.strip()] if os.path.exists(kf) else []
    if not write_keys:   # 兜底：现扫缓存
        for d in sorted(os.listdir(cache)):
            p = os.path.join(cache, d)
            if not (d.startswith('b-') and os.path.isdir(p)):
                continue
            for f in os.listdir(p):
                if f.endswith('_b.metal'):
                    s = open(os.path.join(p, f), errors='ignore').read()
                    if 'access::write' in s or 'access::read_write' in s:
                        write_keys.append(f[:-6])
    packs = {}
    # 优先攻含可写贴图的库（Tier1 的真门槛），其次补几个最简单的片元
    wsel = [k for k in write_keys if k in loc]
    simple = sorted(k for k, (n, _) in loc.items()
                    if stage_of.get(k) == 'ps' and n.endswith('.bin'))[:5]
    targets = list(dict.fromkeys(wsel + simple))
    print(f'目标：含可写贴图 {len(wsel)} 个（Tier1 真门槛）+ 片元 {len(simple)} 个 = {len(targets)}\n')

    jobs = []
    for key in targets:
        name, k = loc[key]
        try:
            rec = get_record(key, name, k, packs)
        except Exception as e:
            print(f'  跳过 {key}: {e}')
            continue
        jobs.append(job_for(key, rec, stage_of.get(key) or name[:2], ''))
    if not jobs:
        sys.exit('没有可用目标')

    spvdir, r = xlate(jobs)
    ok_spv = [j['key'] for j in jobs if os.path.exists(os.path.join(spvdir, j['key'] + '.spv'))]
    print(f'SPIR-V 翻译：{len(ok_spv)}/{len(jobs)} 成功\n')

    print(f'{"着色器":<18}{"阶段":<5}{"档":<4}{"编译":<6}{"缓冲":<6}{"贴图":<6}{"采样":<6}{"可写":<6}判定')
    print('-' * 78)
    verdict = collections.Counter()
    detail = []
    for key in ok_spv:
        spv = os.path.join(spvdir, key + '.spv')
        for mode in ('A', 'C'):
            rc, src, err = msl(spv, mode)
            if rc != 0:
                msg = err.strip().splitlines()[0][:44] if err.strip() else ''
                print(f'{key[:16]:<18}{"":<5}{mode:<4}{"失败":<6}{"":<6}{"":<6}{"":<6}{"":<6}{msg}')
                verdict[mode + '_xc'] += 1
                continue
            c = counts(src)
            if not c:
                verdict[mode + '_noentry'] += 1
                continue
            # A 档资源藏在 struct，C 档在入口签名；各自口径，不叠加
            if mode == 'A':
                b, t, s = c['ab_buf'], c['ab_tex'], c['ab_samp']
            else:
                b, t, s = c['buf'], c['tex'], c['samp']
            over = []
            if b > LIM_BUF: over.append(f'缓冲{b}>{LIM_BUF}')
            if t > LIM_TEX: over.append(f'贴图{t}>{LIM_TEX}')
            if s > LIM_SAMP: over.append(f'采样{s}>{LIM_SAMP}')
            if mode == 'C' and c['ab_write']:
                # C 档下 access::write 变成独立的可写纹理槽，Tier1 允许
                pass
            v = 'OK' if not over else '超限:' + ','.join(over)
            verdict[mode + ('_ok' if not over else '_over')] += 1
            print(f'{key[:16]:<18}{c["kind"][:4]:<5}{mode:<4}{"成功":<6}{b:<6}{t:<6}{s:<6}{c["ab_write"]:<6}{v}')
            detail.append({'key': key, 'mode': mode, 'kind': c['kind'], 'buf': b, 'tex': t,
                           'samp': s, 'write': c['ab_write'], 'over': over})

    print('\n===== 结论 =====')
    print(f"A 档（现状 Tier2 无绑定）：成功 {verdict['A_ok']}，超限 {verdict['A_over']}，失败 {verdict['A_xc']}")
    print(f"C 档（离散绑定 / Tier1）：成功 {verdict['C_ok']}，超限 {verdict['C_over']}，失败 {verdict['C_xc']}")
    print(f"family 5 上限：缓冲<={LIM_BUF}、贴图<={LIM_TEX}、采样<={LIM_SAMP}")
    json.dump(detail, open(os.path.join(OUT, 'result.json'), 'w'), ensure_ascii=False, indent=1)
    print(f'\n明细：{os.path.join(OUT, "result.json")}')


if __name__ == '__main__':
    main()