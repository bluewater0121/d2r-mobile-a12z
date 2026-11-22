// Windows 的触屏操控浮层：把安卓 / iOS / 鸿蒙那份宿主触屏层（dm/ios/Sources/dm_touch_controls.c，状态机与布局共用）接到 Windows，
// 自己画摇杆、圆钮、技能组切换与难度选择条，鼠标 / 触摸屏的点按与拖动当手指喂给它，取手柄那一拍把按键与轴值发布出去。
// DM_WIN_TOUCH=1 打开。开机仍是手柄版布局（DM_APP），进局（dm_hud_info.valid）才切到触屏布局并露出浮层，出局切回手柄版——前端页面（主菜单 / 选角）的按钮栏是
// 各家前端自己画的 UIKit / Java 层，Windows 这一侧还没有，所以只让局内用触屏布局。
// Windows touch-control overlay: hooks the host touch layer Android / iOS / HarmonyOS share (dm/ios/Sources/dm_touch_controls.c, the same state
// machine and layout) into Windows. It draws the stick, the round buttons, the skill-set switch and the difficulty strip itself, feeds mouse /
// touchscreen presses and drags to it as fingers, and publishes the buttons and axes on the controller-read tick.
// Enabled by DM_WIN_TOUCH=1. Boot still uses the controller layout (DM_APP); the touch layout and the overlay only appear once in a run
// (dm_hud_info.valid), and leaving the run switches back -- the front-end pages' (main menu / character select) button ribbon is drawn by each
// platform's own UIKit / Java layer, which Windows does not have yet, so the touch layout is used in a run only.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int dm_win_touch_enabled(void);            // DM_WIN_TOUCH=1 且已启动 | DM_WIN_TOUCH=1 and started
int dm_win_touch_on(void);                 // DM_WIN_TOUCH=1 且已启动（不看在不在局内）：前端页面走界面点击用 | started with DM_WIN_TOUCH=1, regardless of being in a run: the front-end pages use it to route clicks to the UI
void dm_win_touch_start(void *game_hwnd);  // 建浮层窗口、初始化状态机、注册 dm_pad_hook | create the overlay window, init the state machine, register dm_pad_hook
void dm_win_touch_stop(void);
// 鼠标 / 触摸：op 0 按下 1 拖动 2 抬起；nx / ny = 画面归一化坐标（视图矩形内 0..1）
// 返回 1 = 这一下被触屏控件吃掉了；0 = 没落在任何控件上，调用方要照旧把它当普通触点（set_finger）交给游戏——
// 否则角色选择页的「开始游戏」这类界面按钮会被浮层吞掉
// Mouse / touch: op 0 down 1 drag 2 up; nx / ny = normalised picture coordinates
// Returns 1 when the touch controls took the event; 0 when it landed on no control, in which case the caller must still hand it to the game as an
// ordinary touch point (set_finger) -- otherwise front-end buttons such as PLAY on the character select page get swallowed by the overlay
int dm_win_touch_pointer(int op, float nx, float ny, int slot);
// 窗口尺寸或视图矩形变了：vx / vy / vw / vh = 视图矩形（客户区像素）| window size or view rectangle changed
void dm_win_touch_layout(int vx, int vy, int vw, int vh);
void dm_win_touch_cancel(void);            // 失焦：所有手指抬起 | focus lost: lift every finger
void dm_win_touch_free_slot(int slot);     // 新触点落进一个还挂着上次没抬起手指的槽：先清掉 | a new contact lands in a slot still holding an un-lifted finger: clear it first
void dm_win_touch_tick(void);              // 主线程定时（16 ms）：按需重画、按出局 / 进局切布局 | main-thread timer (16 ms): repaint when dirty, switch layout on entering / leaving a run
// 输入来源切换（照 iOS：实体手柄有明确输入 = padMode，碰画面 / 敲键盘 = 触屏）——手柄模式收起浮层，
// 触屏模式露出浮层并切回触屏布局。两边都由输入事件驱动，谁刚有输入就归谁。
// Input-source switch (same as iOS: clear physical-controller input = padMode, touching the picture / typing = touch) -- controller mode hides the
// overlay, touch mode shows it and switches back to the touch layout. Both are driven by input events; whichever just had input owns it.
void dm_win_touch_pad_input(void);
void dm_win_touch_screen_input(void);
int dm_win_touch_pad_mode(void);
int dm_win_touch_in_run(void);             // 在不在局内（手柄侧据此决定切换后要不要补发触发键） | in a run (the controller side uses it to decide whether to re-send the triggering key)

#ifdef __cplusplus
}
#endif
