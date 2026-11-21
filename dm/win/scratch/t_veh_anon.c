// 探针 2：从「匿名映射的可执行内存」里触发写违例（客体代码正是这样跑的：不在任何模块里、没有 .pdata）。
// 变体（argv[1]）：anon = 匿名可执行页里写 0x8；anon_tp = 同上 + TPIDR_EL0 指向自定义槽块。
// Probe 2: trigger a write fault from anonymously mapped executable memory (that is how guest code runs: in no module, without .pdata).
// Variants (argv[1]): anon = write to 0x8 from an anonymous executable page; anon_tp = same plus TPIDR_EL0 pointing at a custom block.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long g_mode;
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    printf("[probe] VEH code=%08lx addr=%p\n", (unsigned long)ep->ExceptionRecord->ExceptionCode,
           ep->ExceptionRecord->ExceptionAddress);
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}
static uint64_t rd_tpidr(void) { uint64_t v; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v)); return v; }
int main(int argc, char **argv) {
    g_mode = argc > 1 && strcmp(argv[1], "anon_tp") == 0;
    AddVectoredExceptionHandler(1, veh);
    // movz x0,#8 ; str wzr,[x0] ; ret
    static const uint32_t code[] = {0xD2800110u, 0xB900001Fu, 0xD65F03C0u};
    uint32_t *p = (uint32_t *)VirtualAlloc(NULL, 4096, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    memcpy(p, code, sizeof code);
    if (g_mode) __asm__ volatile("msr tpidr_el0, %0" :: "r"((uint64_t)calloc(64, 8)));
    printf("[probe] calling anon code at %p tpidr=%#llx\n", (void *)p, (unsigned long long)rd_tpidr());
    fflush(stdout);
    ((void (*)(void))p)();
    printf("[probe] survived\n");
    return 0;
}
