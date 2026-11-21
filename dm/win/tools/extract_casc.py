#!/usr/bin/env python3
# 从 Switch 静态 CASC（RomFS/data 下的 .data 档）里把着色器包 *.bin 原样取出来，平铺到一个目录，
# 供 dm/gfx/tools/dmsh_vk.py 当「Switch 包目录」用（它按 shader_map.tsv 第 1 列的包名平铺打开）。
# 复用 dm/tools/casc_overlay.py 的解析（vfs-1 → TVFS 容器文件表 → EKey → BLTE 解块），不另写一套格式。
# Extract the shader packages (*.bin) from the Switch static CASC (the .data archives under RomFS/data) and lay them out flat in one directory,
# for dm/gfx/tools/dmsh_vk.py to use as the "Switch package directory" (it opens the package names from column 1 of shader_map.tsv flat).
# Reuses the parsing in dm/tools/casc_overlay.py (vfs-1 → TVFS container file table → EKey → BLTE block decode) instead of reimplementing the format.
#
# 用法 | usage: extract_casc.py <RomFS> <输出目录> [包名…]（不给包名 = shader_map.tsv 里出现的全部）
import sys, struct, pathlib, re, zlib
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / 'tools'))
import casc_overlay as c

MAP = HERE.parents[1] / 'gfx' / 'nvn' / 'shader_map.tsv'


def wanted():
    names = set()
    for line in MAP.read_text(encoding='utf-8').splitlines():
        if line.startswith('#') or not line:
            continue
        first = line.split('\t')[0]
        if first.endswith('.bin'):
            names.add(first)
    return names


def main():
    root = pathlib.Path(sys.argv[1]); out = pathlib.Path(sys.argv[2])
    names = set(sys.argv[3:]) or wanted()
    out.mkdir(parents=True, exist_ok=True)
    data_dir = root / 'data'
    lines, values = c.config_values(data_dir / '.build.config')
    vfs_ekey = bytes.fromhex(values['vfs-1'].split()[1])
    vfs_decoded, vfs_encoded = map(int, values['vfs-1-size'].split())
    vfs = bytearray(c.read_data_file(data_dir, vfs_ekey, vfs_encoded))
    if len(vfs) != vfs_decoded or vfs[:4] != b'TVFS':
        sys.exit('vfs-1 不是 TVFS')
    manifest = c.read_manifest(root)
    cft_offset, cft_size = struct.unpack('>II', vfs[28:36])
    cft_end = cft_offset + cft_size
    est_off, est_size = struct.unpack('>II', vfs[38:46])
    especs = bytes(vfs[est_off:est_off + est_size]).split(b'\0')
    ok = bad = 0
    for name in sorted(names):
        dst = out / name
        if dst.is_file() and dst.stat().st_size > 0:
            ok += 1
            continue
        logical = 'data/hd/shaders/' + name.lower()   # 清单里的路径已去掉 "data:" 前缀并转小写 | the manifest path has the "data:" prefix stripped and is lowercased
        row = manifest.get(logical)
        if row is None:
            print('缺清单条目:', logical); bad += 1; continue
        try:
            pos = c.cft_position(vfs, cft_offset, cft_end, manifest, logical)
        except SystemExit:
            print('定位失败:', logical); bad += 1; continue
        ekey = bytes(vfs[pos:pos + 16])
        encoded = struct.unpack('>I', vfs[pos + 16:pos + 20])[0]
        espec_index = vfs[pos + 20]
        decoded = struct.unpack('>I', vfs[pos + 21:pos + 25])[0]
        try:
            espec = especs[espec_index].decode()
            chunk, archive = ekey[9], ekey[10]
            offset = int.from_bytes(ekey[11:16], 'big')
            with (data_dir / f'{chunk:02d}-{archive:08d}.data').open('rb') as stream:
                stream.seek(offset)
                raw = stream.read(encoded)
            if len(raw) != encoded:
                raise ValueError('短读')
            if espec.startswith('b:'):
                content = c.read_data_file(data_dir, ekey, encoded)
            elif espec == 'z':
                content = zlib.decompress(raw)
            else:
                content = raw
        except Exception as e:
            print('解档失败:', logical, e); bad += 1; continue
        if len(content) != decoded:
            print('长度不符:', logical, len(content), decoded); bad += 1; continue
        dst.write_bytes(content)
        ok += 1
    print(f'取出 {ok} 个着色器包，失败 {bad}')
    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())

