#!/usr/bin/env python3
# 从 iOS 的 AppIcon 生成 Windows 的 dmhost.ico（多尺寸），供 dm/win/dmhost.rc 编进 exe 与窗口图标。
# 用法：python dm/win/tools/make_ico.py [图标源 png] [输出 ico]
# 源缺省取 dm/ios/Assets.xcassets/AppIcon.appiconset/AppIcon1024.png（同一套 App 图标，各平台一致）；
# Windows 需要 16~256 的多种尺寸（任务栏 16/24、资源管理器 32/48、大图标 256）。
# Build dmhost.ico (multiple sizes) for Windows from the iOS AppIcon, so dm/win/dmhost.rc can put it into the exe
# and the window icon.
# Usage: python dm/win/tools/make_ico.py [source png] [output ico]
# The source defaults to dm/ios/Assets.xcassets/AppIcon.appiconset/AppIcon1024.png (the same app icon everywhere);
# Windows wants several sizes from 16 to 256 (taskbar 16/24, Explorer 32/48, large icons 256).
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'ios', 'Assets.xcassets', 'AppIcon.appiconset', 'AppIcon1024.png')
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', 'dmhost.ico')

im = Image.open(src)
im = im.convert('RGBA')
if im.size[0] != im.size[1]:
    raise SystemExit('make_ico：图标源不是正方形（%dx%d）' % im.size)
# 先把 1024 缩到 256 再交给 Pillow 逐档缩，小尺寸的文字不会因一次性大比例缩小而糊成一团
# Shrink 1024 to 256 first and let Pillow go down size by size, so the small sizes do not blur into a smear in one big step
base = im.resize((256, 256), Image.LANCZOS)
base.save(out, format='ICO', sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)])
print('make_ico：%s -> %s（%d 字节）' % (os.path.basename(src), os.path.basename(out), os.path.getsize(out)))
