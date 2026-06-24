"""
SC 귓말 메모리 구조 분석기
- 25개 귓말을 순서대로 전송
- 전송 후 메모리 전체 스캔
- 각 마커 주변 JSON 구조 분석
"""
import ctypes, ctypes.wintypes, sys, time, re
sys.stdout.reconfigure(encoding='utf-8')

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS = 0x00000002
MEM_COMMIT  = 0x1000
MEM_PRIVATE = 0x20000
PAGE_READWRITE = 0x04
PAGE_READONLY  = 0x02
kernel32 = ctypes.windll.kernel32
user32   = ctypes.windll.user32

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
        time.sleep(0.015)

def send_whisper(hwnd, target, msg):
    cmd = f'/w {target} {msg}'
    post_enter(hwnd)
    time.sleep(0.4)
    post_text(hwnd, cmd)
    time.sleep(0.2)
    post_enter(hwnd)
    time.sleep(0.3)

def scan_keyword(hp, keyword: bytes, back=600):
    """keyword 위치 + 앞뒤 컨텍스트 반환. MEM_PRIVATE RW/RO 필터."""
    mbi = MEMORY_BASIC_INFORMATION()
    mbi_size = ctypes.sizeof(mbi)
    results = []
    addr = 0
    while kernel32.VirtualQueryEx(hp, ctypes.c_void_p(addr), ctypes.byref(mbi), mbi_size) == mbi_size:
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize
        next_addr = base + size
        if (mbi.State == MEM_COMMIT and mbi.Type == MEM_PRIVATE and
                mbi.Protect in (PAGE_READWRITE, PAGE_READONLY) and 64 <= size <= 0x800000):
            data = read_mem(hp, base, size)
            pos = 0
            while True:
                idx = data.find(keyword, pos)
                if idx == -1: break
                abs_addr = base + idx
                ctx_start = max(0, idx - back)
                ctx_end   = min(len(data), idx + len(keyword) + back)
                ctx = data[ctx_start:ctx_end]
                results.append((abs_addr, ctx, idx - ctx_start))
                pos = idx + 1
        if next_addr <= addr: break
        addr = next_addr
    return results

def extract_json_field(ctx: bytes, field: bytes):
    """ctx 안에서 "field":"VALUE" 또는 \"field\":\"VALUE\" 패턴 추출"""
    for sep in (b'":"', b'\\":\\"'):
        tag = field + sep
        i = ctx.rfind(tag)
        if i == -1: continue
        s = i + len(tag)
        end_char = b'"' if sep == b'":"' else b'\\"'
        e = ctx.find(end_char, s)
        if e != -1 and 0 < e - s < 128:
            val = ctx[s:e].decode('utf-8', 'replace')
            return val
    return None

# ── 메인 ──────────────────────────────────────────────────────────────

pid = find_sc_pid()
if not pid: print('SC 없음'); exit()
hwnd = find_sc_hwnd(pid)
if not hwnd: print('SC 창 없음'); exit()
print(f'SC PID={pid}  HWND=0x{hwnd:X}')

TARGET = 'dkanskskdhk1'

# 25개 메시지: 다양한 내용
MESSAGES = [
    'hello001', 'world002', 'alpha003', 'bravo004', 'charlie005',
    'delta006', 'echo007',  'foxtrot008','golf009',  'hotel010',
    'india011', 'juliet012','kilo013',  'lima014',  'mike015',
    'nova016',  'oscar017', 'papa018',  'quebec019','romeo020',
    'sierra021','tango022', 'uniform023','victor024','whiskey025',
]

hp = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)

print(f'\n=== 귓말 {len(MESSAGES)}개 전송 시작 ===')
sent_order = []
for i, msg in enumerate(MESSAGES):
    marker = f'bwt_{msg}'
    send_whisper(hwnd, TARGET, marker)
    sent_order.append(marker)
    print(f'  [{i+1:02d}] /w {TARGET} {marker}')

print(f'\n전송 완료. 2초 대기 후 메모리 스캔...')
time.sleep(2)

# ── 스캔 ────────────────────────────────────────────────────────────

print('\n=== 메모리 스캔 ===')

# 각 마커를 주소와 함께 수집
found = {}  # marker -> [(abs_addr, type_field, sender_field, content_field)]
for marker in sent_order:
    kw = marker.encode('utf-8')
    results = scan_keyword(hp, kw, back=800)
    entries = []
    for abs_addr, ctx, kw_off in results:
        type_val    = extract_json_field(ctx, b'type')
        sender_val  = extract_json_field(ctx, b'legacyToonName')
        content_val = extract_json_field(ctx, b'content')
        entries.append((abs_addr, type_val, sender_val, content_val))
    found[marker] = entries

kernel32.CloseHandle(hp)

# ── 결과 출력 ────────────────────────────────────────────────────────

print('\n=== 전송 순서 vs 메모리 주소 ===')
print(f'{"순서":<5} {"마커":<20} {"주소":<18} {"type":<20} {"sender":<20} {"content"}')
print('-'*110)

addr_list = []  # (addr, order_idx, marker) — 주소 순서 비교용

for i, marker in enumerate(sent_order):
    entries = found[marker]
    if not entries:
        print(f'{i+1:<5} {marker:<20} {"NOT FOUND":<18}')
        continue
    # 가장 높은 주소 (= 최신 버퍼) 우선
    best = max(entries, key=lambda x: x[0])
    abs_addr, type_val, sender_val, content_val = best
    addr_list.append((abs_addr, i+1, marker))
    print(f'{i+1:<5} {marker:<20} 0x{abs_addr:<16X} {str(type_val):<20} {str(sender_val):<20} {str(content_val)}')

# 주소 순서 분석
print('\n=== 주소 오름차순 정렬 (메모리 내 저장 순서) ===')
addr_list.sort(key=lambda x: x[0])
for rank, (addr, order, marker) in enumerate(addr_list, 1):
    print(f'  메모리순={rank:<3} 전송순={order:<3} 0x{addr:X}  {marker}')

# 최고 주소 = 가장 최근?
if addr_list:
    last_addr, last_order, last_marker = addr_list[-1]
    print(f'\n  → 가장 높은 주소: 전송순={last_order}, marker={last_marker}')
    print(f'    (전송 순서와 주소 순서가 일치하면 "높은 주소 = 가장 최근" 전략이 유효)')
