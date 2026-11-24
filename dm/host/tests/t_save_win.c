// Windows 存档快照的端到端单测（秒级）：直接包含 hle_fs.c，走真实的 MountSave / Commit 路径。
// 覆盖的是「Windows 上一定会发生、而 Mac 上不会」的那几件事：
//   ① 首次挂载把标准存档变成 g000001，且不再生成 .work（旧机制在 Windows 上改名会被挡住）；
//   ② 提交只是「建新代 + 复制 + DONE」，不换目录名：有文件被打开着也能提交；
//   ③ 事务写到一半没提交 → 重挂载后回到快照内容（这是原先丢档的主要入口）；
//   ④ 存档里删不掉的被占用文件 → 记逻辑删除、对客体不可见、不进快照；释放后下次挂载按快照把物理残留清掉；
//   ⑤ 删不掉的被占用文件不阻塞同名的「临时文件 → 改名」替换（游戏存档的三步塌缩成一次替换）。
// Windows save-snapshot end-to-end test (takes seconds): includes hle_fs.c and drives the real MountSave / Commit path.
// It covers exactly the things that inevitably happen on Windows and never on the Mac:
//   (1) the first mount turns a standard save into g000001 and creates no .work (the old mechanism's rename gets blocked);
//   (2) a commit is only mkdir + copy + DONE, never a directory rename: it succeeds even with files open;
//   (3) a transaction written half-way and never committed comes back to the snapshot content on remount (the main old loss path);
//   (4) a locked file inside the save that cannot be deleted is recorded as logically deleted, is invisible to the guest and stays out of the snapshot; after release the next mount removes the leftover;
//   (5) a locked undeletable file does not block the same-name "temp file -> rename" replacement (the game's three steps collapse into one).
//
// 只在 Windows 上编（dm\host\tests\Makefile 是 macOS 的）。构建与运行：
// Windows-only (dm\host\tests\Makefile is for macOS). Build and run from the repo root:
//   aarch64-w64-mingw32-clang -O1 -g -DDM_WIN=1 -D_WIN32_WINNT=0x0A00 -Idm/host -Idm/win/compat/include ^
//     -include dm_win_compat.h -o build/win/t_save_win.exe dm/host/tests/t_save_win.c dm/host/tests/t_fs_stub.c ^
//     dm/host/dm_i18n.c dm/win/compat/misc_win.c dm/win/compat/mman_win.c dm/win/compat/save_txn_win.c ^
//     -static-libgcc -lbcrypt -lpthread
// 或者直接跑 build\win\test-save.ps1（两个存档测试一起编、一起跑）。 / Or just run build\win\test-save.ps1 (builds and runs both save tests).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "../hle_fs.c"

struct dm_flags g_flag; uint64_t g_t0_ns; uint64_t now_ns(void) { return 0; }
void dm_prof_add(int id, uint64_t ns, uint64_t b) { (void)id; (void)ns; (void)b; } void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
extern int g_nfail, g_lasterr;

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  x "); } else printf("  v "); printf(__VA_ARGS__); printf("\n"); } while (0)
static void put(const char *p, const char *s) { FILE *f = fopen(p, "w"); if (f) { fputs(s, f); fclose(f); } }
static int same(const char *p, const char *s) {
    char b[64] = {0}; FILE *f = fopen(p, "r"); if (!f) return 0;
    size_t k = fread(b, 1, sizeof b - 1, f); fclose(f);
    return k == strlen(s) && !memcmp(b, s, k);
}
static int read_fs(const char *path, char *out, size_t n) {
    uint64_t h; size_t got = 0;
    if (h_fs_OpenFile(&h, path, MODE_READ)) return 0;
    Result r = h_fs_ReadFile(&got, h, 0, out, n - 1); h_fs_CloseFile(h); out[got] = 0; return !r;
}
static Result write_save(const char *path, const char *s) {
    uint64_t h; Result r = h_fs_OpenFile(&h, path, MODE_WRITE); if (r) return r;
    h_fs_SetFileSize(h, (int64_t)strlen(s));
    r = h_fs_WriteFile(h, 0, s, strlen(s), NULL); h_fs_CloseFile(h); return r;
}
// 只给共享读+写、不给共享删：和 build\win\hold.py 按住 Settings.json 时一模一样
// Share read+write only, never delete: exactly what build\win\hold.py does to hold Settings.json
static HANDLE hold(const char *p) {
    return CreateFileA(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}
static int depth_of(const char *p) { int d = 0; for (const char *c = p; *c; c++) if (*c == '/' || *c == '\\') d++; return d; }

int main(void) {
    char root[1024], save[1100], txn[1200], p[1200], p2[1300], got[64];
    const char *tmp = getenv("TEMP"); if (!tmp) tmp = ".";
    snprintf(root, sizeof root, "%s/tsw.XXXXXX", tmp);
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    snprintf(save, sizeof save, "%s/save", root); snprintf(txn, sizeof txn, "%s.txn", save);
    mkdir(save, 0755);
    snprintf(p, sizeof p, "%s/HeroA", save); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/HeroA/HeroA.d2s", save); put(p, "AAA");
    setenv("DM_SAVE", save, 1);

    printf("(1) 首次挂载 = 把标准存档收成 g000001\n");
    CHECK(h_fs_MountSave("save", NULL) == RES_OK, "mount");
    snprintf(p, sizeof p, "%s/save.work", root); CHECK(access(p, F_OK) != 0, "no .work (the old clone directory is not created)");
    snprintf(p, sizeof p, "%s/g000001/DONE", txn); CHECK(access(p, F_OK) == 0, "generation g000001 with a DONE marker");
    snprintf(p, sizeof p, "%s/g000001/HeroA/HeroA.d2s", txn); CHECK(same(p, "AAA"), "the snapshot holds the pre-existing file");
    snprintf(p, sizeof p, "%s/HeroA/HeroA.d2s", save); CHECK(same(p, "AAA"), "the live dir still holds it too");
    CHECK(read_fs("save:/HeroA/HeroA.d2s", got, sizeof got) && !strcmp(got, "AAA"), "guest read works");

    printf("(2) 提交：建新代，不换目录名\n");
    CHECK(write_save("save:/HeroA/HeroA.d2s", "AAAA") == RES_OK, "write AAAA");
    CHECK(h_fs_Commit("save") == RES_OK, "commit");
    snprintf(p, sizeof p, "%s/g000002/HeroA/HeroA.d2s", txn); CHECK(same(p, "AAAA"), "the new generation holds the committed bytes");
    CHECK(same(p, "AAAA"), "and the old generation is untouched");

    printf("(3) 事务写到一半被杀 → 重挂载回到快照内容\n");
    CHECK(write_save("save:/HeroA/HeroA.d2s", "HALF") == RES_OK, "write half a transaction");
    snprintf(p, sizeof p, "%s/HeroA/HeroA.d2s", save); CHECK(same(p, "HALF"), "the live dir is dirty, as a killed run leaves it");
    g_nmnt = 0;                                                      // 模拟进程重启 | simulate a process restart
    CHECK(h_fs_MountSave("save", NULL) == RES_OK, "remount");
    snprintf(p, sizeof p, "%s/HeroA/HeroA.d2s", save); CHECK(same(p, "AAAA"), "remount puts the snapshot content back (no more lost saves)");

    printf("(4) 被占用的文件删不掉 → 逻辑删除、不进快照\n");
    snprintf(p, sizeof p, "%s/HeroA/HeroA.d2s", save);
    HANDLE hh = hold(p);
    CHECK(hh != INVALID_HANDLE_VALUE, "hold HeroA.d2s open (share read+write, no delete)");
    CHECK(h_fs_DeleteFile("save:/HeroA/HeroA.d2s") == RES_OK, "DeleteFile reports success (logical delete), not a failure the game would swallow");
    CHECK((g_ntomb) == 1, "one tombstone recorded (actual %d)", g_ntomb);
    int32_t et = -1;
    CHECK(h_fs_GetEntryType(&et, "save:/HeroA/HeroA.d2s") == RES_PATH_NOT_FOUND, "GetEntryType says it does not exist");
    uint64_t rh = 0;
    CHECK(h_fs_OpenFile(&rh, "save:/HeroA/HeroA.d2s", MODE_READ) == RES_PATH_NOT_FOUND, "OpenFile says it does not exist");
    CHECK(h_fs_Commit("save") == RES_OK, "commit while the file is still physically locked");
    snprintf(p, sizeof p, "%s/g000003/HeroA/HeroA.d2s", txn);
    CHECK(access(p, F_OK) != 0, "the snapshot does not resurrect the deleted file (skip list honoured)");
    snprintf(p, sizeof p, "%s/HeroA/HeroA.d2s", save);
    CHECK(access(p, F_OK) == 0, "the physical leftover is still there, as expected");
    CloseHandle(hh);

    printf("(5) 物理残留不会跟着进下一代\n");
    g_nmnt = 0;
    CHECK(h_fs_MountSave("save", NULL) == RES_OK, "remount after release");
    snprintf(p, sizeof p, "%s/HeroA/HeroA.d2s", save);
    CHECK(access(p, F_OK) != 0, "the leftover is gone once nobody holds it any more");

    printf("(6) 被占用过、但已经释放的目标：改名走一次替换\n");
    snprintf(p, sizeof p, "%s/HeroA", save); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/HeroA/HeroA.d2s", save); put(p, "OLD");
    snprintf(p2, sizeof p2, "%s/tmp.d2s", save); put(p2, "NEW");
    HANDLE hh2 = hold(p);
    CHECK(hh2 != INVALID_HANDLE_VALUE, "hold the destination open (transient: antivirus / indexer)");
    CHECK(h_fs_DeleteFile("save:/HeroA/HeroA.d2s") == RES_OK, "DeleteFile -> logical delete");
    CloseHandle(hh2);                                                // 占用解除，物理文件还在 | the holder lets go, the physical file is still there
    CHECK(h_fs_RenameFile("save:/tmp.d2s", "save:/HeroA/HeroA.d2s") == RES_OK, "rename onto the tombstoned name succeeds (one replace instead of the game's three steps)");
    CHECK(read_fs("save:/HeroA/HeroA.d2s", got, sizeof got) && !strcmp(got, "NEW"), "guest reads the new content (actual %s)", got);
    CHECK(h_fs_Commit("save") == RES_OK, "commit");
    snprintf(p, sizeof p, "%s/g000004/HeroA/HeroA.d2s", txn); CHECK(same(p, "NEW"), "the snapshot has NEW");

    printf("(7) 有文件被打开着也能提交（旧机制在这里必挂）\n");
    snprintf(p, sizeof p, "%s/HeroA/HeroA.d2s", save);
    HANDLE hh3 = hold(p);
    CHECK(hh3 != INVALID_HANDLE_VALUE, "hold a file open");
    CHECK(h_fs_Commit("save") == RES_OK, "commit while a file inside the save is open");
    CHECK(atomic_load(&g_save_poison) == 0, "no poison was raised");
    CloseHandle(hh3);

    printf("(8) 占用一直不解除：改名必失败 → 拒绝提交，但绝不假装成功\n");
    snprintf(p, sizeof p, "%s/HeroA/HeroA.d2s", save); put(p, "KEEP");
    snprintf(p2, sizeof p2, "%s/tmp2.d2s", save); put(p2, "NEXT");
    HANDLE hh4 = hold(p);
    CHECK(hh4 != INVALID_HANDLE_VALUE, "hold the destination open for the whole attempt");
    CHECK(h_fs_DeleteFile("save:/HeroA/HeroA.d2s") == RES_OK, "DeleteFile -> logical delete again");
    Result rr = h_fs_RenameFile("save:/tmp2.d2s", "save:/HeroA/HeroA.d2s");
    CHECK(rr != RES_OK, "rename onto a still-locked name fails (no silent takeover is claimed)");
    CHECK(atomic_load(&g_save_poison) != 0, "the transaction is poisoned");
    CHECK(h_fs_Commit("save") == RES_FAIL, "commit is refused instead of committing something wrong");
    snprintf(p, sizeof p, "%s/g000004/HeroA/HeroA.d2s", txn); CHECK(same(p, "NEW"), "the last good generation is untouched");
    CloseHandle(hh4);

    printf("(9) 释放后重新挂载，一切回到正轨\n");
    g_nmnt = 0;
    CHECK(h_fs_MountSave("save", NULL) == RES_OK, "remount");
    CHECK(read_fs("save:/HeroA/HeroA.d2s", got, sizeof got) && !strcmp(got, "NEW"), "the live dir is back to the last good generation (actual %s)", got);

    { char cmd[1200]; (void)depth_of; snprintf(cmd, sizeof cmd, "cmd /c rd /s /q \"%s\"", root); system(cmd); }
    printf("== %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
