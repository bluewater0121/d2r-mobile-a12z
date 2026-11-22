#!/usr/bin/env python3
# 从 iOS 的 AppIcon 生成 Windows 的 dmhost.ico（多尺寸、圆角正方形），供 dm/win/dmhost.rc 编进 exe 与窗口图标。
# 用法：python dm/win/tools/make_ico.py [图标源 png] [输出 ico] [圆角半径比例]
# 源缺省取 dm/ios/Assets.xcassets/AppIcon.appiconset/AppIcon1024.png（同一套 App 图标，各平台一致）；
# Windows 需要 16~256 的多种尺寸（任务栏 16/24、资源管理器 32/48、大图标 256）。
# 圆角：直角四角裁成透明，半径取边长的 22%（Windows 11 的 App 图标观感；0 = 保留直角）。
# 边缘用 4 倍超采样再缩回来，16 px 这一档也不会出现锯齿。
# Build dmhost.ico (multiple sizes, rounded square) for Windows from the iOS AppIcon, so dm/win/dmhost.rc can put it
# into the exe and the window icon.
# Usage: python dm/win/tools/make_ico.py [source png] [output ico] [corner radius ratio]
# The source defaults to dm/ios/Assets.xcassets/AppIcon.appiconset/AppIcon1024.png (the same app icon everywhere);
# Windows wants several sizes from 16 to 256 (taskbar 16/24, Explorer 32/48, large icons 256).
# Corners: the square corners are cut to transparent with a radius of 22% of the side (the Windows 11 app-icon look;
# 0 keeps them square). Edges are rendered at 4x and scaled back down, so even the 16 px size has no jaggies.
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'ios', 'Assets.xcassets', 'AppIcon.appiconset', 'AppIcon1024.png')
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', 'dmhost.ico')
radius_ratio = float(sys.argv[3]) if len(sys.argv) > 3 else 0.22
SIZES = [(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)]
SS = 4   # 超采样倍数 | supersampling factor


def rounded(im, ratio):
    """把方图裁成圆角正方形：四角透明，边缘抗锯齿。| Cut the square into a rounded square: transparent corners, antialiased edge."""
    w, h = im.size
    if ratio <= 0:
        return im
    mask = Image.new('L', (w * SS, h * SS), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w * SS - 1, h * SS - 1], radius=int(w * SS * ratio), fill=255)
    mask = mask.resize((w, h), Image.LANCZOS)
    o = im.copy()
    o.putalpha(Image.composite(im.getchannel('A'), Image.new('L', (w, h), 0), mask))
    return o


im = Image.open(src).convert('RGBA')
if im.size[0] != im.size[1]:
    raise SystemExit('make_ico：图标源不是正方形（%dx%d）' % im.size)
# 先把 1024 缩到 256 再交给 Pillow 逐档缩，小尺寸的文字不会因一次性大比例缩小而糊成一团
# Shrink 1024 to 256 first and let Pillow go down size by size, so the small sizes do not blur into a smear in one big step
base = rounded(im.resize((256, 256), Image.LANCZOS), radius_ratio)
base.save(out, format='ICO', sizes=SIZES)
print('make_ico：%s -> %s（%d 字节，圆角 %.0f%%）' % (os.path.basename(src), os.path.basename(out), os.path.getsize(out), radius_ratio * 100))
