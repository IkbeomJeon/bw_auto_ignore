"""MAPPED 주소 주변 상세 분석"""
import ctypes, ctypes.wintypes, sys, struct
sys.stdout.reconfigure(encoding='utf-8')

PROCESS_VM_READ           = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS        = 0x00000002
MEM_COMMIT = 0x1000
kernel32 = ctypes.windll.kernel32

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

def hex_dump(data, base_addr, width=32):
    lines = []
    for i in range(0, len(data), width):
        chunk = data[i:i+width]
        hex_part  = ' '.join(f'{b:02X}' for b in chunk)
        ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        lines.append(f'  {base_addr+i:016X}  {hex_part:<{width*3}}  {ascii_part}')
    return '\n'.join(lines)

TARGET = 0x00007FF63A2DE288

pid = find_sc_pid()
if not pid: print('SC 없음'); exit()
hp = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)

# 이 주소가 속한 리전 전체 정보 확인
mbi = MEMORY_BASIC_INFORMATION()
kernel32.VirtualQueryEx(hp, ctypes.c_void_p(TARGET), ctypes.byref(mbi), ctypes.sizeof(mbi))
region_base = mbi.BaseAddress or 0
region_size = mbi.RegionSize
alloc_base  = mbi.AllocationBase or 0
print(f'리전 정보:')
print(f'  BaseAddress:      0x{region_base:016X}')
print(f'  AllocationBase:   0x{alloc_base:016X}')
print(f'  RegionSize:       0x{region_size:X} ({region_size//1024}KB)')
print(f'  Type:             0x{mbi.Type:X}')
print(f'  Protect:          0x{mbi.Protect:X}')
print(f'  TARGET offset in region: 0x{TARGET - region_base:X}')

# 리전 전체 읽기 (너무 크면 제한)
read_size = min(region_size, 0x100000)  # 최대 1MB
print(f'\n리전 데이터 읽기: {read_size//1024}KB...')
region_data = read_mem(hp, region_base, read_size)
print(f'실제 읽음: {len(region_data)} bytes')

# TARGET 주변 ±2KB 헥스 덤프
offset = TARGET - region_base
ctx_start = max(0, offset - 2048)
ctx_end   = min(len(region_data), offset + 2048)
ctx = region_data[ctx_start:ctx_end]
print(f'\n=== TARGET(0x{TARGET:X}) 주변 ±2KB 덤프 ===')
print(hex_dump(ctx, region_base + ctx_start))

# ASCII 텍스트 추출 (가독성)
print(f'\n=== 텍스트만 추출 (non-printable → 공백) ===')
text = ''
for b in ctx:
    if 32 <= b < 127:
        text += chr(b)
    elif b == 0x0A or b == 0x0D:
        text += '\n'
    else:
        text += ' '
# 연속 공백 압축
import re
lines = [l.strip() for l in text.split('\n') if l.strip()]
for line in lines:
    line = re.sub(r' {3,}', '   ', line)
    if line:
        print(f'  {line}')

# 'bwtest2>' 패턴 검색으로 다른 메시지 찾기
print(f'\n=== 채팅 패턴 검색 (">" 포함 항목) ===')
patterns = [b'bwtest2>', b'ghfhxhtm>', b'> ']
for pat in patterns:
    pos = 0
    while True:
        idx = region_data.find(pat, pos)
        if idx == -1: break
        ctx2 = region_data[idx:idx+120]
        text2 = ''.join(chr(b) if 32<=b<127 else '.' for b in ctx2)
        print(f'  0x{region_base+idx:016X}: {text2[:80]}')
        pos = idx + 1

kernel32.CloseHandle(hp)
