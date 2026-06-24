"""SC.exe 모듈 범위 내 모든 메모리 리전 타입 출력"""
import ctypes, ctypes.wintypes, sys
sys.stdout.reconfigure(encoding='utf-8')

PROCESS_VM_READ           = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS        = 0x00000002
TH32CS_SNAPMODULE         = 0x00000008
TH32CS_SNAPMODULE32       = 0x00000010
MEM_COMMIT  = 0x1000
MEM_FREE    = 0x10000
MEM_RESERVE = 0x2000
kernel32 = ctypes.windll.kernel32

class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [('dwSize',ctypes.wintypes.DWORD),('cntUsage',ctypes.wintypes.DWORD),
                ('th32ProcessID',ctypes.wintypes.DWORD),('th32DefaultHeapID',ctypes.POINTER(ctypes.c_ulong)),
                ('th32ModuleID',ctypes.wintypes.DWORD),('cntThreads',ctypes.wintypes.DWORD),
                ('th32ParentProcessID',ctypes.wintypes.DWORD),('pcPriClassBase',ctypes.c_long),
                ('dwFlags',ctypes.wintypes.DWORD),('szExeFile',ctypes.c_char*260)]

class MODULEENTRY32(ctypes.Structure):
    _fields_ = [('dwSize',ctypes.wintypes.DWORD),('th32ModuleID',ctypes.wintypes.DWORD),
                ('th32ProcessID',ctypes.wintypes.DWORD),('GlblcntUsage',ctypes.wintypes.DWORD),
                ('ProccntUsage',ctypes.wintypes.DWORD),('modBaseAddr',ctypes.POINTER(ctypes.c_byte)),
                ('modBaseSize',ctypes.wintypes.DWORD),('hModule',ctypes.wintypes.HMODULE),
                ('szModule',ctypes.c_char*256),('szExePath',ctypes.c_char*260)]

class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [('BaseAddress',ctypes.c_void_p),('AllocationBase',ctypes.c_void_p),
                ('AllocationProtect',ctypes.wintypes.DWORD),('PartitionId',ctypes.wintypes.WORD),
                ('RegionSize',ctypes.c_size_t),('State',ctypes.wintypes.DWORD),
                ('Protect',ctypes.wintypes.DWORD),('Type',ctypes.wintypes.DWORD)]

STATE_NAMES = {0x1000: 'COMMIT', 0x2000: 'RESERVE', 0x10000: 'FREE'}
TYPE_NAMES  = {0x1000000: 'IMAGE', 0x40000: 'MAPPED', 0x20000: 'PRIVATE', 0: '---'}
PROT_NAMES  = {0x01:'NOACCESS', 0x02:'R', 0x04:'RW', 0x08:'WC', 0x10:'X',
               0x20:'XR', 0x40:'XRW', 0x80:'XWC', 0x100:'GUARD'}

def find_sc_pid():
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32(); pe.dwSize = ctypes.sizeof(pe)
    if kernel32.Process32First(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.lower() == b'starcraft.exe':
                pid = pe.th32ProcessID; kernel32.CloseHandle(snap); return pid
            if not kernel32.Process32Next(snap, ctypes.byref(pe)): break
    kernel32.CloseHandle(snap); return None

def get_module_base(pid):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(me)
    if kernel32.Module32First(snap, ctypes.byref(me)):
        while True:
            if me.szModule.lower() == b'starcraft.exe':
                base = ctypes.cast(me.modBaseAddr, ctypes.c_void_p).value
                size = me.modBaseSize
                kernel32.CloseHandle(snap); return base, size
            if not kernel32.Module32Next(snap, ctypes.byref(me)): break
    kernel32.CloseHandle(snap); return None, None

pid = find_sc_pid()
if not pid: print('SC 없음'); sys.exit(1)
mod_base, mod_size = get_module_base(pid)
print(f'SC PID={pid}  모듈베이스=0x{mod_base:X}  크기=0x{mod_size:X} ({mod_size//1024}KB)')
print(f'범위: 0x{mod_base:X} ~ 0x{mod_base+mod_size:X}')

hp = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
mbi = MEMORY_BASIC_INFORMATION()
sz  = ctypes.sizeof(mbi)

# 모듈 범위만
addr = mod_base
end  = mod_base + mod_size
total_commit = 0

print(f'\n{"주소":20s} {"크기KB":8s} {"상태":8s} {"타입":8s} {"보호":10s}')
print('-' * 60)
while addr < end:
    r = kernel32.VirtualQueryEx(hp, ctypes.c_void_p(addr), ctypes.byref(mbi), sz)
    if r != sz: break
    base = mbi.BaseAddress or 0
    size = mbi.RegionSize
    state = STATE_NAMES.get(mbi.State, f'{mbi.State:#x}')
    typ   = TYPE_NAMES.get(mbi.Type,  f'{mbi.Type:#x}')
    prot  = PROT_NAMES.get(mbi.Protect, f'{mbi.Protect:#x}')
    if mbi.State == MEM_COMMIT:
        total_commit += size
        print(f'0x{base:016X}  {size//1024:6d}KB  {state:8s} {typ:8s} {prot}')
    next_addr = base + size
    if next_addr <= addr: break
    addr = next_addr

print(f'\n총 COMMIT: {total_commit//1024}KB')
kernel32.CloseHandle(hp)
