"""
write head 포인터 탐색 v3 — SC.exe IMAGE .data 섹션 스캔
링버퍼 슬롯 인덱스(0~9)나 슬롯 주소를 가리키는 포인터가 IMAGE 영역에 있을 것

사용법:
  python find_write_head3.py before
  python find_write_head3.py after
"""
import ctypes, ctypes.wintypes, sys, os, json, struct
sys.stdout.reconfigure(encoding='utf-8')

PROCESS_VM_READ           = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS        = 0x00000002
TH32CS_SNAPMODULE         = 0x00000008
TH32CS_SNAPMODULE32       = 0x00000010
MEM_COMMIT   = 0x1000
MEM_IMAGE    = 0x1000000
MEM_PRIVATE  = 0x20000
kernel32     = ctypes.windll.kernel32

RING_SLOT0   = 0x00007FF63A2DE0CB
SLOT_SIZE    = 0xDA
SLOT_COUNT   = 10
RING_END     = RING_SLOT0 + SLOT_SIZE * SLOT_COUNT

SNAP_PATH    = os.path.join(os.path.dirname(__file__), 'snaps', 'wh3_before.bin')

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

def read_mem(hp, addr, size):
    buf = (ctypes.c_ubyte * size)()
    r = ctypes.c_size_t(0)
    kernel32.ReadProcessMemory(hp, ctypes.c_void_p(addr), buf, size, ctypes.byref(r))
    return bytes(buf[:r.value])

def scan_image_regions(hp, mod_base, mod_size):
    """SC.exe IMAGE 리전 + PRIVATE 리전 스캔 (쓰기 가능한 섹션만)"""
    mbi   = MEMORY_BASIC_INFORMATION()
    sz    = ctypes.sizeof(mbi)
    addr  = mod_base
    end   = mod_base + mod_size
    regions = []
    PAGE_WRITABLE = {0x04, 0x08, 0x20, 0x40, 0x80}  # RW, WC, EXECUTE_RW, EXECUTE_WCopy, etc.
    # 더 넓게: NOACCESS(0x01), GUARD(0x100) 제외, 나머지 모두 포함
    while addr < end:
        r = kernel32.VirtualQueryEx(hp, ctypes.c_void_p(addr), ctypes.byref(mbi), sz)
        if r != sz:
            break
        base  = mbi.BaseAddress or 0
        size  = mbi.RegionSize
        next_addr = base + size
        if (mbi.State == MEM_COMMIT and
                mbi.Protect not in (0x01, 0x100) and
                mbi.Type in (MEM_IMAGE, MEM_PRIVATE)):
            data = read_mem(hp, base, size)
            if data:
                regions.append({
                    'base':  base,
                    'size':  len(data),
                    'type':  mbi.Type,
                    'prot':  mbi.Protect,
                    'data':  data,
                })
        if next_addr <= addr: break
        addr = next_addr
    return regions

pid = find_sc_pid()
if not pid: print('SC 없음'); sys.exit(1)
mod_base, mod_size = get_module_base(pid)
print(f'SC PID={pid}  모듈베이스=0x{mod_base:X}  크기=0x{mod_size:X} ({mod_size//1024}KB)')
hp = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)

if sys.argv[1] == 'before':
    import time
    t0 = time.time()
    regions = scan_image_regions(hp, mod_base, mod_size)
    kernel32.CloseHandle(hp)
    elapsed = time.time() - t0

    total = sum(r['size'] for r in regions)
    print(f'스캔 완료: {elapsed:.1f}초  리전={len(regions)}개  총={total//1024}KB')

    os.makedirs(os.path.dirname(SNAP_PATH), exist_ok=True)
    snap = [(r['base'], r['size'], r['type'], r['data'].hex()) for r in regions]
    with open(SNAP_PATH, 'w') as f:
        json.dump(snap, f)
    print(f'저장 완료: {SNAP_PATH}')
    print('\n→ 이제 귓말을 보내세요.')

elif sys.argv[1] == 'after':
    import time
    with open(SNAP_PATH) as f:
        snap = json.load(f)
    before_map = {s[0]: (s[1], s[2], bytes.fromhex(s[3])) for s in snap}

    t0 = time.time()
    after_regions = scan_image_regions(hp, mod_base, mod_size)
    kernel32.CloseHandle(hp)
    elapsed = time.time() - t0
    print(f'스캔 완료: {elapsed:.1f}초')

    TYPE_NAMES = {MEM_IMAGE: 'IMAGE', MEM_PRIVATE: 'PRIVATE'}

    print('\n=== write head 포인터 후보 ===')
    print(f'링버퍼: 0x{RING_SLOT0:X}~0x{RING_END:X}  슬롯={SLOT_COUNT}개x{SLOT_SIZE}B')

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
        rva_base  = base - mod_base

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
                found.append((abs_addr, bv, av, type_name, rva_base + i, 'IDX' if is_idx else 'PTR32'))

        # 8바이트 단위 diff (64비트 포인터)
        for i in range(0, n - 8, 8):
            bv = struct.unpack_from('<Q', bdata, i)[0]
            av = struct.unpack_from('<Q', adata, i)[0]
            if bv == av: continue

            abs_addr = base + i
            is_ptr64 = ((RING_SLOT0 <= bv < RING_END) or (RING_SLOT0 <= av < RING_END))
            if is_ptr64:
                found.append((abs_addr, bv, av, type_name, rva_base + i, 'PTR64'))

    if found:
        for abs_addr, bv, av, tname, rva, kind in found:
            print(f'  [{kind}][{tname}] 0x{abs_addr:X} (RVA=0x{rva:X}): 0x{bv:X} → 0x{av:X}')
    else:
        print('  없음')

    # 현재 슬롯 상태
    hp2 = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    print(f'\n=== 현재 슬롯 상태 ===')
    for i in range(SLOT_COUNT):
        addr = RING_SLOT0 + i * SLOT_SIZE
        data = read_mem(hp2, addr, SLOT_SIZE)
        text = data.split(b'\x00')[0].decode('ascii', errors='replace')
        print(f'  [{i}] 0x{addr:X}: {text!r}')
    kernel32.CloseHandle(hp2)
