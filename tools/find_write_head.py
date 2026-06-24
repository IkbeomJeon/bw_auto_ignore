"""
write head 포인터 탐색
사용법:
  python find_write_head.py before   # 전송 전 스냅샷
  python find_write_head.py after    # 전송 후 비교 → 변경된 값 = write head 포인터 후보
"""
import ctypes, ctypes.wintypes, sys, os, json, struct
sys.stdout.reconfigure(encoding='utf-8')

PROCESS_VM_READ           = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS        = 0x00000002
kernel32 = ctypes.windll.kernel32

RING_BASE  = 0x00007FF63A2DE0CB   # 슬롯 0 시작 주소
SLOT_SIZE  = 0xDA                  # 218바이트
SLOT_COUNT = 10
RING_SIZE  = SLOT_SIZE * SLOT_COUNT

# write head 포인터는 ring buffer 근처에 있을 가능성이 높음 → ±32KB 스캔
SCAN_BASE  = RING_BASE - 0x8000
SCAN_SIZE  = RING_SIZE + 0x10000

SNAP_PATH  = os.path.join(os.path.dirname(__file__), 'snaps', 'write_head_before.json')

class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [('dwSize',ctypes.wintypes.DWORD),('cntUsage',ctypes.wintypes.DWORD),
                ('th32ProcessID',ctypes.wintypes.DWORD),('th32DefaultHeapID',ctypes.POINTER(ctypes.c_ulong)),
                ('th32ModuleID',ctypes.wintypes.DWORD),('cntThreads',ctypes.wintypes.DWORD),
                ('th32ParentProcessID',ctypes.wintypes.DWORD),('pcPriClassBase',ctypes.c_long),
                ('dwFlags',ctypes.wintypes.DWORD),('szExeFile',ctypes.c_char*260)]

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

def read_slots(hp):
    slots = []
    for i in range(SLOT_COUNT):
        addr = RING_BASE + i * SLOT_SIZE
        data = read_mem(hp, addr, SLOT_SIZE)
        text = data.split(b'\x00')[0].decode('ascii', errors='replace')
        slots.append({'idx': i, 'addr': addr, 'text': text, 'raw': data.hex()})
    return slots

def read_scan_area(hp):
    return read_mem(hp, SCAN_BASE, SCAN_SIZE)

pid = find_sc_pid()
if not pid: print('SC 없음'); sys.exit(1)
hp = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)

if sys.argv[1] == 'before':
    slots = read_slots(hp)
    scan  = read_scan_area(hp)
    os.makedirs(os.path.dirname(SNAP_PATH), exist_ok=True)
    with open(SNAP_PATH, 'w') as f:
        json.dump({'slots': slots, 'scan': scan.hex(),
                   'scan_base': SCAN_BASE, 'ring_base': RING_BASE}, f)
    print('=== 현재 슬롯 상태 ===')
    for s in slots:
        print(f"  슬롯[{s['idx']}] 0x{s['addr']:X}: {s['text']!r}")
    print(f'\n[before] 저장 완료. 이제 귓말을 보내세요.')

elif sys.argv[1] == 'after':
    with open(SNAP_PATH) as f:
        snap = json.load(f)
    before_slots = snap['slots']
    before_scan  = bytes.fromhex(snap['scan'])
    scan_base    = snap['scan_base']

    after_slots = read_slots(hp)
    after_scan  = read_scan_area(hp)

    # ── 슬롯 변화 ──────────────────────────────────────────
    print('=== 슬롯 변화 ===')
    changed_slot = None
    for b, a in zip(before_slots, after_slots):
        changed = b['text'] != a['text']
        mark = '★변경' if changed else '      '
        print(f"  {mark} 슬롯[{a['idx']}] 0x{a['addr']:X}: {b['text']!r} → {a['text']!r}")
        if changed:
            changed_slot = a

    if changed_slot:
        print(f"\n→ 새 귓말 슬롯: [{changed_slot['idx']}] {changed_slot['text']!r}")
        new_idx  = changed_slot['idx']
        new_addr = changed_slot['addr']

        # ── 주변 메모리에서 포인터 변화 탐색 ──────────────
        print(f'\n=== write head 포인터 후보 탐색 ===')
        print(f'(슬롯 인덱스={new_idx}, 슬롯 주소=0x{new_addr:X})')

        found = []
        for i in range(0, min(len(before_scan), len(after_scan)) - 8, 4):
            bval = struct.unpack_from('<Q', before_scan, i)[0]
            aval = struct.unpack_from('<Q', after_scan,  i)[0]
            if bval == aval:
                continue
            abs_addr = scan_base + i
            # 포인터가 ring buffer 슬롯 주소 범위를 가리키는지 확인
            in_ring_before = RING_BASE <= bval < RING_BASE + RING_SIZE
            in_ring_after  = RING_BASE <= aval < RING_BASE + RING_SIZE
            # 또는 슬롯 인덱스 변화 (0~9)
            is_idx_change  = (0 <= bval <= SLOT_COUNT and 0 <= aval <= SLOT_COUNT)

            if in_ring_before or in_ring_after or is_idx_change:
                found.append((abs_addr, bval, aval, in_ring_before or in_ring_after, is_idx_change))

        if found:
            for abs_addr, bval, aval, is_ptr, is_idx in found:
                kind = '★주소포인터' if is_ptr else ('★인덱스' if is_idx else '')
                print(f'  {kind} 0x{abs_addr:X}: 0x{bval:X} → 0x{aval:X}')
        else:
            print('  (없음 — 더 넓은 범위 탐색 필요)')

        # ── 4바이트 단위로도 확인 ──────────────────────────
        print(f'\n=== 4바이트 단위 변화 (링버퍼 주소/인덱스 후보) ===')
        found4 = []
        for i in range(0, min(len(before_scan), len(after_scan)) - 4, 4):
            bval = struct.unpack_from('<I', before_scan, i)[0]
            aval = struct.unpack_from('<I', after_scan,  i)[0]
            if bval == aval: continue
            abs_addr = scan_base + i
            is_idx  = (0 <= bval <= SLOT_COUNT and 0 <= aval <= SLOT_COUNT)
            low_b   = RING_BASE & 0xFFFFFFFF
            in_ring = (low_b <= bval < low_b + RING_SIZE) or (low_b <= aval < low_b + RING_SIZE)
            if is_idx or in_ring:
                found4.append((abs_addr, bval, aval))
        for abs_addr, bval, aval in found4:
            print(f'  0x{abs_addr:X}: {bval} → {aval}')
        if not found4:
            print('  (없음)')
    else:
        print('\n슬롯 변화 없음 — 귓말이 아직 안 왔거나 버퍼가 가득 찼을 수 있음')

kernel32.CloseHandle(hp)
