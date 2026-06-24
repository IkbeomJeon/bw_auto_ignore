"""
write head 포인터 탐색 (IMAGE + PRIVATE + MAPPED 전체)
링버퍼 주변 ±2MB 범위를 before/after diff

사용법:
  python find_write_head2.py before
  python find_write_head2.py after
"""
import ctypes, ctypes.wintypes, sys, os, json, struct, time
sys.stdout.reconfigure(encoding='utf-8')

PROCESS_VM_READ           = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS        = 0x00000002
MEM_COMMIT   = 0x1000
kernel32     = ctypes.windll.kernel32

# 링버퍼 기준 주소 (이전 세션에서 확인)
RING_SLOT0   = 0x00007FF63A2DE0CB
SLOT_SIZE    = 0xDA
SLOT_COUNT   = 10
RING_END     = RING_SLOT0 + SLOT_SIZE * SLOT_COUNT

# 탐색 범위: 링버퍼 ±2MB
SCAN_START   = RING_SLOT0 - 0x200000
SCAN_END     = RING_END   + 0x200000

SNAP_PATH    = os.path.join(os.path.dirname(__file__), 'snaps', 'wh2_before.bin')

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

MEM_IMAGE   = 0x1000000
MEM_MAPPED  = 0x40000
MEM_PRIVATE = 0x20000
TYPE_NAMES  = {MEM_IMAGE: 'IMAGE', MEM_MAPPED: 'MAPPED', MEM_PRIVATE: 'PRIVATE'}

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

def scan_range(hp):
    """SCAN_START~SCAN_END 범위의 모든 committed 리전 읽기"""
    mbi   = MEMORY_BASIC_INFORMATION()
    sz    = ctypes.sizeof(mbi)
    addr  = SCAN_START
    regions = []
    while addr < SCAN_END:
        if kernel32.VirtualQueryEx(hp, ctypes.c_void_p(addr), ctypes.byref(mbi), sz) != sz:
            break
        base  = mbi.BaseAddress or 0
        size  = mbi.RegionSize
        next_addr = base + size
        # committed이고 접근 가능한 리전만
        if (mbi.State == MEM_COMMIT and
                mbi.Protect not in (0x01, 0x100) and  # NOACCESS, GUARD
                base + size > SCAN_START and base < SCAN_END):
            read_start = max(base, SCAN_START)
            read_end   = min(base + size, SCAN_END)
            read_size  = read_end - read_start
            if read_size > 0:
                data = read_mem(hp, read_start, read_size)
                regions.append({
                    'base':  read_start,
                    'size':  read_size,
                    'type':  mbi.Type,
                    'prot':  mbi.Protect,
                    'data':  data,
                })
        if next_addr <= addr: break
        addr = next_addr
    return regions

pid = find_sc_pid()
if not pid: print('SC 없음'); sys.exit(1)
hp = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)

if sys.argv[1] == 'before':
    t0 = time.time()
    regions = scan_range(hp)
    kernel32.CloseHandle(hp)
    elapsed = time.time() - t0

    total = sum(r['size'] for r in regions)
    print(f'스캔 완료: {elapsed:.1f}초  리전={len(regions)}개  총={total//1024}KB')

    # 직렬화 (base, size, type, data hex)
    os.makedirs(os.path.dirname(SNAP_PATH), exist_ok=True)
    snap = [(r['base'], r['size'], r['type'], r['data'].hex()) for r in regions]
    with open(SNAP_PATH, 'w') as f:
        json.dump(snap, f)
    print(f'저장 완료: {SNAP_PATH}')
    print('\n→ 이제 귓말을 보내세요.')

elif sys.argv[1] == 'after':
    with open(SNAP_PATH) as f:
        snap = json.load(f)
    before_map = {s[0]: (s[1], s[2], bytes.fromhex(s[3])) for s in snap}

    t0 = time.time()
    after_regions = scan_range(hp)
    kernel32.CloseHandle(hp)
    elapsed = time.time() - t0
    print(f'스캔 완료: {elapsed:.1f}초')

    # diff
    print('\n=== write head 포인터 후보 ===')
    print(f'기준: 슬롯 인덱스 0~{SLOT_COUNT-1}, 링버퍼 주소 0x{RING_SLOT0:X}~0x{RING_END:X}')

    found = []
    for region in after_regions:
        base  = region['base']
        atype = region['type']
        adata = region['data']

        if base not in before_map:
            continue
        _, btype, bdata = before_map[base]
        n = min(len(bdata), len(adata))

        type_name = TYPE_NAMES.get(atype, f'{atype:#x}')

        # 4바이트 단위 diff
        for i in range(0, n - 4, 4):
            bv = struct.unpack_from('<I', bdata, i)[0]
            av = struct.unpack_from('<I', adata, i)[0]
            if bv == av: continue

            abs_addr = base + i
            is_idx   = (0 <= bv < SLOT_COUNT and 0 <= av < SLOT_COUNT)
            lo_ring  = RING_SLOT0 & 0xFFFFFFFF
            is_ptr32 = ((lo_ring <= bv < lo_ring + SLOT_SIZE*SLOT_COUNT) or
                        (lo_ring <= av < lo_ring + SLOT_SIZE*SLOT_COUNT))

            if is_idx or is_ptr32:
                found.append((abs_addr, bv, av, type_name, 'IDX' if is_idx else 'PTR32'))

        # 8바이트 단위 diff (64비트 포인터)
        for i in range(0, n - 8, 8):
            bv = struct.unpack_from('<Q', bdata, i)[0]
            av = struct.unpack_from('<Q', adata, i)[0]
            if bv == av: continue

            abs_addr = base + i
            is_ptr64 = ((RING_SLOT0 <= bv < RING_END) or (RING_SLOT0 <= av < RING_END))
            if is_ptr64:
                found.append((abs_addr, bv, av, type_name, 'PTR64'))

    if found:
        for abs_addr, bv, av, tname, kind in found:
            print(f'  [{kind}][{tname}] 0x{abs_addr:X}: 0x{bv:X} → 0x{av:X}')
    else:
        print('  없음')

    # 현재 슬롯 내용도 출력
    hp2 = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    print(f'\n=== 현재 슬롯 상태 ===')
    for i in range(SLOT_COUNT):
        addr = RING_SLOT0 + i * SLOT_SIZE
        data = read_mem(hp2, addr, SLOT_SIZE)
        text = data.split(b'\x00')[0].decode('ascii', errors='replace')
        print(f'  [{i}] 0x{addr:X}: {text!r}')
    kernel32.CloseHandle(hp2)
