"""
write head 포인터 탐색 v4 — SC.exe MAPPED RW 섹션 전체 스캔
0x7FF63A010000 ~ 0x7FF63A610000 (6144KB)

사용법:
  python find_write_head4.py before
  python find_write_head4.py after
"""
import ctypes, ctypes.wintypes, sys, os, json, struct, time
sys.stdout.reconfigure(encoding='utf-8')

PROCESS_VM_READ           = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS        = 0x00000002
kernel32     = ctypes.windll.kernel32

RING_SLOT0   = 0x00007FF63A2DE0CB
SLOT_SIZE    = 0xDA
SLOT_COUNT   = 10
RING_END     = RING_SLOT0 + SLOT_SIZE * SLOT_COUNT

# MAPPED RW 섹션 전체
SCAN_START   = 0x7FF63A010000
SCAN_END     = 0x7FF63A610000

SNAP_PATH    = os.path.join(os.path.dirname(__file__), 'snaps', 'wh4_before.bin')

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

MEM_COMMIT = 0x1000

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

def snap_rw(hp):
    """RW MAPPED 섹션 전체 읽기 (6144KB)"""
    size = SCAN_END - SCAN_START
    return read_mem(hp, SCAN_START, size)

pid = find_sc_pid()
if not pid: print('SC 없음'); sys.exit(1)
hp = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)

if sys.argv[1] == 'before':
    t0 = time.time()
    data = snap_rw(hp)
    kernel32.CloseHandle(hp)
    elapsed = time.time() - t0
    print(f'읽기 완료: {elapsed:.2f}초  크기={len(data)//1024}KB')
    os.makedirs(os.path.dirname(SNAP_PATH), exist_ok=True)
    with open(SNAP_PATH, 'wb') as f:
        f.write(data)
    print(f'저장 완료: {SNAP_PATH}')
    print('\n→ 이제 귓말을 보내세요.')

elif sys.argv[1] == 'after':
    with open(SNAP_PATH, 'rb') as f:
        before = f.read()

    t0 = time.time()
    after = snap_rw(hp)
    kernel32.CloseHandle(hp)
    elapsed = time.time() - t0
    print(f'읽기 완료: {elapsed:.2f}초')

    n = min(len(before), len(after))
    print(f'\n=== write head 포인터 후보 ===')
    print(f'링버퍼: 0x{RING_SLOT0:X}~0x{RING_END:X}  슬롯={SLOT_COUNT}개x{SLOT_SIZE}B')

    found4  = []
    found8  = []

    # 4바이트 단위
    for i in range(0, n - 4, 4):
        bv = struct.unpack_from('<I', before, i)[0]
        av = struct.unpack_from('<I', after,  i)[0]
        if bv == av: continue
        abs_addr = SCAN_START + i
        is_idx   = (0 <= bv < SLOT_COUNT and 0 <= av < SLOT_COUNT)
        lo_ring  = RING_SLOT0 & 0xFFFFFFFF
        is_ptr32 = ((lo_ring <= bv < lo_ring + SLOT_SIZE*SLOT_COUNT) or
                    (lo_ring <= av < lo_ring + SLOT_SIZE*SLOT_COUNT))
        if is_idx or is_ptr32:
            found4.append((abs_addr, bv, av, 'IDX' if is_idx else 'PTR32'))

    # 8바이트 단위
    for i in range(0, n - 8, 8):
        bv = struct.unpack_from('<Q', before, i)[0]
        av = struct.unpack_from('<Q', after,  i)[0]
        if bv == av: continue
        abs_addr = SCAN_START + i
        is_ptr64 = ((RING_SLOT0 <= bv < RING_END) or (RING_SLOT0 <= av < RING_END))
        if is_ptr64:
            found8.append((abs_addr, bv, av, 'PTR64'))

    all_found = sorted(set(f[0] for f in found4 + found8))
    if all_found:
        for abs_addr in all_found:
            for f in found4 + found8:
                if f[0] == abs_addr:
                    rva = abs_addr - 0x7FF639270000
                    print(f'  [{f[3]}] 0x{abs_addr:X} (RVA=0x{rva:X}): 0x{f[1]:X} → 0x{f[2]:X}')
                    break
    else:
        print('  없음')
        # 얼마나 바뀌었는지 총 diff 출력
        diff_count = sum(1 for i in range(0, n-4, 4)
                        if struct.unpack_from('<I', before, i)[0] != struct.unpack_from('<I', after, i)[0])
        print(f'  (총 변경된 4B 워드: {diff_count}개)')

    # 현재 슬롯 상태
    hp2 = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    print(f'\n=== 현재 슬롯 상태 ===')
    for i in range(SLOT_COUNT):
        addr = RING_SLOT0 + i * SLOT_SIZE
        data = read_mem(hp2, addr, SLOT_SIZE)
        text = data.split(b'\x00')[0].decode('ascii', errors='replace')
        print(f'  [{i}] 0x{addr:X}: {text!r}')
    kernel32.CloseHandle(hp2)
