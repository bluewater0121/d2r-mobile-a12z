#include <stdio.h>
#include <windows.h>
#include <stdint.h>

static uint64_t rd_tpidr(void) { uint64_t v = 0; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v)); return v; }
static uint64_t rd_tpidrro(void) { uint64_t v = 0; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(v)); return v; }
static uint64_t rd_x18(void) { uint64_t v = 0; __asm__ volatile("mov %0, x18" : "=r"(v)); return v; }
static void wr_tpidr(uint64_t v) { __asm__ volatile("msr tpidr_el0, %0" :: "r"(v)); }
static void wr_x18(uint64_t v) { __asm__ volatile("mov x18, %0" :: "r"(v)); }

static void probe(const char *tag) {
    printf("%-12s tpidr_el0=%#llx tpidrro_el0=%#llx x18=%#llx\n", tag,
           (unsigned long long)rd_tpidr(), (unsigned long long)rd_tpidrro(), (unsigned long long)rd_x18());
}

static void *thr(void *p) { (void)p; probe("thread"); return 0; }

int main(void) {
    probe("main");
    wr_tpidr(0x1122334455667788ull);
    probe("after wr tp");
    wr_x18(0x99aabbccddeeff00ull);
    probe("after wr x18");
    HANDLE h = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)thr, NULL, 0, NULL);
    WaitForSingleObject(h, INFINITE);
    return 0;
}
