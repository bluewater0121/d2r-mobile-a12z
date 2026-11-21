#include <windows.h>
#include <pthread.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <malloc.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

__attribute__((weak)) int probe_weak(void) { return 1; }

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static pthread_once_t o = PTHREAD_ONCE_INIT;
static _Atomic int ai;

static void once_fn(void) {}

static void *thr(void *a) { (void)a; return 0; }

int main(void) {
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1 << 20);
    pthread_create(&t, &at, thr, NULL);
    pthread_join(t, NULL);
    pthread_mutex_lock(&m); pthread_mutex_unlock(&m);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    pthread_cond_timedwait(&c, &m, &ts);
    pthread_once(&o, once_fn);
    DIR *d = opendir(".");
    struct dirent *e;
    while (d && (e = readdir(d))) {}
    if (d) closedir(d);
    struct stat st;
    stat(".", &st);
    printf("%lld\n", (long long)st.st_mtime);
    ai++;
    printf("%d\n", (int)ai);
    return 0;
}
