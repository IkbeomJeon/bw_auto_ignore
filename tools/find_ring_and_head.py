"""
재시작 후 링버퍼 주소 확인 + write head 포인터 탐색
사용법:
  python find_ring_and_head.py before
  python find_ring_and_head.py after <발신자> <내용>
"""
import ctypes, ctypes.wintypes, sys, os, json, struct, re
sys.stdout.reconfigure(encoding='utf-8')

PROCESS_VM_READ           = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS        = 0x00000002
TH32CS_SNAPMODULE         = 0x00000008
TH32CS_SNAPMODULE32       = 0x00000010
MEM_COMMIT  = 0x1000
MEM_MAPPED  = 0x40000
kernel32 = ctypes.windll.kernel32

SNAP_PATH = os.path.join(os.path.dirname(__file__), 'snaps', 'ring_head_before.json')
PREV_RVA  = 0x106E0CB   # 이전 세션에서 발견한 링버퍼 시작 RVA
SLOT_SIZE = 0xDA
SLOT_COUNT = 10

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
                base = ctypes.addressof(me.modBaseAddr.contents)
                size = me.modBaseSize
                kernel32.CloseHandle(snap); return base, size
            if not kernel32.Module32Next(snap, ctypes.byref(me)): break
    kernel32.CloseHandle(snap); return None, None

def read_mem(hp, addr, size):
    buf = (ctypes.c_ubyte * size)()
    r = ctypes.c_size_t(0)
    kernel32.ReadProcessMemory(hp, ctypes.c_void_p(addr), buf, size, ctypes.byref(r))
    return bytes(buf[:r.value])

def find_ring_buffer(hp, mod_base, mod_size):
    """MAPPED 메모리에서 'sender> ' 패턴으로 링버퍼 찾기"""
    mbi = MEMORY_BASIC_INFORMATION()
    sz  = ctypes.sizeof(mbi)
    addr = mod_base
    end  = mod_base + mod_size
    candidates = []
    while addr < end:
        if kernel32.VirtualQueryEx(hp, ctypes.c_void_p(addr), ctypes.byref(mbi), sz) != sz:
            break
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize
        if mbi.State == MEM_COMMIT and mbi.Type == MEM_MAPPED and size > 0:
            data = read_mem(hp, base, min(size, 0x10000))
            # > 패턴 검색
            pos = 0
            while True:
                idx = data.find(b'> ', pos)
                if idx == -1: break
                # 앞에 printable ASCII sender가 있는지 확인
                start = idx - 1
                while start > 0 and 32 <= data[start-1] < 127 and data[start-1] != ord('>'):
                    start -= 1
                if idx - start >= 2:
                    abs_addr = base + start
                    text = data[start:start+60].split(b'\x00')[0].decode('ascii','replace')
                    if '> ' in text and len(text) > 4:
                        candidates.append((abs_addr, text))
                pos = idx + 1
        addr = base + size
        if addr <= (mbi.BaseAddress or 0): break
    return candidates

def read_mapped_region(hp, addr):
    """해당 주소가 속한 MAPPED 리전 전체 읽기"""
    mbi = MEMORY_BASIC_INFORMATION()
    kernel32.VirtualQueryEx(hp, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi))
    base = mbi.BaseAddress or 0
    size = min(mbi.RegionSize, 0x20000)  # 최대 128KB
    return base, read_mem(hp, base, size)

# ── 메인 ──────────────────────────────────────────────────────────────
pid = find_sc_pid()
if not pid: print('SC 없음'); sys.exit(1)
mod_base, mod_size = get_module_base(pid)
print(f'SC PID={pid}  모듈베이스=0x{mod_base:X}  크기=0x{mod_size:X}')
print(f'이전 RVA: 0x{PREV_RVA:X} → 예상 주소: 0x{mod_base + PREV_RVA:X}')

hp = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)

if sys.argv[1] == 'before':
    # 1. 링버퍼 찾기
    print('\n링버퍼 탐색 중...')
    hits = find_ring_buffer(hp, mod_base, mod_size)
    print(f'후보 {len(hits)}개:')
    for addr, text in hits[:20]:
        rva = addr - mod_base
        print(f'  0x{addr:X} (RVA=0x{rva:X}): {text!r}')

    # 2. 이전 RVA 주소에서 읽기
    old_addr = mod_base + PREV_RVA
    old_data = read_mem(hp, old_addr, SLOT_SIZE * SLOT_COUNT)
    old_text = old_data.split(b'\x00')[0].decode('ascii','replace')
    print(f'\n이전 RVA 주소(0x{old_addr:X}) 내용: {old_text!r}')

    # 3. 모듈 전체 MAPPED 영역 스냅샷 (write head 탐색용)
    print('\n모듈 전체 MAPPED 스냅샷 저장 중...')
    mapped_snaps = []
    mbi = MEMORY_BASIC_INFORMATION()
    sz  = ctypes.sizeof(mbi)
    addr = mod_base
    end  = mod_base + mod_size
    while addr < end:
        if kernel32.VirtualQueryEx(hp, ctypes.c_void_p(addr), ctypes.byref(mbi), sz) != sz: break
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize
        if mbi.State == MEM_COMMIT and mbi.Type == MEM_MAPPED and 64 <= size <= 0x100000:
            data = read_mem(hp, base, size)
            mapped_snaps.append({'base': base, 'size': size, 'data': data.hex()})
        addr = base + size
        if addr <= (mbi.BaseAddress or 0): break

    total = sum(s['size'] for s in mapped_snaps)
    print(f'MAPPED 리전 {len(mapped_snaps)}개, 총 {total//1024}KB')

    os.makedirs(os.path.dirname(SNAP_PATH), exist_ok=True)
    with open(SNAP_PATH, 'w') as f:
        json.dump({'mod_base': mod_base, 'snaps': mapped_snaps, 'hits': [(a, t) for a, t in hits]}, f)
    print(f'저장 완료: {SNAP_PATH}')
    print('\n→ 이제 귓말을 보내세요.')

elif sys.argv[1] == 'after':
    sender  = sys.argv[2] if len(sys.argv) > 2 else 'bwtest2'
    content = sys.argv[3] if len(sys.argv) > 3 else 'newmsg1'
    kw = f'{sender}> {content}'.encode('ascii')

    with open(SNAP_PATH) as f:
        snap = json.load(f)
    prev_base = snap['mod_base']

    print(f'\n=== 링버퍼 주소 변화 ===')
    print(f'이전 모듈베이스: 0x{prev_base:X}')
    print(f'현재 모듈베이스: 0x{mod_base:X}')
    diff = mod_base - prev_base
    print(f'차이: {diff:+d} (0x{abs(diff):X})')

    # 귓말 내용으로 새 링버퍼 주소 찾기
    print(f'\n귓말 "{sender}> {content}" 탐색...')
    hits = find_ring_buffer(hp, mod_base, mod_size)
    whisper_hits = [(a, t) for a, t in hits if content in t]
    for addr, text in whisper_hits:
        rva = addr - mod_base
        print(f'  발견: 0x{addr:X} (RVA=0x{rva:X}): {text!r}')
        print(f'  이전 RVA와 동일: {rva == PREV_RVA + (addr - (mod_base + PREV_RVA) - (addr - (mod_base + rva)))}')

    if whisper_hits:
        new_rva = whisper_hits[0][0] - mod_base
        # 슬롯 경계로 정렬
        slot_idx = round((new_rva - PREV_RVA) / SLOT_SIZE) % SLOT_COUNT
        ring_base_rva = new_rva - slot_idx * SLOT_SIZE
        print(f'\n  → 링버퍼 시작 RVA: 0x{ring_base_rva:X}')
        print(f'  → 이전 RVA 0x{PREV_RVA:X}와 {'동일' if ring_base_rva == PREV_RVA else f"다름 (차이={ring_base_rva - PREV_RVA:+d})"}')

    # write head 포인터 탐색 (before/after diff)
    print(f'\n=== write head 포인터 탐색 (전체 MAPPED diff) ===')
    before_snaps = {s['base']: bytes.fromhex(s['data']) for s in snap['snaps']}

    candidates = []
    mbi2 = MEMORY_BASIC_INFORMATION()
    addr2 = mod_base
    end2  = mod_base + mod_size
    while addr2 < end2:
        if kernel32.VirtualQueryEx(hp, ctypes.c_void_p(addr2), ctypes.byref(mbi2), ctypes.sizeof(mbi2)) != ctypes.sizeof(mbi2): break
        base2 = mbi2.BaseAddress or 0
        size2 = mbi2.RegionSize
        if mbi2.State == MEM_COMMIT and mbi2.Type == MEM_MAPPED and 64 <= size2 <= 0x100000:
            after_data = read_mem(hp, base2, size2)
            before_data = before_snaps.get(base2)
            if before_data and len(before_data) == len(after_data):
                n = len(before_data)
                # 4바이트 단위 비교
                for i in range(0, n - 4, 4):
                    bv = struct.unpack_from('<I', before_data, i)[0]
                    av = struct.unpack_from('<I', after_data,  i)[0]
                    if bv != av:
                        # 인덱스 변화 (0~15 범위)
                        if 0 <= bv <= 15 and 0 <= av <= 15:
                            candidates.append((base2 + i, bv, av, 'INDEX'))
                        # 링버퍼 슬롯 주소 범위 포인터
                        ring_abs = mod_base + PREV_RVA
                        if ring_abs <= bv < ring_abs + SLOT_SIZE*SLOT_COUNT or \
                           ring_abs <= av < ring_abs + SLOT_SIZE*SLOT_COUNT:
                            candidates.append((base2 + i, bv, av, 'ADDR32'))
        addr2 = base2 + size2
        if addr2 <= (mbi2.BaseAddress or 0): break

    if candidates:
        for abs_addr, bv, av, kind in candidates:
            rva = abs_addr - mod_base
            print(f'  [{kind}] 0x{abs_addr:X} (RVA=0x{rva:X}): {bv} → {av}')
    else:
        print('  없음 — write head는 PRIVATE 영역에 있을 수 있음')

    kernel32.CloseHandle(hp)
