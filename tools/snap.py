"""사용법: snap.py save <라벨>  |  snap.py compare <라벨A> <라벨B>"""
import ctypes, ctypes.wintypes, sys, os, json

PROCESS_VM_READ           = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS        = 0x00000002
TH32CS_SNAPMODULE         = 0x00000008
TH32CS_SNAPMODULE32       = 0x00000010
kernel32 = ctypes.windll.kernel32

class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [("dwSize",ctypes.wintypes.DWORD),("cntUsage",ctypes.wintypes.DWORD),
                ("th32ProcessID",ctypes.wintypes.DWORD),("th32DefaultHeapID",ctypes.POINTER(ctypes.c_ulong)),
                ("th32ModuleID",ctypes.wintypes.DWORD),("cntThreads",ctypes.wintypes.DWORD),
                ("th32ParentProcessID",ctypes.wintypes.DWORD),("pcPriClassBase",ctypes.c_long),
                ("dwFlags",ctypes.wintypes.DWORD),("szExeFile",ctypes.c_char*260)]

class MODULEENTRY32(ctypes.Structure):
    _fields_ = [("dwSize",ctypes.wintypes.DWORD),("th32ModuleID",ctypes.wintypes.DWORD),
                ("th32ProcessID",ctypes.wintypes.DWORD),("GlblcntUsage",ctypes.wintypes.DWORD),
                ("ProccntUsage",ctypes.wintypes.DWORD),("modBaseAddr",ctypes.POINTER(ctypes.c_byte)),
                ("modBaseSize",ctypes.wintypes.DWORD),("hModule",ctypes.wintypes.HMODULE),
                ("szModule",ctypes.c_char*256),("szExePath",ctypes.c_char*260)]

def find_sc():
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32(); pe.dwSize = ctypes.sizeof(pe)
    if kernel32.Process32First(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.lower() == b"starcraft.exe":
                pid = pe.th32ProcessID; kernel32.CloseHandle(snap); return pid
            if not kernel32.Process32Next(snap, ctypes.byref(pe)): break
    kernel32.CloseHandle(snap); return None

def get_base(pid):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32, pid)
    me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(me)
    if kernel32.Module32First(snap, ctypes.byref(me)):
        while True:
            if me.szModule.lower() == b"starcraft.exe":
                base = ctypes.addressof(me.modBaseAddr.contents); kernel32.CloseHandle(snap); return base
            if not kernel32.Module32Next(snap, ctypes.byref(me)): break
    kernel32.CloseHandle(snap); return None

def read_mem(hp, addr, size):
    buf = (ctypes.c_ubyte * size)()
    r = ctypes.c_size_t(0)
    kernel32.ReadProcessMemory(hp, ctypes.c_void_p(addr), buf, size, ctypes.byref(r))
    return list(buf[:r.value])

SCAN_START = 0x1080000
SCAN_SIZE  = 0x20000
IS_IN_GAME = 0x1090612
SNAP_DIR   = os.path.join(os.path.dirname(__file__), "snaps")

def cmd_save(label):
    os.makedirs(SNAP_DIR, exist_ok=True)
    pid = find_sc()
    if not pid: print("SC 못 찾음"); return
    hp   = kernel32.OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_INFORMATION, False, pid)
    base = get_base(pid)
    data = read_mem(hp, base + SCAN_START, SCAN_SIZE)
    kernel32.CloseHandle(hp)
    path = os.path.join(SNAP_DIR, f"{label}.json")
    json.dump({"base": base, "data": data}, open(path, "w"))
    print(f"저장 완료: {path}  (base=0x{base:X})")

def cmd_compare(la, lb):
    pa = os.path.join(SNAP_DIR, f"{la}.json")
    pb = os.path.join(SNAP_DIR, f"{lb}.json")
    a = json.load(open(pa)); b = json.load(open(pb))
    da, db = a["data"], b["data"]
    diffs = [(i, da[i], db[i]) for i in range(min(len(da),len(db))) if da[i] != db[i]]
    print(f"[{la}] vs [{lb}]  변화: {len(diffs)}개\n")
    for i, va, vb in diffs:
        off = SCAN_START + i
        m = " ★IS_IN_GAME" if off == IS_IN_GAME else ""
        print(f"  0x{off:07X}  {va:3d}→{vb:3d}  (0x{va:02X}→0x{vb:02X}){m}")

if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "save":
        cmd_save(sys.argv[2])
    elif len(sys.argv) >= 4 and sys.argv[1] == "compare":
        cmd_compare(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
