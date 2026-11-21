// mach/mach_time.h（苹果）：单调时钟。Windows 用 QPC，时基 1:1（纳秒），与 mach_timebase_info 的 numer/denom = 1。
// mach/mach_time.h (Apple): monotonic clock. Windows uses QPC with a 1:1 time base (nanoseconds), i.e. mach_timebase_info numer/denom = 1.
#pragma once
#include <stdint.h>
#include <windows.h>
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
// mach_absolute_time 必须是真符号：nvn_entry.S 直接 bl 它（苹果那侧是 libSystem 的函数）
// mach_absolute_time must be a real symbol: nvn_entry.S calls it with bl (on Apple it is a libSystem function)
#ifdef __cplusplus
extern "C" {
#endif
uint64_t mach_absolute_time(void);
#ifdef __cplusplus
}
#endif
static inline int mach_timebase_info(mach_timebase_info_data_t *i) { if (i) { i->numer = 1; i->denom = 1; } return 0; }
static inline void mach_wait_until(uint64_t deadline_ns) {
    uint64_t now = mach_absolute_time();
    if (deadline_ns > now) Sleep((DWORD)((deadline_ns - now) / 1000000ull));
}
