// Windows 实体手柄 → 宿主手柄状态（dm_pad_set），对齐 iOS 的 dm_gamepad.m 与安卓的 PadInput.java：
//   · 手柄「明确输入」= 按下任何一颗键，或把摇杆从中位推出来 → 切手柄布局（dm_padmode_request(1)），并先报键帽样式；
//   · 玩家碰画面（鼠标左键）/ 敲键盘 → 切回触屏布局（0），摇杆要先回中、再推出来才算重新接管；
//   · 触屏布局下手柄摇杆不交给游戏；手柄布局下没推出来的摇杆给 0；
//   · 摇杆迟滞：推过 25% 算推出，回到 15% 以内算回中；
//   · 多个手柄按键取或、每根摇杆取推得最远的。
// 后端：XInput（xinput1_4 / 1_3 / 9_1_0 依次动态加载，不依赖导入库）。XInput 的四个面键是按键帽位置报的：
//   下 = A、右 = B、左 = X、上 = Y（Xbox 手柄字母与位置一致）。游戏是 Switch 版，面键按位置排就是「下 = 确认」，
//   与 PC / Xbox 版同位置同功能（dm_padmap.h 的 dm_padmap_face_xbox，nintendo = 0）。
//   DM_PAD_AB=switch：给按 Switch 字母报键的手柄（或玩家习惯「右 A 确认」）用，面键换成 Switch 语义：右 = A、下 = B（nintendo = 1）。
// 键帽：XInput 手柄报 Xbox 键帽（DM_GLYPH_XBOX）；覆盖层没有那套图时 dm_padmode.c 会退回原版 Switch 键帽。
// 环境变量：DM_PAD=0 关手柄；DM_PAD_AB=switch 换面键语义；DM_PAD_LOG=1 打连接与状态变化；
//   DM_PAD_TEST=<秒> 在没有实体手柄时注入一次 A 短按（验通路用，只在启动时那一次）。
//
// Physical controller on Windows -> host controller state (dm_pad_set), mirroring iOS dm_gamepad.m and Android PadInput.java:
//   clear input (any new button, or a stick pushed out) switches to the controller layout and reports the glyph style first;
//   touching the picture (left mouse button) or typing on the keyboard switches back to touch, and a deflected stick must return to center
//   before taking over again; sticks are not delivered in touch mode and give 0 in controller mode while not pushed out;
//   hysteresis 25% out / 15% in; several controllers OR their buttons and take the farthest stick.
// Backend: XInput (xinput1_4 / 1_3 / 9_1_0 loaded dynamically in that order, no import library). XInput reports the four face buttons by
//   position: bottom = A, right = B, left = X, top = Y (on Xbox pads the letters agree with the positions). The game is the Switch build,
//   so positional face buttons mean bottom = confirm, the same position and function as the PC / Xbox version (dm_padmap_face_xbox with
//   nintendo = 0). DM_PAD_AB=switch flips to the Switch letter semantics (right = A, bottom = B) for pads that report by letter.
// Glyphs: XInput pads report Xbox glyphs (DM_GLYPH_XBOX); when the overlay lacks that set dm_padmode.c falls back to the original Switch glyphs.
// Env: DM_PAD=0 disables; DM_PAD_AB=switch swaps the face buttons; DM_PAD_LOG=1 logs connections and state changes;
//   DM_PAD_TEST=<seconds> injects one short A press at that time (path check without hardware, once at startup).
#include "../host/dm.h"
#include "../host/dm_input.h"
#include "../host/dm_padmap.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

// XInput 的最小自定义声明（只为动态加载；字段顺序与 xinput.h 的 XINPUT_GAMEPAD / XINPUT_STATE 完全一致）
// Minimal XInput declarations for dynamic loading only (field order identical to XINPUT_GAMEPAD / XINPUT_STATE in xinput.h)
typedef struct { unsigned short wButtons; unsigned char bLeftTrigger, bRightTrigger; short sThumbLX, sThumbLY, sThumbRX, sThumbRY; } xi_pad;
typedef struct { unsigned int dwPacketNumber; xi_pad Gamepad; } xi_state;
typedef int(__stdcall *xi_get_state)(unsigned int idx, xi_state *st);

#define XIB_DPAD_UP 0x0001u
#define XIB_DPAD_DOWN 0x0002u
#define XIB_DPAD_LEFT 0x0004u
#define XIB_DPAD_RIGHT 0x0008u
#define XIB_START 0x0010u
#define XIB_BACK 0x0020u
#define XIB_LEFT_THUMB 0x0040u
#define XIB_RIGHT_THUMB 0x0080u
#define XIB_LEFT_SHOULDER 0x0100u
#define XIB_RIGHT_SHOULDER 0x0200u
#define XIB_A 0x1000u
#define XIB_B 0x2000u
#define XIB_X 0x4000u
#define XIB_Y 0x8000u
#define XI_TRIGGER 30            // 扳机当按钮的阈值（0..255） | trigger-as-button threshold (0..255)
#define XI_DEAD_L 7849           // XInput 官方左摇杆死区 | XInput's documented left-stick dead zone
#define XI_DEAD_R 8688           // 摇杆死区 | right-stick dead zone

void dm_padmode_request(int pad);   // 宿主 dm_padmode.c：布局换成原版手柄版 / 触屏版 | host dm_padmode.c: switch to the original controller / touch layout
static xi_get_state g_get_state;
static int g_nintendo, g_log;
static int g_mode;               // 当前布局：1 = 手柄版 | current layout: 1 = controller
static int g_need_center;        // 碰过画面 / 键盘：摇杆要先回中 | screen or keyboard was used: the stick must return to center first
static int g_on[2];              // 摇杆是否推出（带迟滞） | sticks pushed out (with hysteresis)
static uint64_t g_prev_b;

static int32_t axis(short v, int dz) {    if (v > -dz && v < dz) return 0;
    if (v > DM_STICK_MAX) v = DM_STICK_MAX;
    if (v < -DM_STICK_MAX) v = -DM_STICK_MAX;
    return v;
}
static int stick_on(int was, int32_t x, int32_t y) {
    double m = hypot((double)x, (double)y) / DM_STICK_MAX;
    return was ? m >= 0.15 : m >= 0.25;
}

static void set_mode(int pad) {
    if (g_mode == pad) return;
    g_mode = pad;
    dm_padmode_request(pad);   // 宿主 dm_padmode.c：布局换成原版手柄版 / 触屏版 | host dm_padmode.c: switch to the original controller / touch layout
    LOG("手柄：%s", pad ? "有明确输入 → 手柄布局" : "碰了画面 / 敲键盘 → 触屏布局");
}

// 把各手柄合起来、定布局、交给宿主（与 iOS apply() 同一套判据）
// Merge the controllers, decide the layout and hand the result to the host (the same rules as iOS apply())
static void publish(uint64_t b, int32_t ax[4]) {
    g_on[0] = stick_on(g_on[0], ax[0], ax[1]); g_on[1] = stick_on(g_on[1], ax[2], ax[3]);
    if (!g_on[0] && !g_on[1]) g_need_center = 0;   // 摇杆回中了：之后再推出来算数 | stick is back to center: pushing it out counts again
    if ((b & ~g_prev_b) || ((g_on[0] || g_on[1]) && !g_need_center)) {
        dm_padmode_request_glyph(DM_GLYPH_XBOX);   // 先报键帽样式，同一次切换里就换成本套 | report the glyph style first so the same switch uses it
        set_mode(1);
    }
    g_prev_b = b;
    if (!g_mode || !g_on[0]) ax[0] = ax[1] = 0;
    if (!g_mode || !g_on[1]) ax[2] = ax[3] = 0;
    dm_pad_set(b, ax[0], ax[1], ax[2], ax[3]);
}

void dm_win_pad_touch(void) {   // 玩家碰了画面 / 敲键盘：手柄布局 → 触屏布局，摇杆要先回中 | player touched the picture / typed: back to touch, stick must recenter first
    if (!g_mode) return;
    g_need_center = 1;
    set_mode(0);
    dm_pad_set(0, 0, 0, 0, 0);   // 触屏布局里手柄的键与摇杆都不交给游戏 | neither buttons nor sticks go to the game in touch mode
    g_prev_b = 0;
}

// DM_PAD_TEST：没有实体手柄时注入一次 A 短按，验「宿主手柄 → 游戏」这条通路
// DM_PAD_TEST: inject one short A press at that time, to check the host-controller -> game path without hardware
static void maybe_test_press(double t, uint64_t *out_b) {
    static double times[8]; static int n = -1, logged;
    if (n < 0) {   // "52,70,88"：每次在第 N 秒按下 0.25 s | "52,70,88": press for 0.25 s at each listed second
        n = 0; const char *e = getenv("DM_PAD_TEST"); if (!e) return;
        const char *c = e; while (*c && n < 8) { double v = atof(c); if (v > 0) times[n++] = v; c = strchr(c, ','); if (!c) break; ++c; }
    }
    for (int i = 0; i < n; i++) {
        if (t < times[i] || t > times[i] + 0.25) continue;
        *out_b |= DM_BTN_A;
        if (!logged) { logged = 1; LOG("手柄：DM_PAD_TEST 注入 A（t=%.1fs~%.1fs）", times[i], times[i] + 0.25); }
    }
}

static DWORD WINAPI poll_thread(void *p) {
    (void)p;
    xi_state st[4]; uint32_t connected = 0; double t0 = (double)GetTickCount64() / 1000.0;
    for (;;) {
        uint64_t b = 0; int32_t ax[4] = {0, 0, 0, 0}; double best[2] = {-1, -1};
        for (unsigned i = 0; i < 4; i++) {
            memset(&st[i], 0, sizeof st[i]);
            int r = g_get_state ? g_get_state(i, &st[i]) : 1;
            if (r != 0) { if (connected & (1u << i)) { connected &= ~(1u << i); if (g_log) LOG("手柄：槽 %u 断开", i); } continue; }
            if (!(connected & (1u << i))) { connected |= 1u << i; LOG("手柄：槽 %u 已连上", i); }
            const xi_pad *g = &st[i].Gamepad;
            b |= dm_padmap_face_xbox((g->wButtons & XIB_A) != 0, (g->wButtons & XIB_B) != 0,
                                     (g->wButtons & XIB_X) != 0, (g->wButtons & XIB_Y) != 0, g_nintendo);
            if (g->wButtons & XIB_LEFT_SHOULDER) b |= DM_BTN_L;
            if (g->wButtons & XIB_RIGHT_SHOULDER) b |= DM_BTN_R;
            if (g->bLeftTrigger > XI_TRIGGER) b |= DM_BTN_ZL;
            if (g->bRightTrigger > XI_TRIGGER) b |= DM_BTN_ZR;
            if (g->wButtons & XIB_LEFT_THUMB) b |= DM_BTN_LS;
            if (g->wButtons & XIB_RIGHT_THUMB) b |= DM_BTN_RS;
            if (g->wButtons & XIB_START) b |= DM_BTN_PLUS;
            if (g->wButtons & XIB_BACK) b |= DM_BTN_MINUS;
            if (g->wButtons & XIB_DPAD_LEFT) b |= DM_BTN_LEFT;
            if (g->wButtons & XIB_DPAD_UP) b |= DM_BTN_UP;
            if (g->wButtons & XIB_DPAD_RIGHT) b |= DM_BTN_RIGHT;
            if (g->wButtons & XIB_DPAD_DOWN) b |= DM_BTN_DOWN;
            int32_t lx = axis(g->sThumbLX, XI_DEAD_L), ly = axis(g->sThumbLY, XI_DEAD_L);
            int32_t rx = axis(g->sThumbRX, XI_DEAD_R), ry = axis(g->sThumbRY, XI_DEAD_R);
            double ml = hypot((double)lx, (double)ly), mr = hypot((double)rx, (double)ry);
            if (ml > best[0]) { best[0] = ml; ax[0] = lx; ax[1] = ly; }
            if (mr > best[1]) { best[1] = mr; ax[2] = rx; ax[3] = ry; }
        }
        int holder = 0;
        maybe_test_press((double)GetTickCount64() / 1000.0 - t0, &b);
        publish(b, ax);
        Sleep(8);   // 125 Hz：比游戏取手柄的 60 Hz 快，短按不会被漏 | faster than the game's 60 Hz sampling so short presses are not missed
    }
    return 0;
}

void dm_win_pad_start(void) {
    const char *off = getenv("DM_PAD");
    if (off && *off == '0') { LOG("手柄：DM_PAD=0，不接"); return; }
    const char *ab = getenv("DM_PAD_AB");
    g_nintendo = ab && (!strcmp(ab, "switch") || !strcmp(ab, "1"));
    const char *lg = getenv("DM_PAD_LOG");
    g_log = lg && *lg == '1';
    HINSTANCE dll = NULL;
    const char *names[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (unsigned i = 0; i < 3 && !dll; i++) dll = LoadLibraryA(names[i]);
    if (!dll) { LOG("手柄：没有 XInput（xinput1_4/1_3/9_1_0 都加载失败），手柄不可用"); return; }
    g_get_state = (xi_get_state)(void *)GetProcAddress(dll, "XInputGetState");
    if (!g_get_state) { LOG("手柄：XInput 加载了但没有 XInputGetState"); return; }
    LOG("手柄：XInput 已就绪，面键%s（下 = 确认），键帽 Xbox", g_nintendo ? "按 Switch 字母（右 = 确认）" : "按位置");
    HANDLE th = CreateThread(NULL, 0, poll_thread, NULL, 0, NULL);
    if (th) CloseHandle(th);
}
