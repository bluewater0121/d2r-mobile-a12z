#pragma once
// Windows 呈现层对外接口（front_win.c 与 hle 侧用）。与安卓的 vulkan_present.h 一一对应，
// 差别只有一个：窗口是宿主自己建的 HWND，生命周期由前端管，呈现层只在 WM_SIZE 后重建交换链。
// Public interface of the Windows presentation layer (used by front_win.c and the HLE side). One-to-one with Android's vulkan_present.h,
// with a single difference: the window is an HWND the host creates itself, its lifetime belongs to the front end, and the presentation
// layer only rebuilds the swapchain after WM_SIZE.
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
// 建实例 / 设备 / 交换链并调 dm_vk_initialize；shader_dir 里放 .xgs（DM_SHADER_DIR 缺省 "shaders"，相对工作目录）。
// 成功返回 0；失败返回非 0 并把原因写进 error（出图起不来时客体照样跑，见 front_win.c）。
// Create the instance / device / swapchain and call dm_vk_initialize; shader_dir holds the .xgs bundles (DM_SHADER_DIR, default "shaders",
// relative to the working directory). Returns 0 on success; on failure returns non-zero and writes the reason into error (the guest still
// runs when presentation cannot start, see front_win.c).
int dm_win_vk_initialize(int image_count, char *error, size_t bytes);
// 窗口尺寸变了（WM_SIZE）：下一次呈现时按新的客户区重建交换链 | window size changed (WM_SIZE): rebuild the swapchain at the new client area on the next present
void dm_win_vk_notify_resize(void);
int dm_win_vk_has_window(void);
const char *dm_win_vk_device_name(void);
// 最近的实际上屏帧率（整数，约 0.75 s 更新一次；还没上过屏时为 0）。标题栏显示用。
// The latest actual present rate (integer, updated about every 0.75 s; 0 before the first present). For the title bar.
int dm_win_vk_fps(void);
#ifdef __cplusplus
}
#endif
