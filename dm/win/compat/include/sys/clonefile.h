// clonefile / renamex_np（苹果 APFS 写时复制与原子互换）：Windows 上用整树复制与三次改名近似。
// 目的只有一个：存档的 工作目录 ↔ 已提交目录 换入。Windows 没有目录级的原子互换，这里按
//   A → A.old、B → A、A.old → B
// 的顺序完成交换；中途掉电会留下 .old 残留，下次挂载前由 hle_fs.c 的残留清理删掉。语义上「已提交版本要么是旧的、要么是新的」仍然成立
// （每次 rename 本身是原子的），代价是交换窗口里极短的一瞬两个目录名都不完整——这段时间只在进程内，没有第三方观察者。
// clonefile / renamex_np (Apple APFS copy-on-write and atomic swap): approximated on Windows by a whole-tree copy and three renames.
// The only purpose is swapping the save's working directory with the committed directory. Windows has no directory-level atomic swap, so the
// exchange runs in the order A -> A.old, B -> A, A.old -> B; a power loss midway leaves a .old residue that hle_fs.c's leftover cleanup
// removes before the next mount. Semantically "the committed version is either the old or the new one" still holds (each rename is atomic);
// the cost is that for a very short moment inside the swap neither directory name is complete -- only inside this process, with no third-party observer.
#pragma once
#define CLONE_NOFOLLOW 0x0001
#define CLONE_NOOWNERCOPY 0x0002
int clonefile(const char *src, const char *dst, int flags);
#define RENAME_SWAP 0x00000002
#define RENAME_EXCL 0x00000004
int renamex_np(const char *from, const char *to, unsigned flags);
