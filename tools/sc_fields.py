"""
SC:R 전적 API 응답 전체 필드 덤프
- netstat으로 포트 탐색 (메모리 스캔 없음)
- 현재 게임 플레이어 목록 읽기
- 모든 게이트웨이 시도
"""
import sys, ctypes, ctypes.wintypes, urllib.request, json, subprocess, re
sys.stdout.reconfigure(encoding='utf-8')

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [("dwSize",ctypes.wintypes.DWORD),("cntUsage",ctypes.wintypes.DWORD),
                ("th32ProcessID",ctypes.wintypes.DWORD),("th32DefaultHeapID",ctypes.POINTER(ctypes.c_ulong)),
                ("th32ModuleID",ctypes.wintypes.DWORD),("cntThreads",ctypes.wintypes.DWORD),
                ("th32ParentProcessID",ctypes.wintypes.DWORD),("pcPriClassBase",ctypes.c_long),
                ("dwFlags",ctypes.wintypes.DWORD),("szExeFile",ctypes.c_char*260)]

class MODULEENTRY32(ctypes.Structure):
    _fields_ = [("dwSize",ctypes.wintypes.DWORD),("th32ModuleID",ctypes.wintypes.DWORD),
                ("th32ProcessID",ctypes.wintypes.DWORD),("GlblcntUsage",ctypes.wintypes.DWORD),
                ("ProccntUsage",ctypes.wintypes.DWORD),("modBaseAddr",ctypes.POINTER(ctypes.c_byte)),
                ("modBaseSize",ctypes.wintypes.DWORD),("hModule",ctypes.wintypes.HMODULE),
                ("szModule",ctypes.c_char*256),("szExePath",ctypes.c_char*260)]

def find_pid(name):
    snap = kernel32.CreateToolhelp32Snapshot(0x2, 0)
    e = PROCESSENTRY32(); e.dwSize = ctypes.sizeof(PROCESSENTRY32)
    kernel32.Process32First(snap, ctypes.byref(e))
    while True:
        if e.szExeFile.lower() == name.lower().encode():
            kernel32.CloseHandle(snap); return e.th32ProcessID
        if not kernel32.Process32Next(snap, ctypes.byref(e)): break
    kernel32.CloseHandle(snap); return None

def get_module_base(pid):
    snap = kernel32.CreateToolhelp32Snapshot(0x8, pid)
    e = MODULEENTRY32(); e.dwSize = ctypes.sizeof(MODULEENTRY32)
    if not kernel32.Module32First(snap, ctypes.byref(e)):
        kernel32.CloseHandle(snap); return 0
    base = 0
    while True:
        if e.szModule.lower() == b"starcraft.exe":
            base = ctypes.cast(e.modBaseAddr, ctypes.c_void_p).value; break
        if not kernel32.Module32Next(snap, ctypes.byref(e)): break
    kernel32.CloseHandle(snap); return base or 0

def read_mem(hProc, addr, size):
    buf = (ctypes.c_ubyte * size)()
    rd = ctypes.c_size_t(0)
    if kernel32.ReadProcessMemory(hProc, ctypes.c_void_p(addr), buf, size, ctypes.byref(rd)):
        return bytes(buf[:rd.value])
    return b''

def find_port_netstat(pid):
    """netstat으로 SC 프로세스가 리슨 중인 로컬 포트 탐색 (관리자 불필요)"""
    candidates = set()
    try:
        out = subprocess.check_output("netstat -ano", shell=True, text=True, errors='ignore')
        # SC PID 관련 줄 출력 (디버그)
        pid_lines = [l for l in out.splitlines() if str(pid) in l]
        print(f"  netstat SC 관련 줄 {len(pid_lines)}개:")
        for l in pid_lines[:20]: print(f"    {l.strip()}")
        # 127.0.0.1 LISTENING 전체 수집
        listen_lines = [l for l in out.splitlines() if "127.0.0.1" in l and ("LISTEN" in l or str(pid) in l)]
        print(f"  127.0.0.1 LISTEN/SC 줄 {len(listen_lines)}개:")
        for l in listen_lines[:30]: print(f"    {l.strip()}")
        for line in out.splitlines():
            if "127.0.0.1" in line and ("LISTEN" in line or str(pid) in line):
                m = re.search(r'127\.0\.0\.1:(\d+)', line)
                if m:
                    candidates.add(int(m.group(1)))
    except Exception as e:
        print(f"  netstat 오류: {e}")
    # SC 자신의 LISTENING 포트 우선 처리 (PID 기준)
    sc_listen = set()
    try:
        out2 = subprocess.check_output("netstat -ano", shell=True, text=True, errors='ignore')
        for line in out2.splitlines():
            if str(pid) in line and "LISTEN" in line:
                m = re.search(r'127\.0\.0\.1:(\d+)', line)
                if m:
                    sc_listen.add(int(m.group(1)))
    except: pass
    # SC 포트를 맨 앞에, 나머지는 뒤에
    ordered = list(sc_listen) + [p for p in sorted(candidates) if p not in sc_listen]
    print(f"  SC LISTEN 포트: {sorted(sc_listen)}")
    print(f"  전체 후보 포트: {ordered[:10]}...")
    # 프로브: /web-api/ 뿐 아니라 실제 API URL도 시도
    test_name = "test"
    for p in ordered:
        for path in ["/web-api/", f"/web-api/v2/aurora-profile-by-toon/{test_name}/30?request_flags=scr_tooninfo"]:
            try:
                req = urllib.request.Request(f"http://127.0.0.1:{p}{path}",
                    headers={"User-Agent":"StarCraft/1.0"})
                with urllib.request.urlopen(req, timeout=2) as r:
                    print(f"  포트 {p} {path[:20]} -> HTTP {r.status}")
                    if r.status in (200, 404):
                        return p
            except Exception as e:
                if "timed out" not in str(e) and "refused" not in str(e):
                    print(f"  포트 {p} {path[:20]} -> {type(e).__name__}: {e}")
    return None

def read_players(hProc, base):
    PLAYER_TABLE = 0x10931B0
    SLOT_SIZE = 104; NAME_OFF = 8
    buf = read_mem(hProc, base + PLAYER_TABLE, SLOT_SIZE * 8)
    players = []
    for i in range(8):
        slot = buf[i*SLOT_SIZE:(i+1)*SLOT_SIZE]
        if len(slot) < NAME_OFF+1 or slot[0] != 0x01: continue
        nb = slot[NAME_OFF:]; null = nb.find(b'\x00')
        if null > 0:
            try: players.append(nb[:null].decode('utf-8'))
            except: players.append(nb[:null].decode('cp949', errors='replace'))
    return players

def query(name, port):
    import urllib.parse
    encoded = urllib.parse.quote(name, safe='')
    for gw, gwname in [(30,"KR"),(20,"Asia"),(10,"USW"),(11,"USE"),(12,"EU")]:
        url = f'http://127.0.0.1:{port}/web-api/v2/aurora-profile-by-toon/{encoded}/{gw}?request_flags=scr_tooninfo'
        try:
            req = urllib.request.Request(url, headers={"User-Agent":"StarCraft/1.0"})
            with urllib.request.urlopen(req, timeout=5) as r:
                raw = r.read().decode('utf-8')
                data = json.loads(raw)
            bt = data.get('battle_tag','')
            print(f"    {gwname}: battle_tag={repr(bt)}, keys={list(data.keys())[:5]}")
            if not bt: continue
            return gwname, gw, data
        except Exception as e:
            print(f"    {gwname}: {e}")
    return None, None, None

# ── 메인 ──────────────────────────────────────────
pid = find_pid("StarCraft.exe")
if not pid: print("StarCraft 실행 안됨"); exit()
print(f"PID: {pid}")

hProc = kernel32.OpenProcess(0x1F0FFF, False, pid)
base  = get_module_base(pid)
print(f"베이스: 0x{base:X}")

port = find_port_netstat(pid)
if not port:
    print("포트 탐색 실패 - 인게임 상태인지 확인하세요"); exit()
print(f"포트: {port}")

players = read_players(hProc, base)
kernel32.CloseHandle(hProc)
print(f"플레이어: {players}")

for name in players:
    gwname, gw, data = query(name, port)
    if not data:
        print(f"  [{name}] not found"); continue

    print(f"\n{'='*60}")
    print(f"[{name}] @ {gwname}(gw={gw})  battle_tag={data['battle_tag']}")
    print("── 최상위 키 ──")
    for k, v in data.items():
        info = f"len={len(v)}" if isinstance(v, list) else str(v)[:60]
        print(f"  {k}: {type(v).__name__}  {info}")

    ms = data.get('matchmaked_stats', [])
    if ms:
        print(f"\n── matchmaked_stats[0] ({len(ms)}개 중 첫번째) ──")
        print(json.dumps(ms[0], indent=2, ensure_ascii=False))
