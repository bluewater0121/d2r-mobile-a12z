// 苹果 QoS 类在 Windows 上的映射：线程优先级（不做实时调度，避免抢占系统输入 / 音频线程）。
// 数值与苹果 <sys/qos.h> 相同，日志里的 QoS 读数三平台可比；读回的是本线程最后一次设的类。
// Apple QoS classes mapped to Windows thread priorities (no real-time scheduling, to avoid starving system input / audio threads).
// Values match Apple's <sys/qos.h>, so QoS readings in logs compare across the three platforms; the read-back returns the class this thread last set.
#pragma once
#include <pthread.h>
#include <windows.h>
typedef enum {
    QOS_CLASS_USER_INTERACTIVE = 0x21, QOS_CLASS_USER_INITIATED = 0x19, QOS_CLASS_DEFAULT = 0x15,
    QOS_CLASS_UTILITY = 0x11, QOS_CLASS_BACKGROUND = 0x09, QOS_CLASS_UNSPECIFIED = 0x00,
} qos_class_t;
extern __thread qos_class_t dm_qos_self_thread_;
static inline int dm_qos_prio_(qos_class_t c) {
    switch (c) {
    case QOS_CLASS_USER_INTERACTIVE: return THREAD_PRIORITY_HIGHEST;
    case QOS_CLASS_USER_INITIATED: return THREAD_PRIORITY_ABOVE_NORMAL;
    case QOS_CLASS_UTILITY: return THREAD_PRIORITY_BELOW_NORMAL;
    case QOS_CLASS_BACKGROUND: return THREAD_PRIORITY_LOWEST;
    default: return THREAD_PRIORITY_NORMAL;
    }
}
static inline int pthread_set_qos_class_self_np(qos_class_t c, int rel) {
    (void)rel; dm_qos_self_thread_ = c;
    // 失败不致命（保持原优先级），与安卓 compat 的约定一致 | Failure is not fatal (the original priority is kept), same convention as the Android compat
    return SetThreadPriority(GetCurrentThread(), dm_qos_prio_(c)) ? 0 : -1;
}
static inline int pthread_get_qos_class_np(pthread_t t, qos_class_t *c, int *rel) {
    (void)t; if (c) *c = dm_qos_self_thread_; if (rel) *rel = 0; return 0;
}
static inline qos_class_t qos_class_self(void) { return dm_qos_self_thread_; }
