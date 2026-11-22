# 给 dmhost 窗口投鼠标点击（PostMessage，不需要前台焦点）：front_win 把左键当手指按在窗口客户区坐标上。
# Send a mouse click to the dmhost window (PostMessage, no foreground focus needed): front_win treats the left button as a finger at the client coordinate.
# 坐标按窗口客户区像素给；在非 100% 缩放的屏幕上 PostMessage 的坐标会被系统按 DPI 再乘一次（120 dpi = ×1.25），
# 所以这里先按 GetDpiForWindow 除回去，传 --raw 就原样投。
# Coordinates are client pixels; on a display that is not at 100% scaling, PostMessage coordinates get scaled by the DPI a second time
# (120 dpi = x1.25), so they are divided back by GetDpiForWindow here; pass --raw to post them unchanged.
# 用法 | usage: python click_win.py <x> <y> [--hold 毫秒] [--right] [--raw]
import ctypes, sys, time

u32 = ctypes.WinDLL("user32", use_last_error=True)
WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
WM_RBUTTONDOWN, WM_RBUTTONUP = 0x0204, 0x0205
MK_LBUTTON, MK_RBUTTON = 0x0001, 0x0002


def find_window():
    h = u32.FindWindowA(b"dmhost_win", None)
    if not h:
        h = u32.FindWindowA(None, "D2R Mobile（Windows ARM64）".encode())
    return h


def main():
    if len(sys.argv) < 3:
        print("用法 | usage: click_win.py <x> <y> [--hold ms] [--right]")
        return 2
    x, y = int(sys.argv[1]), int(sys.argv[2])
    hold = 140
    if "--hold" in sys.argv:
        hold = int(sys.argv[sys.argv.index("--hold") + 1])
    right = "--right" in sys.argv
    h = find_window()
    if not h:
        print("没找到 dmhost 窗口 | dmhost window not found")
        return 2
    if "--raw" not in sys.argv:
        try:
            u32.GetDpiForWindow.restype = ctypes.c_uint
            dpi = u32.GetDpiForWindow(h) or 96
        except AttributeError:
            dpi = 96
        if dpi != 96:
            x, y = int(round(x * 96.0 / dpi)), int(round(y * 96.0 / dpi))
            print("DPI %d：实际投 (%d,%d)" % (dpi, x, y))
    lp = (y << 16) | (x & 0xFFFF)
    flag = MK_RBUTTON if right else MK_LBUTTON
    down = WM_RBUTTONDOWN if right else WM_LBUTTONDOWN
    up = WM_RBUTTONUP if right else WM_LBUTTONUP
    u32.PostMessageA(h, WM_MOUSEMOVE, 0, lp)
    time.sleep(0.02)
    u32.PostMessageA(h, down, flag, lp)
    time.sleep(hold / 1000.0)
    u32.PostMessageA(h, up, 0, lp)
    print("点击 | click %d,%d hold=%dms right=%s hwnd=%#x" % (x, y, hold, right, h))
    return 0


if __name__ == "__main__":
    sys.exit(main())
