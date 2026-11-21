// dlfcn：宿主用来取图形 / 系统库里的函数（安卓上是 libnativewindow，Windows 上主要是 vulkan-1.dll 与系统 DLL）。
// 句柄就是 HMODULE，但 0 不能当句柄用（LoadLibrary 失败返回 NULL），所以外面套一层。
// dlfcn: the host uses this to fetch functions from graphics / system libraries (libnativewindow on Android; vulkan-1.dll and system DLLs on Windows).
// The handle is an HMODULE, but 0 cannot serve as one (LoadLibrary returns NULL on failure), so it is wrapped.
#pragma once
#define RTLD_LAZY 0
#define RTLD_NOW 1
#define RTLD_GLOBAL 0
#define RTLD_LOCAL 0
void *dlopen(const char *path, int mode);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);
// dladdr：崩溃回溯用它把地址翻成「模块 + 符号」。Windows 上没有符号表（PDB 不在发布包里），
// 只给模块基址与模块文件名，符号名留空 —— 打印成 module+0x… 的形式，配合 host 侧 map 文件仍可定位。
// dladdr: the crash backtrace uses it to turn an address into "module + symbol". Windows has no symbol table (no PDB in a release package),
// so only the module base and file name are given and the symbol name is left empty — it prints as module+0x…, still locatable with the host map file.
typedef struct {
    const char *dli_fname;   // 模块路径 | module path
    void *dli_fbase;         // 模块基址 | module base
    const char *dli_sname;   // 符号名（恒为 NULL）| symbol name (always NULL)
    void *dli_saddr;         // 符号地址（0）| symbol address (0)
} Dl_info;
int dladdr(const void *addr, Dl_info *info);
