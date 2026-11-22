// Windows 触屏操控浮层（接口见 touch_win.h）。
// 状态机与布局 = 安卓 / iOS / 鸿蒙共用的 dm/ios/Sources/dm_touch_controls.c；这里只做三件宿主该做的事：
//   1) 把鼠标 / 触摸屏的事件当手指喂进去（dm_tc_begin/move/end）；
//   2) 在取手柄那一拍（dm_pad_hook）推进状态机，把按键、摇杆、瞄准轴发布出去（照安卓 touch_android.c 的写法：按键按位或、轴非零才采纳，并按「按沿锁存」，
//      免得比一次取手柄还短的点按被游戏漏掉）；
//   3) 自己画：GDI+ 画到 32 位预乘位图，UpdateLayeredWindow 贴成一张分层窗口，透明处点击穿透（输入仍由游戏窗口收，见 front_win.c）。
// Windows touch-control overlay (interface in touch_win.h).
// State machine and layout come from dm/ios/Sources/dm_touch_controls.c, the copy Android / iOS / HarmonyOS share; this file only does the three
// things the host owes it:
//   1) feed mouse / touchscreen events in as fingers (dm_tc_begin/move/end);
//   2) drive the state machine on the controller-read tick (dm_pad_hook) and publish buttons / sticks / aim axes the way Android's
//      touch_android.c does (buttons OR-ed in, axes taken only when non-zero, plus "press edge latching" so a tap shorter than one controller
//      read is not lost by the game);
//   3) draw it itself: GDI+ into a 32-bit premultiplied bitmap, blitted by UpdateLayeredWindow onto a layered window; transparent areas are
//      click-through (input still arrives at the game window, see front_win.c).
#define _CRT_SECURE_NO_WARNINGS
#include "touch_win.h"

#include <windows.h>
#include <ole2.h>      // mingw 的 gdiplus 头要用 ole2 里的 PROPID | mingw's gdiplus header needs PROPID out of ole2
#include <gdiplus.h>

#include <atomic>
#include <math.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "../host/dm.h"
#include "../host/dm_hud.h"
#include "../host/dm_i18n.h"
#include "../host/dm_input.h"
#include "../ios/Sources/dm_touch_controls.h"
}
// dm_padmode 没有公共头，各家前端各自声明（它与 iOS / Mac 同一个实现）| dm_padmode has no public header; each front end declares it (same implementation as iOS / Mac)
extern "C" void dm_padmode_request(int pad);
extern "C" int dm_padmode_current(void);
extern "C" void dm_tp_request(void);   // 回城钮（dm_freetp.c）| the town portal button (dm_freetp.c)

using namespace Gdiplus;

// ---------- 状态 ----------
static std::mutex g_mx;
static dm_tc_state g_tc;
static int g_on;                       // DM_WIN_TOUCH=1 | enabled
static std::atomic<int> g_in_run;      // 局内（浮层可见、触屏布局） | in a run (overlay visible, touch layout)
static std::atomic<int> g_always;      // DM_WIN_TOUCH_ALWAYS=1：前端页面也用触屏布局（页面按钮栏还没有，测试用） | touch layout in the front end too (its ribbon is not implemented yet; for testing)
static HWND g_game, g_ov;
static int g_vx, g_vy, g_vw, g_vh;     // 视图矩形（客户区像素） | view rectangle (client pixels)
static float g_px = 1.0f;              // 每个 dp 几个像素 | pixels per dp
static float g_dpw, g_dph;             // 布局点数（dm_tc 的坐标空间） | layout points (dm_tc's coordinate space)
static uint64_t g_latch;               // 两次取手柄之间按着过的键 | keys held since the last controller read
static std::atomic<int> g_dirty;
static std::atomic<int> g_dump_req;    // 按键组合变了：下一张位图要落盘（按住态取证） | the button set changed: dump the next bitmap (to capture the held look)
static double g_move_x = -1, g_move_y = -1;   // 上次重画时摇杆头的位置 | thumb position at the last repaint

// ---------- 场景（进局 / 出局）----------
// 局内才用触屏布局：前端页面（主菜单 / 选角）的按钮栏、键位提示是各家前端自己的 UIKit / Java 层画的，Windows 还没做那一层，
// 用触屏布局会只剩背景（README 坑 6）。所以开机仍是手柄版，读到玩家（dm_hud_info.valid）切触屏，出去再切回手柄版。
// The touch layout is used only in a run: the front-end pages' (main menu / character select) button ribbon and key hints are drawn by each
// platform's own UIKit / Java layer and Windows does not have that layer yet, so the touch layout would leave only the background there
// (README pitfall 6). Boot therefore stays on the controller layout, switching to touch once the player has been read (dm_hud_info.valid)
// and back to the controller layout on leaving.
static void scene_apply(void) {
    dm_hud_info h; memset(&h, 0, sizeof h);
    dm_hud_get(&h);
    int in_run = h.valid ? 1 : 0;
    if (g_always) in_run = 1;
    if (in_run == g_in_run.load()) return;
    g_in_run.store(in_run);
    dm_padmode_request(in_run ? 0 : 1);   // 局内 = 触屏版，局外 = 手柄版 | in a run = touch layout, outside = controller layout
    g_dirty.store(1);
    LOG("触屏浮层：%s（手柄布局 %d）", in_run ? "进局，切触屏布局并显示浮层" : "出局，切回手柄布局", dm_padmode_current());
}

// ---------- 布局 ----------
static void layout_make(void) {
    float px = g_vh > 0 ? (float)g_vh / 375.0f : 1.0f;   // 375 点 = 手机横屏高度 | 375 points = phone landscape height
    if (px > 3.0f) px = 3.0f;                            // 4K 全屏时控件不再跟着屏高无限放大 | capped so 4K fullscreen does not blow the controls up
    if (px < 0.5f) px = 0.5f;
    g_px = px;
    g_dpw = g_vw > 0 ? (float)g_vw / px : 0.0f;
    g_dph = g_vh > 0 ? (float)g_vh / px : 0.0f;
    dm_tc_resize(&g_tc, g_dpw, g_dph);
}

// ---------- 取手柄那一拍：推进状态机并发布 ----------
static void pad_step(uint64_t now, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    if (!g_on || !g_in_run.load()) return;   // 浮层没开 / 不在局内：照旧让手柄与键盘的输入走 | overlay off / not in a run: leave pad and keyboard input alone
    std::lock_guard<std::mutex> lk(g_mx);
    dm_tc_tick(&g_tc, (double)now / 1e9);
    // 取证：按键组合变化打一行（无桌面截屏权限时，这是「点圆钮真的变成按键」的直接证据）| evidence: log each button-set change (the direct proof
    // that tapping a round button becomes a key press when the desktop cannot be captured)
    static uint64_t last_btn;
    uint64_t out = g_tc.buttons | g_latch;
    if (out != last_btn) { LOG("触屏浮层：按键 %#llx → %#llx（摇杆 %d,%d 瞄准 %d,%d）", (unsigned long long)last_btn, (unsigned long long)out, g_tc.lx, g_tc.ly, g_tc.rx, g_tc.ry); last_btn = out; g_dirty.store(1); g_dump_req.store(1); }
    *buttons |= out;
    g_latch = 0;
    if (!*lx && !*ly) { *lx = g_tc.lx; *ly = g_tc.ly; }
    if (!*rx && !*ry) { *rx = g_tc.rx; *ry = g_tc.ry; }
}

// ---------- 手指 ----------
void dm_win_touch_pointer(int op, float nx, float ny, int slot) {
    if (!g_on) return;
    float x = nx * g_dpw, y = ny * g_dph;
    double now = (double)now_ns() / 1e9;
    std::lock_guard<std::mutex> lk(g_mx);
    if (op == 0) {
        dm_tc_begin_at(&g_tc, slot, x, y, now);
        if (getenv("DM_WIN_TOUCH")) LOG("触屏浮层：手指按下 点(%g,%g) 目标 %d（画布 %gx%g 点，缩放 %g）", x, y, g_tc.finger[slot].target, g_dpw, g_dph, g_px);
        // 回城钮不发按键，点下由宿主施放原版回城技能（dm_freetp.c）| the town portal button sends no key; tapping casts the original town portal on the host side
        if (!g_tc.navigation_mode && g_tc.finger[slot].target == DM_TC_PORTAL) { dm_tc_cancel_finger(&g_tc, slot); dm_tp_request(); }
    } else if (op == 1) dm_tc_move_at(&g_tc, slot, x, y, now);
    else dm_tc_end_at(&g_tc, slot, now);
    g_latch |= g_tc.buttons;
    g_dirty.store(1);
}

void dm_win_touch_cancel(void) {
    if (!g_on) return;
    std::lock_guard<std::mutex> lk(g_mx);
    dm_tc_cancel_all(&g_tc);
    g_latch = 0;
    g_dirty.store(1);
}

// ---------- 绘制 ----------
static HDC g_mdc; static HBITMAP g_dib; static void *g_bits; static int g_bw, g_bh;
static ULONG_PTR g_gdip;
static int g_gdip_on;
static void dump_bmp(void);

static void surface_free(void) {
    if (g_mdc) { DeleteDC(g_mdc); g_mdc = NULL; }
    if (g_dib) { DeleteObject(g_dib); g_dib = NULL; }
    g_bits = NULL; g_bw = g_bh = 0;
}

static int surface_make(int w, int h) {
    if (w <= 0 || h <= 0) return -1;
    if (w == g_bw && h == g_bh && g_bits) return 0;
    surface_free();
    HDC screen = GetDC(NULL);
    BITMAPINFO bmi; memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w; bmi.bmiHeader.biHeight = -h;   // 自上而下 | top-down
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    g_dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!g_dib) return -1;
    g_mdc = CreateCompatibleDC(NULL);
    SelectObject(g_mdc, g_dib);
    g_bw = w; g_bh = h;
    return 0;
}

static void wide(const char *s, wchar_t *out, int n) {
    if (n <= 1) return;
    if (!s) { out[0] = 0; return; }
    int r = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n - 1);
    out[n - 1] = 0;
    if (r <= 0) { out[0] = 0; return; }
    out[r] = 0;
}

static Color col(int r, int g, int b, int a) { return Color(a, r, g, b); }

// 按钮上的字：技能钮写键位字母（没有技能图标时才这么画，和 iOS 取不到图时回退的标签同理）
// Text on the buttons: skill buttons carry the key letter (only drawn when there is no skill icon, the same fallback the iOS labels use)
static const char *key_letter(uint64_t b) {
    switch (b) {
    case DM_BTN_A: return "A"; case DM_BTN_B: return "B"; case DM_BTN_X: return "X"; case DM_BTN_Y: return "Y";
    case DM_BTN_R: return "R"; case DM_BTN_L: return "L"; case DM_BTN_ZL: return "ZL"; case DM_BTN_ZR: return "ZR";
    case DM_BTN_LEFT: return "\xE2\x86\x90"; case DM_BTN_UP: return "\xE2\x86\x91";       // ← ↑
    case DM_BTN_RIGHT: return "\xE2\x86\x92"; case DM_BTN_DOWN: return "\xE2\x86\x93";     // → ↓
    case DM_BTN_MINUS: return "\xE2\x88\x92"; case DM_BTN_PLUS: return "+";
    }
    return "";
}

static void caption(Graphics &g, const char *utf8, float cx, float cy, float size, Color c) {
    wchar_t w[64]; wide(utf8, w, 64);
    if (!w[0]) return;
    Font f(L"Segoe UI", size, FontStyleRegular, UnitPixel);
    StringFormat fmt; fmt.SetAlignment(StringAlignmentCenter); fmt.SetLineAlignment(StringAlignmentCenter);
    SolidBrush br(c);
    g.DrawString(w, -1, &f, PointF(cx, cy - size * 0.55f), &fmt, &br);
}

static void shell(Graphics &g, float cx, float cy, float r, float held) {
    (void)g; (void)cx; (void)cy; (void)r; (void)held;
}

// ---------- 控件外观：照 dm/ios/Sources/DmTouchControlArt.h 的矢量配方（GDI+ 版） ----------
// Control art: the vector recipe from dm/ios/Sources/DmTouchControlArt.h, ported to GDI+
static void grad_v(Graphics &g, const RectF &r, const Color *c, const REAL *pos, int n) {
    // 竖直多色标渐变：mingw 的 gdiplus 头里没有 ColorBlend，逐行插值填（每次只画几十到一百行，代价可忽略）
    // Vertical multi-stop gradient: mingw's gdiplus headers have no ColorBlend, so interpolate row by row (a few dozen to a hundred fills
    // per control, which does not matter here)
    int rows = (int)(r.GetBottom() - r.GetTop());
    if (rows <= 0) return;
    for (int i = 0; i < rows; ++i) {
        float t = (rows == 1) ? 0.0f : (float)i / (float)(rows - 1);
        int seg = 0;
        while (seg + 2 < n && t > pos[seg + 1]) ++seg;
        float span = pos[seg + 1] - pos[seg];
        float u = span > 0 ? (t - pos[seg]) / span : 0.0f;
        if (u < 0) u = 0; if (u > 1) u = 1;
        auto mix = [](int a, int b, float u) { return (int)(a + (b - a) * u + 0.5f); };
        Color v(col(mix(c[seg].GetR(), c[seg + 1].GetR(), u), mix(c[seg].GetG(), c[seg + 1].GetG(), u),
                    mix(c[seg].GetB(), c[seg + 1].GetB(), u), mix(c[seg].GetA(), c[seg + 1].GetA(), u)));
        SolidBrush row(v);
        g.FillRectangle(&row, RectF(r.X, r.Y + (REAL)i, r.Width, 1.0f));
    }
}

static void ellipse_grad(Graphics &g, float cx, float cy, float r, const Color *c, const REAL *pos, int n) {
    GraphicsPath p; p.AddEllipse(cx - r, cy - r, r * 2, r * 2);
    g.SetClip(&p);
    RectF rect(cx - r, cy - r, r * 2, r * 2);
    grad_v(g, rect, c, pos, n);
    g.ResetClip();
}

// dm_button_draw：窄铁边 + 暗灰内槽 + 主钮多一道环；空槽改画十字
// dm_button_draw: a narrow iron rim, a charcoal well and an extra ring for the primary button; empty slots draw a cross instead
static void button_draw(Graphics &g, float cx, float cy, float r, int held, int empty, int primary) {
    float side = r * 2;
    RectF rim(cx - r + side * .018f, cy - r + side * .018f, side - side * .036f, side - side * .036f);
    RectF face(cx - r + side * .055f, cy - r + side * .055f, side - side * .11f, side - side * .11f);
    int ea = empty ? 1 : 0;
    // 外圈金属：极淡的暗红底 + 冷铁高光 | the metal rim: a faint dark-red undertone with a cold-iron highlight
    Color metal[3] = { col(66, 71, 71, ea ? 56 : 230), col(14, 17, 18, ea ? 56 : 242), col(41, 28, 26, ea ? 41 : 178) };
    REAL mp[3] = { 0.0f, 0.62f, 1.0f };
    ellipse_grad(g, cx, cy, r - side * .018f, metal, mp, 3);
    Pen rimpen(col(2, 3, 4, ea ? 97 : 230), side * .025f);
    g.DrawEllipse(&rimpen, rim);
    // 内槽：中心比边缘更暗，按下时不变（iOS 只改上层的细边）| the well: darker at the centre than at the edge, unchanged when held (iOS only changes the upper hairline)
    Color well[2] = { col(5, 5, 5, ea ? 20 : 240), col(13, 13, 13, ea ? 41 : 240) };
    REAL wp[2] = { 0.0f, 1.0f };
    ellipse_grad(g, cx, cy, r - side * .055f, well, wp, 2);
    Pen hair(empty ? col(158, 158, 158, held ? 204 : 153) : col(held ? 163 : 66, held ? 163 : 74, held ? 163 : 71, held ? 184 : 140),
             side * (held ? .018f : (empty ? .012f : .009f)));
    g.DrawEllipse(&hair, face);
    if (primary && !empty) { Pen pr(col(79, 59, 56, 133), side * .012f); float rr = r - side * .035f; g.DrawEllipse(&pr, cx - rr, cy - rr, rr * 2, rr * 2); }
    if (empty) {
        float arm = side * .055f;
        Pen cr(col(153, 166, 163, held ? 191 : 64), side * .018f); cr.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
        g.DrawLine(&cr, cx - arm, cy, cx + arm, cy);
        g.DrawLine(&cr, cx, cy - arm, cx, cy + arm);
    }
}

// dm_joystick_base_draw：底盘向边缘渐隐 + 四段方向刻纹（含缺口）| the base feathers out and carries four directional etch marks with gaps
static void stick_base_draw(Graphics &g, float cx, float cy, float r, int active) {
    float side = r * 2;
    // GDI+ 的路径渐变只有中心 / 边界两色，用内层实填 + 外层渐隐两笔近似原版的三档径向渐变
    // A GDI+ path gradient carries only a centre and a boundary colour, so an inner fill plus a feathered path gradient approximates the three-stop radial
    SolidBrush haze(col(6, 6, 6, active ? 56 : 30));
    g.FillEllipse(&haze, cx - side * .375f, cy - side * .375f, side * .75f, side * .75f);
    GraphicsPath outer; outer.AddEllipse(cx - side * .48f, cy - side * .48f, side * .96f, side * .96f);
    PathGradientBrush pg(&outer);
    pg.SetCenterColor(col(6, 6, 6, active ? 20 : 8));
    Color edge[1] = { col(6, 6, 6, 0) }; int n1 = 1; pg.SetSurroundColors(edge, &n1);
    g.FillPath(&pg, &outer);
    Pen arc(col(161, 161, 161, active ? 31 : 14), side * .004f);
    RectF ring(cx - side * .416f, cy - side * .416f, side * .832f, side * .832f);
    float x = side * .414f, k = side * .028f;
    for (int i = 0; i < 4; ++i) {
        g.DrawArc(&arc, ring, (REAL)(i * 90 + 11.5f), (REAL)(90.0f - 22.9f));
        // 刻纹本体：一个小箭头加一条短尾 | the etching itself: a small chevron plus a short tail
        GraphicsState st = g.Save();
        g.TranslateTransform(cx, cy);
        g.RotateTransform((REAL)(i * 90));
        Pen ch(col(173, 173, 173, active ? 64 : 33), side * .004f); ch.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
        GraphicsPath chev;
        chev.AddLine((REAL)(x + k), 0.0f, (REAL)(x - k), (REAL)(-k * .65f));
        chev.AddLine((REAL)(x - k), (REAL)(-k * .65f), (REAL)(x - k * .42f), 0.0f);
        chev.AddLine((REAL)(x - k * .42f), 0.0f, (REAL)(x - k), (REAL)(k * .65f));
        chev.AddLine((REAL)(x - k), (REAL)(k * .65f), (REAL)(x + k), 0.0f);
        chev.AddLine((REAL)(x - k * .42f), 0.0f, (REAL)(x - k * 2.1f), 0.0f);
        g.DrawPath(&ch, &chev);
        g.Restore(st);
    }
}


// dm_joystick_thumb_draw：低反光黑灰摇杆头 + 弧形高光 | a matte charcoal thumb with a curved highlight
static void stick_thumb_draw(Graphics &g, float cx, float cy, float r, int active) {
    float side = r * 2, rr = r - side * .055f;
    Color fill[3] = { col(59, 59, 59, active ? 199 : 117), col(36, 36, 36, active ? 209 : 125), col(17, 17, 17, active ? 214 : 133) };
    REAL fp[3] = { 0.0f, 0.55f, 1.0f };
    ellipse_grad(g, cx, cy, rr, fill, fp, 3);
    Pen rim(col(6, 6, 6, active ? 166 : 82), side * .035f);
    g.DrawEllipse(&rim, cx - rr, cy - rr, rr * 2, rr * 2);
    Pen hi(col(163, 163, 163, active ? 102 : 56), side * .014f);
    RectF a1(cx - side * .437f, cy - side * .437f, side * .874f, side * .874f);
    g.DrawArc(&hi, a1, 192.6f, 48.2f);
    Pen lo(col(107, 107, 107, active ? 51 : 26), side * .012f);
    RectF a2(cx - side * .410f, cy - side * .410f, side * .820f, side * .820f);
    g.DrawArc(&lo, a2, 201.6f, 43.5f);
}

static void paint(void) {
    if (!g_ov || g_vw <= 0 || g_vh <= 0) return;
    if (surface_make(g_vw, g_vh) != 0) return;
    memset(g_bits, 0, (size_t)g_vw * g_vh * 4);
    {
        // 位图直接包在 DIB 上（预乘 alpha = UpdateLayeredWindow 要的格式）| the bitmap wraps the DIB directly (premultiplied alpha is what UpdateLayeredWindow wants)
        Bitmap bmp(g_vw, g_vh, g_vw * 4, PixelFormat32bppPARGB, (BYTE *)g_bits);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);

        dm_tc_state s;
        uint64_t seen;
        { std::lock_guard<std::mutex> lk(g_mx); s = g_tc; seen = g_tc.enabled_targets; }
        (void)seen;
        const dm_tc_layout &l = s.layout;
        float k = 1.0f;                 // 布局点数 → 像素 | layout points to pixels
        float px = g_px;

        // 1) 摇杆：底盘 + 头（底盘位置 = 按着时的落点，否则静止位）
        // 1) the stick: base plus thumb (base position = the touch point while pressed, otherwise the rest position)
        if (l.joy_radius > 0 && !s.navigation_mode) {
            float ox = s.joy_slot >= 0 ? s.joy_origin_x : l.joy_x;
            float oy = s.joy_slot >= 0 ? s.joy_origin_y : l.joy_y;
            float r = l.joy_radius * px;
            stick_base_draw(g, ox * px, oy * px, r, s.joy_slot >= 0);
            float tx = ox * px + (float)s.lx / 32767.0f * r, ty = oy * px - (float)s.ly / 32767.0f * r;
            float tr = 23.0f * (l.joy_radius / 60.0f) * px;
            stick_thumb_draw(g, tx, ty, tr, s.joy_slot >= 0);
            (void)k;
        }

        // 2) 圆钮：主技能 / 四个技能 / 回城 / 地图。腰带四列与功能入口是底栏的格子（游戏自己画），这里只留命中区
        // 2) round buttons: main / the four skills / town portal / map. The belt columns and the function entry are bottom-bar cells (the game
        //    draws them), so only their hit areas remain here
        for (int i = 0; i < DM_TC_COUNT; ++i) {
            if (!(s.enabled_targets & (1u << i))) continue;
            if (i > DM_TC_PORTAL && i <= DM_TC_MENU) continue;
            const dm_tc_target &t = l.target[i];
            if (t.r <= 0) continue;
            uint64_t b = t.button;
            int held = (b && (s.buttons & b) == b) ? 1 : 0;
            for (int f = 0; f < DM_TC_MAX_TOUCH && !held; ++f)
                if (s.finger[f].active && !s.finger[f].aim_cancelled && s.finger[f].target == i) held = 1;
            float cx = t.x * px, cy = t.y * px, r = t.r * px;
            button_draw(g, cx, cy, r, held, 0, i == DM_TC_MAIN);
            if (i <= DM_TC_SKILL4 || i == DM_TC_PORTAL) {
                const char *lbl = i == DM_TC_PORTAL ? (dm_lang_zh() ? "\xE5\x9B\x9E\xE5\x9F\x8E" : "TP") : key_letter(b);
                // 回城写两个汉字，字号要压到圆内 | the town portal label is two CJK characters, so the font has to stay inside the circle
                caption(g, lbl, cx, cy, (i == DM_TC_PORTAL ? r * 0.46f : (i == DM_TC_MAIN ? r * 0.62f : r * 0.72f)), col(217, 201, 166, held ? 255 : 214));
            } else {
                caption(g, dm_ui("dmTcMap"), cx, cy, r * 0.44f, col(217, 201, 166, held ? 255 : 214));
            }
            if (i == DM_TC_MAIN) {
                // 主钮下面一行小字，和 iOS 的技能名标签同位 | one line of small text under the main button, in the same place as the iOS skill-name label
                caption(g, dm_lang_zh() ? "\xE6\x94\xBB\xE5\x87\xBB" : "ATTACK", cx, cy + r * 0.80f, r * 0.24f, col(200, 186, 152, 170));
            }
        }

        // 3) 技能组切换（Ⅰ / Ⅱ）| 3) skill set switch (I / II)
        if (!s.navigation_mode && l.swap.r > 0) {
            float cx = l.swap.x * px, cy = l.swap.y * px, r = l.swap.r * px;
            int held = 0;
            for (int f = 0; f < DM_TC_MAX_TOUCH && !held; ++f)
                if (s.finger[f].active && s.finger[f].target == DM_TC_SWAP) held = 1;
            button_draw(g, cx, cy, r, held, 0, 0);
            caption(g, s.skill_set ? "\xE2\x85\xA1" : "\xE2\x85\xA0", cx, cy, r * 0.9f, col(217, 201, 166, 230));   // Ⅱ / Ⅰ
        }

        // 4) 难度选择条：展开时画 8 格数字（当前档不透明）| 4) difficulty strip: eight numbered cells while expanded (the current level opaque)
        if (s.diff_open) {
            dm_hud_info h; memset(&h, 0, sizeof h); dm_hud_get(&h);
            for (int i = 0; i < 8; ++i) {
                const dm_tc_target &p = l.pick[i];
                float cx = p.x * px, cy = p.y * px, r = p.r * px;
                int cur = (h.players == i + 1);
                SolidBrush fill(col(10, 9, 7, cur ? 236 : 196));
                Pen rim(col(168, 135, 82, cur ? 230 : 150), 1.4f);
                g.FillEllipse(&fill, cx - r, cy - r, r * 2, r * 2);
                g.DrawEllipse(&rim, cx - r, cy - r, r * 2, r * 2);
                char n[8]; snprintf(n, sizeof n, "%d", i + 1);
                caption(g, n, cx, cy, r * 0.9f, col(226, 214, 182, cur ? 255 : 190));
            }
        }
    }

    POINT src = {0, 0};
    POINT dst; ClientToScreen(g_game, &dst);
    dst.x += g_vx; dst.y += g_vy;
    SIZE sz = {g_vw, g_vh};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_ov, NULL, &dst, &sz, g_mdc, &src, 0, &bf, ULW_ALPHA);
    dump_bmp();
}

// 取证：DM_WIN_TOUCH_DUMP=<路径> 把浮层自己的位图存成 BMP（第一次、之后每 30 次一次）。
// 本机截屏（CopyFromScreen）拿到的是全黑，Vulkan 读回（DM_WIN_SHOT）又不含分层窗口，所以只能自证。
// Evidence: DM_WIN_TOUCH_DUMP=<path> saves the overlay's own bitmap as a BMP (the first one, then every 30th). Screen capture
// (CopyFromScreen) comes back all black here and the Vulkan readback (DM_WIN_SHOT) never contains a layered window, so it has to
// prove itself this way.
static void dump_bmp(void) {
    static int n;
    const char *p = getenv("DM_WIN_TOUCH_DUMP");
    if (!p || !*p || !g_bits) return;
    ++n;
    // 除了「第一次 + 每 5 次」，按键组合一变也存一张（按住态的观感要看这一刻）| besides "first + every 5th", a button-set change also dumps one
    // (the held look only exists at that instant)
    int forced = g_dump_req.exchange(0);
    if (n != 1 && n % 30 != 0 && !forced) return;
    char path[512];
    if (strstr(p, "%d")) snprintf(path, sizeof path, p, n);
    else snprintf(path, sizeof path, "%s", p);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    BITMAPFILEHEADER fh; memset(&fh, 0, sizeof fh);
    BITMAPINFOHEADER ih; memset(&ih, 0, sizeof ih);
    ih.biSize = sizeof ih; ih.biWidth = g_bw; ih.biHeight = g_bh; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    ih.biSizeImage = (uint32_t)g_bw * g_bh * 4;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    // DIB 自上而下、BMP 自下而上：逐行倒着写（alpha 原样带上，PNG 转换用得上）| DIB is top-down, BMP bottom-up: write rows in reverse (alpha kept for the PNG conversion)
    for (int y = g_bh - 1; y >= 0; --y) fwrite((const char *)g_bits + (size_t)y * g_bw * 4, 4, (size_t)g_bw, f);
    fclose(f);
    LOG("触屏浮层：位图已存 %s（%dx%d）", path, g_bw, g_bh);
}

// ---------- 窗口 ----------
static LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;   // 点击穿透到游戏窗口 | clicks fall through to the game window
    return DefWindowProcA(h, m, w, l);
}

void dm_win_touch_layout(int vx, int vy, int vw, int vh) {
    if (!g_on) return;
    g_vx = vx; g_vy = vy; g_vw = vw; g_vh = vh;
    { std::lock_guard<std::mutex> lk(g_mx); layout_make(); }
    g_dirty.store(1);
}

void dm_win_touch_tick(void) {
    if (!g_on) return;
    scene_apply();
    if (!g_in_run.load()) {
        if (IsWindowVisible(g_ov)) ShowWindow(g_ov, SW_HIDE);
        return;
    }
    if (!IsWindowVisible(g_ov)) { ShowWindow(g_ov, SW_SHOWNA); g_dirty.store(1); }
    // 摇杆头没动、状态没变就不重画（拖动时每帧一张全屏位图太贵）| skip repainting when neither the thumb nor anything else moved
    float tx, ty;
    { std::lock_guard<std::mutex> lk(g_mx);
      float r = g_tc.layout.joy_radius;
      float ox = g_tc.joy_slot >= 0 ? g_tc.joy_origin_x : g_tc.layout.joy_x;
      float oy = g_tc.joy_slot >= 0 ? g_tc.joy_origin_y : g_tc.layout.joy_y;
      tx = ox + g_tc.lx / 32767.0f * r; ty = oy - g_tc.ly / 32767.0f * r;
      if (g_tc.joy_slot >= 0) g_dirty.store(1); }
    if (fabsf(tx - g_move_x) > 0.25f || fabsf(ty - g_move_y) > 0.25f) { g_move_x = tx; g_move_y = ty; g_dirty.store(1); }
    if (!g_dirty.exchange(0)) return;
    paint();
}

void dm_win_touch_start(void *game_hwnd) {
    if (g_on) return;
    g_game = (HWND)game_hwnd;
    if (!g_game) return;
    const char *e = getenv("DM_WIN_TOUCH");
    if (!e || strcmp(e, "1") != 0) return;
    g_always = getenv("DM_WIN_TOUCH_ALWAYS") && strcmp(getenv("DM_WIN_TOUCH_ALWAYS"), "1") == 0;

    if (!g_gdip_on) {
        GdiplusStartupInput in; if (GdiplusStartup(&g_gdip, &in, NULL) != Ok) { LOG("触屏浮层：GDI+ 起不来，浮层不画"); return; }
        g_gdip_on = 1;
    }
    HINSTANCE hi = GetModuleHandleA(NULL);
    WNDCLASSEXA wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.lpfnWndProc = ov_proc; wc.hInstance = hi;
    wc.lpszClassName = "dmhost_touch_overlay";
    if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    // WS_EX_TRANSPARENT：鼠标事件全部落到下面的游戏窗口；WS_EX_NOACTIVATE：点它不抢焦点（本来也点不到）
    // WS_EX_TRANSPARENT: every mouse event falls to the game window below; WS_EX_NOACTIVATE: it never takes the focus (unreachable anyway)
    g_ov = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                           wc.lpszClassName, "dmhost touch overlay", WS_POPUP, 0, 0, 1, 1, g_game, NULL, hi, NULL);
    if (!g_ov) { LOG("触屏浮层：覆盖层窗口建不出来（%lu）", GetLastError()); return; }

    { std::lock_guard<std::mutex> lk(g_mx); dm_tc_init(&g_tc, 667.0f, 375.0f); }
    dm_pad_hook = pad_step;
    g_on = 1;
    RECT rc; GetClientRect(g_game, &rc);
    dm_win_touch_layout(0, 0, rc.right, rc.bottom);
    SetTimer(g_game, 3, 30, NULL);   // 30 ms：重画与进 / 出局切换 | 30 ms: repaint and switch layouts on entering / leaving a run
    LOG("触屏浮层：已接（DM_WIN_TOUCH=1，局内显示；DM_WIN_TOUCH_ALWAYS=1 前端页面也用触屏布局）");

    // ---------- 冒烟测试：DM_TOUCH_TEST='<秒>,<目标>' 在开机后按一下某个圆钮（无人操作时验证整条链）----------
    // ---------- smoke test: DM_TOUCH_TEST='<seconds>,<target>' taps one round button after boot (verifies the chain with nobody at the machine) ----------
}

void dm_win_touch_stop(void) {
    if (!g_on) return;
    g_on = 0;
    if (dm_pad_hook == pad_step) dm_pad_hook = NULL;
    if (g_game) KillTimer(g_game, 3);
    if (g_ov) { DestroyWindow(g_ov); g_ov = NULL; }
    surface_free();
}

int dm_win_touch_enabled(void) { return g_on && g_in_run.load(); }
