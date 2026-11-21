// 探针 4：写一个「存在但只读」的页（权限违例，而不是访问未映射地址），并且这一页在我们自备的大保留区里、
// SP 也在同一保留区的块上 —— 与 dmhost 的客体现场尽量一致。看 VEH 拿不拿得到。
// Probe 4: write to a present-but-read-only page (a permission fault rather than an access to an unmapped address),
// with the page inside our own large reservation and SP on a block of the same reservation -- as close to the dmhost
// guest situation as we can get. See whether the VEH sees it.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    printf("[probe] VEH code=%08lx addr=%p info0=%llu info1=%llu\n",
           (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress,
           (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
           (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}
__asm__(
    ".text\n.p2align 2\n.globl ro_fault\n"
    "ro_fault:\n"        // x0 = 目标地址 | target address
    "  str wzr, [x0]\n"
    "  ret\n"
);
extern void ro_fault(void *p);

int main(void) {
    AddVectoredExceptionHandler(1, veh);
    const size_t cap = (size_t)1 << 30;                       // 1 GiB 保留 | 1 GiB reservation
    void *res = VirtualAlloc(NULL, cap, MEM_RESERVE, PAGE_NOACCESS);
    printf("[probe] reservation=%p\n", res); fflush(stdout);
    void *blk = VirtualAlloc((char *)res + 0x10000000, 0x20000, MEM_COMMIT, PAGE_READWRITE);
    VirtualProtect(blk, 0x20000, PAGE_READONLY, NULL);        // 只读：写就权限违例 | read-only: a write is a permission fault
    printf("[probe] writing to read-only page %p\n", blk); fflush(stdout);
    ro_fault(blk);
    printf("[probe] survived\n");
    return 0;
}
