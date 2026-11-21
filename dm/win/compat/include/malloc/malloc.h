// malloc/malloc.h（苹果）：宿主只用 malloc_size / malloc_zone 统计。
// Windows 上 malloc_size 走 _msize（见 dm_win_compat.h），zone 统计用进程堆的近似值。
// malloc/malloc.h (Apple): the host only uses malloc_size / malloc_zone accounting.
// On Windows malloc_size goes through _msize (see dm_win_compat.h), zone accounting is approximated from the process heap.
#pragma once
#include <stddef.h>
typedef struct dm_malloc_zone { void *opaque; } malloc_zone_t;
malloc_zone_t *malloc_default_zone(void);
size_t malloc_zone_size(malloc_zone_t *z, const void *p);
// 苹果的 zone 统计（diag.c 的 L3MEM 报告）：Windows 上用进程工作集近似，字段名保持一致
// Apple's zone statistics (diag.c's L3MEM report): approximated from the process working set on Windows, field names kept identical
typedef struct {
    unsigned blocks_in_use;
    unsigned reserved;
    size_t size_in_use;
    size_t max_size_in_use;
    size_t size_allocated;
} malloc_statistics_t;
void malloc_zone_statistics(malloc_zone_t *z, malloc_statistics_t *s);
