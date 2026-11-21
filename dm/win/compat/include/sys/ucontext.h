// sys/ucontext.h：dm_uctx.h 读「出错现场寄存器」用的 ucontext_t。Windows 上没有信号，现场来自 VEH 的 CONTEXT
// （compat/crash_win.c 把 CONTEXT 抄进这里的同布局结构，dm_uctx.h 的宏照常读写）。
// sys/ucontext.h: the ucontext_t dm_uctx.h uses to read fault-time registers. Windows has no signals; the state comes from the VEH CONTEXT
// (compat/crash_win.c copies CONTEXT into this same-layout struct, and dm_uctx.h's macros read and write it as usual).
#pragma once
#include <stdint.h>
typedef struct { uint64_t regs[31]; uint64_t sp, pc, pstate; } dm_win_mcontext_t;
typedef struct ucontext_t { dm_win_mcontext_t uc_mcontext; unsigned uc_flags; } ucontext_t;
// 从 Windows CONTEXT 抄寄存器（crash_win.c 用） | copy the registers from a Windows CONTEXT (used by crash_win.c)
struct _CONTEXT;
void dm_win_uctx_from_context(ucontext_t *uc, const struct _CONTEXT *c);
