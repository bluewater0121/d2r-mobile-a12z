// 客体线程槽（Windows）：TPIDR_EL0 指向每线程的槽块，改写后的客体代码用「块 + 槽号×8」读写 K18 / KTLS / KTLR / KGATE。
// 为什么可以这样：Windows ARM64 上 TPIDR_EL0 在 EL0 可读可写，且系统跨内核转换、异常、线程切换都按线程保存恢复
// （dm/win/scratch 的探针实测：Sleep / printf / 缺页异常之后读回同一个值）。这台机器上 x18 是 TEB 且写 x18 直接访问违例，
// 所以不能用 x18 当基址；TPIDR_EL0 是唯一一个「每线程、EL0 可用、宿主能写」的寄存器。
// Guest thread slots (Windows): TPIDR_EL0 points at a per-thread slot block, and the rewritten guest code reads and writes K18 / KTLS /
// KTLR / KGATE as "block + slot * 8". Why this works: on Windows ARM64 TPIDR_EL0 is readable and writable at EL0, and the system saves
// and restores it per thread across kernel transitions, exceptions and thread switches (measured with the probes in dm/win/scratch:
// the same value came back after Sleep / printf / a page fault). On this machine x18 is the TEB and writing it faults, so x18 cannot
// serve as the base; TPIDR_EL0 is the only "per-thread, EL0-available, host-writable" register.
#include "dm_win_compat.h"
#include "dm.h"
#include <pthread.h>

#define DM_SLOT_N 64
#define DM_SLOT_BYTES (DM_SLOT_N * 8)

static uint64_t tp_read(void) { uint64_t v = 0; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v)); return v; }
static void tp_write(uint64_t v) { __asm__ volatile("msr tpidr_el0, %0" :: "r"(v)); }

// 槽块随线程退出释放：绑在 pthread 键的析构上（客体线程走 hle_thread 的 pthread_exit，析构会跑）
// The slot block is freed when the thread exits: tied to a pthread key destructor (guest threads exit through hle_thread's pthread_exit, so it runs)
static pthread_key_t g_slot_key;
static pthread_once_t g_slot_once = PTHREAD_ONCE_INIT;
static void slot_free(void *p) { free(p); }
static void slot_key_init(void) { if (pthread_key_create(&g_slot_key, slot_free)) abort(); }

void dm_win_tls_bind(void) {
    if (tp_read()) return;
    uint8_t *b = calloc(1, DM_SLOT_BYTES);
    if (!b) abort();
    tp_write((uint64_t)b);
    pthread_once(&g_slot_once, slot_key_init);
    pthread_setspecific(g_slot_key, b);   // 析构里只 free；槽块很小（512 字节） | the destructor only frees; the block is small (512 bytes)
}
static uint8_t *slots(void) { dm_win_tls_bind(); return (uint8_t *)tp_read(); }

void *dm_slot_get(dm_key_t k) {
    if (k < 0 || k >= DM_SLOT_N) return NULL;
    return *(void **)(slots() + (size_t)k * 8);
}
void dm_slot_set(dm_key_t k, void *v) {
    if (k < 0 || k >= DM_SLOT_N) return;
    *(void **)(slots() + (size_t)k * 8) = v;
}

unsigned dm_win_thread_tid(void) { return (unsigned)GetCurrentThreadId(); }
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; (void)buf; (void)nbytes; return 0; }
