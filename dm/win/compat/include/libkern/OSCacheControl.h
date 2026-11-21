// libkern/OSCacheControl.h（苹果）：指令缓存刷新。ARM64 上写完代码页必须刷新，否则执行的是旧指令。
// Windows 用 FlushInstructionCache（内核按「改页 → 刷 ICache」的正确顺序做，含跨核）。
// libkern/OSCacheControl.h (Apple): instruction cache flush. On ARM64 code pages must be flushed after writing, or old instructions run.
// Windows uses FlushInstructionCache (the kernel orders "change page -> flush ICache" correctly, across cores).
#pragma once
#include <stddef.h>
#include <windows.h>
static inline void sys_icache_invalidate(void *start, size_t len) { FlushInstructionCache(GetCurrentProcess(), start, len); }
static inline void sys_dcache_flush(void *start, size_t len) { (void)start; (void)len; }
