// 调度优先级（Windows）：进程优先级类别 + MMCSS「Games」任务，用来压住周期性尖峰。
// 尖峰的成因不是 GPU / 出图（那几项已实测排除），而是客体主线程每 100~200 ms 被别的线程抢一次 20~30 ms。
// 提进程优先级类别能少被抢一点，但真正的对手是系统里那些不分优先级的后台线程；MMCSS（多媒体类计划服务）的
// "Games" 任务给注册线程更高的基优先级并给它预留 CPU 时间，正是给游戏主循环用的，比整进程提优先级更对症。
//
// 开关（都在 tuning_defaults / front_run 之前读，玩家设了同名变量为准）：
//   DM_WIN_PRIO=off|normal|above|high  进程优先级类别（缺省 above，游戏常见的档位；high 会连 Vulkan 驱动线程一起提）
//   DM_WIN_MMCSS=0                     关掉 MMCSS 注册（缺省开）
//   DM_WIN_MMCSS_PRIO=normal|high      注册后请求的线程优先级（缺省 high）
//
// Scheduling priority (Windows): process priority class + the MMCSS "Games" task, to hold down the periodic spikes.
// The spikes are not GPU / presentation (both measured out); the guest main thread loses 20-30 ms every 100-200 ms to
// another thread. Raising the process class helps a little, but the real opponent is the system's priority-agnostic
// background threads; MMCSS (the multimedia class scheduler) "Games" task gives a registered thread a higher base
// priority and reserves CPU time for it, which is exactly what a game main loop wants and far more on target than
// raising the whole process.
//
// Switches (read before tuning_defaults / front_run; a user-set variable of the same name wins):
//   DM_WIN_PRIO=off|normal|above|high  process priority class (default above, the usual game setting; high lifts the
//                                      Vulkan driver threads too)
//   DM_WIN_MMCSS=0                     turn MMCSS registration off (on by default)
//   DM_WIN_MMCSS_PRIO=normal|high      thread priority asked for after registration (default high)
#define _CRT_SECURE_NO_WARNINGS
#include "../host/dm.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef HANDLE(__stdcall *av_set_t)(const wchar_t *, DWORD *);
typedef BOOL(__stdcall *av_prio_t)(HANDLE, int);
typedef BOOL(__stdcall *av_revert_t)(HANDLE);

#define AVRT_PRIORITY_LOW 0
#define AVRT_PRIORITY_NORMAL 0
#define AVRT_PRIORITY_HIGH 1
#define AVRT_PRIORITY_CRITICAL 2

static int g_init;
static int g_mmcss_on;
static av_set_t g_av_set;
static av_prio_t g_av_prio;
static int g_av_prio_level = AVRT_PRIORITY_HIGH;
static unsigned g_registered;   // 已注册的线程数，只在日志里用 | threads registered so far, for the log only

// DM_WIN_PRIO 里那一档的中文名，日志里看得懂 | the Chinese name of the DM_WIN_PRIO level, so the log reads plainly
static const char *prio_name(int cls) {
    switch (cls) {
    case IDLE_PRIORITY_CLASS: return "idle";
    case BELOW_NORMAL_PRIORITY_CLASS: return "below";
    case NORMAL_PRIORITY_CLASS: return "normal";
    case ABOVE_NORMAL_PRIORITY_CLASS: return "above";
    case HIGH_PRIORITY_CLASS: return "high";
    default: return "?";
    }
}

// 进程级：只做一次。DM_WIN_PRIO 缺省 above —— D2R 主体是单线程提交，进程里那些 Vulkan 驱动线程并不需要
// high，把整进程提到 high 反而会把它们一起抬起来，跟客体主线程抢调度。
// Process level, done once. DM_WIN_PRIO defaults to above: D2R submits from one thread and the Vulkan driver threads
// do not need high, so lifting the whole process to high would raise them as well and let them compete with the guest
// main thread.
void dm_win_sched_init(void) {
    if (g_init) return;
    g_init = 1;

    const char *p = getenv("DM_WIN_PRIO");
    int cls = ABOVE_NORMAL_PRIORITY_CLASS;
    if (p && *p) {
        if (!strcmp(p, "off") || !strcmp(p, "none")) cls = 0;
        else if (!strcmp(p, "normal")) cls = NORMAL_PRIORITY_CLASS;
        else if (!strcmp(p, "below")) cls = BELOW_NORMAL_PRIORITY_CLASS;
        else if (!strcmp(p, "high")) cls = HIGH_PRIORITY_CLASS;
        else if (!strcmp(p, "above")) cls = ABOVE_NORMAL_PRIORITY_CLASS;
    }
    if (cls) {
        if (SetPriorityClass(GetCurrentProcess(), cls)) LOG("调度：进程优先级 %s", prio_name(cls));
        else LOG("【红】调度：设进程优先级 %s 失败（GetLastError=%lu）", prio_name(cls), (unsigned long)GetLastError());
    } else LOG("调度：进程优先级不动（DM_WIN_PRIO=off）");

    const char *m = getenv("DM_WIN_MMCSS");
    g_mmcss_on = !(m && m[0] == '0' && !m[1]);
    const char *lv = getenv("DM_WIN_MMCSS_PRIO");
    if (lv && !strcmp(lv, "normal")) g_av_prio_level = AVRT_PRIORITY_NORMAL;
    if (lv && !strcmp(lv, "critical")) g_av_prio_level = AVRT_PRIORITY_CRITICAL;
    if (!g_mmcss_on) { LOG("调度：MMCSS 关（DM_WIN_MMCSS=0）"); return; }

    HMODULE av = LoadLibraryA("avrt.dll");
    if (av) {
        g_av_set = (av_set_t)(void *)GetProcAddress(av, "AvSetMmThreadCharacteristicsW");
        g_av_prio = (av_prio_t)(void *)GetProcAddress(av, "AvSetMmThreadPriority");
    }
    if (!g_av_set || !g_av_prio) {
        g_mmcss_on = 0;
        LOG("【红】调度：avrt.dll 或它的导出不可用，MMCSS 关");
        return;
    }
    LOG("调度：MMCSS 就绪（任务 Games，请求线程优先级 %s）", g_av_prio_level == AVRT_PRIORITY_NORMAL ? "normal" : "high");
}

// 线程级：在要保帧率的线程自己的上下文里调（MMCSS 是按线程注册的）。每条线程保留自己的任务句柄，
// 句柄一放注册就失效，所以注册成功就不再释放（每线程一个，数量固定，代价可忽略）。
// Thread level: called on the thread that must keep its frame rate (MMCSS registers per thread). Every thread keeps
// its own task handle because releasing it cancels the registration, so a successful handle is never closed (one per
// thread, a fixed count, negligible).
void dm_win_sched_thread(const char *label) {
    if (!g_init) dm_win_sched_init();
    if (!g_mmcss_on) return;
    DWORD task = 0;
    HANDLE h = g_av_set(L"Games", &task);
    if (!h) {
        LOG("【红】调度：%s 注册 MMCSS Games 失败（GetLastError=%lu）", label, (unsigned long)GetLastError());
        return;
    }
    g_av_prio(h, g_av_prio_level);
    g_registered++;
    LOG("调度：%s 已挂 MMCSS Games（任务 %lu），线程优先级 %d（%u）", label, (unsigned long)task,
        (int)GetThreadPriority(GetCurrentThread()), g_registered);
}
#endif
