// 崩溃处理的最薄一层：把向量化异常处理器（VEH）装上去，并把 CONTEXT 抄成 dm_uctx.h 认得的 ucontext_t。
// VEH 在 SEH 之前被调用；返回 EXCEPTION_CONTINUE_EXECUTION 时进程继续（缺页钩子认领时走这条）。
// The thinnest layer of crash handling: install the vectored exception handler (VEH) and copy CONTEXT into the ucontext_t dm_uctx.h understands.
// The VEH is called before SEH; returning EXCEPTION_CONTINUE_EXECUTION resumes the process (the path taken when a page fault hook claims the fault).
#include "dm_win_compat.h"
#include <sys/ucontext.h>

void dm_win_uctx_from_context(ucontext_t *uc, const CONTEXT *c) {
    memset(uc, 0, sizeof *uc);
    for (int i = 0; i <= 28; i++) uc->uc_mcontext.regs[i] = c->X[i];
    uc->uc_mcontext.regs[29] = c->Fp;  uc->uc_mcontext.regs[30] = c->Lr;
    uc->uc_mcontext.sp = c->Sp; uc->uc_mcontext.pc = c->Pc;
    uc->uc_mcontext.pstate = c->Cpsr;
}
// 把钩子改过的寄存器写回现场（缺页钩子目前不改寄存器，留着这条路径只为与苹果的 uc 契约一致）
// Write hook-modified registers back into the context (page fault hooks do not modify registers today; this path exists only for the same uc contract as Apple's)
void dm_win_uctx_to_context(ucontext_t *uc, CONTEXT *c) {
    for (int i = 0; i <= 28; i++) c->X[i] = uc->uc_mcontext.regs[i];
    c->Fp = uc->uc_mcontext.regs[29]; c->Lr = uc->uc_mcontext.regs[30];
    c->Sp = uc->uc_mcontext.sp; c->Pc = uc->uc_mcontext.pc;
}

static int (*g_handler)(EXCEPTION_POINTERS *ep);
// DM_VEH_LOG=1：把每条异常（含二级异常，如 RtlUnwind 期间的 STATUS_BAD_STACK）打到 stderr，排「没有 CRASH 记录就死」用
// DM_VEH_LOG=1: print every exception (including secondary ones such as STATUS_BAD_STACK during RtlUnwind) to stderr, for "died with no crash record"
static int g_veh_log;
// 「继续执行」前把本线程 TEB 里的栈区间放宽到覆盖现场 SP：客体线程跑在客体栈上（SP 不在宿主 1 MiB 栈里），
// 而 Windows 在继续执行这条路上按 TEB 的栈区间校验现场，SP 不在区间内就判「无效现场」直接 fail-fast（0xC0000409，码 0xd = FAST_FAIL_INVALID_SET_OF_CONTEXT）。
// 只放宽、不收窄（宿主的栈区间保留），缺页认领后重执行才有得继续
// Widen this thread's TEB stack range to cover the faulting SP before continuing: guest threads run on guest stacks (SP not inside the host 1 MiB stack),
// and on the continue path Windows validates the context against the TEB stack range; an SP outside it counts as an invalid context and fails fast
// (0xC0000409, code 0xd = FAST_FAIL_INVALID_SET_OF_CONTEXT). Only widen, never narrow (the host range is kept), so a claimed page fault can be re-executed
static void widen_stack_range(uint64_t sp) {
    uint64_t *tib = (uint64_t *)((uintptr_t)NtCurrentTeb() + 8);   // [0]=StackBase [1]=StackLimit | StackBase / StackLimit
    const uint64_t slack = 0x8000;
    if (sp < slack) return;
    if (sp + slack > tib[0]) tib[0] = sp + slack;
    if (sp - slack < tib[1]) tib[1] = sp - slack;
}
// DM_VEH_TRACE=1：处理器进出各打一个字节（「<」= 已返回、「!」= 在处理器内部死的），排「没有崩溃记录就消失」
// DM_VEH_TRACE=1: a byte on entry and just before each return ('<' = returned, '!' = died inside the handler), for "vanished with no crash record"
static int g_veh_trace;
static void vtrace(const char *s) {
    if (!g_veh_trace) { static int init; if (!init) { init = 1; g_veh_trace = getenv("DM_VEH_TRACE") != NULL; } if (!g_veh_trace) return; }
    DWORD n; WriteFile(GetStdHandle(STD_ERROR_HANDLE), s, 2, &n, NULL);
}
static LONG CALLBACK veh_entry(EXCEPTION_POINTERS *ep) {
    // 最薄的一层：连 CRT 都不碰（异常可能落在只有十几 KiB 的客体栈上，格式化字符串会把栈再撑掉一层）
    // The thinnest possible layer: no CRT at all (the exception may land on a guest stack of only a dozen KiB, and formatting strings would eat another layer)
    // 默认不打「VEH」标记：缺页跟踪每帧几十次，全打会把日志冲烂（DM_VEH_LOG / DM_VEH_TRACE 时才打）
    // No "VEH" marker by default: write tracking faults dozens of times per frame and would flood the log (printed only with DM_VEH_LOG / DM_VEH_TRACE)
    static int g_veh_init;
    if (!g_veh_init) { g_veh_init = 1; g_veh_log = getenv("DM_VEH_LOG") != NULL; g_veh_trace = getenv("DM_VEH_TRACE") != NULL; }
    if (g_veh_log || g_veh_trace) { DWORD n; WriteFile(GetStdHandle(STD_ERROR_HANDLE), "VEH\r\n", 5, &n, NULL); }
    if (g_veh_log) {
        const EXCEPTION_RECORD *er = ep->ExceptionRecord;
        fprintf(stderr, "[dm] VEH code=%08lx flags=%lx addr=%p n=%lu pc=%p sp=%p lr=%p\n",
                (unsigned long)er->ExceptionCode, (unsigned long)er->ExceptionFlags, er->ExceptionAddress,
                (unsigned long)er->NumberParameters, (void *)ep->ContextRecord->Pc,
                (void *)ep->ContextRecord->Sp, (void *)ep->ContextRecord->Lr);
        // 帧链（x29）：二级异常（如 STATUS_BAD_STACK）里 pc 在 ntdll，只有帧链能说明是谁在展开栈
        // Frame chain (x29): on a secondary exception such as STATUS_BAD_STACK the pc sits in ntdll, so only the chain shows who was unwinding
        uint64_t *fp = (uint64_t *)ep->ContextRecord->Fp;
        fprintf(stderr, "[dm] VEH chain:");
        for (int i = 0; i < 12 && fp && (uint64_t)fp > 0x10000; i++) {
            uint64_t prev = fp[0], ret = fp[1];
            HMODULE hm = NULL; char path[MAX_PATH] = "";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(uintptr_t)ret, &hm)) GetModuleFileNameA(hm, path, MAX_PATH);
            const char *nm = strrchr(path, '\\'); nm = nm ? nm + 1 : path;
            fprintf(stderr, " [%d]%s+%#llx", i, nm, (unsigned long long)(ret - (uint64_t)(uintptr_t)hm));
            if (prev <= (uint64_t)fp) break;
            fp = (uint64_t *)prev;
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    if (!g_handler) { vtrace("V<"); return EXCEPTION_CONTINUE_SEARCH; }
    // 只处理真正的硬件事件；调试断点（EXCEPTION_BREAKPOINT）也交给分发，由 trap 钩子决定认领与否
    // Only handle real hardware events; debug breaks (EXCEPTION_BREAKPOINT) also go to the dispatch, and the trap hook decides whether to claim them
    if (g_handler(ep)) { widen_stack_range(ep->ContextRecord->Sp); vtrace("V<"); return EXCEPTION_CONTINUE_EXECUTION; }
    vtrace("V<");
    return EXCEPTION_CONTINUE_SEARCH;
}
void dm_win_crash_install(int (*fn)(EXCEPTION_POINTERS *)) {
    g_handler = fn;
    AddVectoredExceptionHandler(1, veh_entry);
}
