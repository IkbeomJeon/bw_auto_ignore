"""MAPPED 채팅 버퍼에서 모든 메시지 항목 추출 및 순서 분석"""
import ctypes, ctypes.wintypes, sys, re
sys.stdout.reconfigure(encoding='utf-8')

PROCESS_VM_READ           = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS        = 0x00000002
kernel32 = ctypes.windll.kernel32

MAPPED_BASE = 0x00007FF63A2DE000  # 이전 분석에서 확인된 리전 베이스

class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [('dwSize',ctypes.wintypes.DWORD),('cntUsage',ctypes.wintypes.DWORD),
                ('th32ProcessID',ctypes.wintypes.DWORD),('th32DefaultHeapID',ctypes.POINTER(ctypes.c_ulong)),
                ('th32ModuleID',ctypes.wintypes.DWORD),('cntThreads',ctypes.wintypes.DWORD),
                ('th32ParentProcessID',ctypes.wintypes.DWORD),('pcPriClassBase',ctypes.c_long),
                ('dwFlags',ctypes.wintypes.DWORD),('szExeFile',ctypes.c_char*260)]

class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [('BaseAddress',ctypes.c_void_p),('AllocationBase',ctypes.c_void_p),
                ('AllocationProtect',ctypes.wintypes.DWORD),('PartitionId',ctypes.wintypes.WORD),
                ('RegionSize',ctypes.c_size_t),('State',ctypes.wintypes.DWORD),
                ('Protect',ctypes.wintypes.DWORD),('Type',ctypes.wintypes.DWORD)]

def find_sc_pid():
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32(); pe.dwSize = ctypes.sizeof(pe)
    if kernel32.Process32First(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.lower() == b'starcraft.exe':
                pid = pe.th32ProcessID; kernel32.CloseHandle(snap); return pid
            if not kernel32.Process32Next(snap, ctypes.byref(pe)): break
    kernel32.CloseHandle(snap); return None

def read_mem(hp, addr, size):
    buf = (ctypes.c_ubyte * size)()
    r = ctypes.c_size_t(0)
    kernel32.ReadProcessMemory(hp, ctypes.c_void_p(addr), buf, size, ctypes.byref(r))
    return bytes(buf[:r.value])

pid = find_sc_pid()
if not pid: print('SC 없음'); exit()
hp = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)

# 리전 크기 확인
mbi = MEMORY_BASIC_INFORMATION()
kernel32.VirtualQueryEx(hp, ctypes.c_void_p(MAPPED_BASE), ctypes.byref(mbi), ctypes.sizeof(mbi))
region_size = mbi.RegionSize
read_size = min(region_size, 0x10000)  # 처음 64KB만
print(f'리전 크기: {region_size//1024}KB  읽기: {read_size//1024}KB')

data = read_mem(hp, MAPPED_BASE, read_size)
kernel32.CloseHandle(hp)

# null-terminated 문자열 추출 (printable ASCII만)
print(f'\n=== 모든 문자열 항목 (주소순) ===')
entries = []
i = 0
while i < len(data):
    if 32 <= data[i] < 127:
        j = i
        while j < len(data) and 32 <= data[j] < 127:
            j += 1
        s = data[i:j].decode('ascii', errors='replace')
        if len(s) >= 4:  # 너무 짧은 건 제외
            entries.append((MAPPED_BASE + i, s))
        i = j
    else:
        i += 1

for addr, s in entries:
    rva = addr - 0x7FF639270000
    print(f'  0x{addr:016X} (RVA=0x{rva:X}): {s!r}')

# > 패턴 (귓말) 찾기
print(f'\n=== 귓말 패턴 "sender> msg" (주소순) ===')
whispers = [(addr, s) for addr, s in entries if '> ' in s]
for i, (addr, s) in enumerate(whispers):
    rva = addr - 0x7FF639270000
    print(f'  [{i}] 0x{addr:016X} (RVA=0x{rva:X}): {s!r}')

if len(whispers) >= 2:
    print(f'\n=== 슬롯 간격 분석 ===')
    for i in range(1, len(whispers)):
        gap = whispers[i][0] - whispers[i-1][0]
        print(f'  [{i-1}]→[{i}] 간격: 0x{gap:X} ({gap}바이트)')
