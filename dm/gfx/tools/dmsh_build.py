#!/usr/bin/env python3
# B 路线离线出包：Switch 原版着色器 → SPIR-V（dmsh_xlate，Ryujinx 翻译器）→ spirv-cross MSL → metallib + 回放反射 JSON。
# 运行时不翻译（iOS 无 JIT）：产物进着色器缓存目录，文件名 = Switch 控制段键（dm_ctl_sha16，与 shader_map.tsv 第 4 列同）+「_b」。
# 查询作答来源：仓内的作答表 nvn/shader_facts.tsv（dmsh_facts.py）——贴图类型、顶点属性 / 片元输出分量类型、计算线程组大小与共享内存；出包不需要 PC 版文件。
# 用法：dmsh_build.py <Switch 包目录> <输出缓存目录> [键…]（不给键 = 全部）；产物实际写进 <输出缓存目录>/b-<链版本>/（IOSOUT 同），链版本见 dmsh_chain.py   环境：XLATE=翻译工具路径，DMSH_CAS=内容寻址缓存目录（缺省为输出缓存目录/cas），J=并发
# 程序本体内嵌的图形着色器（映射表 main.rodata@偏移 行）从 MAIN（Switch main NSO，缺省 3.3.94012 Update/ExeFS/main）解出；解 NSO 要 lz4，用 dm 的 venv python 跑
# 产物：<键>_b.metallib、<键>_b.json（回放反射，含驱动段 c0 / c2 的运行时读取偏移 drv_reads）、<键>_b.metal（MSL）
# 环境 IOSOUT=目录：同一份 MSL 经 dm/ios/tools/ios_metal.compile 同时出 iphoneos metallib + 反射（chain_metal 记 iOS 编译口径）
# 末尾汇总：成功 / 失败数与原因分布；驱动段读取的偏移分布（落在回放层能填的范围外的单列）
# Route B offline build: original Switch shaders → SPIR-V (dmsh_xlate, the Ryujinx translator) → spirv-cross MSL → metallib + replay reflection JSON.
# No translation at runtime (iOS has no JIT): build output goes into the shader cache directory, file name = Switch control segment key (dm_ctl_sha16, same as column 4 of shader_map.tsv) + "_b".
# Source of query answers: the in-repo answer table nvn/shader_facts.tsv (dmsh_facts.py) - texture types, vertex attribute / fragment output component types, compute thread group size and shared memory; the build needs no PC version files.
# Usage: dmsh_build.py <Switch package dir> <output cache dir> [key...] (no keys = all); build output is actually written to <output cache dir>/b-<chain version>/ (same for IOSOUT), for the chain version see dmsh_chain.py   Environment: XLATE=path to the translation tool, DMSH_CAS=content-addressed cache dir (default <output cache dir>/cas), J=concurrency
# Graphics shaders embedded in the executable itself (mapping table rows main.rodata@offset) are extracted from MAIN (Switch main NSO, default 3.3.94012 Update/ExeFS/main); extracting the NSO needs lz4, run with dm's venv python
# Build output: <key>_b.metallib, <key>_b.json (replay reflection, including drv_reads, the runtime read offsets into driver segments c0 / c2), <key>_b.metal (MSL)
# Environment IOSOUT=dir: the same MSL also goes through dm/ios/tools/ios_metal.compile to produce an iphoneos metallib + reflection (chain_metal records the iOS compile convention)
# Final summary: success / failure counts and the distribution of reasons; distribution of driver segment read offsets (those outside the range the replay layer can fill are listed separately)
import ast, inspect, os, re, sys, json, struct, hashlib, shutil, subprocess, tempfile, collections, threading, concurrent.futures as cf
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')   # 工作区根目录（游戏文件、产物、运行现场都在它下面） | Workspace root (game files, build outputs and run artifacts are all under it)
HOME = os.path.expanduser('~')
dm_dir, out = sys.argv[1:3]; want = set(sys.argv[3:]); cache_root = out
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import dmsh_chain, dmsh_fetch, dmsh_facts   # 翻译链版本：先核工具与锁文件，产物进 b-<V>/ | translation chain version: verify the tools against the lock file first; build output goes into b-<V>/
_bad = dmsh_chain.check_tools()
if _bad: sys.exit('\n'.join('【红】翻译链工具与锁文件不符：' + b for b in _bad) + '\n（换了工具就 dmsh_chain.py --update-lock 并入仓；只是没重编翻译器就重编）')
CHAIN = dmsh_chain.version(); out = os.path.join(out, 'b-' + CHAIN)
if f'"{CHAIN}"' not in open(dmsh_chain.HEADER).read(): sys.exit(f'【红】nvn/dmsh_chain.h 不是当前翻译链版本 {CHAIN}：先跑 dmsh_chain.py --header（否则宿主找不到这批库）')
XLATE = os.environ.get('XLATE', f'{DM_ROOT}/build/gfx/dmsh_xlate/dmsh_xlate')
FACTS = dmsh_facts.load()
if not FACTS: sys.exit('【红】没有作答表 nvn/shader_facts.tsv（随仓库；维护者用 dmsh_facts.py --freeze 重出）')
IOSOUT = os.environ.get('IOSOUT')   # 设了就同时出 iOS 版（iphoneos metallib + 反射）到该目录 | if set, also build the iOS version (iphoneos metallib + reflection) into that directory
if IOSOUT:
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../ios/tools')); import ios_metal
    IOSOUT = os.path.join(IOSOUT, 'b-' + CHAIN); os.makedirs(IOSOUT, exist_ok=True)
MAIN = os.environ.get('MAIN', f'{DM_ROOT}/data/switch/3.3.94012/Update/ExeFS/main')
MAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../nvn/shader_map.tsv')
BF_WANT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../nvn/bf_want.tsv')
BF_WANT = {line.strip() for line in open(BF_WANT_FILE) if line.strip() and not line.lstrip().startswith('#')} if os.path.exists(BF_WANT_FILE) else set()
# 资源绑定档位（BIND）：ab = 参数块（argument buffer，要 argument buffers tier 2，即 Metal 3 / Apple GPU family 7 及以上，A14 起）；
#                      discrete = 逐槽绑定（spirv-cross 缺省形态，[[buffer(n)]] / [[texture(n)]] / [[sampler(n)]] 各自独立，Metal 2 起可用，老机型走这条）
# 两档的链版本 V 不同（dmsh_chain.py options() 计入 BIND），产物各自进 b-<V>/，互不覆盖，便于回归对比。
# Resource binding mode (BIND): ab = argument buffer (needs argument buffers tier 2, i.e. Metal 3 / Apple GPU family 7+, A14 onward);
#                      discrete = per-slot binding (spirv-cross default, [[buffer(n)]] / [[texture(n)]] / [[sampler(n)]] each independent, available since Metal 2, for older GPUs)
# The two modes have different chain versions V (BIND is part of dmsh_chain.py options()), so their build output goes to separate b-<V>/ dirs without overwriting, which makes regression comparison easy.
BIND = os.environ.get('DMSH_BIND', 'ab')
if BIND not in ('ab', 'discrete'): sys.exit('【红】DMSH_BIND 只可 ab（参数块）或 discrete（逐槽绑定）')
XC = ['--msl', '--msl-version', '20300'] + (['--msl-argument-buffers'] if BIND == 'ab' else []) + ['--msl-texture-buffer-native', '--msl-pad-fragment-output']
CAS = os.environ.get('DMSH_CAS', os.path.join(cache_root, 'cas'))
CAS_LOCKS, CAS_LOCKS_GUARD = {}, threading.Lock()
PRE_STATS = collections.Counter()
MAC_RECORD = 'xcrun -sdk macosx metal -c; xcrun -sdk macosx metallib'
TEX_BASE, SAMP_BASE, IMG_BASE = 0x168, 0x568, 0x120   # c[2]：图像句柄表 0x120 + 8 r（8 槽，紧接贴图表之前；cs_lut_combine_c 写 u4 = 0x140 反推，u5 = 0x148 核对） | c[2]: image handle table 0x120 + 8 r (8 slots, immediately before the texture table; inferred from cs_lut_combine_c writing u4 = 0x140, checked with u5 = 0x148)

def records(d):
    r = [m.start() for m in re.finditer(b'BTCDBTCD', d)]
    return [d[x:(r[k + 1] if k + 1 < len(r) else len(d))] for k, x in enumerate(r)]

def clone(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + '.tmp-' + str(os.getpid()) + '-' + str(threading.get_ident())
    r = subprocess.run(['cp', '-c', src, tmp], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=60)
    if r.returncode:
        shutil.copyfile(src, tmp)
    os.replace(tmp, dst)

def atomic_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp-' + str(os.getpid()) + '-' + str(threading.get_ident())
    with open(tmp, 'w') as f: json.dump(value, f, ensure_ascii=False, indent=1)
    os.replace(tmp, path)

def source_without_rule_bodies():
    source = open(__file__, 'r').read(); lines = source.splitlines(True); tree = ast.parse(source)
    names = {f.__name__ for f, _ in RULES}
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name in names and node.body:
            first = node.body[0].lineno - 1
            last = max(getattr(x, 'end_lineno', x.lineno) for x in ast.walk(node) if getattr(x, 'lineno', 0) >= node.body[0].lineno)
            indent = re.match(r'\s*', lines[first]).group(0); lines[first:last] = [indent + 'pass\n']
    return ''.join(lines)

def shader_rule_source(name):
    return b''.join(inspect.getsource(f).encode() for f, applies in RULES if applies(name))

def tool_fingerprint():
    sc = subprocess.run(['spirv-cross', '--version'], capture_output=True, text=True, timeout=30)
    return ('lock\0' + open(dmsh_chain.LOCK).read() + '\0options\0' + dmsh_chain.options() +
            '\0xlate\0' + dmsh_chain.xlate_source_sha() + '\0spirv-cross\0' + sc.stdout + sc.stderr +
            '\0xc\0' + repr(XC) + '\0mac\0' + MAC_RECORD + '\0ios\0' + (ios_metal.RECORD if IOSOUT else ''))

def shader_key(key, name, rec):
    pc = dmsh_facts.row(key, FACTS[key]).encode() if key in FACTS else '缺'.encode()
    map_row = MAP_ROWS.get(key, '缺'.encode()) + MAP_CONFLICT_ROWS.get(key, b'')
    embedded = rec if name.startswith('main.rodata@') else b''
    payload = (b'record\0' + rec + b'\0map\0' + map_row + b'\0pc\0' + pc + b'\0embedded\0' + embedded +
               b'\0tools\0' + TOOL_FP + b'\0code\0' + CODE_FP.encode() +
               b'\0rule\0' + shader_rule_source(name))
    return hashlib.sha256(payload).hexdigest()

def cache_meta_path(key): return os.path.join(CAS, 'index', key + '.json')
def cache_path(digest): return os.path.join(CAS, 'shader', digest)

def cache_outputs(key, digest, cached=False):
    root = cache_path(digest); files = {}
    for label, directory, names in (
            ('mac', out, [key + '_b.metallib', key + '_b.metal', key + '_b.json']),
            ('ios', IOSOUT, [key + '_b.metallib', key + '_b.json'])):
        if not directory: continue
        base = os.path.join(root, label) if cached else directory
        found = [n for n in names if os.path.exists(os.path.join(base, n))]
        if key + '_b.metallib' not in found or key + '_b.json' not in found: return None
        files[label] = found
    return root, files

def restore_cache(key, digest):
    found = cache_outputs(key, digest, cached=True)
    if not found: return False
    root, files = found
    for label, names in files.items():
        directory = out if label == 'mac' else IOSOUT
        for name in names:
            src = os.path.join(root, label, name); dst = os.path.join(directory, name)
            if name.endswith('.json'):
                tmp = dst + '.tmp-' + str(os.getpid()) + '-' + str(threading.get_ident())
                with open(tmp, 'wb') as f: f.write(re.sub(rb'("chain"\s*:\s*")[^"]*"', rb'\g<1>' + CHAIN.encode() + b'"', open(src, 'rb').read(), count=1))
                os.replace(tmp, dst)
            else: clone(src, dst)
    return True

def cache_lookup(key, digest, rule_fp, code_fp):
    with CAS_LOCKS_GUARD: lock = CAS_LOCKS.setdefault(('shader', key), threading.Lock())
    with lock:
        if restore_cache(key, digest): PRE_STATS['hit'] += 1; return True, '命中'
        old = json.load(open(cache_meta_path(key))) if os.path.exists(cache_meta_path(key)) else None
        if old and old.get('code_fp') != code_fp: reason = '代码指纹变化：全部重出'
        elif old and old.get('rule_fp') != rule_fp: reason = '规则变'
        else: reason = '新键'
        PRE_STATS['miss'] += 1; PRE_STATS[reason] += 1; return False, reason

def publish_cache(key, digest, rule_fp, code_fp):
    found = cache_outputs(key, digest)
    if not found: return
    root, files = found
    for label, names in files.items():
        d = os.path.join(root, label); os.makedirs(d, exist_ok=True)
        for name in names: clone(os.path.join(out if label == 'mac' else IOSOUT, name), os.path.join(d, name))
        for name in names:
            if name.endswith('.json'):
                src = os.path.join(out if label == 'mac' else IOSOUT, name)
                value = json.load(open(src)); value.pop('fetch', None)
                atomic_json(os.path.join(d, name), value)
    atomic_json(cache_meta_path(key), {'digest': digest, 'rule_fp': rule_fp, 'code_fp': code_fp})

BF_STATS = collections.Counter()

def bf_digest(msl, key):
    h = hashlib.sha256()
    h.update(msl.encode())
    h.update(b'\0mac\0' + MAC_RECORD.encode())
    h.update(b'\0ios\0' + (ios_metal.RECORD if IOSOUT else '').encode())
    h.update(b'\0bf-want\0' + (b'1' if key in BF_WANT else b'0'))
    return h.hexdigest()

def bf_cache_root(digest): return os.path.join(CAS, 'variant', digest)

def bf_cache_restore(key, digest, fmeta, fmsl):
    root = bf_cache_root(digest)
    names = ['metal', 'mac.metallib'] + (['ios.metallib'] if IOSOUT else [])
    if not all(os.path.exists(os.path.join(root, n)) for n in names): return False
    clone(os.path.join(root, 'metal'), fmsl)
    clone(os.path.join(root, 'mac.metallib'), os.path.join(out, key + '_bf.metallib'))
    if IOSOUT: clone(os.path.join(root, 'ios.metallib'), os.path.join(IOSOUT, key + '_bf.metallib'))
    BF_STATS['hit'] += 1
    return True

def bf_cache_publish(digest, fmsl, maclib, ioslib, fmeta):
    root = bf_cache_root(digest); os.makedirs(root, exist_ok=True)
    clone(fmsl, os.path.join(root, 'metal'))
    clone(maclib, os.path.join(root, 'mac.metallib'))
    if IOSOUT: clone(ioslib, os.path.join(root, 'ios.metallib'))
    atomic_json(os.path.join(root, 'fetch.json'), fmeta)

def write_refl(key, refl):
    for d, rec in ((out, MAC_RECORD),) + (((IOSOUT, ios_metal.RECORD),) if IOSOUT else ()):
        value = dict(refl, chain_metal=rec)
        atomic_json(os.path.join(d, key + '_b.json'), value)

def fetch_finish(key, src, refl):
    if refl['stage'] != 'ps': return refl
    # 有可改采样的照旧出变体；没有可改采样的只给名单（bf_want.tsv，宿主规划器产出）里的键出仅输出映射变体
    # Those with rewritable samples get variants as before; those without get an output-mapping-only variant, and only for keys in the list (bf_want.tsv, produced by the host planner)
    fv, fmeta = dmsh_fetch.variant(src, refl['bindings'], om_only_ok=key in BF_WANT)
    if not fv:
        for path in (os.path.join(out, key + '_bf.metallib'), os.path.join(out, key + '_bf.metal')) + ((os.path.join(IOSOUT, key + '_bf.metallib'),) if IOSOUT else ()):
            try: os.unlink(path)
            except FileNotFoundError: pass
        write_refl(key, refl)
        return refl
    BF_STATS['generated'] += 1
    fmsl = os.path.join(out, key + '_bf.metal')
    digest = bf_digest(fv, key)
    with CAS_LOCKS_GUARD: lock = CAS_LOCKS.setdefault(('variant', digest), threading.Lock())
    with lock:
        try:
            if not bf_cache_restore(key, digest, fmeta, fmsl):
                BF_STATS['miss'] += 1
                open(fmsl, 'w').write(fv)
                fair = fmsl[:-6] + '.air'; flib = os.path.join(out, key + '_bf.metallib')
                try:
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', fmsl, '-o', fair], check=True)
                    tmp_flib = flib + '.tmp-' + str(os.getpid())
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', fair, '-o', tmp_flib], check=True)
                    os.replace(tmp_flib, flib)
                    ilib = None
                    if IOSOUT:
                        ilib = os.path.join(IOSOUT, key + '_bf.metallib')
                        ios_metal.compile(fmsl, fmsl[:-6] + '.ios.air', ilib + '.tmp')
                        os.replace(ilib + '.tmp', ilib)
                except subprocess.CalledProcessError:
                    return refl
                BF_STATS['compile'] += 1
                bf_cache_publish(digest, fmsl, flib, ilib, fmeta)
        finally:
            for path in (fmsl, fmsl[:-6] + '.air', fmsl[:-6] + '.ios.air'):
                try: os.unlink(path)
                except FileNotFoundError: pass
    refl['fetch'] = fmeta
    write_refl(key, refl)
    return refl

pc_of, loc, stage_of = {}, {}, {}
MAP_ROWS, MAP_CONFLICT_ROWS = {}, {}
for raw in open(MAP, 'rb'):
    c = raw.rstrip(b'\n').split(b'\t')
    if len(c) > 4: MAP_ROWS.setdefault(c[3].decode(), raw)
for raw in open(MAP + '.conflicts.tsv', 'rb'):
    c = raw.rstrip(b'\n').split(b'\t')
    if len(c) > 1 and not raw.startswith((b'#', b'dm_')): MAP_CONFLICT_ROWS.setdefault(c[0].decode(), raw)
for ln in open(MAP):
    c = ln.rstrip('\n').split('\t')
    if len(c) > 4 and c[0].endswith('.bin'): pc_of.setdefault(c[3], c[4]); loc.setdefault(c[3], (c[0], int(c[1])))
    if len(c) > 4 and c[0].startswith('main.rodata@'):   # 内嵌：位置 = main 映像偏移（控制段起点），阶段取映射表第 3 列 | embedded: location = main image offset (start of the control segment), stage taken from column 3 of the mapping table
        pc_of.setdefault(c[3], c[4]); loc.setdefault(c[3], (c[0], int(c[0].split('@')[1], 16))); stage_of[c[3]] = {'顶点': 'vs', '像素': 'ps'}[c[2]]
for ln in open(MAP + '.conflicts.tsv'):
    c = ln.rstrip('\n').split('\t')
    if len(c) > 1 and not ln.startswith('#') and not ln.startswith('dm_') and c[0] in pc_of: pc_of[c[0]] = c[1]

def main_rec(mi, o):
    """内嵌着色器 → 与包内记录同布局的字节：包内记录 +0x38 起是代码区（0x30 头 + SPH + 指令，标记 78563412），紧接控制段（34127698）；
    内嵌的代码区紧贴在控制段之前（核对 5 个：代码区长 0x100–0x300、c1 均为空，与同名包内记录布局一致）
    Embedded shader → bytes in the same layout as a package record: in a package record the code area starts at +0x38 (0x30 header + SPH + instructions, marker 78563412), immediately followed by the control segment (34127698);
        the embedded code area sits immediately before the control segment (checked 5: code area length 0x100–0x300, c1 empty in all, consistent with the layout of the same-named package records)"""
    g = mi.rfind(bytes.fromhex('78563412'), o - 0x1000, o)
    if g < 0 or mi[o:o + 4] != bytes.fromhex('34127698'): sys.exit(f'【红】main 映像 @{o:#x} 不是内嵌着色器控制段（MAIN={MAIN}）')
    return bytes(0x38) + mi[g:o + 0x880]

def job_for(key, name, rec, w):
    ci = rec.find(bytes.fromhex('34127698')); code = rec[0x38:ci]; stage = stage_of.get(key, name[:2])
    c1o, c1t = struct.unpack_from('<2I', rec, ci + 0x700)   # 控制段 +0x700：c1 相对指令区起点、+0x704：指令区总长（c1 = 两者之间；全库长度 0 / 256 / 512 / 768） | control segment +0x700: c1 relative to the start of the instruction area, +0x704: total length of the instruction area (c1 = what lies between the two; lengths across the whole library are 0 / 256 / 512 / 768)
    c1 = rec[0x38 + c1o:0x38 + c1t]
    if stage != 'cs': code = code[0x30:]
    cp = os.path.join(w, key + '.bin'); open(cp, 'wb').write(code)
    j = {'key': key, 'stage': stage, 'code': cp, 'tex': {}, 'img': {}, 'attr': {}, 'fout': {}, 'c1': c1.hex(),
         'debug': os.environ.get('XLATE_DEBUG', '1') != '0', 'reduced': os.environ.get('XLATE_REDUCED', '1') == '1'}   # 翻译选项（见 dmsh_xlate/Program.cs）；放宽精度缺省开（GPU 浮点末位误差不进游戏状态，mini 整帧 −26%）   # c1：翻译器读间接跳转表用 | translation options (see dmsh_xlate/Program.cs); relaxed precision is on by default (GPU float last-bit error does not enter game state, whole frame −26% on the second Mac)   # c1: used by the translator to read indirect jump tables
    if key not in FACTS: j['pc_missing'] = True; return j, c1   # 作答表里没有：查询按缺省（汇总里单列） | not in the answer table: queries use defaults (listed separately in the summary)
    j.update(FACTS[key])
    return j, c1

def raster_facts(stage, src):
    """读入可省（全覆盖首绘）的运行时证明所需的静态事实（nvn_mtl.m first_draw_covers）：
    顶点：位置 x / y / z 是否原样取自同一个顶点属性的 .x / .y / .z（w 为常数 1 或同属性 .w），主函数无分支、无裁剪距离——是则 pos_attr = 属性槽；
    片元：是否可能丢弃（discard_fragment）、是否写 sample_mask、写出的颜色号。认不出一律按「不可证」（pos_attr = -1 / discard = 1）
    Static facts needed for the runtime proof that the load can be skipped (full-coverage first draw) (nvn_mtl.m first_draw_covers):
        Vertex: whether position x / y / z are taken unchanged from the .x / .y / .z of one and the same vertex attribute (w is the constant 1 or the same attribute's .w), with no branches and no clip distances in the main function - if so, pos_attr = attribute slot;
        Fragment: whether it may discard (discard_fragment), whether it writes sample_mask, and which color indices it writes. Anything unrecognized is treated as "unprovable" (pos_attr = -1 / discard = 1)"""
    if stage == 'ps':
        out = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
        return {'discard': int('discard_fragment' in src), 'sample_mask': int('sample_mask' in (out.group(1) if out else '')),
                'colors': sorted({int(x) for x in re.findall(r'\[\[color\((\d+)\)\]\]', out.group(1) if out else '')})}
    if stage != 'vs': return {}
    body = re.search(r'\bvertex\s+\w+\s+\w+\s*\(.*?\)\s*\{(.*)\}\s*$', src, re.S)   # 主函数是文件里最后一个函数 | the main function is the last function in the file
    if not body or re.search(r'\b(if|for|while|switch|do)\b|clip_distance|ClipDistance', body.group(1)): return {'pos_attr': -1}
    b = body.group(1)
    attr = {m.group(2): int(m.group(3)) for m in re.finditer(r'(\w+)\s+(\w+)\s*\[\[attribute\((\d+)\)\]\]', src)}
    var = {m.group(1): (m.group(2), m.group(3)) for m in re.finditer(r'float\s+(_\w+)\s*=\s*in\.(\w+)\.([xyzw]);', b)}
    last = {}
    for m in re.finditer(r'out\.gl_Position\.([xyzw])\s*=\s*([^;]+);', b): last[m.group(1)] = m.group(2).strip()
    src_of = {c: var.get(v) for c, v in last.items()}
    m0 = src_of.get('x')
    if not m0 or m0[0] not in attr or any(src_of.get(c) != (m0[0], c) for c in 'xyz'): return {'pos_attr': -1}
    if last.get('w') == '1.0': w = 'one'
    elif src_of.get('w') == (m0[0], 'w'): w = 'attr'
    else: return {'pos_attr': -1}
    # uv 直传（合并 B 第 2 步「本像素寻址」证明用）：插值输出 locn K 的 .x / .y 原样取自顶点属性 A 的 .x / .y → uv_from_attr[K] = A
    # uv pass-through (used for the "this-pixel addressing" proof in merge B step 2): the .x / .y of interpolated output locn K are taken unchanged from the .x / .y of vertex attribute A → uv_from_attr[K] = A
    ost = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    locn = {m.group(1): int(m.group(2)) for m in re.finditer(r'(m_\d+) \[\[user\(locn(\d+)\)\]\]', ost.group(1) if ost else '')}
    olast = {}
    for m in re.finditer(r'out\.(m_\d+)\.([xyzw])\s*=\s*([^;]+);', b): olast[(m.group(1), m.group(2))] = m.group(3).strip()
    uv, aff = {}, {}
    for mem, k in locn.items():
        sx, sy = var.get(olast.get((mem, 'x'), '')), var.get(olast.get((mem, 'y'), ''))
        if sx and sy and sx[0] == sy[0] and sx[0] in attr and sx[1] == 'x' and sy[1] == 'y': uv[str(k)] = attr[sx[0]]; continue
        # 仿射：分量 = fma(属性分量, uniform[槽].向量[i].分量, uniform[槽].向量[j].分量)（vp_cN 即 uniform 槽 N − 3）
        # Affine: component = fma(attribute component, uniform[slot].vector[i].component, uniform[slot].vector[j].component) (vp_cN is uniform slot N − 3)
        def fm(v_):
            m = re.search(r'float %s = fma\((_\w+), \(\*spvDescriptorSet0\.vp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw]), \(\*spvDescriptorSet0\.vp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw])\);' % re.escape(v_), b)
            if not m or m.group(1) not in var: return None
            return var[m.group(1)], [int(m.group(2)) - 3, int(m.group(3)), 'xyzw'.index(m.group(4))], [int(m.group(5)) - 3, int(m.group(6)), 'xyzw'.index(m.group(7))]
        fx, fy = fm(olast.get((mem, 'x'), '')), fm(olast.get((mem, 'y'), ''))
        if fx and fy and fx[0][0] == fy[0][0] and fx[0][0] in attr and fx[0][1] == 'x' and fy[0][1] == 'y' and min(fx[1][0], fx[2][0], fy[1][0], fy[2][0]) >= 0:
            aff[str(k)] = {'attr': attr[fx[0][0]], 'sx': fx[1], 'ox': fx[2], 'sy': fy[1], 'oy': fy[2]}
    return {'pos_attr': attr[m0[0]], 'pos_w': w, 'uv_from_attr': uv, 'uv_affine': aff}

def decode_tex(name):
    """fp_t_cb{slots}_{handle} / fp_t_tcb_{handle}：贴图槽、采样器槽（Ryujinx TextureHandle 编码；c[2] 句柄表） | fp_t_cb{slots}_{handle} / fp_t_tcb_{handle}: texture slot, sampler slot (Ryujinx TextureHandle encoding; c[2] handle table)"""
    m = re.match(r'[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', name)
    if not m: return None
    slots = int(m.group(1)) if m.group(1) else -1; h = int(m.group(2), 16)
    tb = (slots & 0xffff) if slots >= 0 else 2; sb = ((slots >> 16) - 1) if slots >= 0 and (slots >> 16) else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    def slot(word, base, cb): return (word * 4 - base) // 8 if cb == 2 and word * 4 >= base and (word * 4 - base) % 8 == 0 else None
    words = [o0] + ([o1] if ty else [])
    if re.match(r'[a-z]+_i', name):   # 图像 | image
        return next((slot(x, IMG_BASE, tb) for x in words if IMG_BASE <= x * 4 < TEX_BASE and slot(x, IMG_BASE, tb) is not None), None), None, ty
    t = next((slot(x, TEX_BASE, tb) for x in words if slot(x, TEX_BASE, tb) is not None and x * 4 < SAMP_BASE), None)
    s = next((slot(x, SAMP_BASE, sb) for x in words if x * 4 >= SAMP_BASE and slot(x, SAMP_BASE, sb) is not None), None)
    return t, s, ty

def entry_args(msl):
    """离散绑定（BIND=discrete）：入口函数签名里的逐槽资源 → [(类型, 名字, 槽类, 槽号)]。
    按 <> 深度感知切分参数，使 texture3d<float, access::write> 这类含逗号的模板类型不被逗号拆开。
    Per-slot binding (BIND=discrete): the resources in the entry signature → [(type, name, slot class, slot number)].
    Parameters are split with <> depth awareness so template types containing commas (e.g. texture3d<float, access::write>) are not torn apart by the comma."""
    m = re.search(r'^(?:vertex|fragment|kernel)\s+\S+\s+\w+\s*\((.*?)\)\s*\{', msl, re.S | re.M)
    if not m: return []
    parts, depth, cur = [], 0, ''
    for ch in m.group(1):
        if ch == '<': depth += 1
        elif ch == '>': depth -= 1
        if ch == ',' and depth == 0: parts.append(cur); cur = ''
        else: cur += ch
    parts.append(cur)
    out = []
    for p in parts:
        s = ' '.join(p.split())
        mm = re.match(r'^(.*?)\s+(\w+)\s*\[\[(buffer|texture|sampler)\((\d+)\)\]\]$', s)
        if mm: out.append((mm.group(1), mm.group(2), mm.group(3), int(mm.group(4))))
    return out

LIM_SAMPLER = 16   # MSL 的 [[sampler(n)]] 只有 0–15 十六个位置（语言上限，不是设备能力）| MSL has only sixteen sampler locations, 0-15 (a language limit, not a device capability)

def sampler_vars(src):
    """入口签名里的 sampler 参数：[(变量名, MSL 槽号)]，按槽号排序 | sampler parameters in the entry signature: [(variable name, MSL slot)], sorted by slot"""
    m = re.search(r'^(?:vertex|fragment|kernel).*?\((.*?)\)\s*\{', src, re.S | re.M)
    if not m: return []
    return sorted(((nm, int(n)) for nm, n in re.findall(r'sampler (\w+) \[\[sampler\((\d+)\)\]\]', m.group(1))), key=lambda x: x[1])

def samp_key(nm):
    """sampler 变量名 → 分组键（种类, NVN 槽）：种类不同（分离采样器 samp / 合并句柄 samp_tex）取值来源不同
    （S.samp[stage][r] 对 S.tex[stage][r] >> 20），槽号相同也不能并成一组
    sampler variable name → group key (kind, NVN slot): different kinds (separate sampler samp / combined handle samp_tex) read from different sources
    (S.samp[stage][r] vs S.tex[stage][r] >> 20), so even with the same slot number they must not be merged into one group"""
    d = decode_tex(nm[:-5]) if nm.endswith('Smplr') else None
    if not d: return None
    return ('samp', d[1]) if d[2] else ('samp_tex', d[0])

def samp_merge_by_reg(src):
    """逐槽档：采样器变量多于 16 个时按 NVN 槽合并成一组（返回 合并后的 MSL、组数或 None、诊断或 None）。
    同一 NVN 槽 r 下的所有 sampler 变量运行时取的是同一个句柄（S.tex[stage][r] >> 20 或 S.samp[stage][r] >> 20），
    任何一次绘制里配置都相同，所以并为一组永远安全——与游戏怎么改采样器配置无关，不需要真机 dump。
    合并是「变量级」的：把同组变量在函数体里的引用全换成组代表，再从签名里删掉多余声明并重编号（共用槽位会报
    cannot reserve 'sampler' resource location，改槽号没用）。全库 6927 个里 25 个需要合并（ps_models_p.bin）。
    Discrete mode: when there are more than 16 sampler variables, merge them by NVN slot (returns the merged MSL, the group count or None, and a diagnostic or None).
    All sampler variables of one NVN slot r read the same handle at run time (S.tex[stage][r] >> 20 or S.samp[stage][r] >> 20),
    so their config is identical in every draw and merging them is always safe - whatever the game does with sampler configs, and without needing a device dump.
    The merge is variable-level: references to the group's other variables in the body are replaced by the group's representative, the extra declarations are dropped from the signature and the rest renumbered (sharing a location reports
    cannot reserve 'sampler' resource location; changing the slot number alone does not help). 25 of the 6927 shaders in the whole library need it (ps_models_p.bin)."""
    sv = sampler_vars(src)
    if len(sv) <= LIM_SAMPLER: return src, None, None
    keys = {nm: samp_key(nm) for nm, _ in sv}
    if any(v is None for v in keys.values()): return src, None, f'{sum(1 for v in keys.values() if v is None)} 个采样器变量名解不出槽（不合并）'
    g = collections.OrderedDict()
    for nm, _slot in sv: g.setdefault(keys[nm], []).append(nm)
    groups = [v for v in g.values()]
    if len(groups) > LIM_SAMPLER: return src, None, f'按 NVN 槽合并后仍有 {len(groups)} 个采样器 > {LIM_SAMPLER}'
    m = re.search(r'^((?:vertex|fragment|kernel).*?\)\s*\{)', src, re.S | re.M)
    if not m: return src, None, '找不到入口函数签名'
    pre, sig, body = src[:m.start(1)], m.group(1), src[m.end(1):]
    for i, members in enumerate(groups):
        rep = members[0]
        for other in members[1:]:
            body = re.sub(r'\b' + re.escape(other) + r'\b', rep, body)
            sig = re.sub(r'\s*sampler ' + re.escape(other) + r' \[\[sampler\(\d+\)\]\],?', '', sig)
        sig = re.sub(r'(sampler ' + re.escape(rep) + r' \[\[sampler\()\d+(\)\]\])', lambda mm: mm.group(1) + str(i) + mm.group(2), sig)
    out = pre + sig + body
    resid = [d for gg in groups for d in gg[1:] if re.search(r'\b' + re.escape(d) + r'\b', out)]
    if resid: return src, None, f'合并后仍残留 {len(resid)} 个被并掉的变量名'
    if len(sampler_vars(out)) != len(groups): return src, None, '合并后的签名采样器数与组数不符'
    return out, len(groups), None

def reflect(key, stage, msl, res):
    """MSL 资源 → 回放反射：kind（同 nvn_refl.h）+ NVN 槽号 + 定位方式
    （ab 档：参数块内序号 arg_id，全部资源同在 buffer 槽 0；discrete 档：槽类 cls + 槽号 slot，各自独立）
    MSL resources → replay reflection: kind (same as nvn_refl.h) + NVN slot number + how to locate it
    (mode ab: index arg_id inside the argument buffer, all resources live in buffer slot 0; mode discrete: slot class cls + slot number slot, each independent)"""
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', msl, re.S)
    sb_cb = {b['binding']: b for b in res['sbufs']}
    binds = []
    if BIND == 'ab':   # 参数块：成员在 struct 里，序号即 [[id(n)]] | argument buffer: members live in a struct, the index is [[id(n)]]
        items = [(m.group(1).strip(), m.group(2), 'argbuf', 0, int(m.group(3)))
                 for m in re.finditer(r'^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)\]\];', body.group(1) if body else '', re.M)]
    else:              # 逐槽：资源在入口签名里，各自占 [[buffer(n)]] / [[texture(n)]] / [[sampler(n)]] | per-slot: resources are in the entry signature, each with its own [[...]]
        items = [(ty, nm, cls, slot, None) for ty, nm, cls, slot in entry_args(msl)]
    for ty, nm, cls, slot, aid in items:
        e = {'msl_name': nm, 'msl_type': ty, 'cls': cls, 'slot': slot}
        if aid is not None: e['arg_id'] = aid
        if nm == 'support_buffer': e.update(kind='support')
        elif re.fullmatch(r'[a-z]+_c(\d+)', nm):
            n = int(re.fullmatch(r'[a-z]+_c(\d+)', nm).group(1))
            e.update(kind='cbuf', reg=n - 3) if 3 <= n <= 16 else e.update(kind='c1' if n == 1 else 'drvcb', bank=n)
        elif re.fullmatch(r'[a-z]+_s(\d+)', nm):
            e.update(kind='sbuf', index=int(re.fullmatch(r'[a-z]+_s(\d+)', nm).group(1)))
        elif nm.endswith('Smplr'):   # 句柄类型 0 = 贴图与采样器合在同一句柄：采样器号取该贴图槽句柄的高位 | handle type 0 = texture and sampler combined in one handle: the sampler index is taken from the high bits of that texture slot's handle
            t, s, ty = decode_tex(nm[:-5]); e.update(kind='samp', reg=s) if ty else e.update(kind='samp_tex', reg=t)
        else:
            d = decode_tex(nm)
            if not d: e.update(kind='?')
            else: e.update(kind='img' if re.match(r'[a-z]+_i', nm) else ('tbuf' if 'texture_buffer' in ty else 'tex'), reg=d[0])
        binds.append(e)
    # 存储缓冲：SPIR-V 绑定号 → c0 偏移 → NVN 存储缓冲槽（c0 基址：顶点 0x110、计算 0x310、像素 0x510，每槽 0x10）
    # Storage buffers: SPIR-V binding number → c0 offset → NVN storage buffer slot (c0 base: vertex 0x110, compute 0x310, pixel 0x510, 0x10 per slot)
    base = {'vs': 0x110, 'cs': 0x310, 'ps': 0x510}.get(stage)   # 外壳 / 域阶段的存储缓冲基址未实测：遇到存储缓冲按「无槽」报 | storage buffer base for the hull / domain stages has not been measured: a storage buffer there is reported as 「无槽」(no slot)
    sb_sorted = sorted(res['sbufs'], key=lambda b: b['binding'])
    for e in binds:
        if e['kind'] == 'sbuf':
            b = sb_sorted[e['index']] if e['index'] < len(sb_sorted) else None
            off = b['cb_offset'] * 4 if b and b['cb_slot'] == 0 else None
            if stage == 'cs' and off is not None and 0x210 <= off < 0x310 and (off - 0x210) % 16 == 0:
                e.update(kind='ubo_ptr', reg=(off - 0x210) // 16)   # 计算阶段 uniform 槽 r：地址在 c0[0x210 + 0x10 r]，经全局内存读（cs_lut_combine_c 读 cb10[66] 核对） | compute-stage uniform slot r: address at c0[0x210 + 0x10 r], read through global memory (checked against cs_lut_combine_c reading cb10[66])
            else: e['reg'] = (off - base) // 16 if off is not None and base is not None and 0 <= off - base < 0x100 and (off - base) % 16 == 0 else None
            e['rw'] = bool(b and b['flags'] & 2)
    return binds

DRV_FILL = {0: [(0x110, 0x210), (0x310, 0x410), (0x510, 0x610)],   # c0：存储缓冲地址表（顶点 / 计算 / 像素，各 16 槽 × 0x10） | c0: storage buffer address tables (vertex / compute / pixel, 16 slots × 0x10 each)
            2: [(0x168, 0x568), (0x568, 0x968)]}                     # c2：贴图句柄表、采样器句柄表（各 128 槽 × 8） | c2: texture handle table, sampler handle table (128 slots × 8 each)

def drv_reads(src):
    """MSL 里对驱动段 c0 / c2 的读取：常量下标 → 字节区间；非常量下标记为动态。
    两档写法不同：ab 档经参数块指针解引用 (*spvDescriptorSet0.fp_cN)._m0[i]；discrete 档直接 fp_cN._m0[i]，故 \\) 可选。
    Reads of driver segments c0 / c2 in the MSL: constant index → byte range; non-constant indices are recorded as dynamic.
    The two modes write it differently: mode ab dereferences the argument buffer pointer (*spvDescriptorSet0.fp_cN)._m0[i]; mode discrete uses fp_cN._m0[i] directly - hence the \\) is optional."""
    out = {}
    for bank in (0, 2):
        offs, dyn = set(), False
        for m in re.finditer(r'fp_c%d\)?\._m0\[([^\]]+)\]' % bank, src):
            idx = m.group(1).rstrip('u')
            if idx.isdigit(): offs.add(int(idx) * 16)
            else: dyn = True
        if offs or dyn: out[str(bank)] = {'vec4_offsets': sorted(offs), 'dynamic': dyn}
    return out

def unfillable(dr):
    bad = []
    for bank, d in dr.items():
        if d['dynamic']: bad.append(f'c{bank} 动态下标')
        for o in d['vec4_offsets']:
            if not any(a <= o and o + 16 <= b or (a <= o < b) for a, b in DRV_FILL[int(bank)]): bad.append(f'c{bank}+{o:#x}')
    return bad

# SHFL wrapper：Ryujinx 翻译器为每个 SHFL 生成的辅助函数（参数 = 值, 异或量, 立即数, 投票输出, lane）
# SHFL wrapper: the helper the Ryujinx translator emits for each SHFL (args = value, xor amount, immediate, vote out, lane)
WRAP_RE = re.compile(r'int _(\d+)\(thread const int& _\d+, thread const int& _\d+, thread const int& _\d+, thread int& _\d+, thread uint& gl_SubgroupInvocationID\)\s*\{(.*?)\n\}', re.S)
# 调用点：紧邻的两行常量是异或量与立即数；立即数 (imm >> 8) & 31 是段掩码（28 = 4 lane 一组即 quad，0 = 整个 wave 32 lane）
# Call site: the two adjacent constants are the xor amount and the immediate; (imm >> 8) & 31 is the segment mask (28 = groups of 4 lanes, i.e. a quad; 0 = the whole 32-lane wave)
SHFL_CALL_RE = re.compile(r'int _\d+ = (\d+);\s*int _\d+ = (\d+);\s*int _\d+;\s*int _\d+ = _(\d+)\(_\d+, _\d+, _\d+, _\d+, gl_SubgroupInvocationID\);')

def shfl_wrappers(src):
    """SHFL wrapper 编号 → [定义匹配, 调用点常量集合 (异或量, 立即数)] | SHFL wrapper id → [definition match, set of call-site constants (xor, imm)]"""
    w = {}
    for m in WRAP_RE.finditer(src): w[m.group(1)] = [m, set()]
    for m in SHFL_CALL_RE.finditer(src):
        if m.group(3) in w: w[m.group(3)][1].add((int(m.group(1)), int(m.group(2))))
    return w

QUAD_DEF = """
#include <metal_quadgroup>
// 老显卡（Apple 5 家族，A12Z）没有 SIMD-group 指令，但有 quad 指令（Apple 工程师：A11 起支持 quad group，不支持 simd group）：
// 掩码 28 的 SHFL 只在 quad 内交换（lane^1 / lane^2），可以用 quad_shuffle 按原语义还原
// Older GPUs (Apple family 5, A12Z) have no SIMD-group instructions but do have quad ones (Apple engineer: quad groups are supported from A11 on, simd groups are not):
// an SHFL with mask 28 only exchanges inside the quad (lane^1 / lane^2), so it can be restored to its original semantics with quad_shuffle
template<typename T> inline T spvQuadShuffle(T value, ushort lane)
{
    return metal::quad_shuffle(value, ushort(lane & 3));
}
"""

def quad_shuffle_restore(src):
    """片元着色器里 quad 作用域的 SHFL（掩码 28）→ quad_shuffle，把「降级成本 lane」换回真交换。

    ps_models_p / ps_terrain / ps_glass_p 等主场景着色器每帧都在用这个交换（整库 180 个着色器、278 处调用），
    它取的是 quad 伙伴的像素值（阴影比较的参考深度等）。降级后取不到（等于取自己），阴影与光照因此算错——
    实测 iPad Pro 2020 上表现为火光附近的地面出现色斑。
    Quad-scoped SHFLs (mask 28) in fragment shaders → quad_shuffle, replacing the "downgrade to this lane" with a real exchange.
    The main scene shaders (ps_models_p / ps_terrain / ps_glass_p ...) use this exchange every frame (180 shaders, 278 call sites across the library);
    it fetches the quad partner's pixel value (the reference depth of the shadow compare among others). Downgraded it is unreachable (it degenerates to
    reading itself), so shadows and lighting come out wrong - measured on iPad Pro 2020 as colour patches on the ground next to firelight.
    """
    if BIND != 'discrete': return src
    w = shfl_wrappers(src)
    quad = [k for k, (m, consts) in w.items() if consts and all(xr < 4 and (imm >> 8) & 31 == 28 for xr, imm in consts)]
    if not quad: return src
    for k in quad:   # 从后往前改，前面的区间不受影响 | back to front so earlier spans stay valid
        m, body = w[k][0], w[k][0].group(2)
        body2 = body.replace('spvSubgroupShuffle(as_type<float>(', 'spvQuadShuffle(as_type<float>(')
        if body2 == body: continue
        src = src[:m.start(2)] + body2 + src[m.end(2):]
    inc = [m.end() for m in re.finditer(r'#include [^\n]*\n', src)]
    return src[:inc[-1]] + QUAD_DEF + src[inc[-1]:] if inc else QUAD_DEF + src

def warp_shuffle_threadgroup(src):
    """整 wave 蝶形归约（掩码 0 = 32 lane 全波，异或 1/2/4/8/16）→ threadgroup 内存 + 屏障。

    只有 cs_light_bin_depth_bounds_c 走这条：它对 32 个 lane 的 tile 深度做 min / max 归约，
    归约结果由 lane 0 用 threadgroup 原子写入。降级（取自己）会让每个 tile 的深度区间塌成一个采样值，
    深度分片的灯光剔除就跟着错——灯光会打到本该被挡住的像素上。
    该 kernel 线程组恰为 [32,1,1]（= 1 个 simdgroup），且归约段在统一控制流里（前面没有 return），
    所以可以用 threadgroup 内存 + 屏障按原语义实现。
    Whole-wave butterfly reductions (mask 0 = all 32 lanes, xor 1/2/4/8/16) → threadgroup memory + barriers.
    Only cs_light_bin_depth_bounds_c takes this path: it reduces the tile depth min / max over the 32 lanes and lane 0 writes
    the result with threadgroup atomics. The downgrade (reading itself) collapses each tile's depth range to a single sample,
    which breaks depth-sliced light culling - lights then land on pixels they should be occluded from.
    This kernel's threadgroup is exactly [32,1,1] (= one simdgroup) and the reduction sits in uniform control flow (no return before it),
    so it can be implemented with threadgroup memory + barriers at full original semantics.
    """
    if BIND != 'discrete': return src
    w = shfl_wrappers(src)
    # 判定：helper 体内有一次对 spvSubgroupShuffle 的实调用。不能按调用点常量判定——该着色器的异或量 /
    # lane 是运行期现算的（as_type<int>(...) ^ mask），SHFL_CALL_RE 匹配不到，ids 为空，函数会静默原样
    # 返回，从外面看和"生效了但不够"一模一样（10-18 实测踩过：装机库没带上这个还原，画面却查不出原因）。
    # Detection: the helper body holds one real call to spvSubgroupShuffle. Detecting by call-site constants
    # does not work - this shader's xor amount / lane is computed at run time (as_type<int>(...) ^ mask), so
    # SHFL_CALL_RE never matches, ids comes out empty and the function silently returns unchanged, which from
    # the outside looks exactly like "it applied but was not enough" (hit for real on 10-18: the installed
    # library shipped without this restoration and the picture gave no clue why).
    ids = [k for k, (m, consts) in w.items() if len(re.findall(r'spvSubgroupShuffle\(', m.group(2))) == 1]
    if len(ids) != 1: return src
    m, body = w[ids[0]][0], w[ids[0]][0].group(2)
    mm = re.search(r'( *)float (_\d+) = spvSubgroupShuffle\(as_type<float>\((_\d+)\), uint\((_\d+)\)\);', body)
    if not mm: return src
    ind, rv, vv, tv = mm.group(1), mm.group(2), mm.group(3), mm.group(4)
    body2 = body[:mm.start()] + (
        f'{ind}threadgroup_barrier(mem_flags::mem_threadgroup);   /* 老显卡无 SIMD-group：整 wave 交换改走 threadgroup 内存 */\n'
        f'{ind}spvWarpTmp[gl_SubgroupInvocationID & 31u] = as_type<uint>({vv});\n'
        f'{ind}threadgroup_barrier(mem_flags::mem_threadgroup);\n'
        f'{ind}float {rv} = as_type<float>(spvWarpTmp[uint({tv}) & 31u]);') + body[mm.end():]
    seg = src[m.start():m.end()]
    seg = seg.replace(body, body2).replace('thread uint& gl_SubgroupInvocationID)', 'thread uint& gl_SubgroupInvocationID, threadgroup uint* spvWarpTmp)')
    src = src[:m.start()] + seg + src[m.end():]
    src = re.sub(r'(_%s\([^;]*?), gl_SubgroupInvocationID\)' % ids[0], r'\1, gl_SubgroupInvocationID, spvWarpTmp)', src)
    km = re.search(r'^kernel void \w+\(.*?\)\s*\n\{\n', src, re.S | re.M)
    if not km: return src
    return src[:km.end()] + '    threadgroup uint spvWarpTmp[32];\n' + src[km.end():]

def grunge_decal_discard(src):
    """贴花 pass 三面测试全没过时整片丢弃（修掉覆盖度公式退化成的小正数）。

    ps_grunge_p 从 texture_buffer 取三组平面系数，逐片测试贴花盒，命中才往颜色 0 累积。
    覆盖度项是 max(0, 1e6/(1e5 - z^3))，z 是世界坐标分量：1e5 是按「z 是零点几」估的量级，
    设计意图是 z^3 << 1e5、覆盖度恒等于 10 的固定增益。实测 z 是数百，z^3 到 1e8~1e9 把 1e5
    彻底淹没，公式退化成 -1e6/z^3 ≈ 0.004 的小正数 —— 于是「一片都没命中」的像素也被写上一层
    极淡的暖色，叠在起伏地面 / 水洼边缘就是可见的红斑（iPad Pro 2020 实测，Mac 上同区间但更淡）。
    相对灵敏度 d(cov)/cov = -3·dz/z，正好对上三个特征：世界锚定、跨分辨率不变、只在地形起伏处出现。

    只掐「未命中」这一支：三面测试全没过就 discard_fragment()，抹掉退化的小正数。
    实测 iPad 红斑消失，其余场景与原版差 1.4/255。这是止血不是恢复 —— 命中的贴花覆盖度仍是
    那个退化小值，本该可见的贴花依然偏淡；要恢复得改公式，那是另一件事。
    与 BIND 无关：公式退化在所有绑定档都成立，所以不加 discrete 判定。

    Decal pass: discard the whole fragment when no decal plane passes the three-plane test.
    ps_grunge_p fetches three plane sets from a texture_buffer and tests each decal box, accumulating
    into colour 0 only on a hit. Its coverage term is max(0, 1e6/(1e5 - z^3)) where z is a world-space
    component: 1e5 assumes z is sub-unit, so that z^3 << 1e5 and the coverage is a constant gain of 10.
    In practice z is in the hundreds, z^3 reaches 1e8-1e9 and swamps 1e5, so the expression degenerates
    to -1e6/z^3 ~= 0.004 - a small positive number. Pixels where no decal was hit therefore still get a
    very faint warm layer, which shows up as red blotches on uneven ground / puddle edges (measured on
    iPad Pro 2020; the same range exists on Mac but stays below the visible threshold).
    d(cov)/cov = -3.dz/z explains all three traits: world-anchored, resolution independent, only on
    undulating terrain.
    Only the "no hit" branch is cut: if every three-plane test fails, discard_fragment() drops the
    degenerate value. Measured: blotches gone on iPad, other scenes within 1.4/255 of stock.
    This is a stop-bleed, not a restoration - a hit decal still carries the degenerate coverage and
    stays too faint; restoring that means fixing the formula, a separate job.
    BIND independent: the degeneracy holds in every binding tier, so there is no discrete-only guard.
    """
    if 'dmDecalHit' in src: return src   # 幂等：已改过就跳过 | idempotent
    head = '    main0_out out = {};\n'
    if src.count(head) != 1:
        raise RuntimeError('grunge_decal_discard：找不到唯一的 `main0_out out = {};`，MSL 形态已变，规则需重写')
    # 三面测试的形态是 `abs(d) <= 0.5` 且结果再被 && 串起来；链终点是那个被 `if (...)` 引用的 bool。
    # 只认左操作数取自 abs 测试的，否则会误抓深度比较（本着色器里 `_626 = _624 && _618` 就是深度区间，
    # 拿它做 discard 条件会整片误杀）。
    absb = {m.group(3) for m in re.finditer(r'float (_\d+) = abs\((_\d+)\);\n *bool (_\d+) = \1 <= 0\.5;', src)}
    finals = [m for m in re.finditer(r'( *)bool (_\d+) = (_\d+) && (_\d+);\n', src)
              if m.group(3) in absb and re.search(r'if \(%s\)' % m.group(2), src)]
    if not finals:
        raise RuntimeError('grunge_decal_discard：找不到被 if 引用的三面测试链终点，MSL 形态已变，规则需重写')
    for m in reversed(finals):   # 从后往前改，前面的位置不受影响 | back to front so earlier offsets stay valid
        src = src[:m.end()] + m.group(1) + 'dmDecalHit = fast::max(dmDecalHit, %s ? 1.0 : 0.0);\n' % m.group(2) + src[m.end():]
    src = src.replace(head, head + '    float dmDecalHit = 0.0;\n', 1)
    out0 = re.search(r'( *)out\.\w+\.x = ', src)
    if not out0:
        raise RuntimeError('grunge_decal_discard：找不到颜色输出 `out.*.x =`，MSL 形态已变，规则需重写')
    return src[:out0.start()] + out0.group(1) + 'if (dmDecalHit < 0.5) { discard_fragment(); }\n' + src[out0.start():]

# 只有这几个着色器名走上面两条规则（规则按名挂，指纹也按名算，所以别的着色器不会因此重出）
# Only these shader names take the two rules above (rules are attached by name and the fingerprint is taken by name, so no other shader is rebuilt because of them)
QUAD_SHFL_NAMES = frozenset(('ps_models_p.bin', 'ps_terrain.bin', 'ps_glass_p.bin', 'ps_foliage_p.bin', 'ps_grunge_p.bin',
                             'ps_detail_normal_p.bin', 'ps_alpha_threshold_p.bin', 'ps_models_blend_p.bin', 'ps_grass_lighting_p.bin'))
WARP_SHFL_NAMES = frozenset(('cs_light_bin_depth_bounds_c.bin',))
GRUNGE_DECAL_NAMES = frozenset(('ps_grunge_p.bin',))

RULES = [(quad_shuffle_restore, lambda n: n in QUAD_SHFL_NAMES), (warp_shuffle_threadgroup, lambda n: n in WARP_SHFL_NAMES), (grunge_decal_discard, lambda n: n in GRUNGE_DECAL_NAMES)]   # 按着色器名挂的 MSL 改写规则（函数, 适用判定）；原有的暗部重映射 toe 已删（回到 Switch 原版亮度） | MSL rewrite rules attached by shader name (function, applicability test); the former dark-range remap toe has been removed (back to the original Switch brightness)
# 第一段代码指纹不包含 dmsh_fetch.py；可读 tile 变体单独按生成 MSL 做第二段缓存。
# The first-stage code fingerprint does not include dmsh_fetch.py; readable-tile variants get a separate second-stage cache keyed on the generated MSL.
CODE_FP = hashlib.sha256(source_without_rule_bodies().encode() + b''.join(b'\0' + open(m.__file__, 'rb').read() for m in (dmsh_chain,) + ((ios_metal,) if IOSOUT else ()))).hexdigest()
TOOL_FP = tool_fingerprint().encode()

def msl_downgrade_subgroup(src):
    """逐槽绑定档（老显卡）专用：把 SIMD-group 操作降级成「只有本 lane」的形式。

    simd_shuffle / simd_ballot 是 Metal 3 起的 SIMD-group 指令，要 A14 / Apple GPU family 7 以上才有；
    A12Z（Apple 5 家族）上没有对应硬件指令。**编译期查不出来**：xcrun metal 编出的 AIR 里只是一个
    air.simd_shuffle 调用，要等到真机 newRenderPipelineState 时按 GPU 做 lower 才炸
    （"Encountered unlowered function call to air.simd_shuffle.f32"，实测 iPad Pro 2020 每帧报几万次，画面全黑）。

    降级后语义不再是真的跨 lane 读值（读不到本 lane 之外的值），但管线能建起来、画面能出；
    用到 subgroup 的那几处特效可能与原版有差。参数块档（Metal 3 设备）不走这里，保持原样。
    Per-slot binding mode (older GPUs) only: downgrade SIMD-group operations to "this lane alone".
    simd_shuffle / simd_ballot are SIMD-group instructions from Metal 3 on, requiring A14 / Apple GPU family 7 or later;
    the A12Z (Apple family 5) has no matching hardware instruction. **This cannot be caught at compile time**: the AIR
    xcrun metal emits only holds an air.simd_shuffle call, and it blows up when the real device lowers it for the GPU
    inside newRenderPipelineState ("Encountered unlowered function call to air.simd_shuffle.f32"; measured tens of
    thousands of times per frame on iPad Pro 2020, with a fully black picture).
    After the downgrade the semantics are no longer a true cross-lane read (values outside this lane are unreachable),
    but pipelines build and the picture shows up; the few effects that use subgroups may differ from the original.
    The argument-buffer mode (Metal 3 devices) does not come through here and keeps the original code.
    """
    s = src
    # spirv-cross 生成的 shuffle helper：跨 lane 读不到，就退回本 lane 的值
    s = re.sub(r'return simd_shuffle\(\s*([^;]+?),\s*[^;]+?\);', r'return \1;', s)
    s = re.sub(r'return !!simd_shuffle\(\(ushort\)value,\s*lane\);', r'return value;', s)
    s = re.sub(r'return \(vec<bool, N>\)simd_shuffle\(\(vec<ushort, N>\)value,\s*lane\);', r'return value;', s)
    # ballot：按只有本 lane 投票（uint4 的低位 = 本 lane 的谓词）
    s = re.sub(r'simd_vote vote = simd_ballot\(value\);',
               r'uint2 vote = uint2(value ? 1u : 0u, 0u);   /* 降级：老显卡无 SIMD-group，按只有本 lane 投票 */', s)
    s = re.sub(r'return uint4\(as_type<uint2>\(\(simd_vote::vote_t\)vote\), 0, 0\);', r'return uint4(vote, 0, 0);', s)
    return s

def finish(key, w, j, c1):
    name, k = loc[key]
    res = json.load(open(os.path.join(w, key + '.res.json')))
    msl = os.path.join(w, key + '.metal'); air = msl[:-6] + '.air'; lib = os.path.join(out, key + '_b.metallib')
    r = subprocess.run(['spirv-cross', *XC, os.path.join(w, key + '.spv'), '--output', msl], capture_output=True, text=True)
    if r.returncode: return 'MSL 失败', r.stderr.strip()[:160], None
    src = open(msl).read()
    for rule, applies in RULES:   # 按着色器名挂的改写（老显卡的两条 subgroup 还原规则） | rewrites attached by shader name (the two subgroup restoration rules for older GPUs)
        if applies(name): src = rule(src)
    notes = []
    if BIND == 'discrete':   # 老显卡上有两处能力缺口要补：SIMD-group 指令（降级）与采样器位置只有 16 个（按 NVN 槽合并）
        src2 = msl_downgrade_subgroup(src)   # Two gaps to fill on older GPUs: SIMD-group instructions (downgrade) and only 16 sampler locations (merge by NVN slot)
        if src2 != src:
            open(msl, 'w').write(src2); src = src2
            if 'quad_shuffle(' in src2: notes.append('逐槽档：quad 内交换已还原为 quad_shuffle（原语义）')
            elif 'spvWarpTmp' in src2: notes.append('逐槽档：整 wave 交换已改用 threadgroup 内存 + 屏障（原语义）')
            else: notes.append('逐槽档：SIMD-group 操作已降级（本 lane）' if 'simd_' not in src2 else '逐槽档：SIMD-group 降级后仍有残留')
        src2, ngroups, why = samp_merge_by_reg(src)
        if why: return '采样器合并失败', why[:160], None
        if ngroups: open(msl, 'w').write(src2); src = src2; notes.append(f'逐槽档：{ngroups} 组采样器（按 NVN 槽合并）')
    try:
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', msl, '-o', air], check=True)
        tmp_lib = lib + '.tmp-' + str(os.getpid())
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', air, '-o', tmp_lib], check=True)
        os.replace(tmp_lib, lib)
    except subprocess.CalledProcessError as e:
        return 'metal 失败', str(e)[:160], None
    clone(msl, os.path.join(out, key + '_b.metal'))
    if IOSOUT:   # iOS：同一份 MSL 走 iOS 的唯一编译口径（dm/ios/tools/ios_metal.py）出 iphoneos metallib | iOS: the same MSL goes through iOS's single compile convention (dm/ios/tools/ios_metal.py) to produce an iphoneos metallib
        ilib = os.path.join(IOSOUT, key + '_b.metallib')
        try:
            ios_metal.compile(msl, msl[:-6] + '.ios.air', ilib + '.tmp')
            os.replace(ilib + '.tmp', ilib)
        except subprocess.CalledProcessError as e: return 'iOS metal 失败', str(e)[:160], None
    entry = re.search(r'(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(', src)
    dr = drv_reads(src)
    refl = {'key': key + '_b', 'route': 'B', 'chain': CHAIN, 'bind_model': BIND, 'dm_ctl': key, 'source': f'{name}#{k}', 'pc_paired': pc_of[key], 'stage': j['stage'],
            'entry': entry.group(2) if entry else 'main0', 'msl_entry_kind': entry.group(1) if entry else None,
            'threadgroup_size': j.get('local'), 'c1': c1.hex(), 'notes': res['notes'] + notes + (['无配对 PC 反射，查询按缺省'] if j.get('pc_missing') else []), 'drv_reads': dr, 'drv_unfillable': unfillable(dr),
            'bindings': reflect(key, j['stage'], src, res),
            'vertex_inputs': [{'location': int(m.group(2)), 'msl_type': m.group(1)} for m in re.finditer(r'(\w+)\s+\w+\s*\[\[attribute\((\d+)\)\]\]', src)],
            'raster': raster_facts(j['stage'], src)}
    write_refl(key, refl)
    fetch_finish(key, src, refl)
    return 'ok', '', refl

keys = sorted(want) if want else sorted(loc)
NJ = int(os.environ.get('J', '8'))
os.makedirs(out, exist_ok=True)
w = tempfile.mkdtemp(prefix='dmsh_', dir=os.environ.get('WORK'))
packs, jobs, c1s = {}, {}, {}
digests, rule_fps, cache_reasons, cache_hits = {}, {}, {}, set()
for key in keys:
    name, k = loc[key]
    if name.startswith('main.rodata@'):
        if 'main' not in packs:
            sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../verify'))
            try: import nsoimg
            except ImportError as e: sys.exit(f'【红】解 main NSO 要 lz4（{e}）：用 $DM_ROOT/build/cache/dm/venv/bin/python 跑')
            packs['main'] = nsoimg.load(MAIN)
        rec = main_rec(packs['main'], k)
        if hashlib.sha256(rec[rec.find(bytes.fromhex('34127698')):][:0x880]).hexdigest()[:16] != key: sys.exit(f'【红】{name} 控制段哈希与映射表键 {key} 不符（MAIN={MAIN}）')
    else:
        if name not in packs: packs[name] = records(open(os.path.join(dm_dir, name), 'rb').read())
        rec = packs[name][k]
    digests[key] = shader_key(key, name, rec)
    rule_fps[key] = hashlib.sha256(shader_rule_source(name)).hexdigest()
    hit, cache_reasons[key] = cache_lookup(key, digests[key], rule_fps[key], CODE_FP)
    if hit: cache_hits.add(key)
    else: jobs[key], c1s[key] = job_for(key, name, rec, w)
misses = [key for key in keys if key not in cache_hits]
chunks = [misses[i::NJ] for i in range(NJ)]
def xl(ch):
    jp = os.path.join(w, f'job_{abs(hash(ch[0]))}.json'); json.dump([jobs[k] for k in ch], open(jp, 'w'))
    return subprocess.run([XLATE, jp, w], capture_output=True, text=True)
with cf.ThreadPoolExecutor(NJ) as ex: xr = list(ex.map(xl, [c for c in chunks if c]))
xerr = {}
for r in xr:
    for ln in r.stderr.splitlines():
        m = re.match(r'【红】(\w+)：(.*)', ln)
        if m: xerr[m.group(1)] = m.group(2)[:160]
def fin(key):
    if key in cache_hits:
        refl = json.load(open(os.path.join(out, key + '_b.json')))
        src = open(os.path.join(out, key + '_b.metal')).read() if refl['stage'] == 'ps' else ''
        return key, 'ok', '', fetch_finish(key, src, refl)
    if key in xerr or not os.path.exists(os.path.join(w, key + '.spv')): return key, '翻译失败', xerr.get(key, '无 spv'), None
    result = (key,) + finish(key, w, jobs[key], c1s[key])
    if result[1] == 'ok': publish_cache(key, digests[key], rule_fps[key], CODE_FP)
    return result
with cf.ThreadPoolExecutor(NJ) as ex: res = list(ex.map(fin, keys))
shutil.rmtree(w, ignore_errors=True)
st = collections.Counter(r[1] for r in res)
print(f'着色器 {len(res)}：成功 {st["ok"]}，' + '，'.join(f'{k} {v}' for k, v in st.items() if k != 'ok'))
print(f'前置缓存：命中 {PRE_STATS["hit"]}，重出 {PRE_STATS["miss"]}')
for reason in ('新键', '规则变', '代码指纹变化：全部重出'):
    if PRE_STATS[reason]: print(f'  {reason} {PRE_STATS[reason]}')
print(f'变体缓存：生成 {BF_STATS["generated"]}，命中 {BF_STATS["hit"]}，重出 {BF_STATS["miss"]}，编译 {BF_STATS["compile"]}')
for lvl in [k for k in st if k != 'ok']:
    for e, n in collections.Counter(r[2] for r in res if r[1] == lvl).most_common(10): print(f'  [{lvl}] {n:5d}  {e}')
ok = [r[3] for r in res if r[1] == 'ok']
rd = [x for x in ok if x['drv_reads']]
print(f'运行时读驱动段的着色器 {len(rd)}（c0 {sum("0" in x["drv_reads"] for x in rd)}，c2 {sum("2" in x["drv_reads"] for x in rd)}）；读到回放层填不了的 {sum(bool(x["drv_unfillable"]) for x in ok)}')
for e, n in collections.Counter(u for x in ok for u in x['drv_unfillable']).most_common(15): print(f'  填不了 {n:5d}  {e}')
SLOTK = {'cbuf', 'samp', 'samp_tex', 'tex', 'img', 'tbuf', 'sbuf', 'ubo_ptr'}
noreg = [x for x in ok if any(b['kind'] in SLOTK and b.get('reg') is None for b in x['bindings'])]
unk = [x for x in ok if any(b['kind'] == '?' for b in x['bindings'])]
print(f'绑定没解出槽号的着色器 {len(noreg)}，种类认不出的 {len(unk)}（运行时加载会报红）')
for e, n in collections.Counter(f"{b['kind']} {b['msl_name'][:28]}" for x in noreg + unk for b in x['bindings'] if b['kind'] == '?' or (b['kind'] in SLOTK and b.get('reg') is None)).most_common(8): print(f'  {n:5d}  {e}')
nt = collections.Counter(nn.split('（')[0] for x in ok for nn in x['notes'])
for e, n in nt.most_common(8): print(f'  查询无 PC 作答 {n:5d}  {e}')
json.dump([{'key': r[0], 'st': r[1], 'err': r[2]} for r in res], open(os.path.join(out, '_b_build.json'), 'w'), ensure_ascii=False)
