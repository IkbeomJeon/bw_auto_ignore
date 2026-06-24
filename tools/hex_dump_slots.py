import ctypes, ctypes.wintypes
import sys
sys.stdout.reconfigure(encoding='utf-8')
kernel32 = ctypes.windll.kernel32
TH32CS_SNAPPROCESS = 0x2

class PE32(ctypes.Structure):
    _fields_ = [('dwSize',ctypes.wintypes.DWORD),('cntUsage',ctypes.wintypes.DWORD),
                ('th32ProcessID',ctypes.wintypes.DWORD),('th32DefaultHeapID',ctypes.POINTER(ctypes.c_ulong)),
                ('th32ModuleID',ctypes.wintypes.DWORD),('cntThreads',ctypes.wintypes.DWORD),
                ('th32ParentProcessID',ctypes.wintypes.DWORD),('pcPriClassBase',ctypes.c_long),
                ('dwFlags',ctypes.wintypes.DWORD),('szExeFile',ctypes.c_char*260)]

snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
pe = PE32(); pe.dwSize = ctypes.sizeof(pe)
pid = None
if kernel32.Process32First(snap, ctypes.byref(pe)):
    while True:
        if pe.szExeFile.lower() == b'starcraft.exe':
            pid = pe.th32ProcessID; break
        if not kernel32.Process32Next(snap, ctypes.byref(pe)): break
kernel32.CloseHandle(snap)
print(f'SC PID: {pid}')

RING_SLOT0 = 0x7FF63A2DE0CB
SLOT_SIZE  = 0xDA
SLOT_COUNT = 10
hp = kernel32.OpenProcess(0x0010|0x0400, False, pid)

def read_mem(addr, size):
    buf = (ctypes.c_ubyte*size)(); r = ctypes.c_size_t(0)
    kernel32.ReadProcessMemory(hp, ctypes.c_void_p(addr), buf, size, ctypes.byref(r))
    return bytes(buf[:r.value])

def printable(b):
    if 32 <= b < 127:
        return chr(b)
    return '\\x{:02x}'.format(b)

for i in range(SLOT_COUNT):
    addr = RING_SLOT0 + i * SLOT_SIZE
    d = read_mem(addr, SLOT_SIZE)
    end = d.find(b'\x00')
    content = d[:end] if end != -1 else d
    if not content:
        print(f'[{i}] (empty)')
        continue
    txt = ''.join(printable(b) for b in content)
    print(f'[{i}] hex({len(content)}B): {content.hex()}')
    print(f'[{i}] txt: {txt}')
    # > 위치 찾기
    gt_pos = content.find(b'> ')
    if gt_pos != -1:
        print(f'[{i}] "> " 위치: {gt_pos}, 앞 바이트: {content[:gt_pos].hex()}')
    print()

kernel32.CloseHandle(hp)
