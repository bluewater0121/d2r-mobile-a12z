// 调度优先级（Windows）：进程优先级类别 + MMCSS「Games」任务。实现在 sched_win.c。
// Scheduling priority (Windows): process priority class + the MMCSS "Games" task. Implemented in sched_win.c.
#ifndef DM_SCHED_WIN_H
#define DM_SCHED_WIN_H

// 进程级，已经调过就什么都不做（幂等） | process level, idempotent
void dm_win_sched_init(void);
// 线程级，必须在要保帧率的那条线程自己的上下文里调 | thread level, from the thread that must keep its frame rate
void dm_win_sched_thread(const char *label);

#endif
