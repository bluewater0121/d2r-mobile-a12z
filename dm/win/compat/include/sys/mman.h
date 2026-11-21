// POSIX mmap 在 Windows 上的实现：VirtualAlloc / VirtualProtect / VirtualFree。
// 只支持宿主实际用到的形态（匿名私有映射、PROT_READ/WRITE/EXEC 任意组合、MS_SYNC），
// 具体参数由 compat/mman_win.c 检查，不符合的直接返错而不是悄悄给出错语义。
// POSIX mmap on Windows: VirtualAlloc / VirtualProtect / VirtualFree.
// Only the forms the host actually uses are supported (anonymous private mappings, any PROT_READ/WRITE/EXEC combination, MS_SYNC);
// compat/mman_win.c checks the arguments and fails instead of silently giving wrong semantics.
#pragma once
#include <stddef.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_PRIVATE 0x02
#define MAP_SHARED 0x01
#define MAP_FIXED 0x10
#define MAP_ANON 0x20
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED ((void *)-1)

#define MS_ASYNC 1
#define MS_INVALIDATE 2
#define MS_SYNC 4

// madvise：宿主只在「解除物理页」时用 MADV_FREE，这里落成 MEM_RESET（内容丢弃、页留着）
// madvise: the host only uses MADV_FREE to drop physical pages, mapped to MEM_RESET here (contents dropped, pages kept)
#define MADV_NORMAL 0
#define MADV_DONTNEED 4
#define MADV_FREE 8

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
int msync(void *addr, size_t len, int flags);
int madvise(void *addr, size_t len, int advice);
// 宿主写客体内存前刷指令缓存（ARM64 上改代码页后必须做）；对应苹果的 sys_icache_invalidate
// Flush the instruction cache before the host executes rewritten guest code (mandatory on ARM64 after changing code pages); the counterpart of Apple's sys_icache_invalidate
void dm_win_icache_invalidate(void *addr, size_t len);
#ifdef __cplusplus
}
#endif
