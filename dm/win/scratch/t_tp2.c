#include <stdio.h>
#include <windows.h>
#include <stdint.h>
#include <setjmp.h>

// Each probe is run in a child process so a fault only kills the probe.
static void try_one(int which) {
    uint64_t v = 0;
    switch (which) {
    case 0: __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v)); break;
    case 1: __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(v)); break;
    case 2: v = 0x1122334455667788ull; __asm__ volatile("msr tpidr_el0, %0" :: "r"(v)); break;
    case 3: __asm__ volatile("mov %0, x18" : "=r"(v)); break;
    case 4: v = 0x99aabbccddeeff00ull; __asm__ volatile("mov x18, %0" :: "r"(v)); break;
    }
    printf("case %d ok v=%#llx\n", which, (unsigned long long)v);
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc > 1) { try_one(atoi(argv[1])); return 0; }
    for (int i = 0; i < 5; i++) {
        char cmd[256]; snprintf(cmd, sizeof cmd, "\"%s\" %d", argv[0], i);
        STARTUPINFOA si = {0}; si.cb = sizeof si; PROCESS_INFORMATION pi;
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            DWORD code = 0; WaitForSingleObject(pi.hProcess, INFINITE); GetExitCodeProcess(pi.hProcess, &code);
            printf("case %d exit=%#lx (%s)\n", i, code, code == 0xc0000005 ? "ACCESS VIOLATION" : "ok");
        }
    }
    return 0;
}
