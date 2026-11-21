// sys/sysctl.h：宿主取进程启动时刻（main.c 的诊断时间线）。Windows 用 GetProcessTimes 算，见 compat/time_win.c，
// 该头只为「无条件包含」处不报缺失。
// sys/sysctl.h: the host reads the process start time (diagnostic timeline in main.c). On Windows it is computed from GetProcessTimes
// (compat/time_win.c); this header only exists so unconditional includes do not fail.
#pragma once
#define CTL_KERN 1
#define KERN_PROC 14
#define KERN_PROC_PID 1
struct kinfo_proc { struct { struct { long tv_sec, tv_usec; } p_starttime; } kp_proc; };
