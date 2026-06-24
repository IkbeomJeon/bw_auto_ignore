"""
SC:R 메모리에서 리플레이 URL 목록 + Blizzard API 엔드포인트 추출
- 게임 실행 중, 인게임 기록 화면 열어놓고 실행
"""
import sys, ctypes, ctypes.wintypes, re, time
sys.stdout.reconfigure(encoding='utf-8')

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

MEM_COMMIT          = 0x1000
MEM_PRIVATE         = 0x20000
MEM_MAPPED          = 0x40000
MEM_IMAGE           = 0x1000000
PAGE_EXECUTE_READ   = 0x20   # 코드 섹션 - 제외
PAGE_EXECUTE_RW     = 0x40   # 코드+쓰기 - 제외

def find_pid(name):
    snap = kernel32.CreateToolhelp32Snapshot(0x2, 0)
    e = PROCESSENTRY32(); e.dwSize = ctypes.sizeof(e)
    kernel32.Process32First(snap, ctypes.byref(e))
    while True:
        if e.szExeFile.lower() == name.lower().encode():
            kernel32.CloseHandle(snap); return e.th32ProcessID
        if not kernel32.Process32Next(snap, ctypes.byref(e)): break
    kernel32.CloseHandle(snap); return None

def read_mem(hp, addr, size):
    buf = (ctypes.c_ubyte * size)()
    r = ctypes.c_size_t(0)
    kernel32.ReadProcessMemory(hp, ctypes.c_void_p(addr), buf, size, ctypes.byref(r))
    return bytes(buf[:r.value])

def scan_all_readable(hp):
    """모든 읽기 가능한 커밋된 영역 스캔"""
    regions = []
    mbi = MEMORY_BASIC_INFORMATION()
    addr = 0
    READABLE = {0x02, 0x04, 0x08, 0x20, 0x40, 0x80}  # PAGE_*READ*
    while kernel32.VirtualQueryEx(hp, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) == ctypes.sizeof(mbi):
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize
        protect = mbi.Protect & 0xFF  # 하위 8비트 (GUARD 등 제거)
        mem_type = mbi.Type
        # IMAGE는 .rdata(하드코딩 URL 포함 가능)만 포함, 코드섹션(EXECUTE) 제외
        if mem_type == MEM_IMAGE and protect in (PAGE_EXECUTE_READ, PAGE_EXECUTE_RW):
            next_addr = base + size
            if next_addr <= addr: break
            addr = next_addr
            continue
        if (mbi.State == MEM_COMMIT and protect in READABLE
                and mem_type in (MEM_PRIVATE, MEM_MAPPED, MEM_IMAGE)
                and 256 <= size <= 64*1024*1024):
            regions.append((base, size, mem_type))
        next_addr = base + size
        if next_addr <= addr: break
        addr = next_addr
    return regions

pid = find_pid("StarCraft.exe")
if not pid:
    print("StarCraft 실행 안됨"); exit()
print(f"PID: {pid}")

hp = kernel32.OpenProcess(0x1F0FFF, False, pid)

print("메모리 스캔 중...")
t = time.time()
regions = scan_all_readable(hp)
private_cnt = sum(1 for r in regions if r[2] == MEM_PRIVATE)
mapped_cnt  = sum(1 for r in regions if r[2] == MEM_MAPPED)
image_cnt   = sum(1 for r in regions if r[2] == MEM_IMAGE)
print(f"영역 수: {len(regions)}개 (PRIVATE={private_cnt}, MAPPED={mapped_cnt}, IMAGE.rdata={image_cnt}), 스캔 시작...")

# 검색 패턴
GCS_PAT   = re.compile(rb'https?://storage\.googleapis\.com/starcraft[^\x00\s"\'<>]{10,}')
BLIZ_PAT  = re.compile(rb'https?://[^\x00\s"\'<>]*(?:blizzard|battle\.net|classic)[^\x00\s"\'<>]{5,}')
REPLAY_API_PAT = re.compile(rb'https?://[^\x00\s"\'<>]*replay[^\x00\s"\'<>]{5,}')

gcs_urls = set()
bliz_urls = set()

scanned = 0
for base, size, _ in regions:
    data = read_mem(hp, base, size)
    if not data:
        continue

    for m in GCS_PAT.finditer(data):
        url = m.group().decode('utf-8', errors='replace').rstrip('.,;)')
        gcs_urls.add(url)

    for m in BLIZ_PAT.finditer(data):
        url = m.group().decode('utf-8', errors='replace').rstrip('.,;)')
        # 너무 긴 건 제외 (바이너리 오탐)
        if len(url) < 300:
            bliz_urls.add(url)

    for m in REPLAY_API_PAT.finditer(data):
        url = m.group().decode('utf-8', errors='replace').rstrip('.,;)')
        if len(url) < 300:
            bliz_urls.add(url)

    scanned += 1
    if scanned % 500 == 0:
        print(f"  {scanned}/{len(regions)} 영역 처리... GCS={len(gcs_urls)}, Bliz={len(bliz_urls)}")

kernel32.CloseHandle(hp)
elapsed = time.time() - t
print(f"\n완료 ({elapsed:.1f}s)")

print(f"\n=== GCS 리플레이 URL: {len(gcs_urls)}개 ===")
for url in sorted(gcs_urls)[:20]:
    print(" ", url)
if len(gcs_urls) > 20:
    print(f"  ... 외 {len(gcs_urls)-20}개")

# aurora_id 추출
aurora_ids = set()
for url in gcs_urls:
    m = re.search(r'/S1-replays/(\d+)/', url)
    if m:
        aurora_ids.add(m.group(1))
print(f"\naurora_id 목록: {aurora_ids}")

print(f"\n=== Blizzard/Battle.net URL: {len(bliz_urls)}개 ===")
# replay 관련만 따로
replay_related = [u for u in bliz_urls if 'replay' in u.lower() or 'history' in u.lower() or 'match' in u.lower()]
other = [u for u in bliz_urls if u not in replay_related]
print(f"[replay/history/match 관련: {len(replay_related)}개]")
for url in sorted(replay_related)[:30]:
    print(" ", url)
print(f"[기타: {len(other)}개 (앞 10개)]")
for url in sorted(other)[:10]:
    print(" ", url)

# 결과 저장
import os, json
out = {
    "gcs_replay_urls": sorted(gcs_urls),
    "aurora_ids": list(aurora_ids),
    "blizzard_urls": sorted(bliz_urls),
}
out_path = r"C:\wd\bw_auto_ignore\tools\snaps\replay_scan_result.json"
with open(out_path, 'w', encoding='utf-8') as f:
    json.dump(out, f, ensure_ascii=False, indent=2)
print(f"\n결과 저장: {out_path}")
