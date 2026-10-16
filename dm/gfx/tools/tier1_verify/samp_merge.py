#!/usr/bin/env python3
# 采样器合并变体生成器：采样器槽超过 16 个的着色器（离散绑定下 MSL 硬性上限 0–15）走这条。
#
# 原理：MSL 里每个 sampler 参数必须独占一个 [[sampler(n)]] 位置（共用会报
# "cannot reserve 'sampler' resource location"），所以合并必须做到「变量级」——
# 把同组的 sampler 变量在函数体里的引用全部换成组代表，再从签名里删掉多余声明并重编号。
# 同组的判据是运行时采样器配置（samp_par）完全相同，故合并后渲染语义不变。
#
# 产物可固化：samp_par 由游戏决定、在运行期稳定，故「配置分组 → 合并 MSL → metallib」
# 只需生成一次（Mac 上离线编），运行时直接加载 metallib，零运行时编译。
#
# 用法：
#   samp_merge.py <spv 或 metal> --by-reg [--out 目录]    按 NVN 槽分组（编产物）
#   samp_merge.py <spv 或 metal> --simulate 8 [--out 目录] 原型：按 i % k 均分（只验证机制）
#   samp_merge.py <spv 或 metal> --plan plan.json [--out 目录] 按 samp_plan.py 出的方案分组
#
# --by-reg 是正路：属于同一个 NVN 槽 r 的 sampler 变量，运行时取的都是同一个句柄
#（S.tex[stage][r] >> 20 或 S.samp[stage][r] >> 20），任何一次绘制里配置都相同，
# 所以把它们并为一组永远安全，与游戏怎么改采样器配置无关——不需要真机 dump 就能定。
# --by-reg is the real path: sampler variables belonging to the same NVN slot r all read the same handle
# (S.tex[stage][r] >> 20 or S.samp[stage][r] >> 20), so their config is identical in every draw;
# merging them into one group is therefore always safe, whatever the game does with sampler configs -- no device dump needed.
import os, re, sys, json, hashlib, subprocess, collections, ast

XC = ['--msl', '--msl-version', '20300', '--msl-texture-buffer-native', '--msl-pad-fragment-output']
LIM_SAMPLER = 16
BUILD = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'dmsh_build.py')

def dmsh_helpers():
    """从 dmsh_build.py 取 decode_tex / entry_args（不执行它的顶层：顶层是全量构建流程）| Take decode_tex / entry_args from dmsh_build.py without executing its top level (which is the full build)"""
    tree = ast.parse(open(BUILD).read())
    ns = {'re': re, 'TEX_BASE': 0x168, 'SAMP_BASE': 0x568, 'IMG_BASE': 0x120}   # 常量见 dmsh_build.py 第 51 行 | constants: see dmsh_build.py line 51
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name in ('decode_tex', 'entry_args'):
            exec(compile(ast.Module([node], []), '<f>', 'exec'), ns)
    return ns

def reg_of(helpers, nm):
    """sampler 变量名 → 它所属的 NVN 槽（与 dmsh_build.reflect 同口径）| sampler variable name → the NVN slot it belongs to (same convention as dmsh_build.reflect)"""
    d = helpers['decode_tex'](nm[:-5]) if nm.endswith('Smplr') else None
    return (d[1] if d and d[2] else d[0]) if d else None

def groups_by_reg(helpers, names):
    """names：[(变量名, 槽位)] → 按 NVN 槽聚类的分组（每组内按 MSL 槽位排序）"""
    g = collections.OrderedDict()
    for nm, slot in sorted(names, key=lambda x: x[1]):
        g.setdefault(reg_of(helpers, nm), []).append(nm)
    return [v for v in g.values()]

XC = ['--msl', '--msl-version', '20300', '--msl-texture-buffer-native', '--msl-pad-fragment-output']
LIM_SAMPLER = 16


def msl_of(src):
    """输入可以是 .spv（调 spirv-cross）或已生成的 .metal"""
    if src.endswith('.spv'):
        r = subprocess.run(['spirv-cross', *XC, src], capture_output=True, text=True)
        if r.returncode:
            sys.exit('spirv-cross 失败：' + r.stderr[:200])
        return r.stdout
    return open(src).read()


def sampler_vars(msl):
    """入口签名里的 sampler 参数：[(变量名, 槽号)]，按槽号排序"""
    m = re.search(r'^(?:vertex|fragment|kernel).*?\((.*?)\)\s*\{', msl, re.S | re.M)
    if not m:
        return []
    out = [(nm, int(n)) for nm, n in re.findall(r'sampler (\w+) \[\[sampler\((\d+)\)\]\]', m.group(1))]
    return sorted(out, key=lambda x: x[1])


def merge(msl, groups):
    """groups：[[变量名...], ...]，每组配置相同；组内首个为代表。
    返回 (合并后的 MSL, 分组映射 {msl 变量名: 槽号})"""
    m = re.search(r'^((?:vertex|fragment|kernel).*?\)\s*\{)', msl, re.S | re.M)
    if not m:
        sys.exit('找不到入口函数签名')
    pre, sig, body = msl[:m.start(1)], m.group(1), msl[m.end(1):]
    mapping = {}
    for g, members in enumerate(groups):
        rep = members[0]
        mapping[rep] = g
        for other in members[1:]:
            # 函数体内全部引用换成代表（sampler 是值类型，可直接替换）
            body = re.sub(r'\b' + re.escape(other) + r'\b', rep, body)
            sig = re.sub(r'\s*sampler ' + re.escape(other) + r' \[\[sampler\(\d+\)\]\],?', '', sig)
        sig = re.sub(r'(sampler ' + re.escape(rep) + r' \[\[sampler\()\d+(\)\]\])',
                     lambda mm: mm.group(1) + str(g) + mm.group(2), sig)
    return pre + sig + body, mapping


def simulate_groups(names, k):
    """原型用：把 n 个变量按 i % k 均分成 k 组（模拟「去重后只剩 k 种配置」）"""
    g = [[] for _ in range(k)]
    for i, nm in enumerate(names):
        g[i % k].append(nm)
    return [x for x in g if x]


def resource_counts(msl):
    m = re.search(r'^(?:vertex|fragment|kernel).*?\((.*?)\)\s*\{', msl, re.S | re.M)
    a = m.group(1) if m else ''
    return {'buffer': len(re.findall(r'\[\[buffer\(\d+\)\]\]', a)),
            'texture': len(re.findall(r'\[\[texture\(\d+\)\]\]', a)),
            'sampler': len(re.findall(r'\[\[sampler\(\d+\)\]\]', a))}


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    src = a[0]
    mode, k, outdir = 'by-reg', 16, 'samp_merge_out'
    for i, x in enumerate(a[1:]):
        if x == '--simulate': mode, k = 'sim', int(a[1:][i + 1])
        elif x == '--by-reg': mode = 'by-reg'
        elif x == '--plan': mode, k = 'plan', a[1:][i + 1]
        elif x == '--out': outdir = a[1:][i + 1]
    os.makedirs(outdir, exist_ok=True)
    key = os.path.basename(src).split('.')[0]
    msl = msl_of(src)
    sv = sampler_vars(msl)
    H = dmsh_helpers()
    regs = {reg_of(H, nm) for nm, _ in sv}
    print(f'{key}：sampler 变量 {len(sv)} 个，分属 {len(regs)} 个 NVN 槽 → 上界 {len(regs)}')
    if len(sv) <= LIM_SAMPLER:
        print(f'  未超 {LIM_SAMPLER}，不需要合并。')
        return 0
    if mode == 'by-reg':
        groups = groups_by_reg(H, sv)
    elif mode == 'plan':
        groups = json.load(open(k, encoding='utf-8'))[key]['groups']
    else:
        groups = simulate_groups([nm for nm, _ in sv], k)
    out, mapping = merge(msl, groups)
    c = resource_counts(out)
    # 自检：被并掉的变量名不得在函数体里残留 | self-check: no merged-away variable name may survive in the body
    dropped = [m for g in groups for m in g[1:]]
    resid = [d for d in dropped if re.search(r'\b' + re.escape(d) + r'\b', out)]
    print(f'  合并为 {len(groups)} 组 → 资源 {c}（sampler {c["sampler"]} ≤ {LIM_SAMPLER}）；残留变量 {len(resid)}')
    if c['sampler'] > LIM_SAMPLER or c['texture'] > 96 or c['buffer'] > 31 or resid:
        print('  【红】自检不通过，不出产物')
        return 1
    p = os.path.join(outdir, f'{key}_sm{len(groups)}.metal')
    open(p, 'w').write(out)
    libs = {}
    for sdk in ('iphoneos', 'macosx'):   # iOS 库随包走；macOS 那份只用来交叉验证口径 | the iOS library ships; the macOS one only cross-checks the convention
        air = p[:-6] + f'.{sdk}.air'
        lib = p[:-6] + f'.{sdk}.metallib'
        r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', p, '-o', air], capture_output=True, text=True)
        if r.returncode:
            print(f'  【红】{sdk} 编译失败：', next((l for l in r.stderr.splitlines() if 'error' in l.lower()), r.stderr[:150]))
            return 1
        subprocess.run(['xcrun', '-sdk', sdk, 'metallib', air, '-o', lib], check=True)
        libs[sdk] = lib
    print(f'  编译通过 → {libs["iphoneos"]}')
    json.dump({'key': key, 'mode': mode, 'groups': groups, 'groups_by_reg': [[reg_of(H, m) for m in g] for g in groups],
               'mapping': mapping, 'resources': c, 'libs': libs},
              open(os.path.join(outdir, f'{key}_sm{len(groups)}.json'), 'w'), ensure_ascii=False, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
