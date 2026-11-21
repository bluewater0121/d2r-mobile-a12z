#!/usr/bin/env python3
"""Mach-O 语法的 .S（苹果构建用）→ COFF 目标可汇编的 GAS 语法（Windows ARM64 构建用）。

两句工程约定：
1) 只做「同一条指令换写法」，不改任何一条指令的语义：标签去掉前导下划线（PE/COFF 的 64 位目标不加前缀），
   adrp 的 @PAGE 变成裸符号，@PAGEOFF 变成 :lo12: 修饰，.cstring 换成 .rdata 里的 .asciz。
2) 不处理的写法（例如 .indirect_symbol、.section __TEXT,__text）一出现就报错退出，而不是悄悄跳过：
   本工程的三个 .S 只有下面这几类指令，出现意外写法时应当停下来看一眼，而不是产出一份「看起来编过了」的目标。

Mach-O flavored .S (used by the Apple build) -> GAS syntax the COFF target can assemble (used by the Windows ARM64 build).

Two engineering rules:
1) Only rewrite the same instruction differently, never change what any instruction means: labels lose the leading underscore (64-bit PE/COFF
   targets take no prefix), adrp's @PAGE becomes a bare symbol, @PAGEOFF becomes the :lo12: modifier, .cstring becomes .asciz inside .rdata.
2) Anything not handled (e.g. .indirect_symbol, .section __TEXT,__text) raises and exits instead of being skipped silently: the three .S files in
   this project use only the cases below, and an unexpected construct should stop us so we look, rather than produce an object that "seemed to assemble".
"""
import argparse
import re
import sys

SYMBOL = re.compile(r'(?<![\w.$:])_([A-Za-z][A-Za-z0-9_.$]*)')
# 允许的写法：注释、标签、.globl/.text/.data/.p2align/.arch_extension/.rept/.endr/.set/.quad、以及普通指令
STRIP_DIRECTIVES = ('.subsections_via_symbols', '.alt_entry', '.private_extern', '.no_dead_strip', '.file')


def convert_line(line):
    stripped = line.strip()
    if stripped.startswith('//') or stripped.startswith('#'):
        return line
    if '.cstring' in line:
        # .cstring "x" → 放进 .rdata 的 asciz；字符串里有下划线也不动
        body = line.split('.cstring', 1)[1].strip()
        return '.section .rdata, "dr"\n    .asciz ' + body
    # .section __TEXT,__text,... 这类 Mach-O 段写法不支持，遇到就停
    if re.match(r'\s*\.section\s+__', line) or '.indirect_symbol' in line or '.zerofill' in line:
        raise SystemExit('asm_coff.py: 不支持这种 Mach-O 写法，先看一眼再决定怎么换：' + line.rstrip())
    for d in STRIP_DIRECTIVES:
        if stripped.startswith(d):
            if stripped.startswith('.private_extern'):
                return re.sub(r'\s*\.private_extern', ' .local', line, count=1)
            return None
    # 先去符号前缀（放最前：SYMBOL 的负向断言挡着冒号，:lo12:_foo 这种形态必须先变成 :lo12:foo）
    # Strip the symbol prefix first (SYMBOL's negative lookbehind rejects a colon, so a :lo12:_foo form must become :lo12:foo first)
    line = SYMBOL.sub(r'\1', line)
    # @PAGEOFF 先换（它包含 @PAGE 前缀字样），再换 @PAGE
    line = re.sub(r'([A-Za-z_][A-Za-z0-9_$.]*)?@PAGEOFF', lambda m: ':lo12:' + m.group(0)[:-len('@PAGEOFF')], line)
    line = re.sub(r'([A-Za-z_][A-Za-z0-9_$.]*)?@PAGE', lambda m: m.group(0)[:-len('@PAGE')], line)
    return line


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('src')
    ap.add_argument('dst')
    a = ap.parse_args()
    out = []
    with open(a.src, encoding='utf-8') as f:
        for line in f.read().splitlines():
            r = convert_line(line)
            if r is not None:
                out.append(r)
    text = '\n'.join(out) + '\n'
    text = re.sub(r'^_([A-Za-z][A-Za-z0-9_.$]*)(:)', r'\1\2', text, flags=re.M)
    with open(a.dst, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
