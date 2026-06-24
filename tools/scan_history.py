"""
SC:R 게임 기록 메모리 스캔 도구

사용법:
  python scan_history.py before   -> 조회 전 스냅샷 저장
  python scan_history.py after    -> 조회 후 스냅샷 저장 & 새로 생긴 JSON 출력
"""
import sys, ctypes, ctypes.wintypes, os, json, pickle, re, time
sys.stdout.reconfigure(encoding='utf-8')

SNAP_DIR   = r"C:\wd\bw_auto_ignore\tools\snaps"
SNAP_BEFORE = os.path.join(SNAP_DIR, "hist_before.pkl")
SNAP_AFTER  = os.path.join(SNAP_DIR, "hist_after.pkl")

MEM_COMMIT     = 0x1000
MEM_PRIVATE    = 0x20000
PAGE_READWRITE = 0x04

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [("dwSize",ctypes.wintypes.DWORD),("cntUsage",ctypes.wintypes.DWORD),
                ("th32ProcessID",ctypes.wintypes.DWORD),("th32DefaultHeapID",ctypes.POINTER(ctypes.c_ulong)),
                ("th32ModuleID",ctypes.wintypes.DWORD),("cntThreads",ctypes.wintypes.DWORD),
                ("th32ParentProcessID",ctypes.wintypes.DWORD),("pcPriClassBase",ctypes.c_long),
                ("dwFlags",ctypes.wintypes.DWORD),("szExeFile",ctypes.c_char*260)]

class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [("BaseAddress",ctypes.c_void_p),("AllocationBase",ctypes.c_void_p),
                ("AllocationProtect",ctypes.wintypes.DWORD),("PartitionId",ctypes.wintypes.WORD),
                ("RegionSize",ctypes.c_size_t),("State",ctypes.wintypes.DWORD),
                ("Protect",ctypes.wintypes.DWORD),("Type",ctypes.wintypes.DWORD)]

def find_sc_pid():
    snap = kernel32.CreateToolhelp32Snapshot(0x2, 0)
    e = PROCESSENTRY32(); e.dwSize = ctypes.sizeof(e)
    kernel32.Process32First(snap, ctypes.byref(e))
    while True:
        if e.szExeFile.lower() == b"starcraft.exe":
            kernel32.CloseHandle(snap); return e.th32ProcessID
        if not kernel32.Process32Next(snap, ctypes.byref(e)): break
    kernel32.CloseHandle(snap); return None

def read_mem(hp, addr, size):
    buf = (ctypes.c_ubyte * size)()
    r = ctypes.c_size_t(0)
    kernel32.ReadProcessMemory(hp, ctypes.c_void_p(addr), buf, size, ctypes.byref(r))
    return bytes(buf[:r.value])

def scan_heap(hp):
    """MEM_PRIVATE + READWRITE 힙 영역 전체 스캔, {base: data} 반환"""
    regions = {}
    mbi = MEMORY_BASIC_INFORMATION()
    addr = 0
    while kernel32.VirtualQueryEx(hp, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) == ctypes.sizeof(mbi):
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize
        if (mbi.State == MEM_COMMIT and mbi.Type == MEM_PRIVATE and
                mbi.Protect == PAGE_READWRITE and 256 <= size <= 32*1024*1024):
            data = read_mem(hp, base, size)
            if data:
                regions[base] = data
        next_addr = base + size
        if next_addr <= addr: break
        addr = next_addr
    return regions

def extract_json_objects(data, min_len=100):
    """바이트에서 JSON 오브젝트 후보 추출"""
    results = []
    text = data.decode('utf-8', errors='replace')
    # '{' 로 시작하는 JSON 오브젝트 찾기
    for m in re.finditer(r'\{[^{}]{50,}', text):
        chunk = m.group()
        # 중괄호 균형 맞추기
        depth = 0
        end = -1
        for i, ch in enumerate(chunk):
            if ch == '{': depth += 1
            elif ch == '}':
                depth -= 1
                if depth == 0:
                    end = i + 1
                    break
        if end == -1: continue
        candidate = chunk[:end]
        if len(candidate) < min_len: continue
        try:
            obj = json.loads(candidate)
            results.append(obj)
        except:
            pass
    return results

def do_before():
    pid = find_sc_pid()
    if not pid: print("SC 없음"); return
    hp = kernel32.OpenProcess(0x1F0FFF, False, pid)
    print(f"PID={pid}, 힙 스캔 중...")
    t = time.time()
    regions = scan_heap(hp)
    kernel32.CloseHandle(hp)
    print(f"스캔 완료: {len(regions)}개 영역, {sum(len(v) for v in regions.values())//1024}KB ({time.time()-t:.1f}s)")
    # 베이스 주소 집합만 저장 (데이터는 저장 안 함 - 너무 큼)
    with open(SNAP_BEFORE, 'wb') as f:
        pickle.dump(set(regions.keys()), f)
    print(f"저장: {SNAP_BEFORE}")
    print("\n이제 인게임에서 내 기록을 조회하세요.")
    print("조회 후: python scan_history.py after")

def do_after():
    if not os.path.exists(SNAP_BEFORE):
        print("먼저 'before' 를 실행하세요"); return
    with open(SNAP_BEFORE, 'rb') as f:
        before_bases = pickle.load(f)

    pid = find_sc_pid()
    if not pid: print("SC 없음"); return
    hp = kernel32.OpenProcess(0x1F0FFF, False, pid)
    print(f"PID={pid}, 힙 스캔 중...")
    t = time.time()
    regions = scan_heap(hp)
    kernel32.CloseHandle(hp)
    print(f"스캔 완료: {len(regions)}개 영역 ({time.time()-t:.1f}s)")

    # 새로 생긴 영역만 추출
    new_bases = set(regions.keys()) - before_bases
    print(f"\n조회 전후 새로 생긴 영역: {len(new_bases)}개")

    found_any = False
    for base in sorted(new_bases):
        data = regions[base]
        # 게임/매치 관련 키워드 필터
        keywords = [b'match', b'game', b'race', b'result', b'win', b'lose',
                    b'terran', b'zerg', b'protoss', b'map', b'replay',
                    b'season', b'opponent', b'toon', b'battle_tag']
        hit = any(kw in data.lower() for kw in keywords)
        if not hit: continue

        print(f"\n{'='*60}")
        print(f"주소: 0x{base:X}  크기: {len(data)} bytes")

        # JSON 추출 시도
        objs = extract_json_objects(data)
        if objs:
            print(f"JSON 오브젝트 {len(objs)}개 발견:")
            for obj in objs[:5]:
                print(json.dumps(obj, indent=2, ensure_ascii=False)[:1000])
        else:
            # 텍스트로 출력
            text = data.decode('utf-8', errors='replace')
            # 키워드 주변 context 출력
            for kw in [b'match', b'race', b'result', b'terran', b'zerg', b'protoss']:
                idx = data.lower().find(kw)
                if idx != -1:
                    s = max(0, idx-50); e = min(len(data), idx+200)
                    snippet = data[s:e].decode('utf-8', errors='replace').replace('\x00','.')
                    print(f"  [{kw.decode()}] ...{snippet}...")
                    break
        found_any = True

    if not found_any:
        print("\n키워드 매칭 영역 없음. 전체 새 영역 텍스트 미리보기:")
        for base in sorted(new_bases)[:10]:
            data = regions[base]
            preview = data[:200].decode('utf-8', errors='replace').replace('\x00','.')
            if any(c.isprintable() for c in preview[:50]):
                print(f"  0x{base:X}: {preview[:100]!r}")

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "before":
        do_before()
    elif cmd == "after":
        do_after()
    else:
        print(__doc__)
