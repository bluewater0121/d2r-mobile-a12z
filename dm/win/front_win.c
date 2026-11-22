// Windows 前端（front.h 的 Windows 实现，对应 Mac 的 front_mac.m、安卓的 front_android.c）。
// 形态：桌面窗口，客体在独立线程跑，主线程只做窗口消息与呈现；键盘 / 鼠标直接写 dm_input / dm_touch。
// 分辨率：缺省原版 16:9（主机 1600x900、掌机 1280x720），DM_GAME_RES=宽x高 可钉住；窗口可缩放，画面按游戏比例居中留边（呈现层负责）。
// 文本输入：swkbd 请求时开一个自己的线程建输入框，调用线程阻塞到确定 / 取消。
// Windows front end (the Windows implementation of front.h, counterpart of Mac's front_mac.m and Android's front_android.c).
// Shape: a desktop window, the guest runs on its own thread, the main thread only handles window messages and presentation; the keyboard / mouse
// write straight into dm_input / dm_touch.
// Resolution: the original 16:9 by default (docked 1600x900, handheld 1280x720), pinnable with DM_GAME_RES=WxH; the window is resizable and the
// picture is centered with margins at the game's aspect (the presentation layer does that).
// Text input: when swkbd asks, our own thread creates an input box and the calling thread blocks until confirm / cancel.
#define _CRT_SECURE_NO_WARNINGS
#include "../host/dm.h"
#include "../host/front.h"
#include "../host/dm_input.h"
#include "../host/dm_uiscreen.h"
#include "vulkan_present_win.h"
#include "sched_win.h"
#include "touch_win.h"
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>

static HWND g_hwnd;
static volatile LONG g_closing;
static void (*g_guest)(void);
static pthread_t g_guest_th;
static _Atomic int g_guest_running;

// ---------- 窗口与呈现 ----------
// 客户区首选 1600x900（按屏幕工作区不超过 90%）；DM_WIN_SIZE=宽x高 覆盖，DM_WIN_FULL=1 直接最大化
// Preferred client area 1600x900 (capped at 90% of the work area); DM_WIN_SIZE=WxH overrides it, DM_WIN_FULL=1 starts maximized
static void client_size(int *w, int *h) {
    int cw = 1600, ch = 900;
    const char *e = getenv("DM_WIN_SIZE");
    if (e) { unsigned a, b; if (sscanf(e, "%ux%u", &a, &b) == 2 && a >= 320 && b >= 240) { cw = (int)a; ch = (int)b; } }
    RECT wa = {0, 0, 0, 0}; SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    int maxw = (int)((wa.right - wa.left) * 0.9), maxh = (int)((wa.bottom - wa.top) * 0.9);
    if (maxw > 0 && cw > maxw) { ch = ch * maxw / cw; cw = maxw; }
    if (maxh > 0 && ch > maxh) { cw = cw * maxh / ch; ch = maxh; }
    *w = cw; *h = ch;
}
// 视图矩形（客户区像素坐标）：游戏比例的那块，居中；比例差 < 1% 就是整个客户区。与呈现层的留边规则一致
// View rectangle (client-area pixels): the block at the game's aspect, centered; a difference below 1% means the whole client area. Same letterbox rule as the presentation layer
static void view_rect(int win_w, int win_h, int out[4]) {
    out[0] = 0; out[1] = 0; out[2] = win_w; out[3] = win_h;
    if (win_w <= 0 || win_h <= 0) return;
    uint64_t r = front_game_res(1); double gw = (double)(uint32_t)r, gh = (double)(r >> 32);
    if (gw <= 0 || gh <= 0) return;
    double ga = gw / gh, wa = (double)win_w / win_h;
    if (fabs(wa / ga - 1) < 0.01) return;
    int w = win_w, h = win_h;
    if (wa > ga) w = (int)floor(win_h * ga); else h = (int)floor(win_w / ga);
    out[0] = (win_w - w) / 2; out[1] = (win_h - h) / 2; out[2] = w; out[3] = h;
}

// ---------- 输入 ----------
// 键位沿用 Mac 宿主那套（原本是苹果虚拟键码，这里换成 Windows 虚拟键码）：WASD = 左摇杆，J/K/U/I = A/B/X/Y，
// Q/E/Z/C = L/R/ZL/ZR，回车 = ＋，Tab = −，空格 = A，方向键 = 十字键。
// 鼠标：左键 = 画面上落一根手指（走 dm_touch_set），右键 = B，滚轮 = 右摇杆上下推一下。
// Key layout follows the Mac host (originally Apple virtual key codes, here Windows virtual-key codes): WASD = left stick, J/K/U/I = A/B/X/Y,
// Q/E/Z/C = L/R/ZL/ZR, Return = +, Tab = -, Space = A, arrow keys = d-pad.
// Mouse: left button = one finger on the picture (through dm_touch_set), right button = B, wheel = nudge the right stick up or down.
static uint64_t g_btn;
static int g_kw, g_ka, g_ks, g_kd;
static int g_stick_rx;             // 滚轮累计的小幅右摇杆（松开后缓回 0） | small right-stick nudge accumulated from the wheel (eases back to 0)
static int g_mouse_down, g_finger;
static float g_mouse_x, g_mouse_y;
static int g_front_click;          // 这一下按下走的是前端页面的界面点击（dm_ui_touch）| this press goes through the front-end UI click path (dm_ui_touch)
void dm_ui_touch(int phase, float nx, float ny);   // dm/host/dm_cursor.c：界面点击（相 0 按下 / 1 移动 / 2 抬起 / 3 作废）| UI click (phase 0 down / 1 move / 2 up / 3 cancelled)

static void push_input(void) {
    int32_t lx = (g_kd - g_ka) * 32767, ly = (g_kw - g_ks) * 32767;
    dm_input_set(g_btn, lx, ly, g_stick_rx, 0);
}
// 实体手柄（pad_win.c）：XInput 轮询线程 → dm_pad_set；敲键盘 / 点画面就切回触屏布局
// Physical controllers (pad_win.c): the XInput poll thread feeds dm_pad_set; typing or clicking the picture switches back to the touch layout
void dm_win_pad_start(void);
void dm_win_pad_touch(void);
static int vk_bit(int vk) {
    switch (vk) {
    case 'J': return 0; case 'K': return 1; case 'U': return 2; case 'I': return 3;      // A B X Y
    case 'F': return 4; case 'G': return 5;                                             // 按摇杆 | press stick
    case 'Q': return 6; case 'E': return 7; case 'Z': return 8; case 'C': return 9;      // L R ZL ZR
    case VK_RETURN: case VK_OEM_PLUS: case VK_ADD: return 10;                             // ＋
    case VK_TAB: return 11;                                                               // −
    case VK_SPACE: return 0;                                                              // 空格 → A（确认） | Space -> A (confirm)
    case VK_LEFT: return 12; case VK_UP: return 13; case VK_RIGHT: return 14; case VK_DOWN: return 15;
    }
    return -1;
}
// 鼠标位置 → 画面归一化坐标（视图矩形外算作画面边缘，点击穿透不了黑边）
// Mouse position -> normalized picture coordinates (outside the view rectangle counts as the picture edge; clicks do not go through the black bars)
static void mouse_to_pic(int cx, int cy, float *nx, float *ny) {
    RECT rc; GetClientRect(g_hwnd, &rc);
    int vr[4]; view_rect(rc.right, rc.bottom, vr);
    float x = vr[2] > 0 ? (float)(cx - vr[0]) / (float)vr[2] : 0.0f;
    float y = vr[3] > 0 ? (float)(cy - vr[1]) / (float)vr[3] : 0.0f;
    if (x < 0) x = 0; if (x > 0.999f) x = 0.999f; if (y < 0) y = 0; if (y > 0.999f) y = 0.999f;
    (void)cx; (void)cy;
    *nx = x; *ny = y;
}
// ---------- 多点触控：一根触点一根手指 ----------
// 浮层的圆钮是按「一根手指一个槽」做的（dm_win_touch_pointer 的 slot），所以这里把每个 Windows 触点（TOUCHINPUT.dwID）
// 分别登记，命中了浮层控件就把它交给触屏状态机，没命中才当游戏的触点喂下去。
// 关键点：喂给游戏的那一份手指表必须**一次提交全部手指**（dm_touch_set(t, n)），不能一根一根覆盖，
// 否则按住摇杆的那根手指会被后按下去的那根冲掉——就是「按住方向键时点技能没反应」。
// The overlay's round buttons work with one finger per slot (dm_win_touch_pointer's slot), so each Windows contact (TOUCHINPUT.dwID) is registered
// separately: a contact that lands on an overlay control goes to the touch state machine, everything else is handed to the game. Key point: the
// finger table handed to the game must be submitted **all at once** (dm_touch_set(t, n)); replacing one finger at a time wipes out the finger
// holding the stick — which is exactly "pressing a skill while holding a direction does nothing".
#define DM_TU_MAX 16
static int g_touch_ok;   // RegisterTouchWindow 成功过：触点那条路是活的，下面才敢把「触点合成出来的鼠标消息」丢掉
static struct { uint32_t id; int slot; int claim; int front; float nx, ny; int used; } g_tu[DM_TU_MAX];

// 触点被系统「顺手」合成出来的那条鼠标消息：GetMessageExtraInfo 里带 0xFF515700 签名（触屏 / 笔），真鼠标没有它。
// iOS / 安卓都只有真触点这一路；Windows 上同一根手指会同时给出 WM_TOUCH 和这条合成鼠标，
// 两边都处理就会把同一根手指登记成两根（一根占着摇杆槽、又一根去抢同一个槽），所以合成的那条直接丢。
// The mouse message the system "helpfully" synthesizes from a contact carries the 0xFF515700 signature in GetMessageExtraInfo (touch / pen);
// a real mouse does not. iOS and Android only ever have real contacts, while on Windows the same finger produces both a WM_TOUCH and this
// synthesized mouse message -- handling both registers one finger twice (one takes the stick slot while the other fights for it), so the
// synthesized one is dropped.
static int mouse_from_touch(void) { return ((uintptr_t)GetMessageExtraInfo() & 0xFFFFFF00u) == 0xFF515700u; }

static int tu_find(uint32_t id) { for (int i = 0; i < DM_TU_MAX; ++i) if (g_tu[i].used && g_tu[i].id == id) return i; return -1; }
static int tu_alloc(uint32_t id) { for (int i = 0; i < DM_TU_MAX; ++i) if (!g_tu[i].used) {
        g_tu[i].id = id; g_tu[i].slot = i; g_tu[i].claim = 0; g_tu[i].front = 0; g_tu[i].nx = g_tu[i].ny = 0; g_tu[i].used = 1; return i; } return -1; }

// 把所有「不归浮层」的触点 + 鼠标那根手指一次性交给游戏｜hand every contact that is not the overlay's (plus the mouse finger) to the game at once
static void touch_publish(void) {
    dm_touch arr[DM_TU_MAX + 2]; int n = 0;
    for (int i = 0; i < DM_TU_MAX; ++i)
        if (g_tu[i].used && !g_tu[i].claim && !g_tu[i].front) { arr[n].id = (uint32_t)(10 + i); arr[n].x = g_tu[i].nx; arr[n].y = g_tu[i].ny; ++n; }
    if (g_mouse_down && g_finger) { arr[n].id = (uint32_t)g_finger; arr[n].x = g_mouse_x; arr[n].y = g_mouse_y; ++n; }
    dm_touch_set(n ? arr : NULL, n);
}

static void set_finger(int down) {    if (down && !g_mouse_down) {
        g_mouse_down = 1; g_finger = 1;
        dm_win_pad_touch();
        dm_win_touch_screen_input();   // 碰画面 = 触屏侧：手柄模式交回触屏（iOS 的 dm_gamepad_touched）| the picture was touched = touch side: hand back from controller mode
    } else if (!down && g_mouse_down) {
        g_mouse_down = 0; g_finger = 0;
        touch_publish();   // 抬起鼠标这根，别把还按着的触点一起清掉 | lift the mouse finger without dropping the contacts still down
        return;
    }
    if (g_mouse_down) touch_publish();
}

// 触点分发：WM_TOUCH 与触摸模拟（DM_TOUCH_SIM）共用同一条路 | contact dispatch shared by WM_TOUCH and the touch simulation (DM_TOUCH_SIM)
static void touch_dispatch(HWND h, UINT cnt, TOUCHINPUT *ti) {    for (UINT k = 0; k < cnt; ++k) {
        // TOUCHINPUT 的坐标是「1/100 像素」的屏幕坐标 | TOUCHINPUT coordinates are screen coordinates in hundredths of a pixel
        POINT pt = { ti[k].x / 100, ti[k].y / 100 };
        ScreenToClient(h, &pt);
        float nx, ny; mouse_to_pic(pt.x, pt.y, &nx, &ny);
        int down = (ti[k].dwFlags & TOUCHEVENTF_DOWN) != 0, up = (ti[k].dwFlags & TOUCHEVENTF_UP) != 0;
        int idx = tu_find(ti[k].dwID);
        if (down && idx < 0) {
            idx = tu_alloc(ti[k].dwID);
            if (idx < 0) continue;
            dm_win_touch_free_slot(g_tu[idx].slot);   // 上一根手指没抬起就占着这个槽：先清掉，否则这次按下被 begin_at 挡掉 | an earlier finger never lifted and still holds this slot: clear it, or begin_at refuses this press
            g_tu[idx].nx = nx; g_tu[idx].ny = ny;
            dm_win_pad_touch(); dm_win_touch_screen_input();   // 碰画面 = 触屏侧 | the picture was touched = touch side
            if (dm_win_touch_enabled() && dm_win_touch_pointer(0, nx, ny, g_tu[idx].slot)) g_tu[idx].claim = 1;
            else if (dm_win_touch_on()) { g_tu[idx].front = 1; dm_ui_touch(0, nx, ny); }
            else touch_publish();
        } else if (idx >= 0) {
            g_tu[idx].nx = nx; g_tu[idx].ny = ny;
            if (g_tu[idx].claim) dm_win_touch_pointer(up ? 2 : 1, nx, ny, g_tu[idx].slot);
            else if (g_tu[idx].front) dm_ui_touch(up ? 2 : 1, nx, ny);
            else touch_publish();
            if (up) g_tu[idx].used = 0;
        }
    }
}

// 触摸模拟：本机跑在远程桌面会话里，系统级的 Touch Injection 不可用（InjectTouchInput 恒 ERROR_INVALID_PARAMETER 87），
// 于是把「几根手指按在客户区哪个像素」直接造成 TOUCHINPUT 交给上面那条分发路径，验证多点本身对不对。
// Touch simulation: this machine runs in a Remote Desktop session where system-level Touch Injection is unavailable (InjectTouchInput
// always returns ERROR_INVALID_PARAMETER 87), so "which fingers press which client pixels" is turned into TOUCHINPUT records and fed into
// the very same dispatch path above, which is what actually validates the multi-touch handling.
void dm_win_front_touch_sim(int count, const int *cxys, int phase) {
    if (!g_hwnd || count <= 0 || count > DM_TU_MAX) return;
    TOUCHINPUT ti[DM_TU_MAX]; memset(ti, 0, sizeof ti);
    for (int k = 0; k < count; ++k) {
        POINT pt = { cxys[2 * k], cxys[2 * k + 1] };
        ClientToScreen(g_hwnd, &pt);
        ti[k].x = pt.x * 100; ti[k].y = pt.y * 100;
        ti[k].dwID = (DWORD)(901 + k);
        ti[k].dwFlags = phase == 0 ? (TOUCHEVENTF_DOWN | TOUCHEVENTF_INRANGE | (k == 0 ? TOUCHEVENTF_PRIMARY : 0))
                     : phase == 2 ? (TOUCHEVENTF_UP | TOUCHEVENTF_INRANGE)
                     : (TOUCHEVENTF_MOVE | TOUCHEVENTF_INRANGE);
    }
    LOG("输入：触摸模拟 phase=%d 根数=%d（第 %d 根 客户区 %d,%d）", phase, count, 0, count > 0 ? cxys[0] : 0, count > 0 ? cxys[1] : 0);
    touch_dispatch(g_hwnd, (UINT)count, ti);
}

// 触屏消息入口：触点发给「手指下面那个窗口」，而浮层是压在游戏客户区上的子窗口，所以两根窗口都可能收到，
// 两边注册了 WM_TOUCH 后都转到这里；target = 换算坐标用的窗口（游戏窗口，mouse_to_pic 用的是它的客户区）。
// Touch message entry: a contact is delivered to the window under the finger, and the overlay is a child window lying over the game's client
// area, so either window can receive it; both register WM_TOUCH and forward here. target = the window the coordinates are mapped into (the
// game window, whose client area mouse_to_pic measures against).
int dm_win_front_touch_msg(HWND target, WPARAM wp, LPARAM lp) {
    UINT cnt = LOWORD(wp);
    TOUCHINPUT ti[DM_TU_MAX];
    if (cnt > DM_TU_MAX) cnt = DM_TU_MAX;
    if (!cnt) { CloseTouchInputHandle((HANDLE)lp); return 0; }
    if (!GetTouchInputInfo((HANDLE)lp, cnt, ti, sizeof(TOUCHINPUT))) { CloseTouchInputHandle((HANDLE)lp); return 0; }
    if (getenv("DM_TOUCH_LOG"))
        for (UINT k = 0; k < cnt; ++k)
            LOG("输入：触点 %u/%u id=%u flags=%#x 屏幕 %ld,%ld", k + 1, cnt, (unsigned)ti[k].dwID, (unsigned)ti[k].dwFlags,
                (long)(ti[k].x / 100), (long)(ti[k].y / 100));
    touch_dispatch(target ? target : g_hwnd, cnt, ti);
    CloseTouchInputHandle((HANDLE)lp);
    return 1;
}

// ---------- 窗口过程 ----------
// 标题栏：分辨率 + 动态帧率（"1600x900 · 43 fps"）。每秒刷一次，值来自呈现层真正上屏的次数
// Title bar: resolution + live frame rate ("1600x900 · 43 fps"). Refreshed once a second from the presentation layer's actual present count
static void update_title(void) {
    if (!g_hwnd) return;
    uint64_t r = front_game_res(1);
    unsigned gw = (unsigned)(r & 0xffffffffu), gh = (unsigned)(r >> 32);
    int fps = dm_win_vk_fps();
    wchar_t t[160];
    swprintf(t, 160, L"D2R Mobile（Windows ARM64） — %ux%u · %d fps", gw, gh, fps);
    SetWindowTextW(g_hwnd, t);
}
static void on_key_msg(int vk, int down) {
    switch (vk) {
    case 'W': g_kw = down; break; case 'A': g_ka = down; break; case 'S': g_ks = down; break; case 'D': g_kd = down; break;
    }
    int b = vk_bit(vk);
    if (down && (b >= 0 || vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D')) {
        dm_padmode_request_glyph(DM_GLYPH_KBD);
        dm_win_pad_touch();   // 键盘 = 触屏侧：手柄布局切回触屏布局 | keyboard counts as the touch side: back to the touch layout
        dm_win_touch_screen_input();
    }
    if (b >= 0) g_btn = down ? (g_btn | 1ull << b) : (g_btn & ~(1ull << b));
    push_input();
}
static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        InterlockedExchange(&g_closing, 1);
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_SIZE:
        // GetClientRect 的第二个参数不能是 NULL（USER32 会直接解引用）；客户端尺寸由呈现层自己取
        // The second argument of GetClientRect must not be NULL (USER32 dereferences it); the presentation layer reads the client size itself
        if (wp != SIZE_MINIMIZED) { RECT rc; GetClientRect(h, &rc); dm_win_vk_notify_resize();
            { int vr[4]; view_rect(rc.right, rc.bottom, vr); dm_win_touch_layout(vr[0], vr[1], vr[2], vr[3]); } }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (msg == WM_SYSKEYDOWN && (wp == VK_F4 || wp == VK_MENU)) break;   // Alt+F4 / Alt 让系统处理 | let the system handle Alt+F4 / Alt
        on_key_msg((int)wp, 1); return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        on_key_msg((int)wp, 0); return 0;
    case WM_MOUSEMOVE: {
        if (g_touch_ok && mouse_from_touch()) return 0;   // 触点自己那条 WM_TOUCH 已经在处理它了 | the contact's own WM_TOUCH already handles it
        int cx = (int)(short)LOWORD(lp), cy = (int)(short)HIWORD(lp);
        mouse_to_pic(cx, cy, &g_mouse_x, &g_mouse_y);
        if (g_front_click) { dm_ui_touch(1, g_mouse_x, g_mouse_y); return 0; }
        // 触屏浮层开着（DM_WIN_TOUCH=1 且局内）：鼠标当一根手指先喂给共用的触屏状态机；没落在控件上它返回 0，照旧当普通触点给游戏
        // Overlay on (DM_WIN_TOUCH=1, in a run): the mouse goes to the shared touch state machine as one finger first; when it lands on no control the
        // call returns 0 and the event still goes to the game as an ordinary touch point
        if (dm_win_touch_enabled() && g_mouse_down && dm_win_touch_pointer(1, g_mouse_x, g_mouse_y, 0)) return 0;
        if (g_mouse_down) set_finger(1);   // 拖动 = 手指移动 | drag = finger move
        return 0; }
    case WM_LBUTTONDOWN:
        if (g_touch_ok && mouse_from_touch()) return 0;
        SetCapture(h); mouse_to_pic((int)(short)LOWORD(lp), (int)(short)HIWORD(lp), &g_mouse_x, &g_mouse_y);
        dm_win_touch_screen_input();   // 点画面（含点到圆钮）= 触屏侧接管，浮层重新露出来 | a press on the picture (round buttons included) = touch side takes over, the overlay comes back
        if (dm_win_touch_enabled() && dm_win_touch_pointer(0, g_mouse_x, g_mouse_y, 0)) { g_mouse_down = 1; return 0; }
        // 没落在浮层控件上的点击 = 界面点击（和 iOS 一样走 dm_ui_touch），别当触点喂给游戏——否则「进入游戏 / 开始游戏」这类按钮、
        // 以及局内的物品栏 / 技能树 / 选项面板全都没反应（面板也是原版 UI，走的是同一条界面点击通道）。
        // A press that lands on none of the overlay's controls is a UI click (routed through dm_ui_touch like iOS does) and must not be handed to the
        // game as a touch point -- otherwise buttons such as PLAY / Start Game, and the in-run inventory / skill-tree / options panels (also original UI,
        // on the same UI click channel), never respond.
        if (dm_win_touch_on()) { g_mouse_down = 1; g_front_click = 1; dm_ui_touch(0, g_mouse_x, g_mouse_y);
            LOG("前端：界面点击 (%.3f,%.3f)（前端页面走 dm_ui_touch，不当触点）", g_mouse_x, g_mouse_y); return 0; }
        set_finger(1); return 0;
    case WM_LBUTTONUP:
        if (g_touch_ok && mouse_from_touch()) return 0;
        if (g_front_click) { g_front_click = 0; g_mouse_down = 0; dm_ui_touch(2, g_mouse_x, g_mouse_y); ReleaseCapture(); return 0; }
        if (g_mouse_down && dm_win_touch_enabled() && dm_win_touch_pointer(2, g_mouse_x, g_mouse_y, 0)) { ReleaseCapture(); g_mouse_down = 0; return 0; }
        ReleaseCapture(); set_finger(0); return 0;
    case WM_RBUTTONDOWN:
        if (dm_win_touch_enabled()) return 0;   // 触屏模式没有右键（B 在圆钮上）| no right button in touch mode (B lives on a round button)
        g_btn |= DM_BTN_B; push_input(); return 0;
    case WM_RBUTTONUP:
        if (dm_win_touch_enabled()) return 0;
        g_btn &= ~(uint64_t)DM_BTN_B; push_input(); return 0;
    case WM_MOUSEWHEEL: {
        if (dm_win_touch_enabled()) return 0;
        int d = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
        g_stick_rx = d * 32767; push_input();
        SetTimer(h, 1, 80, NULL);   // 80 ms 后回中（松开摇杆） | recenter after 80 ms (release the stick)
        return 0; }
    case WM_TIMER:
        if (wp == 1) { KillTimer(h, 1); g_stick_rx = 0; push_input(); }
        else if (wp == 2) update_title();
        else if (wp == 3) dm_win_touch_tick();   // 触屏浮层：30 ms 一拍，按需重画 / 进局切布局 | overlay: 30 ms tick, repaint on demand, switch layout on entering a run
        return 0;
    case WM_TOUCH:
        dm_win_front_touch_msg(h, wp, lp);
        return 0;
    case WM_MOVE:
        // 浮层是独立的窗口，跟着游戏窗口走 | the overlay is a separate window; keep it on top of the game window
        { RECT rc; GetClientRect(h, &rc); int vr[4]; view_rect(rc.right, rc.bottom, vr); dm_win_touch_layout(vr[0], vr[1], vr[2], vr[3]); }
        return 0;
    case WM_SETFOCUS: return 0;
    case WM_KILLFOCUS:
        // 失焦时全抬起，避免「按住状态」卡在游戏里 | Release everything on focus loss so a held state does not stick in the game
        g_kw = g_ka = g_ks = g_kd = 0; g_btn = 0; g_stick_rx = 0; push_input(); if (g_mouse_down) set_finger(0);
        dm_win_touch_cancel();   // 触屏浮层：手指全部抬起 | overlay: lift every finger
        for (int i = 0; i < DM_TU_MAX; ++i) g_tu[i].used = 0;   // 触点也一起清（否则还原后按着的那根没人收）| clear the contacts too, or a held one is never lifted
        g_mouse_down = 0;
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}
static int create_window(void) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // 失败也继续（老系统） | continue even if it fails (older systems)
    HINSTANCE hi = GetModuleHandleA(NULL);
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof wc; wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC; wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hi; wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW); wc.hbrBackground = NULL;
    // 图标来自 dmhost.rc（1 号图标组，tools/make_ico.py 从 iOS 的 AppIcon 生成）：标题栏取小图、Alt+Tab 取大图
    // Icons come from dmhost.rc (icon group 1, tools/make_ico.py from the iOS AppIcon): small one in the title bar, large one in Alt+Tab
    wc.hIcon = LoadIconA(hi, MAKEINTRESOURCEA(1));
    wc.hIconSm = LoadIconA(hi, MAKEINTRESOURCEA(1));
    wc.lpszClassName = "dmhost_win";
    if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return -1;
    int cw, ch; client_size(&cw, &ch);
    RECT rc = {0, 0, cw, ch}; AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    RECT wa = {0, 0, 0, 0}; SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    int wx = wa.left + ((wa.right - wa.left) - ww) / 2, wy = wa.top + ((wa.bottom - wa.top) - wh) / 2;
    g_hwnd = CreateWindowExA(0, wc.lpszClassName, "D2R Mobile（Windows ARM64）", WS_OVERLAPPEDWINDOW,
                             wx, wy, ww, wh, NULL, NULL, hi, NULL);
    if (!g_hwnd) return -1;
    // 注册触屏消息：不注册的话 Windows 只把第一根手指合成成鼠标，浮层就做不出「按住摇杆的同时点技能」
    // Register for touch messages: without it Windows promotes only the first contact into mouse messages, and the overlay could never do
    // "hold the stick while tapping a skill"
    { BOOL ok_tu = RegisterTouchWindow(g_hwnd, 0); g_touch_ok = ok_tu ? 1 : 0;
      // 数字化仪能力：0 根触点 = 这个会话根本没有触摸（远程桌面会话就是这样），多点那条路没法在这台机器上验证
      // Digitizer capability: 0 contacts = this session has no touch at all (exactly what a Remote Desktop session looks like), so the
      // multi-touch path cannot be verified on this machine
      LOG("输入：触屏消息注册 %s（数字化仪 %d，触点上限 %d）", ok_tu ? "成功（可多点）" : "失败（只剩单指 / 鼠标）",
          (int)GetSystemMetrics(94) /* SM_DIGITIZER */, (int)GetSystemMetrics(95) /* SM_MAXIMUMTOUCHES */); }
    // 类图标只管标题栏，任务栏 / Alt+Tab 用窗口图标：两处都显式再设一次（窗口图标优先于类图标）
    // The class icon covers the title bar; the taskbar and Alt+Tab use the window icon: set both explicitly (the window icon wins over the class icon)
    SendMessageA(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)LoadIconA(hi, MAKEINTRESOURCEA(1)));
    SendMessageA(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadIconA(hi, MAKEINTRESOURCEA(1)));
    ShowWindow(g_hwnd, SW_SHOW); UpdateWindow(g_hwnd); SetForegroundWindow(g_hwnd); SetFocus(g_hwnd);
    SetTimer(g_hwnd, 2, 1000, NULL);   // 标题栏每秒刷一次分辨率与帧率 | refresh resolution and frame rate in the title bar once a second
    update_title();
    const char *full = getenv("DM_WIN_FULL");
    if (full && *full == '1') ShowWindow(g_hwnd, SW_MAXIMIZE);
    return 0;
}
static void *guest_main_tramp(void *p) {
    (void)p;
    atomic_store(&g_guest_running, 1);
    // 客体主线程就是提交与出图那条线程（长帧都落在它身上），MMCSS 必须在它自己的上下文里注册
    // The guest main thread is the one that submits and presents (all long frames land there), so MMCSS has to be registered in its own context
    dm_win_sched_thread("客体主线程");
    { void dm_stall_probe_begin(void); dm_stall_probe_begin(); }   // DM_STALL_PROBE：在这条客体主线程上起 PC 采样 | DM_STALL_PROBE: start PC sampling on this guest main thread
    g_guest();
    atomic_store(&g_guest_running, 0);
    LOG("客体线程结束");
    PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
    return NULL;
}

void *front_metal_layer(void) { return (void *)g_hwnd; }
HWND front_win_hwnd(void) { return g_hwnd; }

// 原版 16:9 常数；DM_GAME_RES=宽x高 钉住（掌机按 4/5 同比） | The original 16:9 constants; DM_GAME_RES=WxH pins them (handheld scaled by 4/5)
uint64_t front_game_res(int docked) {
    unsigned w, h; const char *e = getenv("DM_GAME_RES");
    if (e && sscanf(e, "%ux%u", &w, &h) == 2) return docked ? (uint64_t)h << 32 | w : (uint64_t)(h * 4 / 5) << 32 | (uint64_t)((w * 4 / 5 + 3) / 4 * 4);
    return docked ? 900ull << 32 | 1600 : 720ull << 32 | 1280;
}
double front_display_refresh(char *name, size_t n) {
    // 当前显示模式的名义刷新率（DisplayConfig 的细分值在桌面窗口里意义不大，游戏按它也要朝 60 对齐）
    // Nominal refresh rate of the current display mode (the fine-grained DisplayConfig value means little for a desktop window, and the game aligns to 60 anyway)
    DEVMODEA dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        if (name && n) snprintf(name, n, "%dx%d", (int)dm.dmPelsWidth, (int)dm.dmPelsHeight);
        return (double)dm.dmDisplayFrequency;
    }
    if (name && n) name[0] = 0;
    return 0.0;
}
int front_win_view_rect(int win_w, int win_h, int out[4]) { view_rect(win_w, win_h, out); return 1; }
int front_win_window_size(int *w, int *h) {
    if (!g_hwnd) return 0;
    RECT rc; GetClientRect(g_hwnd, &rc); *w = rc.right; *h = rc.bottom;
    return *w > 0 && *h > 0;
}

// ---------- 文本输入 ----------
// 独立线程建一个带 EDIT 的小窗，阻塞到确定 / 取消（主线程的消息循环不能被它卡住）
// A dedicated thread creates a small window with an EDIT and blocks until confirm / cancel (the main thread's message loop must not be stuck by it)
typedef struct {
    const char *title, *sub, *guide, *initial;
    int max_chars, out_cap;
    char *out;
    int result;
    HANDLE done;
    HWND win, edit;
    wchar_t *init_w;
} tin_req;
static wchar_t *u8_to_w(const char *s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = calloc((size_t)n + 1, sizeof *w);
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static LRESULT CALLBACK tin_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    tin_req *r = (tin_req *)GetWindowLongPtrA(h, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: return 0;
    case WM_COMMAND:
        if (r && LOWORD(wp) == 1) {   // 确定 | confirm
            wchar_t buf[4096]; GetWindowTextW(r->edit, buf, (int)(sizeof buf / sizeof buf[0]));
            int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, NULL, 0, NULL, NULL);
            if (n > 0 && r->out && r->out_cap > 0) {
                char *tmp = malloc((size_t)n);
                WideCharToMultiByte(CP_UTF8, 0, buf, -1, tmp, n, NULL, NULL);
                strlcpy(r->out, tmp, (size_t)r->out_cap); free(tmp);
            }
            r->result = 1; DestroyWindow(h);
        } else if (r && LOWORD(wp) == 2) { r->result = 0; DestroyWindow(h); }
        return 0;
    case WM_CLOSE: if (r) r->result = 0; DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}
static void tin_pump(void *p) {
    tin_req *r = p;
    HINSTANCE hi = GetModuleHandleA(NULL);
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof wc; wc.lpfnWndProc = tin_proc; wc.hInstance = hi; wc.hCursor = LoadCursorA(NULL, (IDC_ARROW));
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = "dmhost_textinput";
    RegisterClassExA(&wc);
    wchar_t title[512]; wchar_t *tw = u8_to_w(r->title ? r->title : "输入");
    wcsncpy(title, tw ? tw : L"输入", 511); free(tw);
    int ww = 620, wh = r->guide ? 260 : 210;
    r->win = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, "D2R", WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, ww, wh, g_hwnd, NULL, hi, NULL);
    SetWindowLongPtrA(r->win, GWLP_USERDATA, (LONG_PTR)r);
    SetWindowTextW(r->win, title);
    int y = 12;
    wchar_t *wsub = u8_to_w(r->sub), *wguide = u8_to_w(r->guide);
    if (wsub) { HWND st = CreateWindowExA(0, "STATIC", NULL, WS_CHILD | WS_VISIBLE, 14, y, ww - 30, 20, r->win, NULL, hi, NULL); SetWindowTextW(st, wsub); free(wsub); y += 26; }
    r->edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", NULL, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 14, y, ww - 30, 26, r->win, NULL, hi, NULL);
    if (r->initial) { wchar_t *wi = u8_to_w(r->initial); SetWindowTextW(r->edit, wi); free(wi); }
    SendMessageW(r->edit, EM_SETLIMITTEXT, (WPARAM)(r->max_chars > 0 ? r->max_chars + 8 : 4000), 0);
    y += 36;
    if (wguide) { HWND st = CreateWindowExA(0, "STATIC", NULL, WS_CHILD | WS_VISIBLE, 14, y, ww - 30, 44, r->win, NULL, hi, NULL); SetWindowTextW(st, wguide); free(wguide); y += 50; }
    else y += 10;
    CreateWindowExA(0, "BUTTON", "确定（Enter）", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, ww - 210, wh - 56, 90, 30, r->win, (HMENU)1, hi, NULL);
    CreateWindowExA(0, "BUTTON", "取消（Esc）", WS_CHILD | WS_VISIBLE, ww - 110, wh - 56, 90, 30, r->win, (HMENU)2, hi, NULL);
    EnableWindow(g_hwnd, FALSE);
    ShowWindow(r->win, SW_SHOW); SetForegroundWindow(r->win); SetFocus(r->edit);
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN && GetFocus() == r->edit) { SendMessageA(r->win, WM_COMMAND, 1, 0); continue; }
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) { SendMessageA(r->win, WM_COMMAND, 2, 0); continue; }
        TranslateMessage(&m); DispatchMessageA(&m);
    }
    EnableWindow(g_hwnd, TRUE); SetForegroundWindow(g_hwnd);
    SetEvent(r->done);
}
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!g_hwnd) return -1;
    tin_req r = {0};
    r.title = title; r.sub = sub; r.guide = guide; r.initial = initial; r.max_chars = max_chars;
    r.out = out; r.out_cap = out_cap; r.done = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)tin_pump, &r, 0, NULL);
    if (!th) { CloseHandle(r.done); return -1; }
    WaitForSingleObject(r.done, INFINITE);
    CloseHandle(r.done); CloseHandle(th);
    return r.result;
}

// ---------- 引擎开关（与安卓同一套） ----------
// 这批默认值在 dm/android/jni_android.c 里逐项实测过（营地 60 帧靠的就是它们）：这块 Windows ARM 显卡同属 Adreno + Vulkan，
// 引擎又是同一份代码，所以 Windows 按同一套开。玩家自己设了同名环境变量就以他的为准（setenv overwrite=0）。
// Engine switches, the same set as Android: dm/android/jni_android.c measured each one (they are what holds 60 fps in the camp),
// and this Windows ARM GPU is Adreno + Vulkan on the same engine code, so Windows opens the same set. A user-set environment
// variable of the same name wins (setenv with overwrite=0).
static void env_default(const char *k, const char *v) { if (!getenv(k)) setenv(k, v, 0); }
static void tuning_defaults(void) {
    env_default("DM_3D_SCALE_DEFAULT", "0.85");   // 3D 按 0.85 渲染再上采样，界面仍按全分辨率（手机的「均衡」档） | render 3D at 0.85 and upscale; the UI stays full resolution (the phones' "balanced" level)
    env_default("DM_GFX_NATIVE_BC", "1");          // BC 贴图按原格式采样：GPU 少约 0.5 ms，图像内存 827 → 320 MB | BC textures sampled natively: ~0.5 ms less GPU, image memory 827 -> 320 MB
    env_default("DM_GFX_DEFER_EVENTS", "2");       // SignalEvent 不提交、留到槽退役时公布（安卓 7.7 → 19.5 帧的一条） | SignalEvents are not submitted, published when the slot retires (one of the 7.7 -> 19.5 fps steps on Android)
    env_default("DM_GFX_READ_PREFLIGHT", "0");     // 不在每个绘制前完成全部待写回：省掉每帧 5~6 次「提交完就等」 | no full writeback resolve before every draw: removes 5-6 submit-then-wait points per frame
    env_default("DM_GFX_XVIEW_GUARD", "1");        // 另一槽还在读的回读缓冲，改由 GPU 在它之后写 | readback buffers the other slot still reads are written by the GPU after it
    env_default("DM_GFX_VCACHE", "1");             // 顶点 / 索引的跨帧副本（依赖 nvn_dirty 的脏页跟踪） | cross-frame vertex/index copies (through nvn_dirty's dirty pages)
    env_default("DM_GFX_TEX_DIRTY", "1");          // 贴图内容检查走同一套脏页跟踪 | texture content checks through the same dirty-page tracking
    env_default("DM_GFX_ASYNC_POOL_RESET", "1");   // 命令池在另一线程重置 | command pools reset on another thread
    env_default("DM_GFX_UPLOAD_PROLOGUE", "1");    // 本提交没用过的图像整份上传排到提交前，不在帧中间打断通道 | whole uploads of unused images go ahead of the submission instead of breaking the pass mid-frame
    env_default("DM_GFX_SHADER_REUSE", "1");       // 着色器载入直接用预热线程改写好的 SPIR-V | shader loads take the SPIR-V the warm-up already rewrote
    env_default("DM_GFX_LDC_FOLD", "1");           // 常量缓冲下标可推出的换成常量（GPU 15.70 → 14.34 ms） | constant-buffer indexes that follow from low bits become constants (GPU 15.70 -> 14.34 ms)
    env_default("DM_GFX_FORWARD_TEMPS", "1");      // 只写一次的临时变量直接转发 | single-store temporaries are forwarded
    env_default("DM_GFX_NAN_MINMAX", "1");         // min / max / clamp 遇 NaN 取另一个操作数（Maxwell 规矩） | min/max/clamp take the other operand on NaN (the Maxwell rule)
    env_default("DM_HLE_SPIN_US", "30");           // 客体主线程短等待先自旋 30 µs（每帧约 45 次） | the guest main thread spins 30 us before sleeping (~45 short waits a frame)
    env_default("DM_GFX_PERF_SAMPLE", "0");        // 关掉逐帧分项计时：被计时的那一帧多花约 4 ms，安卓也因此关着（要看分项时 DM_GFX_PERF_SAMPLE=1 打开） | turn off the per-part frame timing: a timed frame costs about 4 ms more, which is why Android has it off too (set DM_GFX_PERF_SAMPLE=1 to look at the parts)
    // 前端路径理顺（dm_cursor.c）那套「替玩家按键」是给触屏定的：有存档就在主菜单替按 X 打开选角页（开机第一屏成了人物列表，原版是主菜单）、
    // 选角列表一收起又替按 X 重开（玩家在选角页按 A 选定后列表被重开，要再按一次才进得去 = 「按 A 在两个界面跳转」）。
    // 键盘 / 手柄自己就能走原版菜单，关掉这套：开机停在主菜单，A = 进入游戏 → 选角页，选角页 A = 开始游戏
    // The "press keys on the player's behalf" front-end path straightening (dm_cursor.c) is designed for touchscreens: with a save it presses X on the
    // main menu to open the character select page (so the first screen was the character list instead of the original main menu), and it presses X to
    // reopen the list as soon as it collapses (after pressing A to pick a character the list got reopened, so a second press was needed to get in —
    // "A jumps between the two screens"). Keyboard / controller can walk the original menu by themselves, so turn it off here:
    // boot stays on the main menu, A = Enter Game -> character select page, and A on the character select page = start the game
    env_default("DM_FE_AUTO", "0");
    // 桌面形态：布局按 Mac App 那套走原版手柄版（DM_APP），不切触屏版。触屏版有整套宿主自画的圆钮 / 底栏 / 按钮栏，Windows 没有那一层绘制；
    // 界面覆盖层里触屏版布局把这些控件整排挪到屏幕外，一旦切过去主菜单就只剩背景、键盘按 A 不响应（实测）。
    // 触屏浮层（DM_WIN_TOUCH=1）仍保持 DM_APP：开机要用手柄版才走得了主菜单 / 选角页，局内再由 front 请求切到触屏版（dm_padmode.c 的 g_lock 在
    // DM_WIN_TOUCH 下不锁）
    // Desktop form: use the original controller layouts the way the Mac App does (DM_APP) and never switch to the touch layouts. The touch version has a whole host-drawn
    // layer of round buttons / bottom bar / button ribbon that Windows does not draw, and the UI overlay's touch layouts move those widgets off screen as a block: switching
    // there leaves the main menu as bare background and the keyboard's A does nothing (measured).
    // The touch overlay (DM_WIN_TOUCH=1) still keeps DM_APP: boot needs the controller layout to walk the main menu / character select, and the front end asks for the
    // touch layout once in a run (dm_padmode.c's g_lock is not taken under DM_WIN_TOUCH)
    env_default("DM_APP", "1");
}

// ---------- 计时器精度 ----------
// Windows 缺省的定时器粒度是 15.6 ms：客体主线程的 pthread_cond_timedwait / nanosleep 超时都要按这个粒度整，
// 于是「差一点等到」的帧会整块多出 15~16 ms（营地实测整帧 13~15 ms 的基线，尖峰恰好落在 24~33 ms）。
// 安卓 / iOS 的定时器是 µs 级，没有这个坑。这里把进程的定时器精度提到 0.5 ms（NtSetTimerResolution），
// 再退一步用 winmm 的 timeBeginPeriod(1)；两者都失败也不影响功能，只是尖峰会回来。
// Timer resolution. Windows' default timer granularity is 15.6 ms, and every pthread_cond_timedwait / nanosleep timeout in the
// guest is rounded up to it, so a frame that just missed its wait gains a whole 15-16 ms (the camp's baseline frame is 13-15 ms
// and the spikes land at 24-33 ms, which is exactly one quantum). Android / iOS timers are microsecond-grade and never see this.
// Request 0.5 ms through NtSetTimerResolution, then fall back to winmm's timeBeginPeriod(1); if both fail nothing breaks, the
// spikes just come back.
static void raise_timer_resolution(void) {
    typedef long(__stdcall * ntstr_t)(unsigned long, int, unsigned long *);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll"); unsigned long cur = 0;
    ntstr_t set = ntdll ? (ntstr_t)(void *)GetProcAddress(ntdll, "NtSetTimerResolution") : NULL;
    if (set && set(5000ul, 1, &cur) >= 0) LOG("计时器精度：请求 0.5 ms，系统给的是 %.2f ms", cur / 10000.0);
    typedef unsigned int(__stdcall * tbp_t)(unsigned int);
    HMODULE winmm = LoadLibraryA("winmm.dll");
    tbp_t begin = winmm ? (tbp_t)(void *)GetProcAddress(winmm, "timeBeginPeriod") : NULL;
    if (begin) begin(1);
}

// ---------- GUI 子系统：没有控制台时把日志落到文件 ----------
// 双击 bat / 从资源管理器直接启动时，GUI 子系统进程没有控制台，stderr 无处可去，排障日志会全丢（上一版双击起就是这个问题：
// 进程秒退、日志零字节，完全看不出原因）。所以：既没有控制台窗口、STD_ERROR 也不是有效的重定向句柄时，把 stdout / stderr
// 都接到 DM_LOG_FILE（缺省当前目录的 dmhost.log）。从终端起、或用 Start-Process -RedirectStandardError 起的时候照旧走原输出。
// GUI subsystem: with no console the log has nowhere to go
// Started by double-clicking the bat or from Explorer, a GUI-subsystem process has no console and stderr lands nowhere, so every
// diagnostic line is lost (exactly what the last round hit: an instant exit with a zero-byte log and no clue why). So: when there is no
// console window and STD_ERROR_HANDLE is not a valid redirected handle, point stdout / stderr at DM_LOG_FILE (default dmhost.log in the
// current directory). Started from a terminal, or with Start-Process -RedirectStandardError, output stays where it was.
int dm_win_stdio_to_log(void) {
    if (GetConsoleWindow()) return 0;
    HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
    if (e && e != INVALID_HANDLE_VALUE) return 0;
    const char *p = getenv("DM_LOG_FILE");
    if (!p || !*p) p = "dmhost.log";
    if (!freopen(p, "w", stderr)) return -1;
    freopen(p, "a", stdout);
    LOG("日志落到 %s（没有控制台：GUI 子系统启动）", p);
    return 0;
}

// ---------- 宿主入口 ----------
void front_run(void (*guest)(void)) {
    // DM_WINDOW=0：不开窗口，当前线程直接跑客体（与 Mac 宿主的无窗口形态同一开关，排障用）
    // DM_WINDOW=0: no window, run the guest directly on the current thread (the same switch as the Mac host's windowless form, for debugging)
    const char *nowin = getenv("DM_WINDOW");
    if (nowin && *nowin == '0') { guest(); return; }
    raise_timer_resolution();
    tuning_defaults();
    dm_win_sched_init();
    if (create_window()) { fprintf(stderr, "[dm] 建窗口失败 errno=%d\n", errno); guest(); return; }
    char err[512] = "";
    if (dm_win_vk_initialize(4096, err, sizeof err)) {
        // 出图起不来（没有 Vulkan 驱动 / 设备不支持）也必须先让客体跑起来：没有呈现就只有逻辑，排障日志照样能看
        // Even if presentation cannot start (no Vulkan driver / unsupported device) the guest must still run: without presentation there is only logic, and the debug log is still readable
        LOG("【红】Vulkan 初始化失败：%s", err);
        MessageBoxA(g_hwnd, err, "Vulkan 不可用", MB_ICONERROR | MB_OK);
    } else LOG("呈现：Vulkan 已就绪（%s）", dm_win_vk_device_name());
    g_guest = guest;
    dm_win_pad_start();
    dm_win_touch_start(g_hwnd);   // DM_WIN_TOUCH=1 才真的接（自己看环境变量）| only connects when DM_WIN_TOUCH=1 (it checks the variable itself)
    // 排障：把映像基址打出来，崩溃日志里的绝对 pc 才能换算成 dmhost.exe 内的偏移（llvm-nm 对照）
    // Diagnostics: print the image base so absolute pcs in crash logs can be turned into offsets inside dmhost.exe (compare with llvm-nm)
    { extern IMAGE_DOS_HEADER __ImageBase; LOG("映像基址 %#llx", (unsigned long long)(uintptr_t)&__ImageBase); }
    if (pthread_create(&g_guest_th, NULL, guest_main_tramp, NULL)) { fprintf(stderr, "[dm] 起客体线程失败\n"); guest(); return; }
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageA(&m); }
    // 关窗口：有局在就先请游戏走一次它自己的存盘、等存档提交（做法同 Mac 壳 app_quit / iOS 壳进后台），再有序收尾。
    // 游戏自己的定时自动存盘约 5 分钟一次，直接关窗口会把这一段进度全丢掉；暂停菜单开着时游戏停拍，dm_save_wait 会临时放开暂停存这一次
    // Window closed: if a run is in progress, first ask the game to do one of its own saves and wait for the save commit (same as the Mac shell's app_quit / the iOS shell going to the background), then shut down in order.
    // The game's own autosave is about once every 5 minutes, so closing the window directly threw away all progress since then; when the pause menu is open the game stops ticking and dm_save_wait temporarily releases the pause for this one save
    LOG("窗口：关闭（客体%s）", atomic_load(&g_guest_running) ? "仍在跑" : "已结束");
    {
        int dm_ingame_now(void); int dm_uiscreen_ingame(void); int dm_save_wait(double max_s, uint64_t *commits, int *froze, double *ms);
        if (!dm_ingame_now() && !dm_uiscreen_ingame()) LOG("窗口：退出（不在局内，没有待存的进度）");
        else {
            uint64_t n = 0; int froze = 0; double ms = 0; int asked = dm_save_wait(3, &n, &froze, &ms);
            if (n && asked) LOG("窗口：退出前等游戏存盘 %.0f ms，期间存档提交 %llu 次%s", ms, (unsigned long long)n, froze ? "（原先暂停着：冻结世界存盘、已恢复暂停）" : "");
            else LOG("【红】窗口：未存盘退出——3 秒内没等到存档提交（%s%s）", asked ? "已请游戏存盘" : "一直没到能请存盘的状态（读取画面？）", froze ? "，原先暂停着、冻结放开过" : "");
        }
    }
    dm_host_shutdown("关窗口");
    if (atomic_load(&g_guest_running)) { Sleep(3000); }
    ExitProcess(0);
}
