// mmap / mprotect / munmap / msync（Windows）：VirtualAlloc / VirtualProtect / VirtualFree。
// 宿主的用法只有三种：装载客体重写映像（匿名映射）、给代码区 / 键页封权限（mprotect，写即崩）、
// 脏页跟踪（写保护后靠缺页钩子，Windows 上是 SEH 访问违例，见 compat/crash_win.c）。
// 因此这里只实现匿名私有映射；带 fd 的映射直接报错（宿主从未用），避免给出错语义。
// mmap / mprotect / munmap / msync (Windows): VirtualAlloc / VirtualProtect / VirtualFree.
// The host uses exactly three forms: mapping a rewritten guest image (anonymous), sealing the code area / key page (mprotect: any write faults),
// and dirty-page tracking (write-protect plus a fault hook, which on Windows is an SEH access violation, see compat/crash_win.c).
// So only anonymous private mappings are implemented; mappings with an fd report an error (the host never uses them) rather than give wrong semantics.
#include "dm_win_compat.h"
#include <sys/mman.h>
#include <sys/clonefile.h>
#include <sys/xattr.h>
#include <dirent.h>

static DWORD page_of(int prot) {
    int r = prot & PROT_READ, w = prot & PROT_WRITE, x = prot & PROT_EXEC;
    if (x && w) return PAGE_EXECUTE_READWRITE;
    if (x && r) return PAGE_EXECUTE_READ;
    if (x) return PAGE_EXECUTE;
    if (w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
    (void)offset;
    if (fd >= 0 || !(flags & MAP_ANON)) { errno = ENOTSUP; return MAP_FAILED; }
    if (!len) { errno = EINVAL; return MAP_FAILED; }
    // MAP_FIXED：宿主没用到；为契约完整实现「在给定地址上占位」| MAP_FIXED: unused by the host; implemented for contract completeness
    void *base = VirtualAlloc((flags & MAP_FIXED) ? addr : NULL, len, MEM_RESERVE | MEM_COMMIT, page_of(prot));
    if (!base) { errno = ENOMEM; return MAP_FAILED; }
    if (prot & PROT_EXEC) dm_win_icache_invalidate(base, len);
    return base;
}
int munmap(void *addr, size_t len) {
    (void)len;
    if (!addr) { errno = EINVAL; return -1; }
    if (!VirtualFree(addr, 0, MEM_RELEASE)) { errno = EINVAL; return -1; }
    return 0;
}
int mprotect(void *addr, size_t len, int prot) {
    // Windows 要求页对齐：向上 / 向下取整到 4 KB。宿主传的地址本来就按 16 KB 对齐
    // Windows requires page alignment: round out to 4 KB. The host's addresses are already 16 KB aligned
    uintptr_t a0 = (uintptr_t)addr & ~(uintptr_t)(DM_HOST_PAGE - 1);
    size_t n = ((uintptr_t)addr + len + DM_HOST_PAGE - 1) & ~(uintptr_t)(DM_HOST_PAGE - 1);
    n -= a0;
    DWORD old = 0;
    if (!VirtualProtect((void *)a0, n, page_of(prot), &old)) { errno = EACCES; return -1; }
    if (prot & PROT_EXEC) dm_win_icache_invalidate((void *)a0, n);
    return 0;
}
int msync(void *addr, size_t len, int flags) { (void)flags; if ((uintptr_t)addr) dm_win_icache_invalidate(addr, len); return 0; }
// madvise：MADV_FREE = 解除物理页（MEM_RESET，内容作废但地址范围仍在，之后写会重新分配）；DONTNEED 同样处理
// madvise: MADV_FREE = drop physical pages (MEM_RESET, contents gone but the range stays, a later write re-allocates); DONTNEED likewise
int madvise(void *addr, size_t len, int advice) {
    if (!addr || !len) { errno = EINVAL; return -1; }
    if (advice != MADV_FREE && advice != MADV_DONTNEED && advice != MADV_NORMAL) { errno = EINVAL; return -1; }
    if (advice == MADV_NORMAL) return 0;
    if (!VirtualAlloc(addr, len, MEM_RESET, PAGE_READWRITE)) { errno = EINVAL; return -1; }
    return 0;
}

// ---------- clonefile ----------
// 整树复制（目录递归、文件 CopyFile）。存档目录只有几十个小文件，成本可接受；
// 失败返回 -1 并把 errno 置成 EIO（调用方只看成功 / 失败 + errno 日志）。
// Whole-tree copy (directories recurse, files via CopyFile). A save directory has only a few dozen small files, so the cost is acceptable;
// a failure returns -1 with errno set to EIO (callers only look at success / failure plus the errno log).
static int copy_tree(const char *src, const char *dst) {
    if (_mkdir(dst) && errno != EEXIST) return -1;
    DIR *d = opendir(src);
    if (!d) return -1;
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sp[1300], dp[1300];
        if (snprintf(sp, sizeof sp, "%s/%s", src, e->d_name) >= (int)sizeof sp || snprintf(dp, sizeof dp, "%s/%s", dst, e->d_name) >= (int)sizeof dp) { rc = -1; break; }
        struct stat st;
        if (stat(sp, &st)) { rc = -1; break; }
        if (S_ISDIR(st.st_mode)) { if (copy_tree(sp, dp)) { rc = -1; break; } }
        else if (!CopyFileA(sp, dp, FALSE)) { errno = EIO; rc = -1; break; }
    }
    closedir(d);
    return rc;
}
int clonefile(const char *src, const char *dst, int flags) {
    (void)flags;
    if (copy_tree(src, dst)) { errno = EIO; return -1; }
    return 0;
}

// ---------- renamex_np（原子互换的近似） ----------
// A → A.rtmp、B → A、A.rtmp → B。每一步都用 MoveFileEx 的「目标存在则替换」语义；
// 中途失败尽量走回滚，回滚也失败时留下 .rtmp 残留（下次挂载前由 hle_fs.c 清理）。
// A -> A.rtmp, B -> A, A.rtmp -> B. Each step uses MoveFileEx's "replace if target exists" semantics; on a mid-way failure we roll back
// as far as we can, and if that fails too the .rtmp residue is left for hle_fs.c to clean before the next mount.
int renamex_np(const char *from, const char *to, unsigned flags) {
    if (flags & RENAME_EXCL) {
        if (!MoveFileExA(from, to, MOVEFILE_WRITE_THROUGH)) { errno = EEXIST; return -1; }
        return 0;
    }
    if (!(flags & RENAME_SWAP)) {
        if (!MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { errno = EIO; return -1; }
        return 0;
    }
    char tmp[1300];
    if (snprintf(tmp, sizeof tmp, "%s.rtmp", from) >= (int)sizeof tmp) { errno = ENAMETOOLONG; return -1; }
    if (!MoveFileExA(from, tmp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { errno = EIO; return -1; }
    if (!MoveFileExA(to, from, 0)) { MoveFileExA(tmp, from, 0); errno = EIO; return -1; }
    if (!MoveFileExA(tmp, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { errno = EIO; return -1; }
    return 0;
}

// ---------- 扩展属性（NTFS 备用数据流） ----------
// 名字拼成 "路径:流名"；宿主只存一项（客体名）。文件或流不存在 → ENODATA（调用方按「没有别名」处理）。
// The name becomes "path:stream"; the host stores one item (the guest name). A missing file or stream gives ENODATA (callers treat it as "no alias").
#ifndef ENODATA
#define ENODATA 61
#endif
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif
static int ads_path(const char *path, const char *name, char *out, size_t n) {
    if (snprintf(out, n, "%s:%s", path, name) >= (int)n) { errno = ENAMETOOLONG; return -1; }
    return 0;
}
ssize_t getxattr(const char *path, const char *name, void *value, size_t size) {
    char p[1300]; if (ads_path(path, name, p, sizeof p)) return -1;
    HANDLE h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { errno = (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) ? ENODATA : EIO; return -1; }
    DWORD got = 0; size_t cap = size;
    if (cap > 0xffffffffu) cap = 0xffffffffu;
    BOOL ok = ReadFile(h, value, (DWORD)cap, &got, NULL);
    CloseHandle(h);
    if (!ok) { errno = EIO; return -1; }
    return (ssize_t)got;
}
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size) { return getxattr(path, name, value, size); }
static int ads_write(const char *path, const char *name, const void *value, size_t size) {
    char p[1300]; if (ads_path(path, name, p, sizeof p)) return -1;
    HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { errno = EIO; return -1; }
    DWORD put = 0; BOOL ok = WriteFile(h, value, (DWORD)size, &put, NULL) && put == (DWORD)size;
    CloseHandle(h);
    if (!ok) { errno = EIO; return -1; }
    return 0;
}
int setxattr(const char *path, const char *name, const void *value, size_t size, int flags) { (void)flags; return ads_write(path, name, value, size); }
int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags) { (void)flags; return ads_write(path, name, value, size); }
int removexattr(const char *path, const char *name) {
    char p[1300]; if (ads_path(path, name, p, sizeof p)) return -1;
    if (DeleteFileA(p)) return 0;
    errno = (GetLastError() == ERROR_FILE_NOT_FOUND) ? ENODATA : EIO;
    return -1;
}
int lremovexattr(const char *path, const char *name) { return removexattr(path, name); }
