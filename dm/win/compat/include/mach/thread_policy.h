// 实时线程策略（苹果）：Windows 上不做实时调度（会抢系统输入 / 音频线程），统一返回失败，宿主按「未生效」走。
// 线程优先级由 pthread/qos.h 的 QoS → Win32 优先级映射负责，诊断日志里的 QoS 读数照常。
// Real-time thread policy (Apple): no real-time scheduling on Windows (it would starve system input / audio threads); always fails, so the
// host takes the "not in effect" path. Thread priority comes from the QoS -> Win32 priority mapping in pthread/qos.h, and QoS readings in
// diagnostic logs are unchanged.
#pragma once
#include "mach/mach.h"
#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT 4
typedef struct { uint32_t period, computation, constraint, preemptible; } thread_time_constraint_policy_data_t;
typedef struct { uint32_t flavor, count; } thread_policy_state_t;
static inline kern_return_t thread_policy_set(thread_act_t t, int flavor, thread_policy_t p, mach_msg_type_number_t n) { (void)t; (void)flavor; (void)p; (void)n; return KERN_FAILURE; }
static inline kern_return_t thread_policy_get(thread_act_t t, int flavor, thread_policy_t p, mach_msg_type_number_t *n, int *get_default) { (void)t; (void)flavor; (void)p; (void)n; if (get_default) *get_default = 0; return KERN_FAILURE; }
