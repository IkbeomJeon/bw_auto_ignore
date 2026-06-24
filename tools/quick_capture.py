"""
빠른 귓말 주소 캡처
사용법:
  python quick_capture.py capture <내용>   # 귓말 내용으로 빠르게 주소 저장
  python quick_capture.py check            # 저장된 주소 재확인 (메시지 사라진 후)
"""
import ctypes, ctypes.wintypes, sys, os, json, time
sys.stdout.reconfigure(encoding='utf-8')

PROCESS_VM_READ           = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS        = 0x00000002
MEM_COMMIT   = 0x1000
MEM_IMAGE    = 0x1000000
MEM_PRIVATE  = 0x20000
MEM_MAPPED   = 0x40000
kernel32 = ctypes.windll.kernel32

SAVE_PATH = os.path.join(os.path.dirname(__file__), 'snaps', 'captured_addrs.json')

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

def type_str(t):
    if t == MEM_IMAGE:   return 'IMAGE'
    if t == MEM_PRIVATE: return 'PRIVATE'
    if t == MEM_MAPPED:  return 'MAPPED'
    return f'{t:#x}'

def fast_scan(hp, keywords):
    """여러 키워드를 한 번의 메모리 순회로 검색. 결과: [(addr, keyword, type, ctx)]"""
    mbi = MEMORY_BASIC_INFORMATION()
    sz  = ctypes.sizeof(mbi)
    results = []
    addr = 0
    while kernel32.VirtualQueryEx(hp, ctypes.c_void_p(addr), ctypes.byref(mbi), sz) == sz:
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize
        next_addr = base + size
        if (mbi.State == MEM_COMMIT and
                mbi.Protect not in (0x01, 0x100) and
                size < 0x10000000):
            data = read_mem(hp, base, size)
            for kw in keywords:
                pos = 0
                while True:
                    idx = data.find(kw, pos)
                    if idx == -1: break
                    abs_addr = base + idx
                    ctx = data[max(0,idx-64):idx+len(kw)+64]
                    results.append({
                        'addr': abs_addr,
                        'kw': kw.decode('utf-8', errors='replace'),
                        'type': mbi.Type,
                        'protect': mbi.Protect,
                        'ctx_hex': ctx.hex(),
                        'ctx_text': ''.join(chr(b) if 32<=b<127 else '.' for b in ctx),
                    })
                    pos = idx + 1
        if next_addr <= addr: break
        addr = next_addr
    return results

def cmd_capture(content):
    pid = find_sc_pid()
    if not pid: print('SC 없음'); return
    hp = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)

    keywords = [
        content.encode('utf-8'),
        content.encode('cp949', errors='ignore'),
        content.encode('utf-16-le'),
    ]
    # 중복 제거
    keywords = list(dict.fromkeys(k for k in keywords if k))

    t0 = time.time()
    results = fast_scan(hp, keywords)
    kernel32.CloseHandle(hp)
    elapsed = time.time() - t0

    print(f'스캔 완료: {elapsed:.1f}초  발견: {len(results)}개')
    for r in results:
        t = type_str(r['type'])
        print(f"  0x{r['addr']:016X} [{t}] kw={r['kw']!r}")
        print(f"    ctx: {r['ctx_text']}")

    os.makedirs(os.path.dirname(SAVE_PATH), exist_ok=True)
    with open(SAVE_PATH, 'w') as f:
        json.dump({'content': content, 'results': results}, f)
    print(f'\n저장 완료: {SAVE_PATH}')
    print('→ 메시지가 화면에서 사라진 후 "check" 실행하세요.')

def cmd_check():
    if not os.path.exists(SAVE_PATH):
        print('캡처 파일 없음. 먼저 capture 실행하세요.'); return

    with open(SAVE_PATH) as f:
        data = json.load(f)

    content  = data['content']
    results  = data['results']
    print(f'검증 대상: {len(results)}개 주소  (content={content!r})')

    pid = find_sc_pid()
    if not pid: print('SC 없음'); return
    hp = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)

    keywords = [
        content.encode('utf-8'),
        content.encode('cp949', errors='ignore'),
        content.encode('utf-16-le'),
    ]
    keywords = list(dict.fromkeys(k for k in keywords if k))

    still_there = 0
    gone = 0
    for r in results:
        addr = r['addr']
        kw   = r['kw'].encode('utf-8', errors='replace')
        # 해당 주소에서 직접 읽기
        size = len(kw) + 4
        chunk = read_mem(hp, addr, size)
        found = any(kw_bytes in chunk for kw_bytes in keywords)
        status = '✓ 남아있음' if found else '✗ 사라짐'
        t = type_str(r['type'])
        print(f"  {status}  0x{addr:016X} [{t}]")
        if found: still_there += 1
        else: gone += 1

    kernel32.CloseHandle(hp)
    print(f'\n결과: 남아있음={still_there}  사라짐={gone}')
    if still_there > 0 and gone == 0:
        print('→ 모두 남아있음: 힙이 free되지 않았거나 정적 메모리')
    elif gone > 0:
        print('→ 일부/전부 사라짐: 동적 메모리이며 free됨')

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(0)
    if sys.argv[1] == 'capture' and len(sys.argv) >= 3:
        cmd_capture(sys.argv[2])
    elif sys.argv[1] == 'check':
        cmd_check()
    else:
        print(__doc__)
