// 探针：验证「客体线程栈 + TPIDR_EL0 指向自定义槽块」时，向量化异常处理器（VEH）是否还能拿到访问违例。
// 三个变体（argv[1]）：plain = 普通栈普通寄存器；gueststk = 自定义栈；gueststk_tp = 自定义栈 + TPIDR_EL0 指向 512 字节槽块。
// Probe: verify whether the vectored exception handler still sees an access violation on a "guest thread stack with TPIDR_EL0
// pointing at a custom slot block" thread.
// Variants (argv[1]): plain = ordinary stack; gueststk = custom stack; gueststk_tp = custom stack + TPIDR_EL0 pointing at a 512-byte block.
#include <windows.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long g_mode;
static volatile int g_hit;
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    fprintf(stderr, "[probe] VEH code=%08lx addr=%p pc=%p sp=%p\n",
            (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress,
            (void *)ep->ContextRecord->Pc, (void *)ep->ContextRecord->Sp);
    fflush(stderr);
    g_hit = 1;
    return EXCEPTION_CONTINUE_SEARCH;   // 交给进程自己收尾 | let the process finish
}
static uint64_t rd_tpidr(void) { uint64_t v; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v)); return v; }
static uint64_t rd_x18(void) { uint64_t v; __asm__ volatile("mov %0, x18" : "=r"(v)); return v; }
static void wr_tpidr(uint64_t v) { __asm__ volatile("msr tpidr_el0, %0" :: "r"(v)); }
static void *fn(void *p) {
    (void)p;
    if (g_mode == 2) wr_tpidr((uint64_t)calloc(64, 8));
    fprintf(stderr, "[probe] thread tpidr=%#llx x18=%#llx\n",
            (unsigned long long)rd_tpidr(), (unsigned long long)rd_x18());
    fflush(stderr);
    *(volatile int *)8 = 1;             // 写 0x8：读可访问、写必然违例 | write to 0x8: readable, so a write faults for sure
    return NULL;
}
int main(int argc, char **argv) {
    g_mode = argc > 1 ? (strcmp(argv[1], "gueststk") == 0 ? 1 : strcmp(argv[1], "gueststk_tp") == 0 ? 2 : 0) : 0;
    AddVectoredExceptionHandler(1, veh);
    pthread_t t;
    pthread_attr_t a; pthread_attr_init(&a);
    if (g_mode) {
        void *stk = calloc(1, 65536);
        pthread_attr_setstack(&a, stk, 65536);
    }
    pthread_create(&t, &a, fn, NULL);
    pthread_join(t, NULL);
    printf("[probe] survived hit=%d\n", g_hit);
    return 0;
}
