// 探针：页面缺页被 VEH 认领（返回 EXCEPTION_CONTINUE_EXECUTION）时，若线程的 SP 不在 TEB 登记的栈区间里
// （客体线程就是这样跑的：dm_thread_enter 把 SP 换到客体栈），系统在「继续执行」这条路上会不会判现场无效而 fail-fast（0xC0000409 / 码 0xd）。
// 三种变体（argv[1]）：heapsp = SP 换到堆上自备栈；tebsp = SP 换到深一点的 TEB 栈上（对照）；nofault = 只换 SP 不制造缺页（对照）。
// Probe: when a page fault is claimed by a VEH (returning EXCEPTION_CONTINUE_EXECUTION), if the thread's SP is not inside the stack range
// recorded in the TEB (guest threads run exactly like that: dm_thread_enter swaps SP onto the guest stack), does the system judge the
// context invalid on the "continue execution" path and fail fast (0xC0000409 / code 0xd)?
// Three variants (argv[1]): heapsp = SP on a heap-provided stack; tebsp = SP deep on the TEB stack (control); nofault = only swap SP, raise no fault (control).
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

static volatile int g_mode;
static void *g_page, *g_stack_top;

__asm__(
".text\n"
".globl probe_call_on_stack\n"
"probe_call_on_stack:\n"
"  mov x9, sp\n"
"  str x9, [x0, #-16]\n"
"  sub sp, x0, #16\n"
"  mov x0, x2\n"
"  blr x1\n"
"  mov x9, sp\n"
"  ldr x9, [x9]\n"
"  mov sp, x9\n"
"  ret\n"
);

typedef void (*body_fn)(void *);
void probe_call_on_stack(void *top, body_fn fn, void *arg);

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t a = (uint64_t)er->ExceptionInformation[1];
    if (a < (uint64_t)g_page || a >= (uint64_t)g_page + 4096) return EXCEPTION_CONTINUE_SEARCH;
    printf("[probe] VEH 认领 addr=%#llx pc=%#llx sp=%#llx\n", (unsigned long long)a,
           (unsigned long long)ep->ContextRecord->Pc, (unsigned long long)ep->ContextRecord->Sp);
    fflush(stdout);
    DWORD old;
    if (!VirtualProtect(g_page, 4096, PAGE_READWRITE, &old)) { printf("[probe] VirtualProtect 失败 %lu\n", GetLastError()); fflush(stdout); return EXCEPTION_CONTINUE_SEARCH; }
    // 继续执行前把 TEB 的栈区间放宽到覆盖现场 SP（不动宿主原有区间）：验证是不是这条校验在杀进程
    // Widen the TEB stack range to cover the faulting SP before continuing (without dropping the host range): test whether this validation is what kills the process
    if (getenv("PROBE_FIX")) {
        uintptr_t *tib = (uintptr_t *)((uintptr_t)NtCurrentTeb() + 8);
        uintptr_t sp = (uintptr_t)ep->ContextRecord->Sp;
        if (sp + 0x2000 > tib[0]) tib[0] = sp + 0x2000;
        if (sp - 0x2000 < tib[1]) tib[1] = sp - 0x2000;
        printf("[probe] 放宽 TEB 栈：base=%#llx limit=%#llx\n", (unsigned long long)tib[0], (unsigned long long)tib[1]); fflush(stdout);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void body(void *arg) {
    (void)arg;
    volatile int *p = (volatile int *)g_page;
    printf("[probe] 进入 body\n"); fflush(stdout);
    if (g_mode == 3) { printf("[probe] nofault：只换 SP 不写\n"); fflush(stdout); return; }
    *p = 0x1234;                      // 第一次：只读页 → 缺页 → VEH 认领 → 继续执行
    printf("[probe] 第一次写完成 val=%#x\n", *p); fflush(stdout);
    *p = 0x5678;                      // 第二次：页已可写，正常写
    printf("[probe] 第二次写完成 val=%#x\n", *p); fflush(stdout);
}

int main(int argc, char **argv) {
    const char *m = argc > 1 ? argv[1] : "heapsp";
    g_mode = !strcmp(m, "heapsp") ? 1 : !strcmp(m, "tebsp") ? 2 : 3;
    AddVectoredExceptionHandler(1, veh);
    g_page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    *(volatile int *)g_page = 0;
    DWORD old;
    if (g_mode != 3) { if (!VirtualProtect(g_page, 4096, PAGE_READONLY, &old)) { printf("[probe] 设只读失败 %lu\n", GetLastError()); return 2; } }
    if (g_mode == 1) {
        g_stack_top = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_stack_top = (void *)((uintptr_t)g_stack_top + 65536 - 64);
        printf("[probe] heapsp：SP → %p（堆上自备栈）\n", g_stack_top); fflush(stdout);
        probe_call_on_stack(g_stack_top, body, NULL);
    } else if (g_mode == 2) {
        uintptr_t base = *(uintptr_t *)((uintptr_t)NtCurrentTeb() + 8);
        uintptr_t lim = *(uintptr_t *)((uintptr_t)NtCurrentTeb() + 0x10);
        uintptr_t sp = base - 4096;   // 深一点、但仍在 TEB 栈区间内 | deeper but still inside the TEB stack range
        printf("[probe] tebsp：base=%#llx limit=%#llx SP → %#llx\n", (unsigned long long)base, (unsigned long long)lim, (unsigned long long)sp); fflush(stdout);
        probe_call_on_stack((void *)sp, body, NULL);
    } else {
        body(NULL);
    }
    printf("[probe] 全部做完\n"); fflush(stdout);
    return 0;
}
