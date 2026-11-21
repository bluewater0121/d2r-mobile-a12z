#!/usr/bin/env python3
"""把 NSO 里的地址（模块内偏移）翻成动态符号名：宿主崩溃 / 断点回溯里只打 offset，查这里。

模块的 MOD0 / 动态段布局与 dm/host/loader.c 的 parse_dynamic 一致：MOD0 在偏移 4 处，dyn 指针在
MOD0+4 处给人读；符号表取 DT_SYMTAB / DT_STRTAB，按「值 <= 偏移 且 偏移 < 值+大小」匹配，
大小缺失（0）时退到「最近的、值更小的那个符号」。

Usage: sym_off.py <nso> <offset> [offset...]（偏移可写 0x2b03ec 或 2b03ec）
"""
import struct, sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../verify'))
import nsoimg


def dyn(img):
    mod0 = struct.unpack_from('<I', img, 4)[0]
    assert img[mod0:mod0 + 4] == b'MOD0', 'no MOD0'
    d = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]
    symtab = strtab = hash_ = 0; n = 0
    while True:
        tag, val = struct.unpack_from('<QQ', img, d + n * 16)
        n += 1
        if tag == 0:
            break
        if tag == 6: symtab = val      # DT_SYMTAB
        elif tag == 5: strtab = val    # DT_STRTAB
        elif tag == 4: hash_ = val      # DT_HASH：它的 nchain 才是符号数 | DT_HASH: its nchain is the symbol count
    return symtab, strtab, hash_


def symbols(img):
    st, strt, hash_ = dyn(img)
    # 符号数取 DT_HASH 的 nchain（sdk 的字符串表在符号表**之前**，用两者差值算会得到负数）
    # The symbol count comes from DT_HASH's nchain (in the sdk the string table sits *before* the symbol table, so a difference gives a negative number)
    nsym = struct.unpack_from('<I', img, hash_ + 4)[0] if hash_ else (strt - st) // 24
    out = []
    for i in range(1, nsym):
        name, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', img, st + i * 24)
        if name == 0 or value == 0:
            continue
        nm = img[strt + name:strt + name + 200].split(b'\0', 1)[0].decode('utf-8', 'replace')
        out.append((value, size, nm))
    out.sort()
    return out


def lookup(syms, off):
    best = None
    for v, sz, nm in syms:
        if v <= off and (sz == 0 or off < v + sz):
            best = (v, sz, nm)
        elif v <= off:
            if best is None or (best[1] == 0 and v > best[0]):
                if best is None or best[1] == 0:
                    best = (v, sz, nm)
        if v > off:
            break
    return best


def main():
    img = nsoimg.load(sys.argv[1])
    syms = symbols(img)
    for a in sys.argv[2:]:
        off = int(a, 16)
        b = lookup(syms, off)
        print(f'{off:#x} -> ' + (f'{b[2]}+{off - b[0]:#x}' if b else '?'))


if __name__ == '__main__':
    main()
