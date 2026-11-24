// 存档快照代的实现（只编进 Windows）。约定见 save_txn_win.h。
// 这里刻意不用 rename：提交路径上只有 mkdir / CreateFile / WriteFile / flush——
// Windows 上这三件事不会被目录里任何一个打开的句柄挡掉，改名会被。
// The implementation of save snapshots (Windows builds only). Contract in save_txn_win.h.
// Deliberately no rename here: the commit path is only mkdir / CreateFile / WriteFile / flush, none of which a handle
// open inside the directory can block, whereas a rename can be blocked by exactly that.
#include "dm_win_compat.h"
#include "save_txn_win.h"
#include <dirent.h>
#include <sys/stat.h>
#include <utime.h>
#include <errno.h>

#define TXN_SUFFIX ".txn"
#define DONE_NAME  "DONE"
#define MAXP       1400
#define MAX_REL    768
#define MAX_SKIP   40

static char g_skip[MAX_SKIP][MAXP];
static int  g_nskip;
static HANDLE g_lock;

// ---------- 路径 ----------
static int sroot(const char *save, char *out, size_t n) {
    size_t l = save ? strlen(save) : 0;
    if (!l || l >= n) return -1;
    memcpy(out, save, l + 1);
    while (l > 1 && (out[l - 1] == '/' || out[l - 1] == '\\')) out[--l] = 0;
    return 0;
}
static int txn_of(const char *root, char *out, size_t n) {
    return snprintf(out, n, "%s%s", root, TXN_SUFFIX) >= (int)n ? -1 : 0;
}
static int join(char *out, size_t n, const char *a, const char *b) {
    size_t la = strlen(a);
    int sep = la && a[la - 1] != '/' && a[la - 1] != '\\';
    return snprintf(out, n, "%s%s%s", a, sep ? "/" : "", b) >= (int)n ? -1 : 0;
}
static int join_rel(char *out, size_t n, const char *rel, const char *name) {
    if (!rel[0]) return strlen(name) < n ? (snprintf(out, n, "%s", name) < 0 ? -1 : 0) : -1;
    return snprintf(out, n, "%s/%s", rel, name) >= (int)n ? -1 : 0;
}
static void gen_name(int k, char *out, size_t n) { snprintf(out, n, "g%06d", k); }
static int is_d2s(const char *name) {
    size_t l = strlen(name);
    return l > 4 && !_stricmp(name + l - 4, ".d2s");
}
// 文件戳用 Windows 自己的 100ns FILETIME，不用 CRT 的 st_mtime（只有秒）。
// 同一秒里原地改写一个同样长度的文件，「大小 + 毫秒」看不出区别，展开时会被跳过 —— 丢的就是最新那份进度。
// The file stamp is Windows' own 100ns FILETIME, not the CRT's second-granular st_mtime: overwriting a same-size
// file in place within the same second looks unchanged by size+second, and the expand would skip it - losing exactly the newest progress.
typedef struct { long long size, stamp; int isdir; } finfo;
static int finfo_of(const char *path, finfo *o) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) { errno = EIO; return -1; }
    o->isdir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    o->size = ((long long)d.nFileSizeHigh << 32) | (long long)d.nFileSizeLow;
    o->stamp = ((long long)d.ftLastWriteTime.dwHighDateTime << 32) | (long long)d.ftLastWriteTime.dwLowDateTime;
    return 0;
}
static FILETIME ft_of(long long stamp) {
    FILETIME f; f.dwLowDateTime = (DWORD)(stamp & 0xffffffffll); f.dwHighDateTime = (DWORD)((unsigned long long)stamp >> 32);
    return f;
}
static int ensure_dir_of(const char *path) {          // 建出文件所在的所有目录 | create every directory leading to the file
    char p[MAXP]; strlcpy(p, path, sizeof p);
    for (char *q = p + 1; *q; q++)
        if (*q == '/' || *q == '\\') { *q = 0; if (_mkdir(p) && errno != EEXIST) return -1; *q = '/'; }
    return 0;
}
static int is_skip(const char *path) {
    for (int i = 0; i < g_nskip; i++) if (!_stricmp(g_skip[i], path)) return 1;
    return 0;
}

// ---------- 摘要 ----------
static unsigned long long mix(const char *rel, long long size, long long mt) {
    unsigned long long h = 0xcbf29ce484222325ull;
    for (const char *c = rel; *c; c++) h = (h ^ (unsigned char)*c) * 0x100000001b3ull;
    h ^= (unsigned long long)size * 0x9E3779B97F4A7C15ull;
    h ^= (unsigned long long)(mt + 1) * 0xC2B2AE3D27D4EB4Full;
    return h ^ (h >> 33);
}

// ---------- 快照内容清单 ----------
typedef struct { char rel[MAX_REL]; long long size, mt; } dent;
typedef struct { int gen, n, cap; long long bytes; unsigned long long digest; dent *e; } dset;

static void ds_free(dset *d) { free(d->e); memset(d, 0, sizeof *d); }
static int ds_add(dset *d, const char *rel, long long size, long long mt) {
    if (d->n == d->cap) { int c = d->cap ? d->cap * 2 : 64; dent *p = realloc(d->e, (size_t)c * sizeof *p); if (!p) return -1; d->e = p; d->cap = c; }
    strlcpy(d->e[d->n].rel, rel, MAX_REL); d->e[d->n].size = size; d->e[d->n].mt = mt; d->n++;
    d->bytes += size; d->digest ^= mix(rel, size, mt);
    return 0;
}
static int ds_find(const dset *d, const char *rel, const dent **out) {
    for (int i = 0; i < d->n; i++) if (!strcmp(d->e[i].rel, rel)) { if (out) *out = &d->e[i]; return i; }
    return -1;
}

// 把一个目录树收进清单（只收文件）| collect a directory tree into the list (files only)
static int walk_collect(const char *dir, const char *rel, dset *d) {
    DIR *x = opendir(dir);
    if (!x) { errno = EIO; return -1; }
    struct dirent *e; char p[MAXP], r[MAX_REL]; int rc = 0;
    while ((e = readdir(x))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (join(p, sizeof p, dir, e->d_name) || join_rel(r, sizeof r, rel, e->d_name)) { rc = -1; break; }
        finfo fi;
        if (finfo_of(p, &fi)) { rc = -1; break; }
        if (fi.isdir) { if (walk_collect(p, r, d)) { rc = -1; break; } }
        else if (ds_add(d, r, fi.size, fi.stamp)) { rc = -1; break; }
    }
    closedir(x);
    return rc;
}

static int wwrite(HANDLE h, const char *s) {
    DWORD nw = 0; size_t l = strlen(s);
    return (WriteFile(h, s, (DWORD)l, &nw, NULL) && nw == l) ? 0 : -1;
}
// DONE 最后写，用 WRITE_THROUGH：它存在 = 这一代已经完整
// DONE is written last with WRITE_THROUGH: its presence means this generation is complete
static int write_done(const char *gen_dir, dset *d) {
    char p[MAXP], line[MAX_REL + 96];
    if (join(p, sizeof p, gen_dir, DONE_NAME)) return -1;
    HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) { errno = EIO; return -1; }
    int bad = 0;
    snprintf(line, sizeof line, "gen %06d\nfiles %d\nbytes %lld\ndigest %016llx\ncreated %lld\n",
             d->gen, d->n, d->bytes, d->digest, (long long)time(NULL));
    if (wwrite(h, line)) bad = 1;
    for (int i = 0; i < d->n && !bad; i++) {
        snprintf(line, sizeof line, "%s\t%lld\t%lld\n", d->e[i].rel, d->e[i].size, d->e[i].mt);
        if (wwrite(h, line)) bad = 1;
    }
    FlushFileBuffers(h); CloseHandle(h);
    if (bad) { remove(p); errno = EIO; }
    return bad ? -1 : 0;
}
static int parse_done(const char *gen_dir, dset *d) {
    char p[MAXP], line[MAX_REL + 96];
    memset(d, 0, sizeof *d);
    if (join(p, sizeof p, gen_dir, DONE_NAME)) return -1;
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    int bad = 0; long long bytes = 0; unsigned long long digest = 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line); while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!strncmp(line, "gen ", 4)) d->gen = atoi(line + 4);
        else if (!strncmp(line, "files ", 6)) { }
        else if (!strncmp(line, "bytes ", 6)) bytes = atoll(line + 6);
        else if (!strncmp(line, "digest ", 7)) digest = strtoull(line + 7, NULL, 16);
        else if (!strncmp(line, "created ", 8)) { }     // 只是给人看的，解析时忽略 | informational only
        else if (line[0]) {
            char *t2 = strrchr(line, '\t'); if (!t2) { bad = 1; break; } *t2 = 0;
            char *t1 = strrchr(line, '\t'); if (!t1) { bad = 1; break; } *t1 = 0;
            if (ds_add(d, line, atoll(t1 + 1), atoll(t2 + 1))) { bad = 1; break; }
        }
    }
    fclose(f);
    if (bad || !d->gen) { ds_free(d); errno = EIO; return -1; }
    d->bytes = bytes; d->digest = digest;
    return 0;
}

// ---------- 复制 ----------
static int copy_file(const char *src, const char *dst, const char *rel, const finfo *si, long long *bytes, unsigned long long *dig) {
    FILE *in = fopen(src, "rb");
    if (!in) { errno = EIO; return -1; }
    SetFileAttributesA(dst, FILE_ATTRIBUTE_NORMAL);
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); errno = EIO; return -1; }
    // 64 KB 缓冲放堆上，不放栈上：这条复制路径跑在客体游戏线程的栈上，主线程栈只有 0x18000（96 KB），
    // 64 KB 的局部数组让 __chkstk 直接探到栈下守卫页，是一种「启动就崩」的越栈（实测 sig 11，pc=__chkstk+0x10）。
    // 同 hle_fs.c 里「两块 64 KB 放栈上直接越栈」的教训。
    // The 64 KB buffer goes on the heap, not the stack: this copy path runs on a guest game thread's stack, the main
    // one is only 0x18000 (96 KB), and a 64 KB local array makes __chkstk hit the guard page below it (measured: a
    // crash right at startup, sig 11, pc=__chkstk+0x10). Same lesson as the two 64 KB blocks in hle_fs.c.
    char *buf = (char *)malloc(65536); size_t got; int bad = 0;
    if (!buf) { fclose(in); fclose(out); errno = ENOMEM; return -1; }
    while ((got = fread(buf, 1, 65536, in)) > 0)
        if (fwrite(buf, 1, got, out) != got) { bad = 1; break; }
    if (ferror(in)) bad = 1;
    if (fclose(out)) bad = 1;
    fclose(in);
    free(buf);
    if (bad) { errno = EIO; return -1; }                 // 半截文件不能留在快照里 | a half-written file must not stay in a snapshot
    { HANDLE h = CreateFileA(dst, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
      if (h != INVALID_HANDLE_VALUE) { FlushFileBuffers(h); CloseHandle(h); } }
    {   // 把源文件的 100ns 时间戳原样搬过去：比对靠它，秒级 utime 会把精度丢掉 | carry the source's 100ns stamp: comparison relies on it, utime would drop to seconds
        FILETIME ft = ft_of(si->stamp);
        HANDLE h = CreateFileA(dst, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { SetFileTime(h, NULL, NULL, &ft); CloseHandle(h); }
        else { errno = EIO; return -1; }
    }
    if (bytes) *bytes += si->size;
    if (dig) *dig ^= mix(rel, si->size, si->stamp);
    return 0;
}
static int copy_tree(const char *src, const char *dst, const char *rel, long long *bytes, unsigned long long *dig, int *nfiles) {
    DIR *x = opendir(src);
    if (!x) { errno = EIO; return -1; }
    struct dirent *e; char sp[MAXP], dp[MAXP], r[MAX_REL]; int rc = 0;
    while ((e = readdir(x))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (join(sp, sizeof sp, src, e->d_name) || join(dp, sizeof dp, dst, e->d_name) ||
            join_rel(r, sizeof r, rel, e->d_name)) { rc = -1; break; }
        finfo fi;
        if (finfo_of(sp, &fi)) { rc = -1; break; }
        if (fi.isdir) {
            if (_mkdir(dp) && errno != EEXIST) { rc = -1; break; }
            if (copy_tree(sp, dp, r, bytes, dig, nfiles)) { rc = -1; break; }
        } else {
            if (is_skip(sp)) continue;                   // 逻辑删除、物理上还删不掉的文件不进快照 | logically deleted but still locked: keep it out
            if (copy_file(sp, dp, r, &fi, bytes, dig)) { rc = -1; break; }
            (*nfiles)++;
        }
    }
    closedir(x);
    return rc;
}

static int rm_tree(const char *p) {
    DIR *x = opendir(p);
    if (!x) return -1;
    struct dirent *e; char c[MAXP]; int rc = 0;
    while ((e = readdir(x))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (join(c, sizeof c, p, e->d_name)) { rc = -1; continue; }
        struct stat sb;
        if (!stat(c, &sb) && S_ISDIR(sb.st_mode)) rc |= rm_tree(c);
        else rc |= remove(c) ? -1 : 0;
    }
    closedir(x);
    return rc | (rmdir(p) ? -1 : 0);
}

// ---------- 代编号 ----------
static int scan_max(const char *root) {
    char txn[MAXP];
    if (txn_of(root, txn, sizeof txn)) return 0;
    DIR *x = opendir(txn);
    if (!x) return 0;
    struct dirent *e; int best = 0;
    while ((e = readdir(x))) {
        if (e->d_name[0] != 'g' || strlen(e->d_name) != 7) continue;
        int digits = 1;
        for (int i = 1; i <= 6; i++) if (e->d_name[i] < '0' || e->d_name[i] > '9') { digits = 0; break; }
        if (!digits) continue;
        int k = atoi(e->d_name + 1);
        char gp[MAXP], dp[MAXP];
        if (join(gp, sizeof gp, txn, e->d_name) || join(dp, sizeof dp, gp, DONE_NAME)) continue;
        if (access(dp, 0)) continue;                     // 没有 DONE = 没写完，不算数 | no DONE means unfinished, not a candidate
        if (k > best) best = k;
    }
    closedir(x);
    return best;
}

// ---------- 对外 ----------
void save_txn_skip(const char *const *paths, int n) {
    g_nskip = 0;
    for (int i = 0; i < n && i < MAX_SKIP; i++) strlcpy(g_skip[g_nskip++], paths[i], MAXP);
}

int save_txn_lock(const char *save) {
    char root[MAXP], txn[MAXP], lk[MAXP];
    if (sroot(save, root, sizeof root) || txn_of(root, txn, sizeof txn) || join(lk, sizeof lk, txn, "instance.lock")) return -1;
    if (g_lock) return 0;
    _mkdir(root); _mkdir(txn);
    g_lock = CreateFileA(lk, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_lock == INVALID_HANDLE_VALUE) { g_lock = NULL; return -1; }
    return 0;
}

static int snapshot_from(const char *src, const char *root, char *gen_out, size_t gn,
                         int *files, long long *bytes, unsigned long long *digest) {
    char txn[MAXP];
    if (txn_of(root, txn, sizeof txn)) return -1;
    _mkdir(root); _mkdir(txn);
    int k = scan_max(root) + 1;
    char gen[MAX_REL], gpath[MAXP];
    gen_name(k, gen, sizeof gen);
    if (join(gpath, sizeof gpath, txn, gen)) return -1;
    if (!access(gpath, 0) && rm_tree(gpath)) { errno = EIO; return -1; }   // 上一轮没写完的同号残骸 | leftover of an unfinished same-numbered attempt
    if (_mkdir(gpath)) { errno = EIO; return -1; }
    int nf = 0; long long nb = 0; unsigned long long dg = 0;
    if (copy_tree(src, gpath, "", &nb, &dg, &nf)) { rm_tree(gpath); errno = EIO; return -1; }
    dset d; memset(&d, 0, sizeof d); d.gen = k;
    if (walk_collect(gpath, "", &d)) { ds_free(&d); rm_tree(gpath); return -1; }
    if (write_done(gpath, &d)) { ds_free(&d); rm_tree(gpath); return -1; }
    if (files) *files = d.n;
    if (bytes) *bytes = d.bytes;
    if (digest) *digest = d.digest;
    if (gen_out && gn) strlcpy(gen_out, gen, gn);
    ds_free(&d);
    return 0;
}

int save_txn_snapshot(const char *save, char *gen, size_t gn, int *files, long long *bytes, unsigned long long *digest) {
    char root[MAXP];
    if (sroot(save, root, sizeof root)) { errno = EINVAL; return -1; }
    return snapshot_from(root, root, gen, gn, files, bytes, digest);
}

static int has_d2s(const char *root) {
    DIR *x = opendir(root);
    if (!x) return 0;
    struct dirent *e; char sub[MAXP]; int found = 0;
    while (!found && (e = readdir(x))) {
        if (e->d_name[0] == '.') continue;
        struct stat sb;
        if (join(sub, sizeof sub, root, e->d_name) || stat(sub, &sb) || !S_ISDIR(sb.st_mode)) {
            if (is_d2s(e->d_name)) found = 1;
            continue;
        }
        DIR *y = opendir(sub); struct dirent *e2;
        while (y && (e2 = readdir(y))) {
            if (e2->d_name[0] == '.') continue;
            if (is_d2s(e2->d_name)) { found = 1; break; }
        }
        if (y) closedir(y);
    }
    closedir(x);
    return found;
}
static int has_any(const char *root) {
    DIR *x = opendir(root);
    if (!x) return 0;
    struct dirent *e; int any = 0;
    while (!any && (e = readdir(x))) if (e->d_name[0] != '.') any = 1;
    closedir(x);
    return any;
}
// 旧布局（.work / .new / .new.rtmp）不删，整体挪进 .txn\legacy-<时间>：查案用，也绝不断了用户的退路
// The old layout's .work/.new/.new.rtmp are moved into .txn\legacy-<time>, never deleted
static void quarantine(const char *root) {
    static const char *suf[3] = { ".work", ".new", ".new.rtmp" };
    char txn[MAXP], leg[MAXP];
    if (txn_of(root, txn, sizeof txn)) return;
    int made = 0;
    for (int i = 0; i < 3; i++) {
        char src[MAXP], dst[MAXP];
        snprintf(src, sizeof src, "%s%s", root, suf[i]);
        if (access(src, 0)) continue;
        if (!made) {
            snprintf(leg, sizeof leg, "%s/legacy-%lld", txn, (long long)time(NULL));
            if (_mkdir(leg) && errno != EEXIST) return;
            made = 1;
        }
        snprintf(dst, sizeof dst, "%s/%s", leg, suf[i] + 1);
        MoveFileExA(src, dst, 0);
    }
}

int save_txn_prepare(const char *save, char *gen, size_t gn) {
    char root[MAXP];
    if (sroot(save, root, sizeof root)) { errno = EINVAL; return -1; }
    _mkdir(root);
    char txn[MAXP];
    if (txn_of(root, txn, sizeof txn)) return -1;
    _mkdir(txn);
    int k = scan_max(root);
    if (k) { gen_name(k, gen, gn); return 1; }
    // 还没有任何快照：迁移旧布局 | no snapshot yet: migrate the old layout
    char src[MAXP]; src[0] = 0;
    if (!has_d2s(root)) {
        char c[MAXP];
        snprintf(c, sizeof c, "%s.new", root);
        if (has_d2s(c)) strlcpy(src, c, sizeof src);
        else {
            snprintf(c, sizeof c, "%s.new.rtmp", root);
            if (has_d2s(c)) strlcpy(src, c, sizeof src);
        }
    }
    if (!src[0] && !has_any(root)) { quarantine(root); return 0; }   // 全新安装 | brand new install
    if (snapshot_from(src[0] ? src : root, root, gen, gn, NULL, NULL, NULL)) return -1;
    if (src[0] && save_txn_restore(root, gen)) return -1;             // 从旧 .new 恢复 | restore from the old .new
    quarantine(root);
    return 1;
}

static void prune(const char *dir, const char *rel, dset *d, int is_root) {
    DIR *x = opendir(dir);
    if (!x) return;
    struct dirent *e; char c[MAXP], r[MAX_REL];
    while ((e = readdir(x))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (join(c, sizeof c, dir, e->d_name) || join_rel(r, sizeof r, rel, e->d_name)) continue;
        finfo fi;
        if (finfo_of(c, &fi)) continue;
        if (fi.isdir) { prune(c, r, d, 0); rmdir(c); }        // 删空的子目录；非空 rmdir 失败，留着 | remove empty subdirs; a non-empty one stays
        else if (ds_find(d, r, NULL) < 0) remove(c);                     // 快照里没有的旧文件 | a stale file the snapshot does not have
    }
    closedir(x);
    if (!is_root) rmdir(dir);
}

static int tree_matches(const char *dir, const char *rel, dset *d, int *seen) {
    DIR *x = opendir(dir);
    if (!x) return 1;
    struct dirent *e; char p[MAXP], r[MAX_REL]; int bad = 0;
    while (!bad && (e = readdir(x))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (join(p, sizeof p, dir, e->d_name) || join_rel(r, sizeof r, rel, e->d_name)) { bad = 1; break; }
        finfo fi;
        if (finfo_of(p, &fi)) { bad = 1; break; }
        if (fi.isdir) { if (tree_matches(p, r, d, seen)) bad = 1; }
        else {
            const dent *t = NULL;
            if (ds_find(d, r, &t) < 0 || t->size != fi.size || t->mt != fi.stamp) bad = 1;
            else (*seen)++;
        }
    }
    closedir(x);
    return bad;
}

int save_txn_restore(const char *save, const char *gen) {
    char root[MAXP], txn[MAXP], gpath[MAXP];
    if (sroot(save, root, sizeof root) || !gen || !gen[0] || txn_of(root, txn, sizeof txn) ||
        join(gpath, sizeof gpath, txn, gen)) { errno = EINVAL; return -1; }
    dset d;
    if (parse_done(gpath, &d)) return -1;
    _mkdir(root);
    int seen = 0;
    if (!tree_matches(root, "", &d, &seen) && seen == d.n) { ds_free(&d); return 0; }   // 已经是这份，零成本 | already this version: no work
    prune(root, "", &d, 1);
    int bad = 0;
    for (int i = 0; i < d.n; i++) {
        char s[MAXP], t[MAXP]; finfo si;
        if (join(s, sizeof s, gpath, d.e[i].rel) || join(t, sizeof t, root, d.e[i].rel)) { bad = 1; break; }
        // 尺寸与时间戳按 DONE 写的走（快照里的文件本来就是照着它复制的），保证展开后与快照逐字段一致
        // Size and stamp come from the DONE record (the snapshot file was copied from exactly that), so the expand lands field-identical
        si.isdir = 0; si.size = d.e[i].size; si.stamp = d.e[i].mt;
        if (ensure_dir_of(t) || copy_file(s, t, d.e[i].rel, &si, NULL, NULL)) { bad = 1; break; }
    }
    ds_free(&d);
    if (bad) { errno = EIO; return -1; }
    return 0;
}

void save_txn_gc(const char *save, int keep) {
    char root[MAXP], txn[MAXP];
    if (sroot(save, root, sizeof root) || txn_of(root, txn, sizeof txn)) return;
    if (keep < 2) keep = 2;
    DIR *x = opendir(txn);
    if (!x) return;
    int nums[64], n = 0, maxk = 0;
    struct dirent *e;
    while ((e = readdir(x)) && n < 64) {
        if (e->d_name[0] != 'g' || strlen(e->d_name) != 7) continue;
        int digits = 1;
        for (int i = 1; i <= 6; i++) if (e->d_name[i] < '0' || e->d_name[i] > '9') { digits = 0; break; }
        if (!digits) continue;
        int k = atoi(e->d_name + 1);
        nums[n++] = k;
        if (k > maxk) maxk = k;
    }
    closedir(x);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) if (nums[j] < nums[i]) { int t = nums[i]; nums[i] = nums[j]; nums[j] = t; }
    // 留下的必须包含最新的那代；只删比它旧、且超出 keep 的 | the newest stays; delete older ones beyond keep
    int kept = 0;
    for (int i = n - 1; i >= 0; i--) {
        if (nums[i] == maxk) { kept++; continue; }
        if (kept < keep) { kept++; continue; }
        char gen[MAX_REL], gp[MAXP];
        gen_name(nums[i], gen, sizeof gen);
        if (!join(gp, sizeof gp, txn, gen)) rm_tree(gp);
    }
}

int save_txn_digest(const char *save, const char *gen, unsigned long long *digest) {
    char root[MAXP], txn[MAXP], gpath[MAXP];
    if (sroot(save, root, sizeof root) || !gen || txn_of(root, txn, sizeof txn) ||
        join(gpath, sizeof gpath, txn, gen)) return -1;
    dset d;
    if (parse_done(gpath, &d)) return -1;
    if (digest) *digest = d.digest;
    ds_free(&d);
    return 0;
}
