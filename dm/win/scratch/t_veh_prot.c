// 探针 5：确认 VirtualProtect 是否真的把页改成只读（ARM64 上验证权限违例是否可达），以及写未提交页时的 VEH。
// Probe 5: confirm VirtualProtect really makes a page read-only (does a permission fault happen at all on ARM64),
// and whether the VEH sees a write to an uncommitted page.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    printf("[probe] VEH code=%08lx addr=%p info0=%llu info1=%llu\n",
           (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress,
           (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
           (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}
__asm__(".text\n.p2align 2\n.globl st_word\n" "st_word:\n  str wzr, [x0]\n  ret\n");
extern void st_word(void *p);

int main(int argc, char **argv) {
    AddVectoredExceptionHandler(1, veh);
    int ro = argc > 1 && argv[1][0] == 'r';
    void *res = VirtualAlloc(NULL, (size_t)1 << 30, MEM_RESERVE, PAGE_NOACCESS);
    void *blk = VirtualAlloc((char *)res + 0x10000000, 0x20000, MEM_COMMIT, PAGE_READWRITE);
    printf("[probe] res=%p blk=%p committed\n", res, blk); fflush(stdout);
    if (ro) {
        DWORD old = 0;
        BOOL ok = VirtualProtect(blk, 0x20000, PAGE_READONLY, &old);
        printf("[probe] VirtualProtect RO ok=%d err=%lu old=%#lx\n", ok, GetLastError(), (unsigned long)old); fflush(stdout);
        if (ok) {
            // 读一页确认内容可读 | read a page back to confirm it is readable
            printf("[probe] read back=%d\n", *(volatile int *)blk); fflush(stdout);
        }
    }
    printf("[probe] writing %p\n", blk); fflush(stdout);
    st_word(blk);
    printf("[probe] survived\n");
    return 0;
}
