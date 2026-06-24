"""
SC:R 귓말 메모리 구조 분석기
1. SC 창에 /w dkanskskdhk1 bwtest123 자동 전송
2. 즉시 메모리 스캔하여 bwtest123 위치 및 주변 구조 분석
"""
import ctypes, ctypes.wintypes, sys, time
sys.stdout.reconfigure(encoding='utf-8')

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS = 0x00000002
MEM_COMMIT = 0x1000
kernel32 = ctypes.windll.kernel32
user32   = ctypes.windll.user32

# ── 프로세스/윈도우 탐색 ──────────────────────────────
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

def find_sc_hwnd(pid):
    result = []
    def cb(hwnd, _):
        p = ctypes.wintypes.DWORD(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            result.append(hwnd)
        return True
    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return result[0] if result else None

def read_mem(hp, addr, size):
    buf = (ctypes.c_ubyte * size)()
    r = ctypes.c_size_t(0)
    kernel32.ReadProcessMemory(hp, ctypes.c_void_p(addr), buf, size, ctypes.byref(r))
    return bytes(buf[:r.value])

# ── 키보드 전송 (PostMessage WM_CHAR 방식) ────────────────
WM_KEYDOWN = 0x0100
WM_KEYUP   = 0x0101
WM_CHAR    = 0x0102
VK_RETURN  = 0x0D

def post_enter(hwnd):
    user32.PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0x001C0001)
    time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_KEYUP,   VK_RETURN, 0xC01C0001)
    time.sleep(0.05)

def post_text(hwnd, text):
    for ch in text:
        user32.PostMessageW(hwnd, WM_CHAR, ord(ch), 0)
        time.sleep(0.02)

# ── 메모리 스캔 ────────────────────────────────────────
def scan_for(hp, keyword: bytes):
    """keyword 가 포함된 메모리 위치와 주변 컨텍스트를 모두 출력"""
    mbi = MEMORY_BASIC_INFORMATION()
    mbi_size = ctypes.sizeof(mbi)
    results = []
    addr = 0
    while kernel32.VirtualQueryEx(hp, ctypes.c_void_p(addr), ctypes.byref(mbi), mbi_size) == mbi_size:
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize
        next_addr = base + size
        if mbi.State == MEM_COMMIT and mbi.Protect not in (0x01, 0x100) and size < 0x4000000:
            data = read_mem(hp, base, size)
            pos = 0
            while True:
                idx = data.find(keyword, pos)
                if idx == -1: break
                abs_addr = base + idx
                ctx = data[max(0,idx-200):idx+200]
                results.append((abs_addr, ctx))
                pos = idx + 1
        if next_addr <= addr: break
        addr = next_addr
    return results

# ── 메인 ──────────────────────────────────────────────
pid = find_sc_pid()
if not pid:
    print('StarCraft.exe 없음'); exit()
print(f'SC PID: {pid}')

hwnd = find_sc_hwnd(pid)
if not hwnd:
    print('SC 창 없음'); exit()
print(f'SC HWND: 0x{hwnd:X}')

hp = kernel32.OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_INFORMATION, False, pid)

WHISPER_TARGET  = 'dkanskskdhk1'   # ← 여기에 본인 아이디 입력
UNIQUE_MARKER   = 'bwtest123'
WHISPER_CMD     = f'/w {WHISPER_TARGET} {UNIQUE_MARKER}'

print(f'\nSC 창에 귓말을 자동 전송합니다 (PostMessage): {WHISPER_CMD}')
time.sleep(1)

# Enter로 채팅창 열기
post_enter(hwnd)
time.sleep(0.5)

# 귓말 명령 입력
post_text(hwnd, WHISPER_CMD)
time.sleep(0.3)

# Enter로 전송
post_enter(hwnd)
print('귓말 전송 완료. 메모리 스캔 시작...')

# 즉시 스캔 (여러 번 시도)
for attempt in range(5):
    time.sleep(0.5)
    results = scan_for(hp, UNIQUE_MARKER.encode('utf-8'))
    if results:
        print(f'\n[시도 {attempt+1}] {UNIQUE_MARKER} 발견! {len(results)}개 위치:')
        for abs_addr, ctx in results:
            print(f'\n  addr=0x{abs_addr:X}')
            # 전체 JSON 맥락 출력
            try:
                text = ctx.decode('utf-8', 'replace')
            except:
                text = repr(ctx)
            print(f'  ctx: {text}')
        break
    else:
        print(f'[시도 {attempt+1}] 아직 없음...')

if not results:
    print('\n귓말 데이터를 메모리에서 찾지 못했습니다.')
    print('SC가 데이터를 아직 처리하지 않았거나 다른 방식으로 저장할 수 있습니다.')

kernel32.CloseHandle(hp)
