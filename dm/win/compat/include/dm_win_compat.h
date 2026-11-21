// Windows（ARM64）构建的兼容层入口：由构建强制包含（-include），与苹果 / 安卓构建互不可见。
// 这里只放「宿主用到的、Windows CRT 没有或语义不同」的那几样：Apple libc 的扩展、POSIX 的 mmap / 资源限制、
// 系统寄存器的线程槽（TPIDR_EL0 = 客体线程槽块，见 compat/tls_win.c 与 loader.c 的 win 分支）。
// Windows (ARM64) build compat entry: force-included by the build (-include), invisible to the Apple / Android builds.
// Only the few things the host uses that the Windows CRT lacks or defines differently: Apple libc extensions, POSIX mmap / resource limits,
// and the system-register thread slot (TPIDR_EL0 = the guest thread slot block, see compat/tls_win.c and the win branch of loader.c).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <malloc.h>
#include <inttypes.h>
#include <assert.h>
#include <dirent.h>
#include <sys/utime.h>
#include <setjmp.h>
#include <pthread.h>
#include <sys/resource.h>

// 整个兼容层按 C 语言链接：定义都在 .c 里，而 C++ 文件（引擎）也会包含这个头。
// The whole compat layer uses C linkage: the definitions live in .c files while C++ files (the engine) include this header too.
#ifdef __cplusplus
extern "C" {
#endif

// ---- 平台标识 ----
// 与 dm.h 的 DM_PHONE 判定配合：Windows 版是给玩家装的产品包，缺省同手机（真实声音、性能记录等）。
// Works with DM_PHONE in dm.h: the Windows build is a product package for players and defaults like the phone builds (real sound, perf log).
#ifndef DM_WIN
#define DM_WIN 1
#endif

// ---- Apple libc 扩展 ----
// strlcpy / strlcat（BSD）：宿主里当普通字符串函数用，返回值同 BSD
// strlcpy / strlcat (BSD): used as plain string functions in the host, BSD return values
size_t strlcpy(char *dst, const char *src, size_t cap);
size_t strlcat(char *dst, const char *src, size_t cap);
// arc4random：只用于打散 / 取随机 ID，不作安全用途（与安卓 compat 同一约定）
// arc4random: only for hashing / random ids, never for security (same convention as the Android compat)
uint32_t arc4random(void);
uint32_t arc4random_uniform(uint32_t upper);
// mkstemp / mkdtemp：CRT 只有 _mktemp_s
// mkstemp / mkdtemp: the CRT only has _mktemp_s
int mkstemp(char *tmpl);
char *mkdtemp(char *tmpl);
// realpath：CRT 的 _fullpath（不解析符号链接，Windows 上也够用）
// realpath: the CRT's _fullpath (does not resolve symlinks, enough on Windows)
char *realpath(const char *path, char *resolved);
// getdtablesize：宿主只拿它当 fd 上限的粗估值
// getdtablesize: the host only uses it as a rough fd limit
int getdtablesize(void);
// getpagesize：Windows 页 4 KB（宿主只拿来做对齐 / 分页换算）| getpagesize: 4 KB pages on Windows (only used for alignment / paging math)
size_t getpagesize(void);
// fsync / ftruncate / lstat / fchmod 等 POSIX 名字
// POSIX names fsync / ftruncate / lstat / fchmod
#define fsync _commit
#define ftruncate(fd, len) _chsize_s((fd), (long long)(len))
#define lstat stat
#define fchmod(fd, mode) ((void)(fd), (void)(mode), 0)
#define chmod _chmod
// 结构体字段：苹果 / Linux 的纳秒时间戳（Windows 只有秒）统一走 dm.h 的 dm_stat_mtime()
// Struct fields: the nanosecond timestamps of Apple / Linux (seconds only on Windows) go through dm_stat_mtime() in dm.h
// S_IS* 补充：Windows 的 stat 没有 socket / fifo / block 的概念，判为否
// S_IS* supplement: Windows stat has no socket / fifo / block notion, all report false
#ifndef S_ISSOCK
#define S_ISSOCK(m) 0
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(m) 0
#endif
#ifndef S_ISBLK
#define S_ISBLK(m) 0
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif
#ifndef S_ISCHR
#define S_ISCHR(m) 0
#endif
#ifndef MAXPATHLEN
#define MAXPATHLEN 260
#endif
#ifndef PATH_MAX
#define PATH_MAX 260
#endif
// open 的标志位：CRT 没有 O_CLOEXEC / O_DIRECTORY / O_NOFOLLOW
// open flags: the CRT has no O_CLOEXEC / O_DIRECTORY / O_NOFOLLOW
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_SYNC
#define O_SYNC 0
#endif
#ifndef F_FULLFSYNC
#define F_FULLFSYNC 0
#endif
#ifndef _O_BINARY
#define _O_BINARY O_BINARY
#endif
// open：CRT 缺省是文本模式，read 遇到 CRLF / 大文件会短读；一律补 _O_BINARY（宿主所有文件都是二进制）
// open: the CRT defaults to text mode and read comes back short on CRLF / large files; always add _O_BINARY (all host files are binary)
int dm_win_open(const char *path, int flags, ...);
#define open dm_win_open

// ---- 时钟 ----
// CLOCK_* 里 CRT 缺少的几个：BOOTTIME / UPTIME / MONOTONIC_RAW 都落到 QPC（与 CLOCK_MONOTONIC 同源），进程 / 线程 CPU 时间落到 GetProcessTimes
// Missing CLOCK_* ids in the CRT: BOOTTIME / UPTIME / MONOTONIC_RAW land on QPC (same basis as CLOCK_MONOTONIC); process / thread CPU time land on GetProcessTimes
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 1000
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW 1001
#endif
#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW 1002
#endif
// clock_gettime：CRT / winpthreads 的实现只认识一部分 id；这里统一走自己的实现（纳秒，QPC）
// clock_gettime: the CRT / winpthreads implementation knows only some ids; everything goes through our own (nanoseconds, QPC)
int dm_win_clock_gettime(int clock_id, struct timespec *ts);
#define clock_gettime dm_win_clock_gettime
// clock_gettime_nsec_np（苹果）：同一实现
// clock_gettime_nsec_np (Apple): same implementation
uint64_t clock_gettime_nsec_np(int clock_id);
// 睡眠：usleep 的实现在 CRT 里（unistd.h），但精度只有 1 ms 级；nanosleep 自己写
// Sleep: usleep exists in the CRT (unistd.h) but only to 1 ms; nanosleep is ours
// (nanosleep 由 compat/time_win.c 提供 | nanosleep comes from compat/time_win.c)

// ---- 线程 ----
// pthread_setname_np 在 winpthreads 有；pthread_gettid_np 没有，用宿主自己记的线程号
// pthread_setname_np exists in winpthreads; pthread_gettid_np does not, so the host records its own tid
unsigned dm_win_thread_tid(void);
#define pthread_gettid_np(t) ((unsigned)dm_win_thread_tid())
// thread_selfcounts（苹果 libSystem 私有接口，hle_nvn.c 的可选诊断）：Windows 没有退休指令计数，返回 0 = 无读数
// thread_selfcounts (Apple libSystem private interface, optional diagnostic in hle_nvn.c): Windows has no retired-instruction counter, return 0 = no reading
int thread_selfcounts(int type, void *buf, size_t nbytes);

// ---- 内存 ----
// malloc_size（苹果）：_msize
// malloc_size (Apple): _msize
#define malloc_size(p) ((size_t)_msize((p)))
// posix_memalign：CRT 的 _aligned_malloc
// posix_memalign: the CRT's _aligned_malloc
int posix_memalign(void **out, size_t align, size_t size);

// ---- 环境变量 ----
// setenv / unsetenv：mingw 有，但 Win32 语义（_putenv_s）下 setenv 的覆盖标志忽略
// setenv / unsetenv: mingw has them, but under Win32 semantics (_putenv_s) the overwrite flag is ignored
#ifndef setenv
int setenv(const char *name, const char *value, int overwrite);
#endif
#ifndef unsetenv
int unsetenv(const char *name);
#endif

// ---- 信号 ----
// 宿主只在 crash.c 与 hle_lifecycle.c 用信号：Windows 上换成 SEH / VEH 与控制台事件（见 compat/crash_win.c），
// 这里保留最小的 type / 常量，真正的安装函数在 dm_win_crash_install()
// The host only uses signals in crash.c and hle_lifecycle.c: on Windows they become SEH / VEH and console events (compat/crash_win.c);
// only the minimal types / constants are kept here, the real installer is dm_win_crash_install()
#ifndef SIGSEGV
#define SIGSEGV 11
#endif
#ifndef SIGBUS
#define SIGBUS 7
#endif
#ifndef SIGILL
#define SIGILL 4
#endif
#ifndef SIGFPE
#define SIGFPE 8
#endif
#ifndef SIGABRT
#define SIGABRT 6
#endif
#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGALRM
#define SIGALRM 14
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef SIGTERM
#define SIGTERM 15
#endif
#ifndef SIGINT
#define SIGINT 2
#endif
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGUSR1
#define SIGUSR1 10
#endif
#ifndef SIGUSR2
#define SIGUSR2 12
#endif
#ifndef SA_SIGINFO
#define SA_SIGINFO 4
#endif
#ifndef SA_ONSTACK
#define SA_ONSTACK 1
#endif
#ifndef SA_RESTART
#define SA_RESTART 2
#endif
// sigaltstack / stack_t：无对应物，空实现（crash.c 在 Windows 上不走备用栈）
// sigaltstack / stack_t: no counterpart, empty (crash.c does not use an alternate stack on Windows)
typedef struct { void *ss_sp; size_t ss_size; int ss_flags; } stack_t;
static inline int sigaltstack(const stack_t *ss, stack_t *old) { (void)ss; (void)old; return 0; }
#define SIGSTKSZ 0

// sigaction / siginfo_t：Windows 没有，但引擎的写跟踪（xbg_game.cpp 的 wtrack）靠它认领写保护页的缺页。
// 这里给出同形结构，真正注册的处理器由 crash_win.c 的 VEH 在缺页时调用（认领了就让 PC 原地继续）。
// sigaction / siginfo_t: absent on Windows, but the engine's write tracking (wtrack in xbg_game.cpp) claims faults on write-protected pages.
// The same-shaped structs live here; the registered handler is called by crash_win.c's VEH on a fault (a claimed fault resumes in place).
#ifndef SA_NODEFER
#define SA_NODEFER 8
#endif
#ifndef SA_RESETHAND
#define SA_RESETHAND 16
#endif
#ifndef SIG_DFL
#define SIG_DFL ((void (*)(int))0)
#endif
#ifndef SIG_IGN
#define SIG_IGN ((void (*)(int))1)
#endif
typedef struct { int si_signo; int si_code; int si_errno; void *si_addr; } siginfo_t;
typedef struct { unsigned long bits[4]; } sigset_t;
struct sigaction {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, siginfo_t *, void *);
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};
static inline int sigemptyset(sigset_t *s) { for (int i = 0; i < 4; i++) s->bits[i] = 0; return 0; }
static inline int sigfillset(sigset_t *s) { for (int i = 0; i < 4; i++) s->bits[i] = ~0ul; return 0; }
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
// 缺页时分发：返回 1 = 处理器认领（继续执行），0 = 没人认领（走崩溃上报）
// Dispatch on a fault: 1 = a handler claimed it (resume), 0 = nobody claimed it (go to the crash report)
int dm_win_sig_raise(int sig, void *addr);
// signal：只劫持 SIGSEGV（引擎复位用），其余信号仍交给 CRT
// signal: only SIGSEGV is intercepted (the engine resets it); other signals still go to the CRT
#define signal dm_win_signal
void (*dm_win_signal(int sig, void (*fn)(int)))(int);
// windef.h 的 near / far 是 16 位时代的空宏，会把变量名 near 撕碎；dllimport 之类宿主用不到，一并撤掉
// windef.h's near / far are empty macros from the 16-bit era and shred a variable named near; dllimport and friends are unused by the host, so drop them too
#undef near
#undef far
#undef small

// ---- 其它 ----
// _exit 由 CRT 提供；kill(自进程) 表示「就地结束」，只支持自己
// _exit comes from the CRT; kill(self) means "end in place" and only supports self
static inline int kill(int pid, int sig) { (void)pid; (void)sig; TerminateProcess(GetCurrentProcess(), 1); return 0; }
// sysconf：只给 _SC_PAGESIZE / _SC_CLK_TCK / _SC_NPROCESSORS_ONLN 用
// sysconf: only _SC_PAGESIZE / _SC_CLK_TCK / _SC_NPROCESSORS_ONLN
#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 29
#endif
#ifndef _SC_CLK_TCK
#define _SC_CLK_TCK 2
#endif
#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
#endif
long sysconf(int name);
// 页大小：Windows 4 KB（宿主的 16 KB 对齐只是更粗的粒度，不影响正确性）
// Page size: 4 KB on Windows (the host's 16 KB alignment is just a coarser granularity and does not affect correctness)
#ifndef DM_HOST_PAGE
#define DM_HOST_PAGE 4096u
#endif

// ---- 再补几样 POSIX 名字（宿主到处在用） ----
// 目录创建是两参数写法；fcntl 只用来探 fd 有效性；utimes 只用来改文件时间（秒级，Windows 的 utime 精度）
// A few more POSIX names the host uses everywhere: two-argument mkdir; fcntl only probes fd validity;
// utimes only sets file times (second granularity, the Windows utime resolution)
#define mkdir(p, m) _mkdir(p)
#ifndef F_GETFD
#define F_GETFD 1
#endif
#ifndef F_SETFD
#define F_SETFD 2
#endif
#ifndef FD_CLOEXEC
#define FD_CLOEXEC 1
#endif
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
int fcntl(int fd, int cmd, ...);
int dirfd(DIR *d);
int utimes(const char *path, const struct timeval tv[2]);
// strcasestr（BSD）：大小写不敏感的子串查找 | case-insensitive substring search
char *strcasestr(const char *hay, const char *needle);
// localtime_r：CRT 只有 localtime_s（参数顺序相反，且线程安全）
// localtime_r: the CRT only has localtime_s (reversed arguments, thread safe)
struct tm *localtime_r(const time_t *t, struct tm *out);
// gmtime_r / timegm：同样是 _s 版本的反向包装 | the same reversed wrapper over the _s variants
struct tm *gmtime_r(const time_t *t, struct tm *out);
time_t timegm(struct tm *tm);
// sigsetjmp / siglongjmp：Windows 没有信号掩码，等同 setjmp / longjmp
// sigsetjmp / siglongjmp：Windows 的 CRT longjmp 走 RtlUnwind（要求每一帧都有 .pdata 展开信息），而客体（NSO）没有 .pdata，
// 客体线程 ExitThread 从客体栈跳回宿主栈时会以 STATUS_BAD_STACK 结束整个进程。这里换成纯寄存器恢复的汇编实现（与苹果 longjmp 等价）。
// sigsetjmp / siglongjmp: the Windows CRT longjmp goes through RtlUnwind, which needs .pdata unwind info on every frame; the guest (NSO) has
// none, so a guest thread's ExitThread jumping from the guest stack back to the host stack ended the process with STATUS_BAD_STACK. Replaced
// with an assembly implementation that only restores registers (the equivalent of Apple's longjmp).
typedef uint64_t dm_jmp_buf[14];   // x19–x28、fp、lr、sp + 余量 | x19-x28, fp, lr, sp plus slack
int dm_win_setjmp(dm_jmp_buf b);
void dm_win_longjmp(dm_jmp_buf b, int v) __attribute__((noreturn));
typedef dm_jmp_buf sigjmp_buf;
#define sigsetjmp(env, save) dm_win_setjmp(env)
#define siglongjmp dm_win_longjmp

// ---- 常用但不齐全的 libc ----
// memmem / getline / pread / pwrite：BSD / POSIX，CRT 没有
// memmem / getline / pread / pwrite: BSD / POSIX, absent from the CRT
void *memmem(const void *hay, size_t haylen, const void *needle, size_t nlen);
long getline(char **line, size_t *cap, FILE *f);
long pread(int fd, void *buf, size_t n, long long off);
long pwrite(int fd, const void *buf, size_t n, long long off);
void arc4random_buf(void *buf, size_t n);
// readlink：Windows 的符号链接要 DeviceIoControl 才解得开，宿主这里只用它判断「是不是链接」，一律报「不是」
// readlink: Windows symlinks need DeviceIoControl to resolve; the host only uses it to ask "is this a link", so always report "not a link"
static inline int readlink(const char *p, char *b, size_t n) { (void)p; (void)b; (void)n; errno = EINVAL; return -1; }
// pause：没有信号可等，就是永远睡 | nothing to wait for, so sleep forever
static inline int pause(void) { Sleep(INFINITE); return 0; }

// ---- 对齐分配 ----
// C11 的 aligned_alloc 在 CRT 里没有，而调用方一律用 free() 收（线程栈、压缩上下文）；所以对齐块从一块专用
// 虚拟地址区间取，dm_free 用两次比较就能认出（普通指针照旧走 CRT free，热路径只多两条比较）。
// The CRT has no C11 aligned_alloc and callers always release with free() (thread stacks, compression contexts); aligned
// blocks therefore come from a dedicated virtual address range that dm_free recognises with two comparisons (ordinary
// pointers still go to the CRT free, so the hot path costs only two extra compares).
void *aligned_alloc(size_t align, size_t size);
void dm_free(void *p);
#define free(p) dm_free(p)

// ---- 线程栈 ----
// pthread_get_stackaddr_np / pthread_get_stacksize_np（苹果）：只答得了「本线程」，别的一律 0
// pthread_get_stackaddr_np / pthread_get_stacksize_np (Apple): only the current thread can be answered, others get 0
void *pthread_get_stackaddr_np(pthread_t t);
size_t pthread_get_stacksize_np(pthread_t t);

// ---- PE 段表 ----
// 取本映像里名为 name 的段的首址与虚拟长度（找不到返回 NULL、*bytes = 0）。
// 苹果用 getsectiondata、ELF 用 __start_/__stop_ 边界符号；PE/COFF 两者都没有，只能自己走段表。
// Returns the start address and virtual size of the section named `name` in this image (NULL and *bytes = 0 when absent).
// Apple uses getsectiondata and ELF uses the __start_/__stop_ boundary symbols; PE/COFF has neither, so walk the section table.
void *dm_pe_section(const char *name, unsigned long *bytes);
// 把本线程栈所属分配区间里「已保留未提交」的页全部提交：客体的深帧 / 大局部数组不再撞上访问违例
// Commit every reserved-but-uncommitted page of this thread's stack allocation, so a deep guest frame or a large local array
// no longer lands on an access violation
void dm_win_stack_precommit(void);

#ifdef __cplusplus
}   // extern "C"
#endif
