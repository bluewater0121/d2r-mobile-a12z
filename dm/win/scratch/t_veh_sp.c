// 探针 3：把 SP 切到一块「堆上自备的客体栈」（不改 TEB 的栈边界）后触发写违例，看 VEH 还能不能拿到。
// 客体线程在 Windows 上就是这样跑的（dm_thread_enter 直接换 SP 进客体栈）；如果这里 VEH 拿不到异常，
// 说明「客体栈不在 TEB 栈边界内」会让异常分发在进入处理器之前就放弃。
// Probe 3: switch SP to a heap-provided "guest stack" (without touching the TEB stack limits), then trigger a write fault
// and see whether the VEH still sees it. Guest threads run exactly this way on Windows (dm_thread_enter just swaps SP into
// the guest stack); if the VEH does not see the exception here, then "a guest stack outside the TEB stack limits" makes
// exception dispatch give up before reaching any handler.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    printf("[probe] VEH code=%08lx addr=%p sp=%p\n", (unsigned long)ep->ExceptionRecord->ExceptionCode,
           ep->ExceptionRecord->ExceptionAddress, (void *)ep->ContextRecord->Sp);
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}
// 换 SP 到 buf 顶部后写 0x8（写必然违例）；x19 保存旧 SP
__asm__(
    ".text\n.p2align 2\n.globl switch_sp_fault\n"
    "switch_sp_fault:\n"
    "  mov x19, sp\n"
    "  add x9, x0, x1\n"
    "  and x9, x9, #~15\n"
    "  mov sp, x9\n"
    "  movz x0, #8\n"
    "  str wzr, [x0]\n"          // 必然写违例 | write faults for sure
    "  mov sp, x19\n"
    "  ret\n"
);
extern void switch_sp_fault(void *buf, size_t len);

int main(void) {
    AddVectoredExceptionHandler(1, veh);
    void *stk = calloc(1, 65536);
    printf("[probe] switching sp to %p + 65536 and writing 0x8\n", stk);
    fflush(stdout);
    switch_sp_fault(stk, 65536);
    printf("[probe] survived\n");
    return 0;
}
