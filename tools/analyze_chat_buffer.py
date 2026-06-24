"""
SC:R 인게임 채팅 버퍼 구조 분석기

사용법:
  python analyze_chat_buffer.py before
      → 귓말 받기 전 스냅샷 저장

  python analyze_chat_buffer.py after <귓말내용>
      → 귓말 받은 후 해당 내용으로 메모리 검색
        static/dynamic 구분, 주변 구조 덤프
"""
import ctypes, ctypes.wintypes, sys, os, json, struct
sys.stdout.reconfigure(encoding='utf-8')

PROCESS_VM_READ           = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS        = 0x00000002
TH32CS_SNAPMODULE         = 0x00000008
TH32CS_SNAPMODULE32       = 0x00000010
MEM_COMMIT   = 0x1000
kernel32 = ctypes.windll.kernel32

SNAP_PATH = os.path.join(os.path.dirname(__file__), 'snaps', 'chat_before.json')

# ── 구조체 ────────────────────────────────────────────────────────────
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

MEM_IMAGE   = 0x1000000
MEM_MAPPED  = 0x40000
MEM_PRIVATE = 0x20000

# ── 유틸 ─────────────────────────────────────────────────────────────
def find_sc_pid():
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32(); pe.dwSize = ctypes.sizeof(pe)
    if kernel32.Process32First(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.lower() == b'starcraft.exe':
                pid = pe.th32ProcessID; kernel32.CloseHandle(snap); return pid
            if not kernel32.Process32Next(snap, ctypes.byref(pe)): break
    kernel32.CloseHandle(snap); return None

def get_module_range(pid):
    """starcraft.exe 모듈의 base, size 반환 (정적 영역 판별용)"""
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(me)
    if kernel32.Module32First(snap, ctypes.byref(me)):
        while True:
            if me.szModule.lower() == b'starcraft.exe':
                base = ctypes.addressof(me.modBaseAddr.contents)
                size = me.modBaseSize
                kernel32.CloseHandle(snap)
                return base, size
            if not kernel32.Module32Next(snap, ctypes.byref(me)): break
    kernel32.CloseHandle(snap)
    return None, None

def read_mem(hp, addr, size):
    buf = (ctypes.c_ubyte * size)()
    r = ctypes.c_size_t(0)
    kernel32.ReadProcessMemory(hp, ctypes.c_void_p(addr), buf, size, ctypes.byref(r))
    return bytes(buf[:r.value])

def region_type_str(mbi_type):
    if mbi_type == MEM_IMAGE:   return 'IMAGE(정적)'
    if mbi_type == MEM_MAPPED:  return 'MAPPED'
    if mbi_type == MEM_PRIVATE: return 'PRIVATE(힙/스택)'
    return f'TYPE={mbi_type:#x}'

def protect_str(p):
    table = {0x02:'RO', 0x04:'RW', 0x08:'RW_COPY', 0x10:'EXEC',
             0x20:'EXEC_R', 0x40:'EXEC_RW', 0x80:'EXEC_RW_COPY'}
    return table.get(p & 0xFF, f'{p:#x}')

def hex_dump(data, base_addr, width=16):
    lines = []
    for i in range(0, len(data), width):
        chunk = data[i:i+width]
        hex_part  = ' '.join(f'{b:02X}' for b in chunk)
        ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        lines.append(f'  {base_addr+i:016X}  {hex_part:<{width*3}}  {ascii_part}')
    return '\n'.join(lines)

def scan_keyword_all(hp, keyword_bytes, ctx_size=256):
    """모든 committed 메모리에서 keyword 검색, (abs_addr, mbi) 반환"""
    mbi = MEMORY_BASIC_INFORMATION()
    sz  = ctypes.sizeof(mbi)
    results = []
    addr = 0
    while kernel32.VirtualQueryEx(hp, ctypes.c_void_p(addr), ctypes.byref(mbi), sz) == sz:
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize
        next_addr = base + size
        if (mbi.State == MEM_COMMIT and
                mbi.Protect not in (0x01, 0x100) and  # PAGE_NOACCESS, PAGE_GUARD
                size < 0x10000000):
            data = read_mem(hp, base, size)
            pos = 0
            while True:
                idx = data.find(keyword_bytes, pos)
                if idx == -1: break
                abs_addr = base + idx
                ctx_start = max(0, idx - ctx_size)
                ctx_end   = min(len(data), idx + len(keyword_bytes) + ctx_size)
                ctx = data[ctx_start:ctx_end]
                results.append((abs_addr, bytes(mbi.Type), mbi.Protect, ctx, idx - ctx_start))
                pos = idx + 1
        if next_addr <= addr: break
        addr = next_addr
    return results

# ── before 모드 ───────────────────────────────────────────────────────
def cmd_before(pid, hp, mod_base, mod_size):
    os.makedirs(os.path.dirname(SNAP_PATH), exist_ok=True)

    # 현재 whisperInbound 주소 목록 저장
    TERM = b'"type":"whisperInbound"}]'
    TERM_ESC = b'\\"type\\":\\"whisperInbound\\"}]'

    addrs = []
    for kw in (TERM, TERM_ESC):
        for abs_addr, mtype, prot, ctx, kw_off in scan_keyword_all(hp, kw, ctx_size=32):
            addrs.append(abs_addr)

    snap = {'mod_base': mod_base, 'mod_size': mod_size, 'inbound_addrs': addrs}
    with open(SNAP_PATH, 'w') as f:
        json.dump(snap, f)

    print(f'[before] 저장 완료: whisperInbound 주소 {len(addrs)}개')
    print(f'파일: {SNAP_PATH}')
    print('\n이제 인게임에서 귓말을 받으세요. 그 후 after 모드 실행.')

# ── after 모드 ────────────────────────────────────────────────────────
def cmd_after(pid, hp, mod_base, mod_size, content):
    # before 스냅샷 로드
    before_addrs = set()
    if os.path.exists(SNAP_PATH):
        with open(SNAP_PATH) as f:
            snap = json.load(f)
        before_addrs = set(snap.get('inbound_addrs', []))
        print(f'[before 스냅샷] whisperInbound {len(before_addrs)}개 로드\n')
    else:
        print('[경고] before 스냅샷 없음. after만 분석합니다.\n')

    # ① 귓말 내용을 여러 인코딩으로 검색
    encodings = [
        ('UTF-8',    content.encode('utf-8')),
        ('CP949',    content.encode('cp949',   errors='ignore')),
        ('UTF-16LE', content.encode('utf-16-le')),
    ]

    print('=' * 70)
    print(f'검색어: "{content}"')
    print('=' * 70)

    for enc_name, kw in encodings:
        results = scan_keyword_all(hp, kw, ctx_size=128)
        if not results:
            print(f'\n[{enc_name}] 미발견')
            continue

        print(f'\n[{enc_name}] {len(results)}개 발견')
        for abs_addr, mtype_bytes, prot, ctx, kw_off in results:
            mtype = struct.unpack('<I', mtype_bytes[:4])[0] if len(mtype_bytes) >= 4 else 0

            # 정적/동적 판별
            in_module = (mod_base is not None and mod_base <= abs_addr < mod_base + mod_size)
            locality   = '★정적(IMAGE)' if in_module else region_type_str(mtype)

            print(f'\n  주소: 0x{abs_addr:016X}  [{locality}]  보호={protect_str(prot)}')
            if mod_base:
                rva = abs_addr - mod_base
                print(f'  RVA : 0x{rva:X}  (모듈베이스=0x{mod_base:X})')

            # 컨텍스트 헥스덤프
            ctx_base = abs_addr - kw_off
            print(f'  주변 덤프 ({len(ctx)} bytes):')
            print(hex_dump(ctx, ctx_base))

            # 텍스트로도 출력 (ASCII/UTF-8 가독 범위만)
            readable = ctx.decode('utf-8', errors='replace')
            readable = ''.join(c if c.isprintable() else '·' for c in readable)
            print(f'  텍스트: {readable[:120]}')

    # ② whisperInbound 터미널도 확인 (JSON 방식)
    print('\n' + '=' * 70)
    print('[JSON whisperInbound 검색]')
    TERM     = b'"type":"whisperInbound"}]'
    TERM_ESC = b'\\"type\\":\\"whisperInbound\\"}]'
    NAME_TAG = b'legacyToonName":"'
    NAME_ESC = b'legacyToonName\\":\\"'

    new_count = 0
    for kw in (TERM, TERM_ESC):
        name_tag = NAME_TAG if kw == TERM else NAME_ESC
        results = scan_keyword_all(hp, kw, ctx_size=512)
        for abs_addr, mtype_bytes, prot, ctx, kw_off in results:
            mtype = struct.unpack('<I', mtype_bytes[:4])[0] if len(mtype_bytes) >= 4 else 0
            is_new = abs_addr not in before_addrs
            marker = '★NEW' if is_new else '   '

            # sender 추출
            ni = ctx.rfind(name_tag)
            sender = ''
            if ni != -1:
                ns = ni + len(name_tag)
                ne = ctx.find(b'"', ns)
                if ne != -1: sender = ctx[ns:ne].decode('utf-8', 'replace')

            in_module = (mod_base is not None and mod_base <= abs_addr < mod_base + mod_size)
            locality   = '정적' if in_module else region_type_str(mtype)

            print(f'  {marker} 0x{abs_addr:016X} [{locality}] sender={sender!r}')
            if is_new:
                new_count += 1

    print(f'\n→ 새로 생긴 whisperInbound: {new_count}개')

# ── 메인 ─────────────────────────────────────────────────────────────
if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] not in ('before', 'after'):
        print(__doc__); sys.exit(0)

    pid = find_sc_pid()
    if not pid: print('SC 없음'); sys.exit(1)
    print(f'SC PID: {pid}')

    mod_base, mod_size = get_module_range(pid)
    if mod_base:
        print(f'모듈 베이스: 0x{mod_base:X}  크기: 0x{mod_size:X}')
    else:
        print('[경고] 모듈 베이스를 찾지 못했습니다.')

    hp = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)

    if sys.argv[1] == 'before':
        cmd_before(pid, hp, mod_base, mod_size)
    elif sys.argv[1] == 'after':
        if len(sys.argv) < 3:
            print('사용법: after <귓말내용>'); sys.exit(1)
        cmd_after(pid, hp, mod_base, mod_size, sys.argv[2])

    kernel32.CloseHandle(hp)
