// sys/resource.h：宿主只用 rlimit（栈上限、fd 上限）与 nice 值（线程优先级诊断）。Windows 没有对应语义，
// 给出「取到的是权限内的上限、设置成功但不生效」的实现，并保证调用方拿到的数不小于自己请求的数。
// sys/resource.h: the host only uses rlimit (stack limit, fd limit) and nice values (thread priority diagnostics). Windows has no
// counterpart; the implementation reports "the limit within our rights, set succeeds without effect" and never less than requested.
#pragma once
#include <stdint.h>
#include <sys/time.h>
#ifdef __cplusplus
extern "C" {
#endif

#define RLIMIT_CPU 0
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA 2
#define RLIMIT_STACK 3
#define RLIMIT_CORE 4
#define RLIMIT_RSS 5
#define RLIMIT_MEMLOCK 6
#define RLIMIT_NPROC 7
#define RLIMIT_NOFILE 8

#define RLIM_INFINITY (~0ull)
typedef uint64_t rlim_t;
struct rlimit { rlim_t rlim_cur, rlim_max; };

#define PRIO_PROCESS 0
#define PRIO_PGRP 1
#define PRIO_USER 2

// getrusage：宿主只读 ru_maxrss（常驻峰值）与 CPU 时间，字段按 Linux 的习惯（KB）填
// getrusage: the host only reads ru_maxrss (peak resident set) and CPU times; fields follow the Linux convention (KB)
#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN -1
struct rusage { struct timeval ru_utime, ru_stime; long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap, ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw; };

int getrlimit(int resource, struct rlimit *r);
int setrlimit(int resource, const struct rlimit *r);
int getpriority(int which, int who);
int setpriority(int which, int who, int prio);
int getrusage(int who, struct rusage *r);
#ifdef __cplusplus
}
#endif
