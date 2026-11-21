# 给 dmhost 的窗口直接投键盘消息（不需要前台焦点）：PostMessage WM_KEYDOWN / WM_KEYUP 到 dmhost_win 类窗口。
# 用途：无桌面截图权限时，用它把游戏从菜单推进到局内，再靠 DM_WIN_SHOT 落盘核对画面。
# Sends keyboard messages straight to the dmhost window (no foreground focus needed): PostMessage WM_KEYDOWN / WM_KEYUP to the dmhost_win class window.
# Use: without desktop capture permission, drive the game from the menu into a run and check the picture with DM_WIN_SHOT files.
#
# 用法 | usage: python keys_win.py <键序列> [--delay 毫秒]
#   键序列：单字符 = 按一下（如 ja 表示 J、A 各按一下）；"J:800" = 按下 J 保持 800 ms 再松（走路用）
#   key sequence: a plain char = tap it (e.g. ja taps J then A); "J:800" = hold J for 800 ms then release (for walking)
import ctypes, ctypes.wintypes as wt, sys, time

u32 = ctypes.WinDLL("user32", use_last_error=True)
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
VK = {"up": 0x26, "down": 0x28, "left": 0x25, "right": 0x27, "enter": 0x0D, "esc": 0x1B, "tab": 0x09, "space": 0x20}


def find_window():
    h = u32.FindWindowA(b"dmhost_win", None)
    if not h:
        h = u32.FindWindowA(None, "D2R Mobile（Windows ARM64）".encode())
    return h


def vk_of(tok):
    if tok.lower() in VK:
        return VK[tok.lower()]
    if len(tok) == 1:
        return ord(tok.upper())
    raise SystemExit("未知键 | unknown key: " + tok)


def main():
    seq = sys.argv[1] if len(sys.argv) > 1 else "j"
    delay = 120
    if "--delay" in sys.argv:
        delay = int(sys.argv[sys.argv.index("--delay") + 1])
    h = find_window()
    if not h:
        print("没找到 dmhost 窗口 | dmhost window not found")
        return 2
    print("hwnd=%#x" % h)
    for item in seq.split():
        tok, _, hold = item.partition(":")
        hold_ms = int(hold) if hold else 140
        vk = vk_of(tok)
        # 按下与松开之间必须留时间：客体每帧采样一次按键状态，同一帧里按下又松开会被当成没按。
        # Keep a real gap between down and up: the guest samples button state once per frame, so a down+up inside one frame reads as no press.
        u32.PostMessageA(h, WM_KEYDOWN, vk, 0x00000001)
        time.sleep(hold_ms / 1000.0)
        u32.PostMessageA(h, WM_KEYUP, vk, 0xC0000001)
        print("按键 | key %s (vk=%#x)，保持 %d ms" % (tok, vk, hold_ms))
        time.sleep(delay / 1000.0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
