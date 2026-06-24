"""링버퍼 실시간 모니터 — 변경된 슬롯만 출력"""
import ctypes, ctypes.wintypes, time, sys
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
if not pid:
    print('SC 없음'); sys.exit(1)
print(f'SC PID: {pid}  모니터링 시작...')

RING_SLOT0 = 0x7FF63A2DE0CB
SLOT_SIZE  = 0xDA
SLOT_COUNT = 10
hp = kernel32.OpenProcess(0x0010|0x0400, False, pid)

def read_mem(addr, size):
    buf = (ctypes.c_ubyte*size)(); r = ctypes.c_size_t(0)
    kernel32.ReadProcessMemory(hp, ctypes.c_void_p(addr), buf, size, ctypes.byref(r))
    return bytes(buf[:r.value])

def slot_text(d):
    end = d.find(b'\x00')
    content = d[:end] if end != -1 else d
    return ''.join(chr(b) if 32<=b<127 else ('?' if b<0x80 else '\u25a0') for b in content)

def read_all():
    return [read_mem(RING_SLOT0 + i*SLOT_SIZE, SLOT_SIZE) for i in range(SLOT_COUNT)]

prev = read_all()
print('초기 상태:')
for i, d in enumerate(prev):
    t = slot_text(d)
    if t: print(f'  [{i}] {t!r}')
print('\n--- 변경 감지 중 (Ctrl+C로 종료) ---\n')

while True:
    time.sleep(0.5)
    cur = read_all()
    for i in range(SLOT_COUNT):
        if cur[i] != prev[i]:
            old = slot_text(prev[i])
            new = slot_text(cur[i])
            ts = time.strftime('%H:%M:%S')
            print(f'[{ts}] 슬롯[{i}] 변경:')
            if old: print(f'  이전: {old!r}')
            print(f'  이후: {new!r}')
            # 핵심 바이트 hex
            end = cur[i].find(b'\x00')
            content = cur[i][:end] if end != -1 else cur[i]
            if content:
                print(f'  hex:  {content.hex()}')
            print()
    prev = cur
