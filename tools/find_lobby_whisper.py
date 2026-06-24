"""
로비 귓말 버퍼 탐색 — 전체 MAPPED RW 섹션 before/after diff
사용법:
  python find_lobby_whisper.py before
  python find_lobby_whisper.py after <키워드>
"""
import ctypes, ctypes.wintypes, sys, os, time, struct
sys.stdout.reconfigure(encoding='utf-8')

PROCESS_VM_READ           = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS        = 0x00000002
kernel32 = ctypes.windll.kernel32

# 알려진 인게임 링버퍼 (비교용)
RING_SLOT0  = 0x7FF63A2DE0CB
SLOT_SIZE   = 0xDA
SLOT_COUNT  = 10

SNAP_PATH = os.path.join(os.path.dirname(__file__), 'snaps', 'lobby_before.bin')

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
    buf = (ctypes.c_ubyte * size)(); r = ctypes.c_size_t(0)
    kernel32.ReadProcessMemory(hp, ctypes.c_void_p(addr), buf, size, ctypes.byref(r))
    return bytes(buf[:r.value])

def scan_all_committed(hp):
    """전체 프로세스의 committed 리전 읽기 (MAPPED + PRIVATE)"""
    mbi = MEMORY_BASIC_INFORMATION()
    sz  = ctypes.sizeof(mbi)
    addr = 0
    regions = []
    while True:
        r = kernel32.VirtualQueryEx(hp, ctypes.c_void_p(addr), ctypes.byref(mbi), sz)
        if r != sz: break
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize
        if (mbi.State == 0x1000 and  # MEM_COMMIT
                mbi.Protect not in (0x01, 0x100) and  # not NOACCESS/GUARD
                mbi.Type in (0x40000, 0x20000) and  # MAPPED or PRIVATE
                64 <= size <= 0x800000):
            data = read_mem(hp, base, size)
            if data:
                regions.append((base, data))
        next_addr = base + size
        if next_addr <= addr: break
        addr = next_addr
    return regions

pid = find_sc_pid()
if not pid: print('SC 없음'); sys.exit(1)
hp = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)

if sys.argv[1] == 'before':
    t0 = time.time()
    regions = scan_all_committed(hp)
    kernel32.CloseHandle(hp)
    elapsed = time.time() - t0
    total = sum(len(d) for _, d in regions)
    print(f'스캔: {elapsed:.1f}초  리전={len(regions)}  총={total//1024}KB')

    os.makedirs(os.path.dirname(SNAP_PATH), exist_ok=True)
    import json
    snap = [(base, data.hex()) for base, data in regions]
    with open(SNAP_PATH, 'w') as f:
        json.dump(snap, f)
    print(f'저장: {SNAP_PATH}')
    print('\n→ 이제 귓말을 보내세요.')

elif sys.argv[1] == 'after':
    keyword = sys.argv[2].encode('utf-8') if len(sys.argv) > 2 else b'whdiff'

    import json
    with open(SNAP_PATH) as f:
        snap = json.load(f)
    before_map = {s[0]: bytes.fromhex(s[1]) for s in snap}

    t0 = time.time()
    after_regions = scan_all_committed(hp)
    kernel32.CloseHandle(hp)
    elapsed = time.time() - t0
    print(f'스캔: {elapsed:.1f}초')

    print(f'\n=== 키워드 "{keyword.decode()}" 포함된 변경 리전 ===')
    found = []
    for base, adata in after_regions:
        if base not in before_map:
            continue
        bdata = before_map[base]
        if bdata == adata[:len(bdata)]: continue  # 변화 없음

        # 키워드 검색
        if keyword in adata:
            idx = adata.find(keyword)
            # 해당 위치 전후 context
            start = max(0, idx - 60)
            end   = min(len(adata), idx + 100)
            ctx   = adata[start:end]
            abs_addr = base + idx
            printable = ''.join(chr(b) if 32<=b<127 else '\\x{:02x}'.format(b) for b in ctx)
            print(f'\n  주소: 0x{abs_addr:X}  (리전베이스: 0x{base:X})')
            print(f'  hex:  {ctx.hex()}')
            print(f'  txt:  {printable}')
            found.append((abs_addr, ctx))

    if not found:
        # 키워드 없으면 단순 변경된 리전 목록
        print('  키워드 미발견. 변경된 리전:')
        for base, adata in after_regions:
            if base in before_map and before_map[base] != adata[:len(before_map[base])]:
                print(f'  0x{base:X} ({len(adata)//1024}KB)')
