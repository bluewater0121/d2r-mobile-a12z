# 最小调试器：用 DEBUG_ONLY_THIS_PROCESS 起 dmhost，把每条异常（一/二次）的代码、地址、pc/lr/sp 与 x0..x3 打出来。
# 用途：宿主 VEH 没记录就退出（STATUS_* 直接成了进程退出码）时定位现场。
# Minimal debugger: launch dmhost with DEBUG_ONLY_THIS_PROCESS and print every exception (first and second chance)
# with its code, address, pc/lr/sp and x0..x3.
# Use: when the host dies with no VEH record (the STATUS_* code itself became the exit code).
#
# 用法 | usage: python dbg_win.py <exe> <args...>
import ctypes, ctypes.wintypes as wt, sys, os

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

DEBUG_ONLY_THIS_PROCESS = 0x00000002
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001
EXCEPTION_DEBUG_EVENT = 1
CREATE_THREAD_DEBUG_EVENT = 2
CREATE_PROCESS_DEBUG_EVENT = 3
EXIT_THREAD_DEBUG_EVENT = 4
EXIT_PROCESS_DEBUG_EVENT = 5
LOAD_DLL_DEBUG_EVENT = 6
UNLOAD_DLL_DEBUG_EVENT = 7
OUTPUT_DEBUG_STRING_EVENT = 8
RIP_EVENT = 9

CONTEXT_ARM64_FULL = 0x00400007
THREAD_GET_CONTEXT = 0x0008
THREAD_QUERY_INFORMATION = 0x0040


class STARTUPINFOA(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPSTR), ("lpDesktop", wt.LPSTR),
                ("lpTitle", wt.LPSTR), ("dwX", wt.DWORD), ("dwY", wt.DWORD),
                ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD), ("dwXCountChars", wt.DWORD),
                ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD), ("dwFlags", wt.DWORD),
                ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD), ("lpReserved2", ctypes.c_void_p),
                ("hStdInput", wt.HANDLE), ("hStdOutput", wt.HANDLE), ("hStdError", wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
                ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


# ARM64 CONTEXT 布局（只取要用的字段；X[0] 在 8，Fp=240，Lr=248，Sp=256，Pc=264）
class CTX(ctypes.Structure):
    _fields_ = [("ContextFlags", wt.DWORD), ("Cpsr", wt.DWORD), ("X", ctypes.c_uint64 * 29),
                ("Fp", ctypes.c_uint64), ("Lr", ctypes.c_uint64), ("Sp", ctypes.c_uint64),
                ("Pc", ctypes.c_uint64), ("pad", ctypes.c_uint64 * 64)]


def mod_of(addr):
    """地址 → 模块名+偏移 | address -> module name + offset"""
    h = wt.HMODULE()
    need = ctypes.c_ulong()
    if not k32.GetModuleHandleExW(0x4, ctypes.c_wchar_p(None) if False else None, ctypes.byref(h)):
        pass
    # GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 0x4
    if not k32.GetModuleHandleExW(0x4 | 0x2, ctypes.c_void_p(addr), ctypes.byref(h)):
        return "?"
    buf = ctypes.create_string_buffer(1024)
    k32.GetModuleFileNameA(h, buf, 1024)
    name = buf.value.decode("latin1").split("\\")[-1]
    base = k32.GetModuleHandleA(ctypes.c_char_p(buf.value.split(b"\\")[-1]))
    # 基址取该模块的加载地址 | base address of that module
    k32.GetModuleHandleExW(0x4 | 0x2, ctypes.c_void_p(addr), ctypes.byref(h))
    return "%s+%#x" % (name, addr - ctypes.cast(h, ctypes.c_void_p).value)


MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_FREE = 0x10000
MEM_PRIVATE = 0x20000
MEM_MAPPED = 0x40000
MEM_IMAGE = 0x1000000


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_uint64), ("AllocationBase", ctypes.c_uint64),
                ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD), ("pad0", wt.WORD),
                ("RegionSize", ctypes.c_uint64), ("State", wt.DWORD), ("Protect", wt.DWORD),
                ("Type", wt.DWORD), ("pad1", wt.DWORD)]


def vquery(hp, addr):
    """VirtualQueryEx：地址所在区间 → 基址/大小/状态/类型/保护/分配基址"""
    mbi = MBI()
    if not k32.VirtualQueryEx(hp, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        return "VQ失败"
    tname = {MEM_PRIVATE: "private", MEM_MAPPED: "mapped", MEM_IMAGE: "image", 0: "-"}.get(mbi.Type, hex(mbi.Type))
    sname = {MEM_COMMIT: "commit", MEM_RESERVE: "reserve", MEM_FREE: "free"}.get(mbi.State, hex(mbi.State))
    return ("region=%#x+%#x state=%s type=%s prot=%#x alloc=%#x"
            % (mbi.BaseAddress, mbi.RegionSize, sname, tname, mbi.Protect, mbi.AllocationBase))


def teb_of(tid):
    """线程 → TEB 地址（NtQueryInformationThread 0 类）"""
    ntdll = ctypes.WinDLL("ntdll")
    h = k32.OpenThread(0x0040 | 0x0800, False, tid)   # QUERY_INFORMATION | (0x0800 = THREAD_QUERY_LIMITED_INFORMATION? 这里用 0x0040)
    if not h:
        return None
    buf = ctypes.create_string_buffer(48)
    st = ntdll.NtQueryInformationThread(ctypes.c_void_p(h), 0, buf, 48, None)
    k32.CloseHandle(h)
    if st != 0:
        return None
    return ctypes.c_uint64.from_buffer(buf, 8).value


def main():
    exe = os.path.abspath(sys.argv[1])
    cmd = " ".join(['"%s"' % exe] + sys.argv[2:])
    si = STARTUPINFOA(); si.cb = ctypes.sizeof(si)
    pi = PROCESS_INFORMATION()
    if not k32.CreateProcessA(exe.encode(), cmd.encode(), None, None, True,
                              DEBUG_ONLY_THIS_PROCESS, None, None, ctypes.byref(si), ctypes.byref(pi)):
        print("CreateProcess failed: %d" % ctypes.get_last_error()); return 1
    print("pid=%d" % pi.dwProcessId, flush=True)
    k32.DebugSetProcessKillOnExit(False)
    de = ctypes.create_string_buffer(512)
    n = 0
    while True:
        if not k32.WaitForDebugEvent(de, 0xFFFFFFFF):
            break
        code = ctypes.c_uint32.from_buffer(de, 0).value
        tid = ctypes.c_uint32.from_buffer(de, 8).value
        if code == EXCEPTION_DEBUG_EVENT:
            er = 16
            ecode = ctypes.c_uint32.from_buffer(de, er).value
            eflags = ctypes.c_uint32.from_buffer(de, er + 4).value
            eaddr = ctypes.c_uint64.from_buffer(de, er + 16).value
            nparam = ctypes.c_uint32.from_buffer(de, er + 24).value
            args = [ctypes.c_uint64.from_buffer(de, er + 32 + 8 * i).value for i in range(min(nparam, 4))]
            first = ctypes.c_uint32.from_buffer(de, er + 152).value
            h = k32.OpenThread(0x0002 | 0x0008 | 0x0040 | 0x0010, False, tid)
            buf = ctypes.create_string_buffer(4096)
            ctypes.c_uint32.from_buffer(buf, 0).value = 0x00400007
            ok = 0
            if h:
                ok = k32.GetThreadContext(h, buf)
                k32.CloseHandle(h)
            ctx = CTX()
            if ok:
                # 直接用原始缓冲区补齐 CTX（X 在 +8，Sp 在 +256，Pc 在 +264）
                ctypes.memmove(ctypes.byref(ctx), buf, 40)
                for i in range(29):
                    ctx.X[i] = ctypes.c_uint64.from_buffer(buf, 8 + 8 * i).value
                ctx.Fp = ctypes.c_uint64.from_buffer(buf, 240).value
                ctx.Lr = ctypes.c_uint64.from_buffer(buf, 248).value
                ctx.Sp = ctypes.c_uint64.from_buffer(buf, 256).value
                ctx.Pc = ctypes.c_uint64.from_buffer(buf, 264).value
            print("[exc] tid=%d code=%08x flags=%x addr=%#x (%s) first=%d n=%d args=%s"
                  % (tid, ecode, eflags, eaddr, mod_of(eaddr), first, nparam,
                     " ".join("%#x" % a for a in args)), flush=True)
            print("      pc=%s lr=%s sp=%#x fp=%#x x0=%#x x1=%#x x2=%#x x3=%#x ctxok=%d err=%d"
                  % (mod_of(ctx.Pc), mod_of(ctx.Lr), ctx.Sp, ctx.Fp,
                     ctx.X[0], ctx.X[1], ctx.X[2], ctx.X[3], ok, ctypes.get_last_error()), flush=True)
            hp = ctypes.c_void_p(pi.hProcess)
            print("      fault %s" % vquery(hp, eaddr), flush=True)
            tb = teb_of(tid)
            if tb:
                sb = ctypes.c_uint64(); sl = ctypes.c_uint64(); got = ctypes.c_size_t()
                k32.ReadProcessMemory(hp, ctypes.c_void_p(tb + 8), ctypes.byref(sb), 8, ctypes.byref(got))
                k32.ReadProcessMemory(hp, ctypes.c_void_p(tb + 0x10), ctypes.byref(sl), 8, ctypes.byref(got))
                print("      teb=%#x base=%#x limit=%#x（栈内=%d）" % (tb, sb.value, sl.value,
                      int(sl.value <= eaddr < sb.value)), flush=True)
            for a in args[1:2]:
                if a:
                    print("      写目标 %s" % vquery(hp, a), flush=True)
                    print("      写目标附近:")
                    q = a & ~0xfff
                    for _ in range(5):
                        q -= 0x1000
                    mbi = MBI()
                    if k32.VirtualQueryEx(hp, ctypes.c_void_p(q), ctypes.byref(mbi), ctypes.sizeof(mbi)):
                        for _ in range(12):
                            print("        %#x+%#x st=%s ty=%s prot=%#x alloc=%#x"
                                  % (mbi.BaseAddress, mbi.RegionSize, hex(mbi.State), hex(mbi.Type), mbi.Protect, mbi.AllocationBase), flush=True)
                            q = mbi.BaseAddress + mbi.RegionSize
                            if not k32.VirtualQueryEx(hp, ctypes.c_void_p(q), ctypes.byref(mbi), ctypes.sizeof(mbi)):
                                break
            try:
                buf = ctypes.create_string_buffer(64)
                got = ctypes.c_size_t()
                if k32.ReadProcessMemory(hp, ctypes.c_void_p(ctx.Sp), buf, 64, ctypes.byref(got)) and got.value:
                    print("      sp内存: " + " ".join("%016x" % ctypes.c_uint64.from_buffer(buf, i * 8).value for i in range(min(8, got.value // 8))), flush=True)
            except Exception as e:
                print("      sp读取失败 %s" % e, flush=True)
            n += 1
            k32.ContinueDebugEvent(ctypes.c_uint32.from_buffer(de, 4).value, tid, DBG_EXCEPTION_NOT_HANDLED)
        elif code == EXIT_PROCESS_DEBUG_EVENT:
            print("[exit] code=%#x" % ctypes.c_uint32.from_buffer(de, 16 + 4).value, flush=True)
            k32.ContinueDebugEvent(ctypes.c_uint32.from_buffer(de, 4).value, tid, DBG_CONTINUE)
            break
        else:
            if code == LOAD_DLL_DEBUG_EVENT:
                buf = wt.DWORD(0)
                k32.GetFinalPathNameByHandleA(ctypes.c_void_p(ctypes.c_uint64.from_buffer(de, 16).value), None, 0, 0)
            k32.ContinueDebugEvent(ctypes.c_uint32.from_buffer(de, 4).value, tid, DBG_CONTINUE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
