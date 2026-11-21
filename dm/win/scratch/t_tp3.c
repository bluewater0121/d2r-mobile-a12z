#include <stdio.h>
#include <windows.h>
#include <stdint.h>

static uint64_t rd(void) { uint64_t v = 0; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v)); return v; }
static void wr(uint64_t v) { __asm__ volatile("msr tpidr_el0, %0" :: "r"(v)); }

static LONG CALLBACK on_veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        printf("VEH: rv=%#llx fault-pc=%#llx\n",
               (unsigned long long)rd(),
               (unsigned long long)ep->ContextRecord->Pc);
        ep->ContextRecord->Pc += 4;           // skip the faulting str
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(void) {
    wr(0x1122334455667788ull);
    printf("set         %#llx\n", (unsigned long long)rd());
    Sleep(10);
    printf("after Sleep %#llx\n", (unsigned long long)rd());
    printf("and stdout  %#llx\n", (unsigned long long)rd());
    volatile int arr[4] = {1, 2, 3, 4};
    long s = 0; for (int i = 0; i < 1000; i++) s += arr[i & 3];
    printf("after loop  %#llx s=%ld\n", (unsigned long long)rd(), s);
    AddVectoredExceptionHandler(1, on_veh);
    *(volatile int *)0x10 = 1;                // fault, the handler continues
    printf("end         %#llx\n", (unsigned long long)rd());
    return 0;
}
