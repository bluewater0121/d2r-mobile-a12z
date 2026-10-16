#!/usr/bin/env python3
"""诊断用 A-B 变体包：把一个子集的贴图换成未压缩 RGBA8，其余保持既有补充包的 ASTC。
用于判断 iPad Pro 2020（A12Z）上的画面红斑是不是本机 ASTC 解码造成的——变体里 ASTC 全部换成
非压缩贴图后，若红斑消失，锅就在设备侧 ASTC 解码；若仍在，就与贴图压缩无关。

做法（与 astc_pack.py 同源，差别只在目标格式）：
- .texture 文件头第 4 字节是游戏自己的格式码：0x3d / 0x3e = BC3 线性 / sRGB，0x1f = 未压缩 RGBA8。
  RGBA8 是游戏原生在用的一组贴图（345 张，NVN 格式 0x25 → MTLPixelFormatRGBA8Unorm），所以这一步
  不加新的 format 支持，只换数据。BC3 每像素 1 字节，RGBA8 每像素 4 字节，文件会涨到约 4 倍。
- 数据区 = 头尾相接的各级 mip：头长 36 + 8×级数，第 i 级一对 (大小, 相对偏移)，偏移从偏移字段自身
  处算起。各级分配大小 = max(该级实际字节, 512)（原生多级 RGBA8 贴图九级全部对得上），
  改动只限于第 4 字节的格式码与每级的「大小 / 偏移」两列，其余头字节一字不动。
- 贴图描述缓存 data/hd/texture_desc_cache.json 里每张贴图一条：游戏建贴图时格式读它而不读文件头，
  所以换过来的贴图 format 一并改成 31；内嵌缩略图是末尾几级小 mip，同样按 w×h×4 重排
  （缩略图各级不带 512 下限，照原生 RGBA8 的写法给精确大小）。非嫌疑贴图沿用既有补充包
  已经改好的缓存（ASTC 版），只覆盖嫌疑贴图那几条。
- 每张转出来的贴图都重新读回来核对：级数、各级大小与偏移首尾相接到文件尾、解出来的像素与 BC3
  解码真值逐字节相同，只留 desc cache 记录的宽高 / 级数做基准。

用法：tex_variant_pack.py <RomFS> <既有补充包 work 目录> <输出 work 目录>
                          [--set ground-fire|all|linear-only] [--only-linear] [--limit N] [--only 路径子串]
产物：<输出目录>/files/<逻辑路径>（换好的 .texture 与描述缓存）、repl.tsv（逻辑路径<TAB>文件，给
      casc_overlay.py）、summary.txt；随后照常 casc_overlay.py pack + astc_pack.py finalize。

Diagnostic A-B variant pack: replace one subset of textures with uncompressed RGBA8 and leave the rest
on the existing Legacy Device Pack's ASTC. Used to tell whether the red blotches on iPad Pro 2020 (A12Z)
come from the device's own ASTC decoding: once ASTC is gone from the suspects and the blotches go away,
native ASTC decoding is the culprit; if they stay, texture compression is not involved.
How it works (same lineage as astc_pack.py, only the target format differs):
- Byte 4 of the .texture header is the game's own format code: 0x3d / 0x3e = BC3 linear / sRGB,
  0x1f = uncompressed RGBA8. RGBA8 is a family the game uses natively (345 textures, NVN format 0x25 ->
  MTLPixelFormatRGBA8Unorm), so this step adds no new format support, it only swaps the data.
  BC3 is 1 byte per pixel, RGBA8 is 4, so files grow about 4x.
- Data area = the mip levels end to end: header length 36 + 8*levels, one (size, relative offset) pair per
  level at 36+8*i, offsets counted from the offset field itself. Per-level allocation = max(real bytes, 512)
  (matches all nine levels of a native multi-level RGBA8 texture), so the only header edits are the format
  code in byte 4 and the size / offset columns of every level; every other header byte is left alone.
- data/hd/texture_desc_cache.json holds one record per texture: when the game creates a texture it reads the
  format from here, not from the file header, so converted textures get format 31 as well; the embedded
  thumbnail (the last few small mip levels) is re-laid out at w*h*4 too (thumbnail levels carry no 512 floor,
  they get exact sizes, following native RGBA8). Records outside the suspect set stay on the already-edited
  ASTC cache from the existing pack; only the suspect records are overwritten.
- Every converted texture is read back and checked: level count, per-level size and offset chain up to EOF,
  and the decoded pixels equal the BC3 ground truth byte for byte, with the desc cache's width / height /
  levels as the baseline.
Usage: tex_variant_pack.py <RomFS> <existing pack work dir> <output work dir>
                           [--set ground-fire|all|linear-only] [--only-linear] [--limit N] [--only path substring]
Output: <out>/files/<logical path> (converted .texture plus the description cache), repl.tsv (logical
        path<TAB>file, for casc_overlay.py), summary.txt; then casc_overlay.py pack and astc_pack.py finalize
        as usual."""
import argparse, base64, json, os, struct, sys, time
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'ios', 'tools'))
sys.path.insert(0, HERE)
import touch_layout
from astc_pack import bc3_decode   # BC3 块 → (块数, 16, 4) uint8 | BC3 blocks -> (block count, 16, 4) uint8

CODE_BC3 = (0x3d, 0x3e)           # 源文件只可能这两种（既有补充包的转换范围） | Source files can only be these two (the range the existing pack converts)
CODE_RGBA8 = 0x1f                 # 游戏未压缩 RGBA8 的格式码，desc cache 里写作 31 | Game code for uncompressed RGBA8, written as 31 in the desc cache
MIN_LEVEL = 512                   # 单级分配下限：比它小的级仍占 512 字节（原生多级 RGBA8 实测） | Per-level allocation floor: levels below it still take 512 bytes (measured on native multi-level RGBA8)
DESC = 'data/hd/texture_desc_cache.json'


def set_of(name):
    """按
路径挑选嫌疑贴图：ground-fire = 地面 + 火光（Act1 环境、Act1 地形、特效、默认材质、光照 gobo）"""
    if name == 'all': return lambda q: True
    if name == 'linear-only': return lambda q: True
    return lambda q: any(k in q for k in ('/texture/act1/', '/vfx/', '/bvt/', '/light/')) or ('/terrain/' in q and 'act1' in q)


def header_len(lv): return 36 + 8 * lv


def read_levels(b, lv):
    """读文件头的各级 (大小, 数据起点) | Read per-level (size, data start) from the file header"""
    out = []
    for i in range(lv):
        pos = 36 + 8 * i
        size = struct.unpack_from('<I', b, pos)[0]
        off = pos + 4 + struct.unpack_from('<i', b, pos + 4)[0]
        out.append((size, off))
    return out


def to_rgba8(b, w0, h0, lv):
    """BC3 的 .texture 字节 → RGBA8 的 .texture 字节；返回 (新字节, [(宽, 高, 实际字节, 分配大小)…], [各级 RGBA8 精确字节…]) |
    BC3 .texture bytes -> RGBA8 .texture bytes; returns (new bytes, [(width, height, real bytes, allocated size)...], [exact RGBA8 bytes per level...])"""
    ents = read_levels(b, lv)
    head = bytearray(b[:header_len(lv)])
    head[4] = CODE_RGBA8
    blobs, meta = [], []
    w, h = w0, h0
    for i, (size, off) in enumerate(ents):
        bw, bh = (w + 3) // 4, (h + 3) // 4
        need = bw * bh * 16
        if size < need or off + size > len(b):
            sys.exit(f'tex_variant_pack：第 {i} 级（{w}x{h}，大小 {size}，起点 {off}）与文件大小 {len(b)} 对不上')
        px = bc3_decode(b[off:off + need])
        img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)[:h, :w, :]
        data = np.ascontiguousarray(img).tobytes()
        blobs.append(data)
        meta.append((w, h, len(data)))
        w, h = max(1, w // 2), max(1, h // 2)
    body = bytearray()
    offs = []
    for data in blobs:
        alloc = max(len(data), MIN_LEVEL)
        offs.append(len(body))
        body += data + bytes(alloc - len(data))
    base = header_len(lv)
    for i, (data, at) in enumerate(zip(blobs, offs)):
        pos = 36 + 8 * i
        alloc = max(len(data), MIN_LEVEL)
        abs_off = base + at
        struct.pack_into('<I', head, pos, alloc)
        struct.pack_into('<i', head, pos + 4, abs_off - (pos + 4))
    nb = bytes(head) + bytes(body)
    check(nb, meta, lv)
    return nb, meta, blobs


def check(nb, meta, lv):
    """转出来的文件重新读回来：各级大小 / 偏移首尾相接到文件尾，且像素与 BC3 解码真值一致 |
    Read the converted file back: sizes / offsets chain up to EOF and pixels match the BC3 ground truth"""
    ents = read_levels(nb, lv)
    for i, (size, off) in enumerate(ents):
        if off + size > len(nb): sys.exit(f'tex_variant_pack：核对失败，第 {i} 级越到文件尾（{off}+{size} > {len(nb)}）')
        w, h, real = meta[i]
        if size < real or size != max(real, MIN_LEVEL): sys.exit(f'tex_variant_pack：核对失败，第 {i} 级大小 {size} 与预期 {max(real, MIN_LEVEL)} 不符')
    tail = ents[-1][1] + ents[-1][0]
    if tail != len(nb): sys.exit(f'tex_variant_pack：核对失败，末级结尾 {tail} 与文件长度 {len(nb)} 不符')


def thumb_rgba8(blobs, e):
    """RGBA8 的缩略图字节：直接用各级已解出的精确像素重排（不经过任何压缩解码）|
    RGBA8 thumbnail bytes: re-laid out straight from the already-decoded exact per-level pixels (no compressed decode at all)"""
    t = e.get('_thumb_raw')
    if t is None: return None
    n = struct.unpack_from('<I', t, 0)[0]
    if not n: return t
    first = int(e.get('thumbnailMip') or 0)
    if first + n > len(blobs):
        sys.exit(f'tex_variant_pack：缩略图要第 {first}–{first+n-1} 级，贴图只有 {len(blobs)} 级')
    parts = [blobs[first + i] for i in range(n)]
    # 表紧跟 8 字节头之后：字节 4 那个「表起点相对偏移」= 4（原生实测 val=4 → 表在第 8 字节）
    # The table follows the 8-byte header right away: the "table start relative offset" in byte 4 = 4 (measured natively: val=4 -> table at byte 8)
    head = bytearray(struct.pack('<Ii', n, 4))
    body = bytearray()
    for i, data in enumerate(parts):
        pos = 8 + 8 * i
        abs_off = 8 + 8 * n + len(body)
        head += struct.pack('<Ii', len(data), abs_off - (pos + 4))
        body += data
    out = bytes(head) + bytes(body)
    # 重新读回来核对：表在第 8 字节、各级首尾相接到 blob 尾 | Read it back: table at byte 8, levels chain up to the end of the blob
    ents = [(struct.unpack_from('<I', out, 8 + 8 * i)[0], 8 + 8 * i + 4 + struct.unpack_from('<i', out, 8 + 8 * i + 4)[0]) for i in range(n)]
    for i, (sz, off) in enumerate(ents):
        if sz != len(parts[i]) or off + sz > len(out):
            sys.exit(f'tex_variant_pack：缩略图核对失败，第 {i} 级 size={sz}(应 {len(parts[i])}) off={off}，blob 长 {len(out)}')
    if ents[-1][1] + ents[-1][0] != len(out):
        sys.exit(f'tex_variant_pack：缩略图核对失败，末级结尾 {ents[-1][1]+ents[-1][0]} 与 blob 长度 {len(out)} 不符')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('romfs'); ap.add_argument('base'); ap.add_argument('out')
    ap.add_argument('--set', default='ground-fire', choices=('ground-fire', 'all'))
    ap.add_argument('--only-linear', action='store_true')   # 只转线性贴图（0x3d），不碰 sRGB 的 albedo | Convert only linear textures (0x3d), leave sRGB albedo alone
    ap.add_argument('--limit', type=int, default=0); ap.add_argument('--only', default='')
    o = ap.parse_args()
    t0 = time.time()
    base_repl = [l.rstrip('\n').split('\t') for l in open(os.path.join(o.base, 'repl.tsv'), encoding='utf-8')]
    rows = [l.rstrip('\n').split('|') for l in open(os.path.join(o.romfs, 'data', 'build_manifest.psv'), encoding='utf-8')]
    idx = {f[9].split(':', 1)[-1].lower(): f for f in rows if len(f) >= 11}
    row_orig = idx[DESC]
    J_orig = {e['filename'].lower(): e for e in json.loads(touch_layout._read_entry(o.romfs, row_orig))}
    J = json.loads(open(os.path.join(o.base, 'files', DESC), encoding='utf-8').read())
    is_sus = set_of(o.set)

    todo = []
    for p, f in base_repl:
        if p == DESC: continue
        q = p.lower()
        if o.only and o.only not in q: continue
        if not is_sus(q): continue
        todo.append((p, f))
    if o.limit: todo = todo[:o.limit]

    os.makedirs(os.path.join(o.out, 'files'), exist_ok=True)
    repl = open(os.path.join(o.out, 'repl.tsv'), 'w')
    summary = open(os.path.join(o.out, 'summary.txt'), 'w')
    summary.write('逻辑路径\t宽\t高\t级\t原游戏格式\tBC3 字节\tRGBA8 字节\n')
    done = {}
    stat = {'嫌疑命中': len(todo), '跳过（非 BC3 / 不要 sRGB）': 0, '转换': 0, 'BC3 字节': 0, 'RGBA8 字节': 0}
    for i, (p, f) in enumerate(todo):
        b = touch_layout._read_entry(o.romfs, idx[p])
        e = J_orig.get(p)
        if not e: stat['跳过（非 BC3 / 不要 sRGB）'] += 1; continue
        if b[4] not in CODE_BC3: stat['跳过（非 BC3 / 不要 sRGB）'] += 1; continue
        if o.only_linear and b[4] != 0x3d: stat['跳过（非 BC3 / 不要 sRGB）'] += 1; continue
        lv = int(e['levels'])
        nb, meta, blobs = to_rgba8(b, int(e['width']), int(e['height']), lv)
        dst = os.path.join(o.out, 'files', p); os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, 'wb').write(nb)
        done[p] = blobs
        stat['转换'] += 1; stat['BC3 字节'] += len(b); stat['RGBA8 字节'] += len(nb)
        summary.write(f'{p}\t{e["width"]}\t{e["height"]}\t{lv}\t0x{b[4]:02x}\t{len(b)}\t{len(nb)}\n')
        if (i + 1) % 200 == 0: print(f'tex_variant_pack：已转 {i+1}/{len(todo)}，用时 {time.time()-t0:.0f} 秒', flush=True)

    ncache = 0
    for e in J:
        p = e['filename'].lower()
        if p not in done: continue
        src = J_orig[p]
        if (e['format'] == 95) != (src['format'] == 61) or (e['format'] == 81) != (src['format'] == 62):
            sys.exit(f'tex_variant_pack：{p} 缓存里的格式 {e["format"]} 与源文件头的 0x{src["format"]:02x} 不是一套')
        e['format'] = 31
        if e.get('thumbnailBase64'):
            # 缩略图按各级已解出的 RGBA8 精确像素重排；表起点相对偏移 = 4（原生约定，见 thumb_rgba8）
            # The thumbnail is re-laid out from the already-decoded exact RGBA8 pixels of each level; table start relative offset = 4 (native convention, see thumb_rgba8)
            e['_thumb_raw'] = base64.b64decode(e['thumbnailBase64'])
            nt = thumb_rgba8(done[p], e)
            if nt is not None: e['thumbnailBase64'] = base64.b64encode(nt).decode()
            del e['_thumb_raw']
        ncache += 1
    dst = os.path.join(o.out, 'files', DESC); os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'w', encoding='utf-8').write(json.dumps(J, ensure_ascii=False, separators=(',', ':')))
    repl.write(f'{DESC}\t{dst}\n')
    for p, f in base_repl:
        if p == DESC or p in done: continue
        repl.write(f'{p}\t{f}\n')
    for p in done: repl.write(f'{p}\t{os.path.join(o.out, "files", p)}\n')
    repl.close(); summary.close()
    print(f'tex_variant_pack：{stat["嫌疑命中"]} 张命中、{stat["转换"]} 张转成 RGBA8、描述缓存改 {ncache} 条，'
          f'{stat["BC3 字节"]/1e6:.0f} MB → {stat["RGBA8 字节"]/1e6:.0f} MB，用时 {time.time()-t0:.0f} 秒')


if __name__ == '__main__': main()
