// 苹果 Mach 接口在 Windows 上的替身：只覆盖宿主实际调用到的部分，语义按「能读到就读、读不到就失败」处理。
//   vm_read_overwrite（读可能无效的地址而不崩）= ReadProcessMemory 读本进程；
//   task_info(TASK_VM_INFO) 的 phys_footprint / resident_size = GetProcessMemoryInfo；
//   线程「端口」= 宿主线程号（tid）；thread_suspend / thread_get_state / task_threads / thread_info 无对应物，返回失败
//   （诊断里的「别的线程现场」在 Windows 上不导出，游戏本身不受影响；诊断用 DM_DBG 时失去的只是这部分日志）。
// Apple Mach interfaces on Windows: only the parts the host really calls, with "read it if we can, fail otherwise" semantics.
//   vm_read_overwrite (read a possibly invalid address without crashing) = ReadProcessMemory on this process;
//   phys_footprint / resident_size of task_info(TASK_VM_INFO) = GetProcessMemoryInfo;
//   a thread "port" = the host thread id; thread_suspend / thread_get_state / task_threads / thread_info have no counterpart and fail
//   (the "other thread's state" part of the diagnostics is not exported on Windows; the game itself is unaffected -- with DM_DBG only those log lines are lost).
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>
#include <psapi.h>
#include <pthread.h>
// 宿主的线程号（compat/tls_win.c 登记，主线程在启动时填）| the host thread id (registered by compat/tls_win.c; the main thread sets it at startup)
unsigned dm_win_thread_tid(void);

typedef int kern_return_t;
typedef unsigned mach_port_t;
typedef mach_port_t thread_act_t;
typedef thread_act_t *thread_act_array_t;
typedef unsigned mach_msg_type_number_t;
typedef uintptr_t vm_address_t;
typedef size_t vm_size_t;
typedef int vm_prot_t;
typedef int *task_info_t;
typedef int *thread_info_t;
typedef unsigned *thread_state_t;
typedef void *thread_policy_t;

#define KERN_SUCCESS 0
#define KERN_FAILURE 5
#define KERN_INVALID_ARGUMENT 4

#define MACH_PORT_NULL 0
#define mach_task_self() 0
#define mach_thread_self() dm_win_thread_tid()
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { (void)t; return dm_win_thread_tid(); }
static inline kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name) { (void)task; (void)name; return KERN_SUCCESS; }
// vm_deallocate：宿主只在测试 / 诊断里用过；按「整块释放」处理 | vm_deallocate: only used in tests / diagnostics; treated as "free the whole block"
static inline kern_return_t vm_deallocate(mach_port_t task, vm_address_t a, vm_size_t n) { (void)task; (void)n; free((void *)a); return KERN_SUCCESS; }
// vm_allocate：同上 | vm_allocate: same
static inline kern_return_t vm_allocate(mach_port_t task, vm_address_t *out, vm_size_t n, int flags) { (void)task; (void)flags; *out = (vm_address_t)malloc(n); return *out ? KERN_SUCCESS : KERN_FAILURE; }

static inline kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t n, vm_address_t to, vm_size_t *got) {
    (void)task; SIZE_T got_n = 0;
    BOOL ok = ReadProcessMemory(GetCurrentProcess(), (const void *)addr, (void *)to, n, &got_n);
    if (got) *got = ok ? (vm_size_t)got_n : 0;
    return ok && got_n == n ? KERN_SUCCESS : KERN_FAILURE;
}

#define TASK_VM_INFO 22
typedef struct { uint64_t virtual_size, resident_size, resident_size_max, phys_footprint; } task_vm_info_data_t;
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(int)))
static inline kern_return_t task_info(mach_port_t task, int flavor, task_info_t out, mach_msg_type_number_t *count) {
    (void)task; (void)count;
    if (flavor != TASK_VM_INFO) return KERN_FAILURE;
    PROCESS_MEMORY_COUNTERS pmc; if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return KERN_FAILURE;
    task_vm_info_data_t *vi = (task_vm_info_data_t *)out;
    vi->resident_size = vi->phys_footprint = (uint64_t)pmc.WorkingSetSize;
    vi->virtual_size = (uint64_t)pmc.PagefileUsage;
    vi->resident_size_max = (uint64_t)pmc.PeakWorkingSetSize;
    return KERN_SUCCESS;
}

// 线程寄存器（字段名与苹果 arm_thread_state64_t 相同，hle_thread.c 的诊断代码不用改）
// Thread registers (fields named as in Apple's arm_thread_state64_t, so hle_thread.c's diagnostic code needs no change)
typedef struct { uint64_t __x[29], __fp, __lr, __sp, __pc; uint32_t __cpsr, __pad; } arm_thread_state64_t;
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_thread_state64_t) / sizeof(uint32_t)))
#define __darwin_arm_thread_state64_get_pc(s) ((s).__pc)
#define __darwin_arm_thread_state64_get_lr(s) ((s).__lr)
#define __darwin_arm_thread_state64_get_sp(s) ((s).__sp)
#define __darwin_arm_thread_state64_get_fp(s) ((s).__fp)
typedef struct { int seconds, microseconds; } time_value_t;
typedef struct { time_value_t user_time, system_time; int cpu_usage, policy, run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(int)))

// 扩展线程信息：Windows 上只用 GetThreadTimes 填 CPU 时间，优先级三项留给诊断显示
// Extended thread info: Windows fills CPU time from GetThreadTimes; the three priority fields are only for the diagnostic display
typedef struct {
    time_value_t pth_user_time, pth_system_time;
    int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority;
    char pth_name[64];
} thread_extended_info_data_t;
#define THREAD_EXTENDED_INFO 14
#define THREAD_EXTENDED_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_extended_info_data_t) / sizeof(int)))

// 无对应物的几样：明确失败（调用方都按失败走兜底）| What has no counterpart: fails explicitly (all callers have a failure fallback)
static inline kern_return_t thread_suspend(thread_act_t t) { (void)t; return KERN_FAILURE; }
static inline kern_return_t thread_resume(thread_act_t t) { (void)t; return KERN_FAILURE; }
static inline kern_return_t thread_get_state(thread_act_t t, int f, thread_state_t o, mach_msg_type_number_t *c) { (void)t; (void)f; (void)o; (void)c; return KERN_FAILURE; }
static inline kern_return_t task_threads(mach_port_t t, thread_act_array_t *o, mach_msg_type_number_t *n) { (void)t; if (o) *o = NULL; if (n) *n = 0; return KERN_FAILURE; }
static inline kern_return_t thread_info(thread_act_t t, int flavor, thread_info_t out, mach_msg_type_number_t *count) {
    (void)t;
    if (flavor != THREAD_BASIC_INFO || !out) return KERN_FAILURE;
    thread_basic_info_data_t *bi = (thread_basic_info_data_t *)out;
    *bi = (thread_basic_info_data_t){0};
    if (count) *count = THREAD_BASIC_INFO_COUNT;
    return KERN_SUCCESS;
}
static inline kern_return_t thread_extended_info(thread_act_t t, thread_info_t out, mach_msg_type_number_t *count) {
    (void)t;
    if (!out) return KERN_FAILURE;
    thread_extended_info_data_t *xi = (thread_extended_info_data_t *)out;
    *xi = (thread_extended_info_data_t){0};
    if (count) *count = THREAD_EXTENDED_INFO_COUNT;
    return KERN_SUCCESS;
}
