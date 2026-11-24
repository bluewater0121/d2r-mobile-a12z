// 存档快照代（Windows）：<save> 是每局的展开副本，<save>.txn 里是一串自描述的只读快照。
// 提交 = 在 .txn 里造一代新快照，最后写一个 DONE 文件；发布不改变任何目录的名字。
// 依据：Windows 的目录改名会被目录里任何一个打开的句柄挡掉（杀毒 / 索引器 / 我们自己刚读过的文件），
// 而「建目录 + 建文件 + 写 + flush」不会被挡。任何时刻被杀，编号最大的、带 DONE 的快照永远是最后一次成功提交。
// Save snapshots (Windows): <save> is this run's expanded copy, <save>.txn holds a run of self-describing read-only snapshots.
// Commit = build a new generation inside .txn and write a DONE file last; publishing never renames a directory.
// Rationale: on Windows a directory rename fails whenever any handle has a file inside it open (antivirus / indexer / a file we
// just read), while mkdir + create + write + flush never fail. Killed at any moment, the highest-numbered generation carrying a
// DONE file is always the last successful commit.
#pragma once
#include <stddef.h>

// 独占锁（<save>.txn/instance.lock）：成功 0；另一个进程在跑返回 -1 | Exclusive lock: 0 on success, -1 if another instance holds it
int save_txn_lock(const char *save);
// 启动：建目录、迁移旧布局（把旧 .work/.new 隔离而不是删）、找最新快照（gen 形如 "g000003"）。
// 返回 1 = 有快照可用，0 = 全新（还没有任何存档），-1 = 出错
// Startup: create dirs, migrate the old layout (quarantine the old .work/.new instead of deleting), find the newest snapshot.
// Returns 1 = snapshot available, 0 = brand new, -1 = error
int save_txn_prepare(const char *save, char *gen, size_t gn);
// 用快照展开 <save>（DONE 清单逐条比对，一致就直接跳过）。返回 0 = 成功
// Expand a snapshot back over <save> (skip when the DONE list matches exactly). 0 = success
int save_txn_restore(const char *save, const char *gen);
// 提交：<save> 整树 → 新一代快照。files/bytes/digest 是这次快照的读数（传 NULL 可以不取）
// Commit: the whole <save> tree -> a new snapshot generation. files/bytes/digest are readouts (may be NULL)
int save_txn_snapshot(const char *save, char *gen, size_t gn, int *files, long long *bytes, unsigned long long *digest);
// 提交时要跳过的绝对路径（存档挂载内被逻辑删除的文件，物理上还删不掉的那几个）。传 NULL 清空
// Absolute paths to skip while snapshotting (files logically deleted inside the save mount that could not be removed physically)
void save_txn_skip(const char *const *paths, int n);
// 保留最近 keep 代（含最新），删更旧的；失败一律忽略，下一轮再试
// Keep the newest `keep` generations; failures are ignored and retried later
void save_txn_gc(const char *save, int keep);
// 读某代的 DONE 摘要（不扫目录），用于「这次和上次一模一样」的诊断
// Digest of a generation's DONE file, for the "identical to the previous one" diagnostic
int save_txn_digest(const char *save, const char *gen, unsigned long long *digest);
