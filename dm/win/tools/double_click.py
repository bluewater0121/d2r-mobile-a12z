# 给 dmhost 窗口投两次左键点击（同一次进程内，间隔精确）：测「双击」手势用。
# 两次 click_win 各起一个 python 进程时，起进程的几十毫秒会把间隔推过 DM_TC_DOUBLE_TAP_S(0.40 s)，
# 看起来像「双击不成立」，所以这里把两下放在一个进程里按毫秒排。
# Send two left clicks to the dmhost window from a single process with exact timing (for testing the double-tap gesture).
# Launching two click_win processes adds tens of milliseconds per launch, pushing the interval past DM_TC_DOUBLE_TAP_S (0.40 s)
# and making it look as if the double tap never forms, so both clicks are timed in milliseconds here.
# 用法 | usage: python double_click.py <x> <y> [--hold 120] [--gap 150] [--n 1] [--raw]
import ctypes, sys, time

u32 = ctypes.WinDLL("user32", use_last_error=True)
WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
MK_LBUTTON = 0x0001


def find_window():
    h = u32.FindWindowA(b"dmhost_win", None)
    if not h:
        h = u32.FindWindowA(None, "D2R Mobile（Windows ARM64）".encode())
    return h


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) < 2:
        print("用法 | usage: double_click.py <x> <y> [--hold 120] [--gap 150] [--n 1] [--raw]")
        return 2
    x, y = int(args[0]), int(args[1])
    opts = {a: sys.argv[sys.argv.index(a) + 1] for a in ("--hold", "--gap", "--n") if a in sys.argv}
    hold = int(opts.get("--hold", 120)) / 1000.0
    gap = int(opts.get("--gap", 150)) / 1000.0
    n = int(opts.get("--n", 1))
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
    lp = (y << 16) | (x & 0xFFFF)
    u32.PostMessageA(h, WM_MOUSEMOVE, 0, lp)
    for _ in range(n):
        u32.PostMessageA(h, WM_LBUTTONDOWN, MK_LBUTTON, lp)
        time.sleep(hold)
        u32.PostMessageA(h, WM_LBUTTONUP, 0, lp)
        time.sleep(gap)
    print("双击 | double click %d,%d hold=%dms gap=%dms n=%d hwnd=%#x" % (x, y, hold * 1000, gap * 1000, n, h))
    return 0


if __name__ == "__main__":
    sys.exit(main())
