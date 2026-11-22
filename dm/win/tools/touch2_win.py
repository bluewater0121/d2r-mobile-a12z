# 注入两根手指（Windows Touch Injection），验证浮层「按住左边摇杆的同时点右边技能钮」。
# Inject two fingers through Windows Touch Injection to verify the overlay's "hold the left stick while tapping a skill on the right".
#
# 用真正走 WM_TOUCH 的输入，而不是 PostMessage 合成的鼠标：Windows 默认只把第一根手指合成成鼠标，
# 多点必须能真的进来，才能证明 front_win.c 的 WM_TOUCH 分支把每根触点分成了独立手指。
# This uses real touch input that goes through WM_TOUCH instead of a synthesized mouse: Windows only promotes the first contact into mouse
# messages, so only genuine multi-touch can prove that front_win.c's WM_TOUCH branch splits each contact into its own finger.
#
# 用法 | usage: python touch2_win.py <x1> <y1> <x2> <y2> [--hold 毫秒] [--client]
#   坐标默认是窗口客户区像素（和 click_win.py 同一套位图坐标）；--client 就是默认，--screen 表示直接给屏幕物理像素
#   Coordinates are client pixels by default (the same bitmap coordinates click_win.py takes); --screen passes physical screen pixels as-is
import ctypes, sys, time
from ctypes import wintypes as W

u = ctypes.WinDLL("user32", use_last_error=True)

PT_TOUCH = 2
POINTER_FLAG_INRANGE = 0x00000002
POINTER_FLAG_INCONTACT = 0x00000004
POINTER_FLAG_DOWN = 0x00010000
POINTER_FLAG_UP = 0x00040000
POINTER_FLAG_PRIMARY = 0x00002000
TOUCH_MASK_CONTACTAREA = 0x00000001
TOUCH_MASK_PRESSURE = 0x00000004
TOUCH_FEEDBACK_DEFAULT = 1


class POINTER_INFO(ctypes.Structure):
    _fields_ = [
        ("pointerType", ctypes.c_uint32), ("pointerId", ctypes.c_uint32), ("frameId", ctypes.c_uint32), ("pointerFlags", ctypes.c_uint32),
        ("sourceDevice", W.HANDLE), ("hwndTarget", W.HANDLE),
        ("ptPixelLocation", W.POINT), ("ptHimetricLocation", W.POINT),
        ("ptPixelLocationRaw", W.POINT), ("ptHimetricLocationRaw", W.POINT),
        ("dwTime", ctypes.c_uint32), ("historyCount", ctypes.c_uint32), ("InputState", ctypes.c_int32),
        ("dwKeyStates", ctypes.c_uint32), ("PerformanceCount", ctypes.c_uint64), ("ButtonChangeType", ctypes.c_uint32),
    ]


class POINTER_TOUCH_INFO(ctypes.Structure):
    _fields_ = [
        ("pointerInfo", POINTER_INFO),
        ("touchFlags", ctypes.c_uint32), ("touchMask", ctypes.c_uint32),
        ("rcContact", W.RECT), ("rcContactRaw", W.RECT),
        ("orientation", ctypes.c_uint32), ("pressure", ctypes.c_uint32),
    ]


def find_window():
    h = u.FindWindowA(b"dmhost_win", None)
    if not h:
        h = u.FindWindowA(None, "D2R Mobile（Windows ARM64）".encode())
    return h


def client_to_screen(h, x, y):
    pt = W.POINT(x, y)
    u.ClientToScreen(h, ctypes.byref(pt))
    return pt.x, pt.y


def make(x, y, pid, flags):
    t = POINTER_TOUCH_INFO()
    ctypes.memset(ctypes.byref(t), 0, ctypes.sizeof(t))
    t.pointerInfo.pointerType = PT_TOUCH
    t.pointerInfo.pointerId = 0   # 新触点由系统分配 ID | the system assigns the id for a new contact
    t.pointerInfo.frameId = 0
    t.pointerInfo.pointerFlags = flags
    t.pointerInfo.ptPixelLocation = W.POINT(x, y)
    t.pointerInfo.ptPixelLocationRaw = W.POINT(x, y)
    # HIMETRIC 单位是 0.01 mm，注入时留 0 让系统自己换算 | himetric is 0.01 mm, leave it 0 and let the system fill it in
    t.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE
    t.rcContact = W.RECT(x - 2, y - 2, x + 2, y + 2)
    t.rcContactRaw = W.RECT(x - 2, y - 2, x + 2, y + 2)
    t.pressure = 320
    return t


def inject(pts, hold_ms):
    u.InitializeTouchInjection.restype = W.BOOL
    u.InitializeTouchInjection.argtypes = (ctypes.c_uint32, ctypes.c_int)
    u.InjectTouchInput.argtypes = (ctypes.c_uint32, ctypes.POINTER(POINTER_TOUCH_INFO))
    if not u.InitializeTouchInjection(10, TOUCH_FEEDBACK_DEFAULT):
        print("InitializeTouchInjection 失败 | failed, error=%d" % ctypes.get_last_error())
        return False
    down = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_PRIMARY
    down2 = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT
    up = POINTER_FLAG_UP | POINTER_FLAG_INRANGE
    arr = (POINTER_TOUCH_INFO * 1)()
    arr[0] = make(pts[0][0], pts[0][1], 1, down)
    if not u.InjectTouchInput(1, ctypes.byref(arr[0])):
        print("第一根注入失败 | first finger failed, error=%d" % ctypes.get_last_error())
        return False
    time.sleep(0.15)
    arr[0] = make(pts[1][0], pts[1][1], 2, down2)
    if not u.InjectTouchInput(1, ctypes.byref(arr[0])):
        print("第二根注入失败 | second finger failed, error=%d" % ctypes.get_last_error())
        return False
    time.sleep(hold_ms / 1000.0)
    arr[0] = make(pts[1][0], pts[1][1], 2, up)
    u.InjectTouchInput(1, ctypes.byref(arr[0]))
    time.sleep(0.15)
    arr[0] = make(pts[0][0], pts[0][1], 1, up)
    u.InjectTouchInput(1, ctypes.byref(arr[0]))
    return True


def main():
    if len(sys.argv) < 5:
        print("用法 | usage: touch2_win.py <x1> <y1> <x2> <y2> [--hold ms] [--screen]")
        return 2
    pts = [(int(sys.argv[1]), int(sys.argv[2])), (int(sys.argv[3]), int(sys.argv[4]))]
    hold = 400
    if "--hold" in sys.argv:
        hold = int(sys.argv[sys.argv.index("--hold") + 1])
    h = find_window()
    if not h:
        print("没找到 dmhost 窗口 | dmhost window not found")
        return 2
    if "--screen" not in sys.argv:
        pts = [client_to_screen(h, x, y) for (x, y) in pts]
    print("两根手指 %s 按住 %d ms（窗口 %#x）" % (pts, hold, h))
    return 0 if inject(pts, hold) else 1


if __name__ == "__main__":
    sys.exit(main())
