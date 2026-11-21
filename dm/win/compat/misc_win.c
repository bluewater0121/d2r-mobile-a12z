// Windows 上宿主用到的 POSIX / 苹果 libc 杂项（与苹果 / 安卓构建里的同名接口语义一致）。
// 约定：任何一项都可能被热路径调用（strlcpy、clock_gettime、malloc_size），所以这里不放锁、不打日志；
// 失败一律走 errno / 返回值，不 abort（只有分配槽块失败这种不可恢复的才 abort，见 tls_win.c）。
// The POSIX / Apple libc odds and ends the host uses on Windows (semantics identical to the same-named interfaces in the Apple / Android builds).
// Convention: any of these may be called on a hot path (strlcpy, clock_gettime, malloc_size), so no locks and no logging here;
// failures go through errno / return values and never abort (only unrecoverable ones such as a failed slot allocation abort, see tls_win.c).
#include "dm_win_compat.h"
#include <bcrypt.h>
#include <stdarg.h>
#include <psapi.h>
#include <dlfcn.h>
#include <malloc/malloc.h>
#include <pthread/qos.h>

// 苹果的 mach_absolute_time（nvn_entry.S 以 bl 调用，供 DM_NVN_PROF 计时）：QPC 纳秒
// Apple's mach_absolute_time (called with bl from nvn_entry.S for DM_NVN_PROF timing): QPC nanoseconds
uint64_t mach_absolute_time(void) {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    if (!f.QuadPart) return (uint64_t)c.QuadPart;
    // 先乘后除会溢出：拆成 秒 + 余数 | multiplying first would overflow: split into seconds plus remainder
    uint64_t s = (uint64_t)c.QuadPart / (uint64_t)f.QuadPart, r = (uint64_t)c.QuadPart % (uint64_t)f.QuadPart;
    return s * 1000000000ull + (uint64_t)((double)r * 1e9 / (double)f.QuadPart);
}
// qos 头里的线程变量（构造时一律「未指定」）| the thread variable from the qos header (always "unspecified" at construction)
__thread qos_class_t dm_qos_self_thread_ = QOS_CLASS_UNSPECIFIED;
#undef signal   // 下面要定义自己的 dm_win_signal，这里先放掉宏 | define dm_win_signal below, so drop the macro here
#undef free     // 这个文件里的 free 就是 CRT 的（dm_free 要落到它）| free in this file is the CRT's (dm_free falls through to it)
#undef open     // dm_win_open 里要调真正的 _open | dm_win_open calls the real _open

// ---------- 文件 ----------
// 一律二进制模式，见 dm_win_compat.h | always binary mode, see dm_win_compat.h
int dm_win_open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    return _open(path, flags | _O_BINARY, mode);
}


// ---------- 字符串 ----------
size_t strlcpy(char *dst, const char *src, size_t cap) {
    size_t n = src ? strlen(src) : 0;
    if (cap) { size_t k = n < cap - 1 ? n : cap - 1; if (n) memcpy(dst, src, k); dst[k] = 0; }
    return n;
}
size_t strlcat(char *dst, const char *src, size_t cap) {
    size_t d = strlen(dst), n = src ? strlen(src) : 0;
    if (d >= cap) return cap + n;
    if (n) { size_t k = n < cap - d - 1 ? n : cap - d - 1; memcpy(dst + d, src, k); dst[d + k] = 0; }
    return d + n;
}

// ---------- 随机 ----------
// 系统首选随机源（bcrypt）；拿不到就退到 QPC + 地址混洗。只给打散 / ID 用 | the system preferred RNG (bcrypt); falls back to QPC + address mixing. Only for hashing / ids
uint32_t arc4random(void) {
    uint32_t v = 0;
    if (BCryptGenRandom(NULL, (PUCHAR)&v, sizeof v, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) return v;
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    v = (uint32_t)(c.QuadPart ^ (uint64_t)(uintptr_t)&v);
    v ^= v >> 16; v *= 0x7feb352d; v ^= v >> 15; v *= 0x846ca68b; v ^= v >> 16;
    return v;
}
uint32_t arc4random_uniform(uint32_t upper) { return upper ? arc4random() % upper : 0; }

// ---------- 临时文件 ----------
// XXXXXX 换成 6 位随机十六进制，O_CREAT|O_EXCL 保证唯一（与 BSD 一样失败重试有限次）
// XXXXXX is replaced by 6 random hex digits and O_CREAT|O_EXCL guarantees uniqueness (bounded retries on failure, as BSD)
int mkstemp(char *tmpl) {
    size_t n = strlen(tmpl); if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX")) { errno = EINVAL; return -1; }
    char *x = tmpl + n - 6;
    for (int i = 0; i < 200; i++) {
        uint32_t r = arc4random();
        for (int k = 0; k < 6; k++) x[k] = "0123456789abcdef"[(r >> (k * 4)) & 15];
        int fd = _open(tmpl, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, 0600);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST; return -1;
}
char *mkdtemp(char *tmpl) {
    size_t n = strlen(tmpl); if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX")) { errno = EINVAL; return NULL; }
    char *x = tmpl + n - 6;
    for (int i = 0; i < 200; i++) {
        uint32_t r = arc4random();
        for (int k = 0; k < 6; k++) x[k] = "0123456789abcdef"[(r >> (k * 4)) & 15];
        if (!_mkdir(tmpl)) return tmpl;
        if (errno != EEXIST) return NULL;
    }
    errno = EEXIST; return NULL;
}

char *realpath(const char *path, char *resolved) {
    char buf[PATH_MAX];
    if (!_fullpath(buf, path, sizeof buf)) return NULL;
    if (resolved) { strcpy(resolved, buf); return resolved; }
    char *p = malloc(strlen(buf) + 1); if (!p) return NULL; strcpy(p, buf); return p;
}
int getdtablesize(void) { return 8192; }

// ---------- 时钟 ----------
// 单调时基一律 QPC（纳秒）；进程 / 线程 CPU 时间用 GetProcessTimes / GetThreadTimes。
// CLOCK_REALTIME 用 FILETIME 换算到 unix 秒，保留纳秒 —— 游戏会按它写存档时间戳，精度要够。
// Monotonic always QPC (nanoseconds); process / thread CPU time from GetProcessTimes / GetThreadTimes.
// CLOCK_REALTIME uses FILETIME converted to unix seconds with nanoseconds -- the game writes save timestamps from it and needs the precision.
static uint64_t qpc_ns(void) {
    LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
static uint64_t filetime_ns(const FILETIME *ft) {
    uint64_t t = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;   // 100 ns since 1601 | 100 ns since 1601
    return t * 100ull;
}
int dm_win_clock_gettime(int clock_id, struct timespec *ts) {
    if (!ts) { errno = EINVAL; return -1; }
    uint64_t ns = 0;
    switch (clock_id) {
    case CLOCK_REALTIME: {
        FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
        int64_t n = (int64_t)filetime_ns(&ft) - 11644473600000000000ll;   // 1601 → 1970 | 1601 -> 1970
        if (n < 0) n = 0; ns = (uint64_t)n; break; }
    case CLOCK_PROCESS_CPUTIME_ID: {
        FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        ns = filetime_ns(&k) + filetime_ns(&u); break; }
    case CLOCK_THREAD_CPUTIME_ID: {
        FILETIME c, e, k, u; GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
        ns = filetime_ns(&k) + filetime_ns(&u); break; }
    default: ns = qpc_ns(); break;   // MONOTONIC / BOOTTIME / UPTIME_RAW / MONOTONIC_RAW
    }
    ts->tv_sec = (time_t)(ns / 1000000000ull); ts->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}
uint64_t clock_gettime_nsec_np(int clock_id) { struct timespec t; return dm_win_clock_gettime(clock_id, &t) ? 0 : (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
// nanosleep 由 winpthreads 的 pthread_time.h 提供（真等待定时器，精度优于 Sleep）| nanosleep comes from winpthreads' pthread_time.h (a real waitable timer, better than Sleep)
size_t getpagesize(void) { return DM_HOST_PAGE; }
long sysconf(int name) {
    switch (name) {
    case _SC_PAGESIZE: return (long)DM_HOST_PAGE;
    case _SC_CLK_TCK: return 100;
    case _SC_NPROCESSORS_ONLN: { SYSTEM_INFO si; GetSystemInfo(&si); return (long)si.dwNumberOfProcessors; }
    default: return -1;
    }
}
void dm_win_icache_invalidate(void *addr, size_t len) { FlushInstructionCache(GetCurrentProcess(), addr, len); }

// ---------- 环境变量 ----------
int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value ? value : "") ? -1 : 0;
}
int unsetenv(const char *name) { return _putenv_s(name, "") ? -1 : 0; }

// ---------- 内存 ----------
int posix_memalign(void **out, size_t align, size_t size) {
    if (!out || align < sizeof(void *)) return 22;
    void *p = _aligned_malloc(size, align); if (!p) return 12;
    *out = p; return 0;
}
malloc_zone_t *malloc_default_zone(void) { static malloc_zone_t z; return &z; }
size_t malloc_zone_size(malloc_zone_t *z, const void *p) { (void)z; return p ? _msize(p) : 0; }
void malloc_zone_statistics(malloc_zone_t *z, malloc_statistics_t *s) {
    (void)z;
    if (!s) return;
    PROCESS_MEMORY_COUNTERS pmc;
    memset(s, 0, sizeof *s);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) { s->size_in_use = pmc.WorkingSetSize; s->max_size_in_use = pmc.PeakWorkingSetSize; s->size_allocated = pmc.PagefileUsage; }
}

// ---------- fcntl / 目录 ----------
// 只用来探 fd 是否有效（diag.c 的句柄计数）：有 CRT 句柄即视为已开
// Only probes fd validity (diag.c's handle count): a live CRT handle means open
int fcntl(int fd, int cmd, ...) {
    if (cmd == F_GETFD || cmd == F_SETFD) { intptr_t h = _get_osfhandle(fd); return h == -1 ? -1 : 0; }
    return 0;
}
// DIR 没有 fd 概念：/dev/fd 在 Windows 上本来就打不开，这里返回 -1 让调用方跳过自身比较
// DIR has no fd notion: /dev/fd cannot be opened on Windows anyway, so -1 makes the caller skip the self comparison
int dirfd(DIR *d) { (void)d; return -1; }
// utimes：CRT 只有秒级 utime | utimes: the CRT only has second-granularity utime
int utimes(const char *path, const struct timeval tv[2]) {
    if (!path || !tv) { errno = EINVAL; return -1; }
    struct _utimbuf ub; ub.actime = tv[0].tv_sec; ub.modtime = tv[1].tv_sec;
    return _utime(path, &ub);
}
// localtime_r：localtime_s 参数顺序相反 | localtime_s takes its arguments in the opposite order
struct tm *localtime_r(const time_t *t, struct tm *out) {
    if (!t || !out) { errno = EINVAL; return NULL; }
    return localtime_s(out, t) ? NULL : out;
}
struct tm *gmtime_r(const time_t *t, struct tm *out) {
    if (!t || !out) { errno = EINVAL; return NULL; }
    return gmtime_s(out, t) ? NULL : out;
}
time_t timegm(struct tm *tm) { return (time_t)_mkgmtime64(tm); }

// ---------- 常用但不齐全的 libc ----------
void *memmem(const void *hay, size_t haylen, const void *needle, size_t nlen) {
    if (!hay || !needle || !nlen || haylen < nlen) return NULL;
    const char *h = hay, *n = needle;
    for (size_t i = 0; i + nlen <= haylen; i++) if (h[i] == n[0] && !memcmp(h + i, n, nlen)) return (void *)(h + i);
    return NULL;
}
// getline：按块读到 \n 或 EOF，返回读到的字节数（0 = 结束），-1 = 错。调用方用 free() 收 line
// getline: reads up to \n or EOF in chunks, returns the bytes read (0 = end), -1 = error. The caller frees line with free()
long getline(char **line, size_t *cap, FILE *f) {
    if (!line || !cap || !f) { errno = EINVAL; return -1; }
    if (!*line || !*cap) { *cap = 128; if (!(*line = malloc(*cap))) return -1; }
    size_t n = 0; int c;
    while ((c = fgetc(f)) != EOF) {
        if (n + 1 >= *cap) { size_t nc = *cap * 2; char *nl = realloc(*line, nc); if (!nl) return -1; *line = nl; *cap = nc; }
        (*line)[n++] = (char)c;
        if (c == '\n') break;
    }
    if (!n && c == EOF) return -1;
    (*line)[n] = 0;
    return (long)n;
}
// pread / pwrite：CRT 没有，只能「定位 + 读写」两步；多个线程共用同一个 fd 时靠一把锁保证两步原子
// pread / pwrite: absent from the CRT, only "seek + read/write" is possible; a lock keeps both steps atomic when threads share an fd
static SRWLOCK g_pio_lock = SRWLOCK_INIT;
long pread(int fd, void *buf, size_t n, long long off) {
    AcquireSRWLockExclusive(&g_pio_lock);
    long long back = _lseeki64(fd, 0, SEEK_CUR);
    long long got = _lseeki64(fd, off, SEEK_SET);
    long r = got == off ? (long)_read(fd, buf, (unsigned)n) : -1;
    if (back >= 0) _lseeki64(fd, back, SEEK_SET);
    ReleaseSRWLockExclusive(&g_pio_lock);
    return r;
}
long pwrite(int fd, const void *buf, size_t n, long long off) {
    AcquireSRWLockExclusive(&g_pio_lock);
    long long back = _lseeki64(fd, 0, SEEK_CUR);
    long long got = _lseeki64(fd, off, SEEK_SET);
    long r = got == off ? (long)_write(fd, buf, (unsigned)n) : -1;
    if (back >= 0) _lseeki64(fd, back, SEEK_SET);
    ReleaseSRWLockExclusive(&g_pio_lock);
    return r;
}
void arc4random_buf(void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) { uint32_t v = arc4random(); size_t k = n < 4 ? n : 4; memcpy(p, &v, k); p += k; n -= k; }
}

// ---------- 对齐分配（专用地址区间 + 两次比较认出，见头文件说明） ----------
// ---------- aligned allocation (dedicated address range recognised by two compares, see the header) ----------
static char *g_arena; static size_t g_arena_cap; static volatile long long g_arena_off;
#define DM_ARENA_CAP ((size_t)1 << 32)   // 保留 4 GiB 地址空间（不提交，不占物理内存）| reserve 4 GiB of address space (nothing committed, no physical memory)
static int arena_ready(void) {
    if (g_arena) return 1;
    size_t cap = DM_ARENA_CAP;
    void *p = VirtualAlloc(NULL, cap, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) { cap = (size_t)256 << 20; p = VirtualAlloc(NULL, cap, MEM_RESERVE, PAGE_NOACCESS); }
    if (!p) return 0;
    g_arena = p; g_arena_cap = cap;
    if (getenv("DM_ARENA_LOG")) fprintf(stderr, "[dm] 对齐分配保留区 base=%p cap=%zu\n", (void *)p, cap);
    return 1;
}
void *aligned_alloc(size_t align, size_t size) {
    if (align < 16) align = 16;
    if (align & (align - 1)) { errno = EINVAL; return NULL; }
    if (!size) size = 1;
    if (!arena_ready()) { errno = ENOMEM; return NULL; }
    size_t need = size + align + 64, len = (need + 4095) & ~(size_t)4095;
    long long off = InterlockedExchangeAdd64(&g_arena_off, (long long)(len + 4096));
    if ((size_t)off >= g_arena_cap) { errno = ENOMEM; return NULL; }
    uintptr_t base = ((uintptr_t)(g_arena + off) + 4095) & ~(uintptr_t)4095;
    void *blk = VirtualAlloc((void *)base, len, MEM_COMMIT, PAGE_READWRITE);
    if (!blk) { errno = ENOMEM; return NULL; }
    uintptr_t al = ((uintptr_t)blk + 64) & ~(uintptr_t)(align - 1);
    ((void **)al)[-2] = blk; ((size_t *)al)[-1] = len;   // 归还时要块基址与长度（MEM_DECOMMIT 必须给全范围）| the block base and length are needed on release (MEM_DECOMMIT needs the full range)
    return (void *)al;
}
// 认领对齐块：地址落在保留区间内即是；释放用 MEM_DECOMMIT，保留的地址空间不还给系统
// Claim an aligned block: an address inside the reserved range; release uses MEM_DECOMMIT and the reserved space is not returned
void dm_free(void *p) {
    if (!p) return;
    if (g_arena && (uintptr_t)p - (uintptr_t)g_arena < g_arena_cap) {
        void *blk = ((void **)p)[-2]; size_t len = ((size_t *)p)[-1];
        VirtualFree(blk, len, MEM_DECOMMIT);
        return;
    }
    free(p);
}

// ---------- 线程栈 ----------
// 只答本线程（GetCurrentThreadStackLimits，Win8+）；别的线程要读它的 TEB，宿主用不到
// Only the current thread is answered (GetCurrentThreadStackLimits, Win8+); other threads would need their TEB, unused by the host
void *pthread_get_stackaddr_np(pthread_t t) {
    if (t != pthread_self()) return NULL;
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    return (void *)hi;
}
size_t pthread_get_stacksize_np(pthread_t t) {
    if (t != pthread_self()) return 0;
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    return (size_t)(hi - lo);
}
// strcasestr：逐位小写比较（只用于配置 / 路径匹配，不在热路径）
// strcasestr: compares character by character in lower case (config / path matching only, never a hot path)
char *strcasestr(const char *hay, const char *needle) {
    if (!hay || !needle) return NULL;
    size_t n = strlen(needle);
    if (!n) return (char *)hay;
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < n && p[k] && tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k])) k++;
        if (k == n) return (char *)p;
    }
    return NULL;
}

// ---------- 栈预提交 ----------
// Windows 的线程栈只保留地址区间、按「碰到保护页」逐步提交：客体代码一帧就下探几 KB 以上（大数组 / 深调用）时，
// 越过的部分还处于「已保留未提交」，硬件给的是访问违例而不是「栈增长」，进程直接死（sdk memset+0xc 写栈下沿即此）。
// 而 SleepThread 之类把 SP 放到客体栈后，宿主这层栈实际用量很小，客体这层反而深——于是按「本线程栈所属的那块分配」
// 把区间内所有已保留未提交的页一次性提交，让大帧不再落空。分配区间超过 32 MiB 的不碰（客体地址空间的映射不是线程栈）。
// Windows thread stacks only reserve the range and commit pages as the guard page is touched: when a guest frame dives more than a few KB
// (a big array or a deep call chain) the part it skips is still reserved-but-uncommitted, the hardware reports an access violation instead of
// "stack growth", and the process dies (writing just below the stack's low edge in sdk memset+0xc is exactly this).
// So commit every reserved-but-uncommitted page of this thread's stack allocation up front. Allocations over 32 MiB are left alone
// (guest address-space mappings are not thread stacks).
void dm_win_stack_precommit(void) {
    if (getenv("DM_STACK_NOCOMMIT")) return;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)__builtin_frame_address(0), &mbi, sizeof mbi)) return;
    char *base = (char *)mbi.AllocationBase;
    if (!base) return;
    char *p = base; size_t total = 0; int n = 0;
    while (n++ < 64 && VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi && (char *)mbi.AllocationBase == base) {
        total += mbi.RegionSize;
        if (total > ((size_t)32 << 20)) return;   // 不是线程栈，放弃 | not a thread stack, give up
        if (mbi.State == MEM_RESERVE) VirtualAlloc(p, mbi.RegionSize, MEM_COMMIT, PAGE_READWRITE);
        p = (char *)mbi.BaseAddress + mbi.RegionSize;
    }
}

// ---------- 资源限制 ----------
// 报「权限内的上限」，设置成功但不生效：宿主的调用方都只是记录 / 展示 | report "the limit within our rights"; set succeeds without effect: all callers only record / display
int getrlimit(int resource, struct rlimit *r) {
    if (!r) { errno = EINVAL; return -1; }
    if (resource == RLIMIT_STACK) { r->rlim_cur = r->rlim_max = RLIM_INFINITY; return 0; }
    if (resource == RLIMIT_NOFILE) { r->rlim_cur = r->rlim_max = 8192; return 0; }
    r->rlim_cur = r->rlim_max = RLIM_INFINITY; return 0;
}
int setrlimit(int resource, const struct rlimit *r) { (void)resource; (void)r; return 0; }
// getrusage：常驻峰值用工作集（KB，Linux 口径），CPU 时间取 GetProcessTimes
// getrusage: peak resident set from the working set (KB, Linux convention), CPU times from GetProcessTimes
int getrusage(int who, struct rusage *r) {
    (void)who;
    if (!r) { errno = EINVAL; return -1; }
    memset(r, 0, sizeof *r);
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) r->ru_maxrss = (long)(pmc.PeakWorkingSetSize / 1024);
    FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    uint64_t ut = ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime) / 10, st = ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) / 10;   // 100ns → 微秒 | 100ns to microseconds
    r->ru_utime.tv_sec = (long)(ut / 1000000); r->ru_utime.tv_usec = (long)(ut % 1000000);
    r->ru_stime.tv_sec = (long)(st / 1000000); r->ru_stime.tv_usec = (long)(st % 1000000);
    return 0;
}
int getpriority(int which, int who) { (void)which; (void)who; return 0; }
int setpriority(int which, int who, int prio) {
    (void)which; (void)who;
    // 只对「本线程」有意义：nice 越小越优先 | only meaningful for "this thread": smaller nice = higher priority
    int p = prio <= -8 ? THREAD_PRIORITY_HIGHEST : prio <= -3 ? THREAD_PRIORITY_ABOVE_NORMAL : prio >= 10 ? THREAD_PRIORITY_LOWEST : prio >= 5 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL;
    SetThreadPriority(GetCurrentThread(), p);
    return 0;
}

// ---------- dlfcn ----------
// 句柄：把 HMODULE 加 1，保证非空（dlopen 失败时返回 NULL，调用方都判 NULL）
// Handle: HMODULE + 1 so it is never null (dlopen returns NULL on failure and every caller tests for NULL)
void *dlopen(const char *path, int mode) {
    (void)mode;
    HMODULE m = LoadLibraryA(path);
    if (!m && path && !strchr(path, '\\') && !strchr(path, '/')) { char dll[128]; snprintf(dll, sizeof dll, "%s.dll", path); m = LoadLibraryA(dll); }
    return m ? (void *)((uintptr_t)m + 1) : NULL;
}
void *dlsym(void *handle, const char *name) {
    if (!handle) return NULL;
    return (void *)GetProcAddress((HMODULE)((uintptr_t)handle - 1), name);
}
int dlclose(void *handle) { return handle ? (FreeLibrary((HMODULE)((uintptr_t)handle - 1)) ? 0 : -1) : 0; }
char *dlerror(void) { return (char *)"win32 dl error"; }
// dladdr：模块基址 + 文件名，符号留空（发布包没有 PDB）| dladdr: module base + file name, symbol left empty (release packages carry no PDB)
int dladdr(const void *addr, Dl_info *info) {
    if (!addr || !info) return 0;
    HMODULE m = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)addr, &m) || !m) return 0;
    static char name[MAX_PATH];   // 崩溃路径单线程，静态缓冲够用 | the crash path is single-threaded, a static buffer is enough
    if (!GetModuleFileNameA(m, name, sizeof name)) name[0] = 0;
    info->dli_fname = name; info->dli_fbase = (void *)m; info->dli_sname = NULL; info->dli_saddr = NULL;
    return 1;
}

// ---------- sigaction ----------
// 只做「登记 + 缺页时分发」，没有任何真正的信号机制：Windows 的缺页是 VEH 事件（见 crash_win.c 与 crash.c）。
// 索引按信号号开 32 格，登记发生在引擎启动阶段（单线程），之后只读。
// Only "register + dispatch on fault", no real signals: Windows faults are VEH events (see crash_win.c and crash.c).
// Indexed by signal number in 32 slots; registration happens at engine start (single-threaded) and it is read-only afterwards.
static struct sigaction g_sig[32];
int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (sig < 0 || sig >= 32) { errno = EINVAL; return -1; }
    if (old) *old = g_sig[sig];
    if (act) g_sig[sig] = *act;
    return 0;
}
int dm_win_sig_raise(int sig, void *addr) {
    if (sig < 0 || sig >= 32) return 0;
    struct sigaction *sa = &g_sig[sig];
    void (*sai)(int, siginfo_t *, void *) = sa->sa_sigaction;
    void (*sig_h)(int) = sa->sa_handler;
    if (sai) { siginfo_t si = { sig, 1, 0, addr }; sai(sig, &si, NULL); }
    else if (sig_h && sig_h != SIG_DFL && sig_h != SIG_IGN) sig_h(sig);
    else return 0;
    // 处理器返回后仍是「有效处理器」才算认领：wtrack 认领不了时会把 SIGSEGV 复位成 SIG_DFL，再返回即落到崩溃上报
    // A handler still installed after returning counts as a claim: when wtrack cannot claim, it resets SIGSEGV to SIG_DFL,
    // so the return falls through to the crash report
    struct sigaction *now = &g_sig[sig];
    if (now->sa_sigaction) return 1;
    return now->sa_handler && now->sa_handler != SIG_DFL && now->sa_handler != SIG_IGN;
}
// signal：SIGSEGV 走上面的表（引擎复位用），别的信号老实交给 CRT
// signal: SIGSEGV uses the table above (the engine resets it); other signals go to the CRT as is
void (*dm_win_signal(int sig, void (*fn)(int)))(int) {
    if (sig == SIGSEGV) {
        void (*old)(int) = g_sig[sig].sa_handler;
        g_sig[sig].sa_handler = fn; g_sig[sig].sa_sigaction = NULL; g_sig[sig].sa_flags = 0;
        return old;
    }
    return signal(sig, fn);
}

// PE 段表取段：DM_SECT_GET 在 Windows 上的后端（苹果 getsectiondata / ELF 边界符号在 COFF 上都没有）。
// Walks the PE section table: the Windows backend of DM_SECT_GET (neither getsectiondata nor ELF boundary symbols exist on COFF).
void *dm_pe_section(const char *name, unsigned long *bytes) {
    extern IMAGE_DOS_HEADER __ImageBase;
    const uint8_t *base = (const uint8_t *)&__ImageBase;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + __ImageBase.e_lfanew);
    const IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sh++) {
        // 段名 8 字节，短于 8 的以 0 结尾（长名走 /nn 形式，宿主不用）
        // Section names are 8 bytes, zero-padded when shorter (long names use the /nn form, unused by the host)
        char nm[9]; memcpy(nm, sh->Name, 8); nm[8] = 0;
        if (strcmp(nm, name)) continue;
        *bytes = sh->Misc.VirtualSize;
        if (!*bytes) *bytes = sh->SizeOfRawData;
        return (void *)(base + sh->VirtualAddress);
    }
    *bytes = 0;
    return NULL;
}

// setjmp / longjmp 的纯汇编实现：只恢复寄存器与 sp，不经过 Windows 的 RtlUnwind（展开要求 .pdata，客体没有）。
// 客体线程 ExitThread 从客体栈跳回宿主栈就靠这条路径（hle_thread.c 的 g_keyhome）。
// A pure-assembly setjmp / longjmp: restores only the registers and sp, never going through the Windows RtlUnwind (unwinding needs .pdata,
// which the guest lacks). A guest thread's ExitThread relies on it to jump from the guest stack back to the host stack (g_keyhome in hle_thread.c).
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl dm_win_setjmp\n"
    "dm_win_setjmp:\n"
    "  stp x19, x20, [x0, #0]\n"
    "  stp x21, x22, [x0, #16]\n"
    "  stp x23, x24, [x0, #32]\n"
    "  stp x25, x26, [x0, #48]\n"
    "  stp x27, x28, [x0, #64]\n"
    "  stp x29, x30, [x0, #80]\n"
    "  mov x1, sp\n"
    "  str x1, [x0, #96]\n"
    "  mov w0, #0\n"
    "  ret\n"
    ".p2align 2\n"
    ".globl dm_win_longjmp\n"
    "dm_win_longjmp:\n"
    "  ldp x19, x20, [x0, #0]\n"
    "  ldp x21, x22, [x0, #16]\n"
    "  ldp x23, x24, [x0, #32]\n"
    "  ldp x25, x26, [x0, #48]\n"
    "  ldp x27, x28, [x0, #64]\n"
    "  ldp x29, x30, [x0, #80]\n"
    "  ldr x1, [x0, #96]\n"
    "  mov sp, x1\n"
    "  mov w0, w1\n"
    "  cmp w0, #0\n"
    "  csinc w0, w0, wzr, ne\n"
    "  ret\n"
    ".text\n"
);
