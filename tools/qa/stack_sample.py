"""Sample the main thread's call stack of a running x64 process through dbghelp.

  python stack_sample.py <pid> <symbol dir> [samples] [interval_s] [depth]

For a rig engine that looks stuck when the in-engine profiler cannot be stopped (dar_profile_stop
runs on the game thread): the symbol dir is the rig root, with the PDBs of the deployed build next
to the binaries. Suspends the thread only for the walk; reads the target through
ReadProcessMemory, so nothing in the target is called and no lock of it can be held against us.
"""
import ctypes, ctypes.wintypes as wt, sys, time, collections

pid = int(sys.argv[1]); sympath = sys.argv[2]
samples = int(sys.argv[3]) if len(sys.argv) > 3 else 8
interval = float(sys.argv[4]) if len(sys.argv) > 4 else 0.4
depth = int(sys.argv[5]) if len(sys.argv) > 5 else 40

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
dbg = ctypes.WinDLL('dbghelp', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)

PROCESS_ALL_ACCESS = 0x1F0FFF
THREAD_ALL_ACCESS = 0x1F03FF
TH32CS_SNAPTHREAD = 0x4

class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ThreadID', wt.DWORD),
                ('th32OwnerProcessID', wt.DWORD), ('tpBasePri', wt.LONG), ('tpDeltaPri', wt.LONG),
                ('dwFlags', wt.DWORD)]

class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', ctypes.c_char * 512)]

k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
dbg.SymFunctionTableAccess64.restype = ctypes.c_void_p
dbg.SymFunctionTableAccess64.argtypes = [wt.HANDLE, ctypes.c_uint64]
dbg.SymGetModuleBase64.restype = ctypes.c_uint64
dbg.SymGetModuleBase64.argtypes = [wt.HANDLE, ctypes.c_uint64]
FTA = ctypes.WINFUNCTYPE(ctypes.c_void_p, wt.HANDLE, ctypes.c_uint64)
GMB = ctypes.WINFUNCTYPE(ctypes.c_uint64, wt.HANDLE, ctypes.c_uint64)
fta = FTA(lambda h, a: dbg.SymFunctionTableAccess64(h, a))
gmb = GMB(lambda h, a: dbg.SymGetModuleBase64(h, a))
dbg.StackWalk64.argtypes = [wt.DWORD, wt.HANDLE, wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                            ctypes.c_void_p, FTA, GMB, ctypes.c_void_p]

hp = k32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
if not hp:
    sys.exit(f"OpenProcess failed {ctypes.get_last_error()}")
dbg.SymSetOptions(0x2 | 0x4 | 0x10)  # UNDNAME | DEFERRED_LOADS | LOAD_LINES
if not dbg.SymInitializeW(hp, ctypes.c_wchar_p(sympath), True):
    sys.exit(f"SymInitialize failed {ctypes.get_last_error()}")

# The main thread is the oldest one of the process.
snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
te = THREADENTRY32(); te.dwSize = ctypes.sizeof(te)
tids = []
ok = k32.Thread32First(snap, ctypes.byref(te))
while ok:
    if te.th32OwnerProcessID == pid:
        tids.append(te.th32ThreadID)
    ok = k32.Thread32Next(snap, ctypes.byref(te))
k32.CloseHandle(snap)
FT = wt.FILETIME
best = None
for tid in tids:
    ht = k32.OpenThread(THREAD_ALL_ACCESS, False, tid)
    if not ht:
        continue
    c, e, kt, ut = FT(), FT(), FT(), FT()
    k32.GetThreadTimes(ht, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
    created = (c.dwHighDateTime << 32) | c.dwLowDateTime
    user = (ut.dwHighDateTime << 32) | ut.dwLowDateTime
    if best is None or created < best[1]:
        best = (tid, created, user)
    k32.CloseHandle(ht)
main_tid = best[0]
print(f"main thread {main_tid}, user time {best[2] / 1e7:.1f} s, {len(tids)} threads")

modname_cache = {}
def modname(base):
    if base not in modname_cache:
        buf = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(hp, ctypes.c_void_p(base), buf, 260)
        modname_cache[base] = buf.value or hex(base)
    return modname_cache[base]

def name(addr):
    si = SYMBOL_INFO(); si.SizeOfStruct = 88; si.MaxNameLen = 500
    disp = ctypes.c_uint64()
    base = dbg.SymGetModuleBase64(hp, addr)
    m = modname(base) if base else '?'
    if dbg.SymFromAddr(hp, ctypes.c_uint64(addr), ctypes.byref(disp), ctypes.byref(si)):
        return f"{m}!{si.Name.decode(errors='replace')}+0x{disp.value:x}"
    return f"{m}+0x{addr - base:x}" if base else hex(addr)

ht = k32.OpenThread(THREAD_ALL_ACCESS, False, main_tid)
ctx_raw = ctypes.create_string_buffer(1232 + 16)
ctx_addr = (ctypes.addressof(ctx_raw) + 15) & ~15
top = collections.Counter()
for s in range(samples):
    if k32.SuspendThread(ht) == 0xFFFFFFFF:
        sys.exit("SuspendThread failed")
    try:
        ctypes.memset(ctx_addr, 0, 1232)
        ctypes.c_uint32.from_address(ctx_addr + 0x30).value = 0x0010000B  # CONTEXT_FULL (AMD64)
        if not k32.GetThreadContext(ht, ctypes.c_void_p(ctx_addr)):
            print("GetThreadContext failed", ctypes.get_last_error()); continue
        rip = ctypes.c_uint64.from_address(ctx_addr + 0xF8).value
        rsp = ctypes.c_uint64.from_address(ctx_addr + 0x98).value
        rbp = ctypes.c_uint64.from_address(ctx_addr + 0xA0).value
        frame = ctypes.create_string_buffer(0x400)
        fa = ctypes.addressof(frame)
        for off, val in ((0, rip), (32, rbp), (48, rsp)):
            ctypes.c_uint64.from_address(fa + off).value = val
            ctypes.c_uint32.from_address(fa + off + 12).value = 3  # AddrModeFlat
        pcs = []
        for _ in range(depth):
            if not dbg.StackWalk64(0x8664, hp, ht, ctypes.c_void_p(fa), ctypes.c_void_p(ctx_addr), None, fta, gmb, None):
                break
            pc = ctypes.c_uint64.from_address(fa).value
            if not pc:
                break
            pcs.append(pc)
    finally:
        k32.ResumeThread(ht)
    names = [name(pc) for pc in pcs]
    print(f"--- sample {s}")
    for n in names:
        print("   ", n)
    if names:
        top[names[0].split('+0x')[0]] += 1
    time.sleep(interval)
print("--- leaf functions:", top.most_common(10))
