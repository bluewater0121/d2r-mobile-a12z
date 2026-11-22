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

#include <string>
#include <atomic>
#include <math.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "../host/dm.h"
#include "../host/dm_hud.h"
#include "../host/dm_actions.h"
#include "../ios/Sources/dm_touch_game_state.h"
#include "../ios/Sources/dm_touch_native_art.h"
uint32_t dm_padmode_rebuilds(void);   // dm/host/dm_padmode.c：布局重建次数（变化 = HUD 控件全换，技能钮要重新认）| layout rebuild count (a change means the HUD widgets were all replaced)
#include "../host/dm_i18n.h"
#include "../host/dm_input.h"
#include "../ios/Sources/dm_touch_controls.h"
}
// dm_padmode 没有公共头，各家前端各自声明（它与 iOS / Mac 同一个实现）| dm_padmode has no public header; each front end declares it (same implementation as iOS / Mac)
extern "C" void dm_padmode_request(int pad);
extern "C" int dm_padmode_current(void);
extern "C" int dm_padmode_busy(void);
extern "C" void dm_tp_request(void);   // 回城钮（dm_freetp.c）| the town portal button (dm_freetp.c)

using namespace Gdiplus;

// ---------- 状态 ----------
static std::mutex g_mx;
static dm_tc_state g_tc;
static int g_on;                       // DM_WIN_TOUCH=1 | enabled
static int g_noart;                    // DM_WIN_TOUCH_NOICON=1：只画矢量外观、不贴技能图（排查用）| vector look only, no skill art (debugging)
static std::atomic<int> g_in_run;      // 局内（浮层可见、触屏布局） | in a run (overlay visible, touch layout)
static std::atomic<int> g_always;      // DM_WIN_TOUCH_ALWAYS=1：前端页面也用触屏布局（页面按钮栏还没有，测试用） | touch layout in the front end too (its ribbon is not implemented yet; for testing)
static HWND g_game, g_ov;
static std::atomic<int> g_pad_in;      // 实体手柄的「明确输入」接管（iOS 的 _padMode）：成立就收起浮层、手柄布局归手柄 | controller owns the input: overlay hidden
static int g_vx, g_vy, g_vw, g_vh;     // 视图矩形（客户区像素） | view rectangle (client pixels)
static float g_px = 1.0f;              // 每个 dp 几个像素 | pixels per dp
static float g_dpw, g_dph;             // 布局点数（dm_tc 的坐标空间） | layout points (dm_tc's coordinate space)
static uint64_t g_latch;               // 两次取手柄之间按着过的键 | keys held since the last controller read
static std::atomic<int> g_dirty;
static std::atomic<int> g_dump_req;    // 按键组合变了：下一张位图要落盘（按住态取证） | the button set changed: dump the next bitmap (to capture the held look)
static uint64_t g_dump_last_ns;        // 上一次落盘的时刻（按住时每 0.35 s 再存一张）| time of the last dump (one more every 0.35 s while held)
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
    // 手柄刚有明确输入时不抢布局：那时手柄版归手柄（iOS 的 _padMode 同样只由 dm_gamepad.m 切）
    // Do not grab the layout while the controller owns the input: the controller layout belongs to the controller then (iOS's _padMode is likewise
    // switched only by dm_gamepad.m)
    if (in_run == g_in_run.load()) return;
    g_in_run.store(in_run);
    dm_padmode_request((in_run && !g_pad_in.load()) ? 0 : 1);   // 局内 = 触屏版，局外 = 手柄版 | in a run = touch layout, outside = controller layout
    g_dirty.store(1);
    LOG("触屏浮层：%s（手柄布局 %d）", in_run ? "进局，切触屏布局并显示浮层" : "出局，切回手柄布局", dm_padmode_current());
}

// ---------- 输入来源（照 iOS 的 dm_gamepad.m / front_ios.m 的 _padMode）----------
// iOS：实体手柄一有明确输入 → 手柄模式，_overlay.hidden = YES（顺便 dm_tc_cancel_all 把按着的手指全抬了）；碰屏幕 → 触屏模式，浮层重现。
// 不做这一层的后果（本机实测）：局内浮层一直挂着，手柄按了键、游戏却还在触屏布局里，两边各说各话。
// iOS: clear physical-controller input -> controller mode, _overlay.hidden = YES (and dm_tc_cancel_all lifts every finger); touching the screen ->
// touch mode and the overlay comes back. Without this layer (measured here) the overlay stays up for the whole run, the player presses a controller
// button and the game sits in the touch layout, each side talking past the other.
void dm_win_touch_pad_input(void) {
    if (!g_on) return;
    if (g_pad_in.exchange(1)) return;
    dm_win_touch_cancel();   // 按着的手指全抬，免得切过去时还留着一个按住的键 | lift every finger so no key stays held across the switch
    g_dirty.store(1);
    LOG("触屏浮层：手柄有输入 → 收起浮层（手柄布局）");
}
void dm_win_touch_screen_input(void) {
    if (!g_on) return;
    if (!g_pad_in.exchange(0)) return;
    if (g_in_run.load()) dm_padmode_request(0);   // 碰画面 = 触屏侧（iOS 的 dm_gamepad_touched → 触屏模式）；请求相同值是无操作
    g_dirty.store(1);
    LOG("触屏浮层：碰了画面 / 敲键盘 → 露出浮层（触屏布局）");
}
int dm_win_touch_pad_mode(void) { return g_pad_in.load(); }
int dm_win_touch_in_run(void) { return g_in_run.load(); }

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
    if (!g_on || !g_in_run.load() || g_pad_in.load()) return;   // 浮层没开 / 不在局内 / 手柄接管中：照旧让手柄与键盘的输入走 | overlay off / not in a run / controller owns it: leave pad and keyboard input alone
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
static int g_claim[DM_TC_MAX_TOUCH];    // 这根手指归触屏控件（命中了圆钮 / 摇杆 / 边缘区，或面板态的界面指针）| this finger belongs to the touch controls

int dm_win_touch_pointer(int op, float nx, float ny, int slot) {
    if (!g_on) return 0;
    if (slot < 0 || slot >= DM_TC_MAX_TOUCH) return 0;
    float x = nx * g_dpw, y = ny * g_dph;
    double now = (double)now_ns() / 1e9;
    std::lock_guard<std::mutex> lk(g_mx);
    if (op == 0) {
        // 落点先按「差一点」修正一次再交给状态机：指头落在圆钮外沿 25% 以内 = 就算按在这颗钮的正中。
        // 只对圆钮（主技能 / 四个技能 / 回城）这样做，且只在完全没命中任何控件时；摇杆不吸——它的落点就是底盘位置。
        // 修正放在 begin_at 之前（一次进入），不做事后 cancel + 重按：两次状态迁移会把瞄准轴带偏。
        // Correct the landing point once before handing it to the state machine: a finger within a quarter radius outside a round button counts as
        // centred on it. Only the round buttons (main / four skills / portal), and only when nothing at all was hit; the stick never snaps, since its
        // touch point *is* the base position. The correction happens before begin_at (one entry), not as a cancel plus a second press afterwards:
        // two state transitions kick the aim axis off centre.
        if (dm_tc_hit(&g_tc.layout, x, y) == DM_TC_NONE) {
            const dm_tc_layout &l = g_tc.layout; int best = -1; float bd = 0;
            for (int i = 0; i <= DM_TC_PORTAL; ++i) {
                if (!(g_tc.enabled_targets & (1u << i))) continue;
                const dm_tc_target &t = l.target[i];
                if (!(t.r > 0) || !t.button) continue;
                float d = sqrtf((x - t.x) * (x - t.x) + (y - t.y) * (y - t.y));
                if (d <= t.r * 1.25f && (best < 0 || d < bd)) { best = i; bd = d; }
            }
            if (best >= 0) {
                LOG("触屏浮层：落点吸附到圆钮 %d（离中心 %.1f 点，半径 %.1f）", best, (double)bd, (double)l.target[best].r);
                x = l.target[best].x; y = l.target[best].y;
            }
        }
        dm_tc_begin_at(&g_tc, slot, x, y, now);
        // 只有真命中控件才算「吃掉」：没命中就撤销这根手指，让调用方当普通触点交给游戏（前端页面的按钮、局内界面按钮都靠它点）
        // Only a real hit is consumed: on a miss the finger is taken back so the caller hands it to the game as an ordinary touch point
        // (front-end buttons and in-run UI buttons are clicked that way)
        int claimed = g_tc.finger[slot].target != DM_TC_NONE || g_tc.pointer_slot == slot || g_tc.joy_slot == slot;
        int portal = claimed && !g_tc.navigation_mode && g_tc.finger[slot].target == DM_TC_PORTAL;
        if (getenv("DM_WIN_TOUCH")) LOG("触屏浮层：手指按下 点(%g,%g) 目标 %d %s（画布 %gx%g 点，缩放 %g）", x, y, g_tc.finger[slot].target,
                                        claimed ? "→ 触屏控件" : "→ 交给游戏触点", g_dpw, g_dph, g_px);
        if (!claimed) { dm_tc_cancel_finger(&g_tc, slot); return 0; }
        // 回城钮不发按键，点下由宿主施放原版回城技能（dm_freetp.c）| the town portal button sends no key; tapping it casts the original town portal on the host side
        if (portal) { dm_tc_cancel_finger(&g_tc, slot); dm_tp_request(); return 1; }   // 一次性，这根手指到此为止 | one-shot: this finger ends here
        g_claim[slot] = 1;
    } else if (op == 1) {
        if (!g_claim[slot]) return 0;
        dm_tc_move_at(&g_tc, slot, x, y, now);
    } else {
        if (!g_claim[slot]) return 0;
        dm_tc_end_at(&g_tc, slot, now);
        g_claim[slot] = 0;
    }
    g_latch |= g_tc.buttons;
    g_dirty.store(1);
    return 1;
}

void dm_win_touch_cancel(void) {
    if (!g_on) return;
    std::lock_guard<std::mutex> lk(g_mx);
    dm_tc_cancel_all(&g_tc);
    for (int i = 0; i < DM_TC_MAX_TOUCH; ++i) g_claim[i] = 0;
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

// ---------- 技能图集（native_art 产物，安卓同一个生成器）| skill atlas (native_art output, the same generator Android uses) ----------
static Bitmap *g_atlas[DM_TA_ATLASES];
static int g_atlas_dir_said;

// 图集文件：<DM_WIN_TOUCH_ART 或 cwd\touchart\TouchArt>\dm_native_skill_<i>.png
// Atlas file: <DM_WIN_TOUCH_ART or cwd\touchart\TouchArt>\dm_native_skill_<i>.png
static Bitmap *atlas_image(int i) {
    if (i < 0 || i >= DM_TA_ATLASES) return NULL;
    if (g_atlas[i]) return g_atlas[i];
    const char *dir = getenv("DM_WIN_TOUCH_ART");
    if (!dir || !*dir) dir = "touchart\\TouchArt";
    char path[512];
    snprintf(path, sizeof path, "%s\\%s.png", dir, dm_ta_atlases[i].asset);
    wchar_t wp[600]; wide(path, wp, 600);
    Bitmap *b = new Bitmap(wp, FALSE);
    if (!b || b->GetLastStatus() != Ok) {
        delete b;
        if (!g_atlas_dir_said) { g_atlas_dir_said = 1; LOG("触屏浮层：技能图集读不到（%s），技能钮回退成键位字母", path); }
        g_atlas[i] = (Bitmap *)1;   // 标记已试过，别每次都开文件 | mark as tried so the file is not opened every frame
        return NULL;
    }
    g_atlas[i] = b;
    return b;
}

// 技能钮上的图标：取当前绑定 + 原版图集切片（按下用 pressed_frame），裁成圆、直径取按钮的 .76 / .68，和 iOS 的 drawButton 同一套读法
// The icon on a skill button: current binding plus a slice of the original atlas (pressed_frame while held), cut into a circle, with the diameter
// at .76 / .68 of the button — the same reading as iOS's drawButton
static void skill_icon_draw(Graphics &g, const dm_ta_glyph &gl, int held, float cx, float cy, float r, int bare) {
    dm_ta_frame fr;
    if (!dm_ta_frame_at(gl.atlas, held ? gl.pressed_frame : gl.normal_frame, &fr)) return;
    Bitmap *b = atlas_image(gl.atlas);
    if (!b) return;
    float size = r * 2 * (bare ? 0.76f : 0.68f);
    float fit = size / (float)(fr.width > fr.height ? fr.width : fr.height);   // iOS：按最长边贴合 | iOS: fit by the longest edge
    int w = (int)(fr.width * fit + 0.5f), h = (int)(fr.height * fit + 0.5f);
    if (w < 2 || h < 2) return;
    // 原版图集里每格是一块带方角的技能牌，iOS 把它裁进椭圆。GDI+ 的 SetClip 在从这里画位图时不起作用（同一帧图仍是方的），
    // 所以自己按像素把圆外的 alpha 抹掉；iOS 的柔光（CGContextSetShadowWithColor，白 .65 / alpha .32）也一起烘进这张小图：
    // 每次重画就只剩一次 DrawImage，不必每个钮现搭一个 PathGradientBrush
    // Every frame is a square-cornered skill tile and iOS clips it into an ellipse. GDI+ SetClip does not take effect for this DrawImage here
    // (the frame still came out square), so the alpha outside the circle is cleared per pixel; iOS's glow (CGContextSetShadowWithColor, white .65 /
    // alpha .32) is baked into the same small bitmap, so a repaint costs one DrawImage per button instead of building a PathGradientBrush each time
    float mr = 0.82f * r;                          // iOS 的裁圆半径：按钮外接矩形内缩 9%（直径 2r）| iOS's clip radius: the button rect inset by 9% (diameter 2r)
    int m = (int)(r * (held ? 0.10f : 0.055f)) + 2;   // 柔光边宽（按住更大）| glow margin (larger while held)
    int bw = w + m * 2, bh = h + m * 2;
    struct Masked { int atlas, frame, w, h, m; Bitmap *bmp; };
    static Masked cache[48]; static int ncache;
    Bitmap *masked = NULL;
    for (int i = 0; i < ncache; ++i)
        if (cache[i].atlas == gl.atlas && cache[i].frame == (held ? gl.pressed_frame : gl.normal_frame) && cache[i].w == w && cache[i].h == h && cache[i].m == m) masked = cache[i].bmp;
    if (!masked) {
        Bitmap *tmp = new Bitmap(bw, bh, PixelFormat32bppPARGB);
        if (!tmp || tmp->GetLastStatus() != Ok) { delete tmp; return; }
        { Graphics tg(tmp); tg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
          tg.DrawImage(b, RectF((REAL)m, (REAL)m, (REAL)w, (REAL)h), (REAL)fr.x, (REAL)fr.y, (REAL)fr.width, (REAL)fr.height, UnitPixel); }
        Rect rl(0, 0, bw, bh); BitmapData bd;
        if (tmp->LockBits(&rl, ImageLockModeWrite, PixelFormat32bppPARGB, &bd) == Ok) {
            float mx = bw / 2.0f, my = bh / 2.0f;
            for (int y = 0; y < bh; ++y) {
                unsigned char *row = (unsigned char *)bd.Scan0 + (size_t)y * bd.Stride;
                for (int x = 0; x < bw; ++x) {
                    float d = sqrtf((x + 0.5f - mx) * (x + 0.5f - mx) + (y + 0.5f - my) * (y + 0.5f - my));
                    float cover = mr - d + 0.5f;
                    unsigned char *p = row + x * 4;
                    if (cover >= 1) continue;                       // 圆内且够深：图自己 | well inside the circle: the tile itself
                    if (cover > 0) { for (int c = 0; c < 4; ++c) p[c] = (unsigned char)(p[c] * cover); continue; }
                    float t = (d - mr) / (float)m;                  // 圆外：柔光由内向外淡出 | outside the circle: the glow fades outwards
                    if (t >= 1) { p[0] = p[1] = p[2] = p[3] = 0; continue; }
                    p[3] = (unsigned char)(82 * (1.0f - t));         // 预乘：alpha .32 | premultiplied: alpha .32
                    p[0] = p[1] = p[2] = (unsigned char)(53 * (1.0f - t));
                }
            }
            tmp->UnlockBits(&bd);
        }
        masked = tmp;
        if (ncache < 48) { cache[ncache].atlas = gl.atlas; cache[ncache].frame = held ? gl.pressed_frame : gl.normal_frame; cache[ncache].w = w; cache[ncache].h = h; cache[ncache].m = m; cache[ncache].bmp = tmp; ++ncache; }
    }
    // 非主钮上移 .12r（iOS 的 ir 里 t->r*.12），静止态 .94 透明
    // Non-primary buttons sit .12r higher (t->r*.12 in iOS's ir), and are .94 alpha at rest
    float ix = cx, iy = cy - (bare ? 0.0f : r * 0.12f);
    if (!held) { ImageAttributes ia; ColorMatrix cm = {}; cm.m[0][0] = cm.m[1][1] = cm.m[2][2] = 1.0f; cm.m[3][3] = 0.94f; cm.m[4][4] = 1.0f;
        ia.SetColorMatrix(&cm); g.DrawImage(masked, RectF(ix - bw / 2.0f, iy - bh / 2.0f, (REAL)bw, (REAL)bh), 0.0f, 0.0f, (REAL)bw, (REAL)bh, UnitPixel, &ia); }
    else g.DrawImage(masked, RectF(ix - bw / 2.0f, iy - bh / 2.0f, (REAL)bw, (REAL)bh), 0.0f, 0.0f, (REAL)bw, (REAL)bh, UnitPixel);
}

// 固定图标（地图 / 功能入口）：原版小图三帧横排，36x36 一格，按下取第二格
// Fixed icons (map / function entry): the original small image holds three 36x36 frames side by side; held takes the second
static void fixed_icon_draw(Graphics &g, const char *asset, int held, float cx, float cy, float r) {
    const char *dir = getenv("DM_WIN_TOUCH_ART");
    if (!dir || !*dir) dir = "touchart\\TouchArt";
    static Bitmap *cache[4]; static const char *names[4];
    int slot = -1;
    for (int i = 0; i < 4; ++i) { if (names[i] && !strcmp(names[i], asset)) { slot = i; break; } if (!names[i]) { slot = i; names[i] = asset; break; } }
    if (slot < 0) return;
    if (!cache[slot]) {
        char path[512]; snprintf(path, sizeof path, "%s\\%s.png", dir, asset);
        wchar_t wp[600]; wide(path, wp, 600);
        Bitmap *b = new Bitmap(wp, FALSE);
        if (!b || b->GetLastStatus() != Ok) { delete b; cache[slot] = (Bitmap *)1; return; }
        cache[slot] = b;
    }
    if (cache[slot] == (Bitmap *)1) return;
    Bitmap *b = cache[slot];
    float side = r * 1.63f;   // iOS：36 点图标 / 22 点半径 | iOS: a 36-point icon on a 22-point radius
    g.DrawImage(b, RectF(cx - side / 2, cy - side / 2, side, side), (REAL)(held ? 36 : 0), 0.0f, 36.0f, 36.0f, UnitPixel);
}

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

static void caption(Graphics &g, const char *utf8, float cx, float cy, float size, Color c) {    wchar_t w[64]; wide(utf8, w, 64);
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
    if (!g_ov || g_vw <= 0 || g_vh <= 0) { static int once; if (!once++) LOG("触屏浮层：paint 跳过（窗口 %p 视图 %dx%d）", (void *)g_ov, g_vw, g_vh); return; }
    if (surface_make(g_vw, g_vh) != 0) { static int once; if (!once++) LOG("触屏浮层：paint 跳过（surface_make 失败 %dx%d）", g_vw, g_vh); return; }
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
        // 当前技能绑定的快照（锁内一份副本；共享桥保证只读）| snapshot of the current skill bindings (one copy; the shared bridge keeps it read-only)
        dm_tg_snapshot snap; memset(&snap, 0, sizeof snap); dm_touch_game_snapshot(&snap); const uint64_t now = now_ns();
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
            // 技能钮：先问当前绑定取到哪张图标；取不到（不在局内 / 共享表还没认全 HUD）才回退成键位字母，空槽画十字
            // Skill buttons: ask the current binding which icon it maps to; only when that is unavailable (not in a run, or the shared table has
            // not fully identified the HUD yet) fall back to the key letter, and an empty slot draws a cross
            int empty = 0, charges = 0;
            dm_ta_glyph glg; memset(&glg, 0, sizeof glg); glg.status = DM_TA_UNKNOWN;
            if (i <= DM_TC_SKILL4 && b) {
                // 快照年龄：桥里六槽「一组」要等整帧的六次调用才拼齐（实测约 4 次/秒），iOS 的 250 ms 门在这里会把两组之间
                // 的空档全判过期、图标退回键位字母。图标是纯装饰、换技能一秒内看出来就够了，所以这里拿快照自己的采集时刻当查询
                // 时刻（只放宽这一步的年龄判定，快照本身仍按它自己的规则失效）。
                // Snapshot age: in the bridge a six-slot "group" needs the whole frame's six calls (about 4 groups a second in practice),
                // so iOS's 250 ms gate would call every gap between groups expired and the icons would fall back to key letters. The icon is
                // decoration and one second of lag is plenty to notice a rebind, so query as of the snapshot's own capture time (only this
                // age check is relaxed; the snapshot itself still expires by its own rules).
                uint64_t q = now;
                if (snap.captured_ns && (snap.captured_ns > q || q - snap.captured_ns > DM_TG_MAX_AGE_NS)) q = snap.captured_ns + 1;
                glg = dm_ta_current_set(&snap, b, q, (int)s.skill_set);
                if (glg.status == DM_TA_EMPTY) empty = 1;
            } else if (i == DM_TC_PORTAL) {
                glg = dm_ta_skill(359);   // 回城：原版通用的那张技能图（iOS 同一张）| town portal: the original game's shared skill image (the same one iOS uses)
            }
            button_draw(g, cx, cy, r, held, empty, i == DM_TC_MAIN);
            if (glg.status == DM_TA_GLYPH && !g_noart) {
                skill_icon_draw(g, glg, held, cx, cy, r, !empty && i == DM_TC_MAIN);
                // 副手组「Ⅱ」与充能剩余次数：和 iOS 同一处（右上 / 右下），只在技能钮上画
                // The off-hand-set "II" and the remaining charges, in the same places as iOS (top right / bottom right), skill buttons only
                int ssKind = dm_swapskill_slot(glg.slot, &charges);
                if (ssKind & 1) {
                    float br = r * 0.30f, bx = cx + r * 0.74f, by = cy - r * 0.74f;
                    SolidBrush bg(col(28, 24, 20, 242));
                    g.FillEllipse(&bg, bx - br, by - br, br * 2, br * 2);
                    Pen rim(col(161, 128, 69, 255), (REAL)fmax(1.0f, br * 0.14f));
                    g.DrawEllipse(&rim, bx - br * 0.92f, by - br * 0.92f, br * 1.84f, br * 1.84f);
                    caption(g, "\xE2\x85\xA1", bx, by, br * 1.30f, col(217, 201, 166, 255));
                }
                if (ssKind & 2)
                    caption(g, std::to_string(charges < 0 ? 0 : charges).c_str(), cx + r * 0.62f, cy + r * 0.46f, r * 0.30f,
                            col(226, 208, 170, 255));
            } else if (i == DM_TC_MAP) {
                fixed_icon_draw(g, "dm_native_map", held, cx, cy, r);
            } else if (!empty) {
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
    POINT dst = {0, 0};
    if (!ClientToScreen(g_game, &dst)) {
        static int once;
        if (!once++) LOG("触屏浮层：ClientToScreen 失败（游戏窗口 %p，IsWindow=%d，错误 %lu）", (void *)g_game, IsWindow(g_game), GetLastError());
        RECT rc; GetWindowRect(g_game, &rc); dst.x = rc.left; dst.y = rc.top;
    }
    dst.x += g_vx; dst.y += g_vy;
    SIZE sz = {g_vw, g_vh};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    BOOL ok = UpdateLayeredWindow(g_ov, NULL, &dst, &sz, g_mdc, &src, 0, &bf, ULW_ALPHA);
    // 分层窗口贴图是否真的成功：失败时桌面截屏又拿不到证据，只能靠返回值和 GetLastError 说话
    // Whether the layered blit actually succeeded: with no usable screen capture, the return value and GetLastError are the only evidence
    static int said;
    int now = ok ? 1 : 2;
    if (now != said) {
        said = now;
        if (ok) LOG("触屏浮层：贴图成功，窗口 %ld,%ld %ldx%ld（可见 %d）", (long)dst.x, (long)dst.y, (long)sz.cx, (long)sz.cy, IsWindowVisible(g_ov));
        else LOG("触屏浮层：贴图失败，GetLastError=%lu", GetLastError());
    }
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
    g_dump_last_ns = now_ns();
    LOG("触屏浮层：位图已存 %s（%dx%d）", path, g_bw, g_bh);
}

// ---------- 窗口 ----------
static LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;   // 点击穿透到游戏窗口 | clicks fall through to the game window
    return DefWindowProcA(h, m, w, l);
}

// z 序排名（0 = 最靠前）：从桌面的最上层窗口顺着 GW_HWNDNEXT 往下数。GW_HWNDPREV 只给紧邻一号，中间夹着任务栏 / IME 时看不出
// Z-order rank (0 = frontmost): walk GW_HWNDNEXT down from the desktop's top window. GW_HWNDPREV gives only the immediate neighbour, which hides
// the overlay's real position when the taskbar / IME sits in between
static int z_rank(HWND h) {
    if (!h) return -1;
    int i = 0;
    for (HWND w = GetTopWindow(NULL); w; w = GetWindow(w, GW_HWNDNEXT)) { if (w == h) return i; if (++i > 4000) break; }
    return -1;
}

void dm_win_touch_layout(int vx, int vy, int vw, int vh) {    if (!g_on) return;
    g_vx = vx; g_vy = vy; g_vw = vw; g_vh = vh;
    { std::lock_guard<std::mutex> lk(g_mx); layout_make(); }
    g_dirty.store(1);
}

void dm_win_touch_tick(void) {
    if (!g_on) return;
    scene_apply();
    {
        // 告诉共享桥「在不在局内 / 场景与 HUD 换过没有」：桥只在 scene_game 且 world_epoch 非零时才认那几次 getter 调用
        // Tell the shared bridge whether we are in a run and whether the scene / HUD changed: the bridge only accepts those getter calls when
        // scene_game is set and world_epoch is non-zero
        static uint64_t scene_epoch = 1, world_epoch = 1;
        static int prev_run = -1;
        static uint32_t prev_rebuilds;
        int run = g_in_run.load();
        if (run != prev_run) { ++scene_epoch; prev_run = run; }
        uint32_t rb = dm_padmode_rebuilds();
        if (rb != prev_rebuilds) { prev_rebuilds = rb; ++world_epoch; }   // 布局重建 = HUD 控件全换，技能钮要重新认 | a layout rebuild replaced every HUD widget, so the buttons have to re-identify
        dm_touch_game_set_scene(run, scene_epoch, world_epoch);
    }
    // 布局切换事务进行中也收起：整块界面正在重建成另一版，浮层压在上面会盖住重建中的界面（iOS 的 front_ios.m:497 每帧把 dm_padmode_busy() 也算进 hide）
    // Also hidden while a layout switch transaction runs: the whole UI is being rebuilt into the other version, and an overlay on top would cover the
    // half-built screens (iOS front_ios.m:497 folds dm_padmode_busy() into hide every frame)
    static int revealed;   // 这一次露出做过「抬到最前」了没有（隐藏时清零）| whether this reveal already did the raise (cleared on hide)
    {
        int run = g_in_run.load(), pad = g_pad_in.load(), busy = dm_padmode_busy() ? 1 : 0;
        int want = (run && !pad && !busy) ? 1 : 0;
        // 显隐变化时打一行（三个判据都打出来）：真机上「按了手柄浮层还在」时，一眼能看出是哪个判据没成立
        // Log one line per visibility change with all three conditions: when the overlay stays up on a real controller press, this shows
        // immediately which condition did not hold
        static int want_prev = -1;
        if (want != want_prev) { want_prev = want; LOG("触屏浮层：%s（运行 %d 手柄接管 %d 切换中 %d）", want ? "露出" : "收起", run, pad, busy); }
        if (!want) { if (IsWindowVisible(g_ov)) ShowWindow(g_ov, SW_HIDE); revealed = 0; return; }
    }
    if (!IsWindowVisible(g_ov)) {
        ShowWindow(g_ov, SW_SHOWNA);
        g_dirty.store(1);
    }
    // 露出之后必须确认它真的压在游戏窗口上面。`SetWindowPos(g_ov, g_game, …)` 那个写法是把浮层插到 g_game **下面**（z 序里 after = below），
    // 于是「贴图成功、可见=1、桌面上什么都没有」；而改变窗口大小 / 最小化再恢复会让系统把 owned 窗口重新抬到 owner 上面，所以那两下能把它弄出来。
    // 现在露出时用 HWND_TOPMOST 把它放到 topmost 带的最顶，并用 z 序排名核对（GW_HWNDPREV 只给紧邻上一号，中间夹着任务栏 / IME 就看不出来）。
    // After revealing, make sure it really sits above the game window. `SetWindowPos(g_ov, g_game, ...)` inserts the overlay *below* g_game
    // (after = below in z-order), which gives "blit succeeded, visible=1, nothing on screen"; resizing or minimizing/restoring makes the system
    // raise the owned window above its owner, which is why those two gestures brought it back. HWND_TOPMOST puts it at the top of the topmost band
    // instead, and the z-order rank is checked (GW_HWNDPREV only gives the immediate neighbour, and the taskbar / IME can sit in between).
    {
        int vis = IsWindowVisible(g_ov) ? 1 : 0;
        // 只有当游戏窗口在前台时才把浮层抬到最顶：不然玩家切到别的程序，浮层会压在别人上面
        // Only raise the overlay while the game window is in the foreground: otherwise it would sit on top of whatever app the player switched to
        HWND fg = GetForegroundWindow();
        int front = fg && (fg == g_game || IsChild(g_game, fg));
        if (vis && !revealed) {
            revealed = 1;
            if (front) SetWindowPos(g_ov, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            RECT rr; GetWindowRect(g_ov, &rr);
            LOG("触屏浮层：露出 可见=%d 矩形=%ld,%ld %ldx%ld 视图=%d,%d %dx%d z序 浮层=%d 游戏=%d（小 = 更靠前）", vis,
                (long)rr.left, (long)rr.top, (long)(rr.right - rr.left), (long)(rr.bottom - rr.top), g_vx, g_vy, g_vw, g_vh,
                z_rank(g_ov), z_rank(g_game));
        } else if (vis && front) {
            // 露着的时候每 8 拍核一次 z 序：点游戏窗口 / 切回前台都会把游戏窗口抬上来，浮层就被压回它下面（视觉上「浮层没了」）
            // Every 8 ticks while visible, check the z order: clicking into the game or switching back to it raises the game window, which drops
            // the overlay behind it (visually "the overlay is gone")
            static int beat;
            if (++beat >= 8) {
                beat = 0;
                int a = z_rank(g_ov), b = z_rank(g_game);
                if (a < 0 || (b >= 0 && a > b)) {
                    SetWindowPos(g_ov, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                    LOG("触屏浮层：掉到游戏窗口后面，重新抬起（浮层 %d 游戏 %d）", a, b);
                }
            }
        }
    }
    // 摇杆头没动、状态没变就不重画（拖动时每帧一张全屏位图太贵）| skip repainting when neither the thumb nor anything else moved
    float tx, ty;
    { std::lock_guard<std::mutex> lk(g_mx);
      float r = g_tc.layout.joy_radius;
      float ox = g_tc.joy_slot >= 0 ? g_tc.joy_origin_x : g_tc.layout.joy_x;
      float oy = g_tc.joy_slot >= 0 ? g_tc.joy_origin_y : g_tc.layout.joy_y;
      tx = ox + g_tc.lx / 32767.0f * r; ty = oy - g_tc.ly / 32767.0f * r;
      if (g_tc.joy_slot >= 0) g_dirty.store(1); }
    // 按住时每 0.35 s 再存一张：第一次落盘可能赶在按键发布之前（那一张还是静止态）
    // While something is held, dump one more every 0.35 s: the first dump can land before the buttons are published (that one is still the resting look)
    if (g_tc.buttons && now_ns() - g_dump_last_ns > 350000000ull) g_dump_req.store(1);
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
    { const char *ni = getenv("DM_WIN_TOUCH_NOICON"); g_noart = ni && strcmp(ni, "1") == 0; }

    if (!g_gdip_on) {
        GdiplusStartupInput in; if (GdiplusStartup(&g_gdip, &in, NULL) != Ok) { LOG("触屏浮层：GDI+ 起不来，浮层不画"); return; }
        g_gdip_on = 1;
    }
    LOG("触屏浮层：技能绑定门 %d（1 = 产物里有 OBS_BINDING_GETTER，技能钮能取到图标）", gate_obs_present(6));
    // 挂上共享的技能绑定桥（和安卓 / iOS 同一份 dm_touch_game_bridge.c）：它接的是产物里那颗 getter 观测门
    // Hook up the shared binding bridge (the same dm_touch_game_bridge.c Android / iOS use): it attaches to the getter observation gate in the build
    {
        const char *nb = getenv("DM_WIN_TOUCH_NOBIND");   // 排查用：不挂绑定读取（技能钮回退字母）| debugging: skip the binding bridge (skill buttons fall back to letters)
        int attached = (nb && strcmp(nb, "1") == 0) ? 0 : dm_touch_game_attach();
        int nm = 0; for (int i = 0; i < g_nmod; ++i) ++nm;
        LOG("触屏浮层：技能绑定读取%s（模块 %d，main %s）", attached ? "已挂上" : "没挂上（技能钮回退成键位字母）", nm,
            attached ? "认出" : "没认出");
    }
    HINSTANCE hi = GetModuleHandleA(NULL);
    WNDCLASSEXA wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.lpfnWndProc = ov_proc; wc.hInstance = hi;
    wc.lpszClassName = "dmhost_touch_overlay";
    if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    // WS_EX_TOPMOST：游戏窗口自己是 topmost（front_win.c 的 create_window），普通 owned 弹出窗会被压在下面（贴图成功但看不见）
    // WS_EX_LAYERED：贴预乘位图；WS_EX_TRANSPARENT：鼠标事件全部落到下面的游戏窗口；WS_EX_NOACTIVATE：点它不抢焦点
    // WS_EX_TOPMOST: the game window itself is topmost (create_window in front_win.c), and an ordinary owned popup ends up *under* it
    // (the blit succeeds but nothing is visible). WS_EX_LAYERED: blit a premultiplied bitmap; WS_EX_TRANSPARENT: every mouse event falls to
    // the game window below; WS_EX_NOACTIVATE: it never takes the focus (unreachable anyway)
    g_ov = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                           wc.lpszClassName, "dmhost touch overlay", WS_POPUP, 0, 0, 1, 1, g_game, NULL, hi, NULL);
    if (!g_ov) { LOG("触屏浮层：覆盖层窗口建不出来（%lu）", GetLastError()); return; }

    { std::lock_guard<std::mutex> lk(g_mx); dm_tc_init(&g_tc, 667.0f, 375.0f); }
    dm_pad_hook = pad_step;
    g_on = 1;
    RECT rc; GetClientRect(g_game, &rc);
    dm_win_touch_layout(0, 0, rc.right, rc.bottom);
    SetTimer(g_game, 3, 16, NULL);   // 16 ms：重画与进 / 出局切换（原先 30 ms，按住态跟着手指走会慢半拍）| 16 ms: repaint and switch layouts (was 30 ms, which made the held look lag the finger)
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
int dm_win_touch_on(void) { return g_on; }
