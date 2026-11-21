#include <stdio.h>
#include <windows.h>
#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>

static pthread_key_t k[4];

static void scan(void *base, uint64_t magic) {
    uint8_t *p = (uint8_t *)base;
    for (int off = -8192; off < 8192; off += 8) {
        uint64_t v;
        // guard against unreadable pages
        if (IsBadReadPtr(p + off, 8)) continue;
        v = *(uint64_t *)(p + off);
        if (v == magic) printf("   magic %#llx at base%+d\n", (unsigned long long)magic, off);
        if (v == magic + 1) printf("   magic+1 %#llx at base%+d\n", (unsigned long long)magic, off);
    }
}

static void *thr(void *a) {
    (void)a;
    printf("thread self=%#llx\n", (unsigned long long)pthread_self());
    for (int i = 0; i < 4; i++) pthread_setspecific(k[i], (void *)(0xAAAA0000ull + i));
    scan((void *)pthread_self(), 0xAAAA0000ull);
    for (int i = 0; i < 4; i++) printf("  get[%d]=%#llx\n", i, (unsigned long long)pthread_getspecific(k[i]));
    return NULL;
}

int main(void) {
    for (int i = 0; i < 4; i++) { int r = pthread_key_create(&k[i], NULL); printf("key[%d]=%u r=%d\n", i, k[i], r); }
    printf("main self=%#llx\n", (unsigned long long)pthread_self());
    for (int i = 0; i < 4; i++) pthread_setspecific(k[i], (void *)(0xAAAA0000ull + i));
    scan((void *)pthread_self(), 0xAAAA0000ull);
    for (int i = 0; i < 4; i++) printf("  get[%d]=%#llx\n", i, (unsigned long long)pthread_getspecific(k[i]));
    pthread_t t; pthread_create(&t, NULL, thr, NULL); pthread_join(t, NULL);
    return 0;
}
