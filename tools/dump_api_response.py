"""
SC:R 전적 API 응답 전체 JSON 덤프 (분석용)
- 게임 중 실행 필요
- 결과를 tools/snaps/api_response_<이름>.json 에 저장
"""
import sys, ctypes, ctypes.wintypes, urllib.request, json, subprocess, re, os
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

def find_port(pid):
    candidates = set()
    sc_listen = set()
    try:
        out = subprocess.check_output("netstat -ano", shell=True, text=True, errors='ignore')
        for line in out.splitlines():
            if "127.0.0.1" in line and ("LISTEN" in line or str(pid) in line):
                m = re.search(r'127\.0\.0\.1:(\d+)', line)
                if m:
                    candidates.add(int(m.group(1)))
            if str(pid) in line and "LISTEN" in line:
                m = re.search(r'127\.0\.0\.1:(\d+)', line)
                if m:
                    sc_listen.add(int(m.group(1)))
    except: pass
    ordered = list(sc_listen) + [p for p in sorted(candidates) if p not in sc_listen]
    for p in ordered:
        try:
            req = urllib.request.Request(f"http://127.0.0.1:{p}/web-api/",
                headers={"User-Agent":"StarCraft/1.0"})
            with urllib.request.urlopen(req, timeout=2) as r:
                if r.status in (200, 404): return p
        except: pass
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

def query_full(name, port):
    import urllib.parse
    encoded = urllib.parse.quote(name, safe='')
    best = None
    for gw, gwname in [(30,"KR"),(20,"Asia"),(10,"USW"),(11,"USE"),(12,"EU")]:
        url = f'http://127.0.0.1:{port}/web-api/v2/aurora-profile-by-toon/{encoded}/{gw}?request_flags=scr_tooninfo'
        try:
            req = urllib.request.Request(url, headers={"User-Agent":"StarCraft/1.0"})
            with urllib.request.urlopen(req, timeout=5) as r:
                raw = r.read().decode('utf-8')
                data = json.loads(raw)
            print(f"    {gwname}(gw={gw}): battle_tag={repr(data.get('battle_tag',''))}, keys={list(data.keys())}")
            if data.get('battle_tag'):
                return gwname, gw, data, raw
            if best is None and data:
                best = (gwname, gw, data, raw)  # battle_tag 없어도 일단 저장
        except Exception as e:
            print(f"    {gwname}(gw={gw}): {e}")
    if best:
        print(f"  battle_tag 없지만 응답 있는 첫 번째 게이트웨이 사용: {best[0]}")
        return best
    return None, None, None, None

# ── 메인 ──────────────────────────────────────────
pid = find_pid("StarCraft.exe")
if not pid: print("StarCraft 실행 안됨"); exit()
print(f"PID: {pid}")

hProc = kernel32.OpenProcess(0x1F0FFF, False, pid)
base  = get_module_base(pid)
port  = find_port(pid)
if not port: print("포트 탐색 실패 - 게임 중인지 확인"); exit()
print(f"포트: {port}")

players = read_players(hProc, base)
kernel32.CloseHandle(hProc)
print(f"플레이어: {players}")

out_dir = os.path.join(os.path.dirname(__file__), "snaps")
os.makedirs(out_dir, exist_ok=True)

for name in players:
    gwname, gw, data, raw = query_full(name, port)
    if not data:
        print(f"  [{name}] not found"); continue

    # 파일 저장
    safe_name = re.sub(r'[\\/:*?"<>|]', '_', name)
    out_path = os.path.join(out_dir, f"api_response_{safe_name}.json")
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(raw)
    print(f"\n[{name}] @ {gwname}(gw={gw}) → 저장: {out_path}")

    # 구조 요약 출력
    print("── 최상위 키 ──")
    for k, v in data.items():
        info = f"len={len(v)}" if isinstance(v, list) else str(v)[:80]
        print(f"  {k}: {type(v).__name__}  {info}")

    # stats[] 첫 번째 항목 전체 출력
    stats = data.get('stats', [])
    if stats:
        print(f"\n── stats[0] ({len(stats)}개 중 첫번째) 전체 키 ──")
        print(json.dumps(stats[0], indent=2, ensure_ascii=False))

    # matchmaked_stats[] 첫 번째 항목 전체 출력
    ms = data.get('matchmaked_stats', [])
    if ms:
        print(f"\n── matchmaked_stats[0] ({len(ms)}개 중 첫번째) 전체 키 ──")
        print(json.dumps(ms[0], indent=2, ensure_ascii=False))
