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

static void push_input(void) {
    int32_t lx = (g_kd - g_ka) * 32767, ly = (g_kw - g_ks) * 32767;
    dm_input_set(g_btn, lx, ly, g_stick_rx, 0);
}
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
    *nx = x; *ny = y;
}
static void set_finger(int down) {
    if (down && !g_mouse_down) {
        g_mouse_down = 1; g_finger = 1;
    } else if (!down && g_mouse_down) {
        g_mouse_down = 0; g_finger = 0;
        dm_touch_set(NULL, 0);
        return;
    }
    if (g_mouse_down) { dm_touch t = {(uint32_t)g_finger, g_mouse_x, g_mouse_y}; dm_touch_set(&t, 1); }
}

// ---------- 窗口过程 ----------
static void on_key_msg(int vk, int down) {
    switch (vk) {
    case 'W': g_kw = down; break; case 'A': g_ka = down; break; case 'S': g_ks = down; break; case 'D': g_kd = down; break;
    }
    int b = vk_bit(vk);
    if (down && (b >= 0 || vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D')) dm_padmode_request_glyph(DM_GLYPH_KBD);
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
        if (wp != SIZE_MINIMIZED) { RECT rc; GetClientRect(h, &rc); dm_win_vk_notify_resize(); }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (msg == WM_SYSKEYDOWN && (wp == VK_F4 || wp == VK_MENU)) break;   // Alt+F4 / Alt 让系统处理 | let the system handle Alt+F4 / Alt
        on_key_msg((int)wp, 1); return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        on_key_msg((int)wp, 0); return 0;
    case WM_MOUSEMOVE: {
        int cx = (int)(short)LOWORD(lp), cy = (int)(short)HIWORD(lp);
        mouse_to_pic(cx, cy, &g_mouse_x, &g_mouse_y);
        if (g_mouse_down) set_finger(1);   // 拖动 = 手指移动 | drag = finger move
        return 0; }
    case WM_LBUTTONDOWN:
        SetCapture(h); mouse_to_pic((int)(short)LOWORD(lp), (int)(short)HIWORD(lp), &g_mouse_x, &g_mouse_y); set_finger(1); return 0;
    case WM_LBUTTONUP:
        ReleaseCapture(); set_finger(0); return 0;
    case WM_RBUTTONDOWN: g_btn |= DM_BTN_B; push_input(); return 0;
    case WM_RBUTTONUP: g_btn &= ~(uint64_t)DM_BTN_B; push_input(); return 0;
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
        g_stick_rx = d * 32767; push_input();
        SetTimer(h, 1, 80, NULL);   // 80 ms 后回中（松开摇杆） | recenter after 80 ms (release the stick)
        return 0; }
    case WM_TIMER:
        if (wp == 1) { KillTimer(h, 1); g_stick_rx = 0; push_input(); }
        return 0;
    case WM_SETFOCUS: return 0;
    case WM_KILLFOCUS:
        // 失焦时全抬起，避免「按住状态」卡在游戏里 | Release everything on focus loss so a held state does not stick in the game
        g_kw = g_ka = g_ks = g_kd = 0; g_btn = 0; g_stick_rx = 0; push_input(); if (g_mouse_down) set_finger(0);
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
    ShowWindow(g_hwnd, SW_SHOW); UpdateWindow(g_hwnd); SetForegroundWindow(g_hwnd); SetFocus(g_hwnd);
    const char *full = getenv("DM_WIN_FULL");
    if (full && *full == '1') ShowWindow(g_hwnd, SW_MAXIMIZE);
    return 0;
}
static void *guest_main_tramp(void *p) {
    (void)p;
    atomic_store(&g_guest_running, 1);
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

// ---------- 宿主入口 ----------
void front_run(void (*guest)(void)) {
    // DM_WINDOW=0：不开窗口，当前线程直接跑客体（与 Mac 宿主的无窗口形态同一开关，排障用）
    // DM_WINDOW=0: no window, run the guest directly on the current thread (the same switch as the Mac host's windowless form, for debugging)
    const char *nowin = getenv("DM_WINDOW");
    if (nowin && *nowin == '0') { guest(); return; }
    if (create_window()) { fprintf(stderr, "[dm] 建窗口失败 errno=%d\n", errno); guest(); return; }
    char err[512] = "";
    if (dm_win_vk_initialize(4096, err, sizeof err)) {
        // 出图起不来（没有 Vulkan 驱动 / 设备不支持）也必须先让客体跑起来：没有呈现就只有逻辑，排障日志照样能看
        // Even if presentation cannot start (no Vulkan driver / unsupported device) the guest must still run: without presentation there is only logic, and the debug log is still readable
        LOG("【红】Vulkan 初始化失败：%s", err);
        MessageBoxA(g_hwnd, err, "Vulkan 不可用", MB_ICONERROR | MB_OK);
    } else LOG("呈现：Vulkan 已就绪（%s）", dm_win_vk_device_name());
    g_guest = guest;
    // 排障：把映像基址打出来，崩溃日志里的绝对 pc 才能换算成 dmhost.exe 内的偏移（llvm-nm 对照）
    // Diagnostics: print the image base so absolute pcs in crash logs can be turned into offsets inside dmhost.exe (compare with llvm-nm)
    { extern IMAGE_DOS_HEADER __ImageBase; LOG("映像基址 %#llx", (unsigned long long)(uintptr_t)&__ImageBase); }
    if (pthread_create(&g_guest_th, NULL, guest_main_tramp, NULL)) { fprintf(stderr, "[dm] 起客体线程失败\n"); guest(); return; }
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageA(&m); }
    // 关窗口：已进局且还有没提交的进度，由 dm_host_shutdown 走正常收尾（存档提交完再退）
    // Window closed: if a run is in progress with uncommitted progress, dm_host_shutdown does the orderly shutdown (commit the save, then exit)
    LOG("窗口：关闭（客体%s）", atomic_load(&g_guest_running) ? "仍在跑" : "已结束");
    dm_host_shutdown("关窗口");
    if (atomic_load(&g_guest_running)) { Sleep(3000); }
    ExitProcess(0);
}
