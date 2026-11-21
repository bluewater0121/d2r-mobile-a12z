// 卡顿取栈：长帧里把客体主线程的 PC 直接采出来，看时间到底花在哪个函数（DM_STALL_PROBE=1）
// Stall sampling: during a long frame, read the guest main thread's PC so we can see which function the time goes to (DM_STALL_PROBE=1)
//
// 为什么需要它：长帧日志已经证明「整帧 30~42 ms、出帧工作只有 2~4 ms、NVN 4~5 ms、GPU 7.8 ms、取图等待 0」，
// 也就是时间花在客体主线程自己的代码里，而 NVN/HLE 各段的埋点都不含它。挂在 HLE 上的埋点看不出来，只能直接读 PC。
// Why: the long-frame log already showed that a 30~42 ms frame spends only 2~4 ms in frame work, 4~5 ms in NVN,
// 7.8 ms on the GPU and 0 waiting for the drawable -- the time goes into the guest main thread's own code, which none of the
// NVN/HLE probes cover. Only a direct PC sample can name it.
//
// 做法：宿主起一个采样线程，每 1 ms 看一次「上一帧出图到现在多久」；超过阈值就 SuspendThread + GetThreadContext 取 Pc，
// 一帧采到几次，帧结束后把命中的函数按次数排序打一行。符号用模块自带的 symtab（装载器已建 symh，这里按地址反查）。
// How: a sampler thread checks every 1 ms how long it has been since the last present; past the threshold it does
// SuspendThread + GetThreadContext for Pc, a few samples per frame, and when the frame ends prints one line ranking the hit
// functions. Symbols come from the module's own symtab (the loader already built symh; here we search by address).
#include <dm.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdatomic.h>

#ifndef CONTEXT_ARM64
#define CONTEXT_ARM64 0x00400000L
#endif

extern uint64_t now_ns(void);
extern void dm_win_tls_bind(void);

static _Atomic uint64_t g_last_present;   // 上一次出图的时刻（now_ns 基） | time of the last present (now_ns base)
static HANDLE g_th; static DWORD g_tid; static int g_on; static int g_init;
static unsigned g_reports;

// 按地址找最近的已定义符号（线性扫一遍模块 symtab，只在每次卡顿结束时对命中前几名调用，代价可忽略）
// Find the nearest defined symbol below an address (one linear pass over the module symtab; called only a few times per
// stall report, so the cost is negligible)
static const char *sym_at(uint64_t pc, const char **mod, uint64_t *off) {
    for (int i = 0; i < g_nmod; i++) {
        module_t *m = &g_mod[i];
        if (!m->base || !m->symtab || pc < (uint64_t)m->base || pc >= (uint64_t)m->base + m->size) continue;
        uint64_t o = pc - (uint64_t)m->base, best = ~0ull; unsigned bi = 0;
        for (unsigned k = 0; k < m->nsym; k++) {
            const Elf64_Sym *s = &m->symtab[k];
            if (!s->st_shndx || s->st_value > o) continue;        // 未定义或在本地址之上 | undefined, or above this address
            if (s->st_size && o >= s->st_value + s->st_size) continue;   // 有大小且落在区间外 | has a size and falls outside it
            if (best == ~0ull || s->st_value > best ||
                (s->st_value == best && (s->st_info & 0xf) == 2)) { best = s->st_value; bi = k; }
        }
        if (best == ~0ull) continue;
        *mod = m->name; *off = o - best;
        return m->strtab + m->symtab[bi].st_name;
    }
    return NULL;
}

#include <tlhelp32.h>

#define PROBE_MAX 12
#define RING_MAX 64
static struct { uint64_t pc, lr; } g_ring[RING_MAX]; static unsigned g_ringn;
static struct { uint64_t pc, lr; unsigned n; } g_hit[PROBE_MAX]; static unsigned g_nhit;
static HANDLE g_snap;   // 系统 DLL 快照，按基址反查名字 | snapshot of system DLLs, for looking names up by base

static int pc_is_host(uint64_t b, uint64_t pc) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)b;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(b + dos->e_lfanew);
    return pc >= b && pc < b + (uint64_t)nt->OptionalHeader.SizeOfImage;
}

static char g_sysname[64];
// 系统 DLL 里的地址：按 VirtualQuery 拿分区基址，再在模块快照里找同基址的名字
// Address inside a system DLL: take the allocation base with VirtualQuery, then find the module with that base in the snapshot
static const char *sys_mod(uint64_t pc, uint64_t *off) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)pc, &mi, sizeof mi) || !mi.AllocationBase) return "?";
    uint64_t b = (uint64_t)(uintptr_t)mi.AllocationBase; *off = pc - b;
    if (!g_snap && !(g_snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId()))) return "系统";
    MODULEENTRY32W me; me.dwSize = sizeof me;
    for (BOOL ok = Module32FirstW(g_snap, &me); ok; ok = Module32NextW(g_snap, &me))
        if ((uint64_t)(uintptr_t)me.modBaseAddr == b) { for (int i = 0; i < 63; i++) { g_sysname[i] = (char)me.szModule[i]; if (!me.szModule[i]) break; } g_sysname[63] = 0; return g_sysname; }   // 宽字符名转窄，只用于日志 | wide name to narrow, log only
    return "系统";
}

static void probe_report(uint64_t stall_ns, unsigned total) {
    if (!g_nhit || g_reports >= 60) return;
    char line[1400]; size_t o = 0;
    if (g_nhit > 4) g_nhit = 4;
    for (unsigned i = 0; i < g_nhit; i++) {
        char a[160], l[160];
        const char *mod = "?"; uint64_t off = 0;
        const char *nm = sym_at(g_hit[i].pc, &mod, &off);
        if (nm) snprintf(a, sizeof a, "%s:%s+%#llx", mod, strlen(nm) > 60 ? "(长名)" : nm, (unsigned long long)off);
        else {
            extern IMAGE_DOS_HEADER __ImageBase;
            uint64_t b = (uint64_t)(uintptr_t)&__ImageBase;
            if (pc_is_host(b, g_hit[i].pc)) snprintf(a, sizeof a, "host+%#llx", (unsigned long long)(g_hit[i].pc - b));
            else { uint64_t o2 = 0; const char *s = sys_mod(g_hit[i].pc, &o2); snprintf(a, sizeof a, "%s+%#llx", s, (unsigned long long)o2); }
        }
        off = 0; mod = "?";
        nm = sym_at(g_hit[i].lr, &mod, &off);
        if (nm) snprintf(l, sizeof l, "%s:%s+%#llx", mod, strlen(nm) > 60 ? "(长名)" : nm, (unsigned long long)off);
        else {
            extern IMAGE_DOS_HEADER __ImageBase;
            uint64_t b = (uint64_t)(uintptr_t)&__ImageBase;
            if (pc_is_host(b, g_hit[i].lr)) snprintf(l, sizeof l, "host+%#llx", (unsigned long long)(g_hit[i].lr - b));
            else { uint64_t o2 = 0; const char *s = sys_mod(g_hit[i].lr, &o2); snprintf(l, sizeof l, "%s+%#llx", s, (unsigned long long)o2); }
        }
        o += snprintf(line + o, sizeof line - o, "%s <- %s %u/%u；", a, l, g_hit[i].n, total);
    }
    LOG("[卡顿] 整帧 %.1f ms，采到 %u 次，命中：%s（第 %u 次记录）", stall_ns / 1e6, total, line, ++g_reports);
}

static uint64_t g_max_age; static unsigned g_samples; static int g_stall;

// 整帧采样：帧内每 1 ms 采一次（>10 ms 起采），帧结束后如果这一帧超了 22 ms，就把整帧的命中排一排——
// 早段（CPU 突增那段）与晚段（等待）都在里面，比只看尾部更能说明时间花在哪
// Whole-frame sampling: sample once per 1 ms during the frame (from 10 ms on); when the frame turns out to exceed 22 ms, rank all its hits --
// both the early part (the CPU burst) and the late part (the wait) are included, which says more than looking at the tail alone
static DWORD WINAPI sampler(LPVOID arg) { (void)arg; dm_win_tls_bind();
    while (g_on) {
        Sleep(1);
        uint64_t last = atomic_load(&g_last_present); if (!last) continue;
        uint64_t age = now_ns() - last;
        if (age < 10000000ull) {                          // 帧已结束：结算上一帧 | frame finished: settle the previous frame
            if (g_stall && g_ringn) {
                g_nhit = 0;
                for (unsigned i = 0; i < g_ringn; i++) {
                    unsigned k; for (k = 0; k < g_nhit; k++) if (g_hit[k].pc == g_ring[i].pc) { g_hit[k].n++; break; }
                    if (k == g_nhit && g_nhit < PROBE_MAX) { g_hit[g_nhit].pc = g_ring[i].pc; g_hit[g_nhit].lr = g_ring[i].lr; g_hit[g_nhit].n = 1; g_nhit++; }
                }
                probe_report(g_max_age, g_ringn);
            }
            g_ringn = 0; g_samples = 0; g_max_age = 0; g_stall = 0;
            continue;
        }
        if (age > 80000000ull) continue;                  // 读取画面/换场景的长帧不采（另有 150 ms 一级的属于加载） | skip loading-screen or scene-change frames (the 150 ms tier is loading)
        if (age > g_max_age) g_max_age = age;
        if (age > 22000000ull) g_stall = 1;
        CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(g_th) == (DWORD)-1) continue;
        if (GetThreadContext(g_th, &c) && g_ringn < RING_MAX) { g_ring[g_ringn].pc = (uint64_t)c.Pc; g_ring[g_ringn].lr = (uint64_t)c.Lr; g_ringn++; g_samples++; }
        ResumeThread(g_th);
    }
    return 0;
}

// 由 front_win.c 在客体主线程上调用一次：读开关、起采样线程；采样目标线程在第一次出图时登记（见 dm_stall_probe_frame）
// Called once on the guest main thread from front_win.c: read the switch, start the sampler; the thread to sample is registered on
// the first present (see dm_stall_probe_frame)
void dm_stall_probe_begin(void) {
    const char *e = getenv("DM_STALL_PROBE"); if (!e || !atoi(e)) return;
    if (g_init) return; g_init = 1;
    g_on = 1;
    HANDLE t = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    if (t) { SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL); CloseHandle(t); }
    LOG("卡顿取栈：已开（每 1 ms 采一次出图线程 PC，阈值 22 ms）");
    for (int i = 0; i < g_nmod; i++) LOG("卡顿取栈：模块 %s 基址 %#llx 大小 %#llx 符号 %u 个", g_mod[i].name,
        (unsigned long long)(uintptr_t)g_mod[i].base, (unsigned long long)g_mod[i].size, g_mod[i].nsym);
}
// 每次出图记一次：首帧把「出图线程」登记为采样目标（客体主线程未必是出图的那条）
// Once per present: the first call registers the present thread as the sampling target (the guest main thread need not be the one presenting)
void dm_stall_probe_frame(uint64_t present_ns) {
    if (!g_init) return;
    if (!g_th) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_th, 0, FALSE, DUPLICATE_SAME_ACCESS);
        g_tid = GetCurrentThreadId();
        LOG("卡顿取栈：出图线程是 tid %u（采样目标已登记）", (unsigned)g_tid);
    }
    atomic_store(&g_last_present, present_ns);
}
#else
void dm_stall_probe_begin(void) {}
void dm_stall_probe_frame(uint64_t present_ns) { (void)present_ns; }
#endif

