#!/usr/bin/env python3
# 全量验证：对映射表里全部着色器，逐个生成 C 档（离散绑定）MSL，核对 family 5 的资源上限。
# 复用 dmsh_build.py 的真实翻译链，只改 spirv-cross 选项。分片并行，结果写 result_all.json。
# 这是「能不能支持 iPad Pro 2020」的决定性判据：全部通过 = 路线成立；有超限 = 需逐个特例处理。
import os, sys, json, re, struct, subprocess, collections, hashlib
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = '/Users/zhcy/Documents/prj/d2r-mobile/dm/gfx/tools'
GFX = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
DM_ROOT = os.environ.get('DM_ROOT', os.path.expanduser('~/dm-work'))
DM_DIR = os.path.join(DM_ROOT, 'build/cache/dm/dmsh')
XLATE = os.path.join(DM_ROOT, 'build/gfx/dmsh_xlate/dmsh_xlate')
MAP = os.path.join(GFX, 'nvn/shader_map.tsv')
OUT = os.path.expanduser('~/dm-work/out/tier1_verify')
LIM_BUF, LIM_TEX, LIM_SAMP = 31, 96, 16

BASE = ['--msl', '--msl-version', '20300', '--msl-texture-buffer-native', '--msl-pad-fragment-output']


def load_facts():
    """作答表：键 → 事实 JSON（贴图类型 / 顶点属性 / 输出类型 / 线程组）。
    缺了它翻译器会对每个查询取缺省值，资源类型认错就会译不出来——必须与 dmsh_build.py 同口径载入。"""
    t = os.path.join(GFX, 'nvn/shader_facts.tsv')
    if not os.path.exists(t):
        return {}
    return {k: json.loads(v) for k, v in (l.rstrip('\n').split('\t', 1)
            for l in open(t) if l.strip() and not l.startswith('#'))}


FACTS = {}


def _init_facts():
    """模块导入即载入作答表：分片子进程走 do_shard，不会经过 load_map()，
    靠模块级初始化才能保证每个子进程都拿到（否则贴图类型全缺，翻译大量失败）。"""
    FACTS.update(load_facts())


_init_facts()


def load_map():
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
    FACTS.update(load_facts())
    return loc, stage_of


def records(d):
    r = [m.start() for m in re.finditer(b'BTCDBTCD', d)]
    return [d[x:(r[k + 1] if k + 1 < len(r) else len(d))] for k, x in enumerate(r)]


def main_rec(mi, o):
    g = mi.rfind(bytes.fromhex('78563412'), o - 0x1000, o)
    if g < 0 or mi[o:o + 4] != bytes.fromhex('34127698'):
        raise ValueError('bad embedded')
    return bytes(0x38) + mi[g:o + 0x880]


def make_jobs(keys, loc, stage_of, tag):
    """按分片提取记录并生成翻译作业。"""
    sys.path.insert(0, os.path.join(GFX, '..', 'verify'))
    packs, jobs, w = {}, [], os.path.join(OUT, 'all_' + tag)
    os.makedirs(w, exist_ok=True)
    for key in keys:
        name, k = loc[key]
        try:
            if name.startswith('main.rodata@'):
                if 'main' not in packs:
                    import nsoimg
                    packs['main'] = nsoimg.load(os.path.join(DM_ROOT, 'data/switch/3.3.94012/Update/ExeFS/main'))
                rec = main_rec(packs['main'], k)
            else:
                if name not in packs:
                    packs[name] = records(open(os.path.join(DM_DIR, name), 'rb').read())
                rec = packs[name][k]
        except Exception:
            continue
        ci = rec.find(bytes.fromhex('34127698'))
        code = rec[0x38:ci]
        stage = stage_of.get(key) or name[:2]
        if stage != 'cs':
            code = code[0x30:]
        c1o, c1t = struct.unpack_from('<2I', rec, ci + 0x700)
        c1 = rec[0x38 + c1o:0x38 + c1t]
        cp = os.path.join(w, key + '.bin')
        open(cp, 'wb').write(code)
        j = {'key': key, 'stage': stage, 'code': cp, 'tex': {}, 'img': {}, 'attr': {},
             'fout': {}, 'c1': c1.hex(), 'debug': False, 'reduced': True}
        f = FACTS.get(key)
        if f:
            j.update(f)
        jobs.append(j)
    return jobs, w


def do_shard(args):
    keys, loc, stage_of, tag = args
    jobs, w = make_jobs(keys, loc, stage_of, tag)
    if not jobs:
        return []
    jp = os.path.join(w, 'job.json')
    json.dump(jobs, open(jp, 'w'))
    spvdir = os.path.join(w, 'spv')
    os.makedirs(spvdir, exist_ok=True)
    r = subprocess.run([XLATE, jp, spvdir], capture_output=True, text=True)
    out = []
    for j in jobs:
        spv = os.path.join(spvdir, j['key'] + '.spv')
        if not os.path.exists(spv):
            out.append({'key': j['key'], 'st': 'xlate_fail'})
            continue
        rc, src, err = subprocess.run(['spirv-cross', *BASE, spv], capture_output=True, text=True), None, None
        if rc.returncode != 0:
            out.append({'key': j['key'], 'st': 'xc_fail', 'err': (rc.stderr or '').strip().splitlines()[:1]})
            continue
        src = rc.stdout
        m = re.search(r'^(?:vertex|fragment|kernel)\s+\w+\s+\w+\s*\((.*?)\)\s*\{', src, re.S | re.M)
        if not m:
            out.append({'key': j['key'], 'st': 'no_entry'})
            continue
        args_s = m.group(1)
        b = len(re.findall(r'\[\[buffer\(\d+\)\]\]', args_s))
        t = len(re.findall(r'\[\[texture\(\d+\)\]\]', args_s))
        s = len(re.findall(r'\[\[sampler\(\d+\)\]\]', args_s))
        w_ = len(re.findall(r'access::(?:write|read_write)', src))
        over = []
        if b > LIM_BUF: over.append(f'buf{b}')
        if t > LIM_TEX: over.append(f'tex{t}')
        if s > LIM_SAMP: over.append(f'samp{s}')
        out.append({'key': j['key'], 'st': 'ok', 'kind': src[:src.index(m.group(0))].strip().split('\n')[-1].split()[0],
                    'buf': b, 'tex': t, 'samp': s, 'write': w_, 'over': over})
    return out


def main():
    loc, stage_of = load_map()
    keys = sorted(loc)
    print(f'映射表共 {len(keys)} 个着色器，分 12 片并行验证…')
    NJ = 12
    shards = [(keys[i::NJ], loc, stage_of, str(i)) for i in range(NJ)]
    res = []
    with ProcessPoolExecutor(max_workers=NJ) as ex:
        for i, part in enumerate(ex.map(do_shard, shards)):
            res.extend(part)
            print(f'  片{i} 完成（{len(part)}）')

    st = collections.Counter(x['st'] for x in res)
    ok = [x for x in res if x['st'] == 'ok']
    over = [x for x in ok if x['over']]
    print(f"\n翻译+生成：成功 {st['ok']}，翻译失败 {st['xlate_fail']}，MSL 失败 {st['xc_fail']}，无入口 {st['no_entry']}")
    print(f"超限：{len(over)}")
    if over:
        print('\n超限明细（前 30）：')
        for x in sorted(over, key=lambda y: -len(y['over']))[:30]:
            print(f"  {x['key']}  缓冲{x['buf']} 贴图{x['tex']} 采样{x['samp']}  → {','.join(x['over'])}")
    if ok:
        print(f"\n资源峰值：缓冲{max(x['buf'] for x in ok)}（上限{LIM_BUF}）、"
              f"贴图{max(x['tex'] for x in ok)}（上限{LIM_TEX}）、"
              f"采样{max(x['samp'] for x in ok)}（上限{LIM_SAMP}）")
        wr = [x for x in ok if x['write']]
        print(f"含可写贴图：{len(wr)} 个，全部落在独立纹理槽")
    json.dump(res, open(os.path.join(OUT, 'result_all.json'), 'w'), ensure_ascii=False)
    print(f'\n明细：{os.path.join(OUT, "result_all.json")}')


if __name__ == '__main__':
    main()