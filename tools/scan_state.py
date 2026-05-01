"""
SC:R 화면 상태 메모리 자동 스캐너
실행 후 SC에서 화면을 전환하면 변화된 오프셋을 자동으로 추적합니다.
Ctrl+C로 종료.
"""
import ctypes, ctypes.wintypes, time, sys

PROCESS_VM_READ           = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS        = 0x00000002
TH32CS_SNAPMODULE         = 0x00000008
TH32CS_SNAPMODULE32       = 0x00000010

kernel32 = ctypes.windll.kernel32

class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize",              ctypes.wintypes.DWORD),
        ("cntUsage",            ctypes.wintypes.DWORD),
        ("th32ProcessID",       ctypes.wintypes.DWORD),
        ("th32DefaultHeapID",   ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID",        ctypes.wintypes.DWORD),
        ("cntThreads",          ctypes.wintypes.DWORD),
        ("th32ParentProcessID", ctypes.wintypes.DWORD),
        ("pcPriClassBase",      ctypes.c_long),
        ("dwFlags",             ctypes.wintypes.DWORD),
        ("szExeFile",           ctypes.c_char * 260),
    ]

class MODULEENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize",        ctypes.wintypes.DWORD),
        ("th32ModuleID",  ctypes.wintypes.DWORD),
        ("th32ProcessID", ctypes.wintypes.DWORD),
        ("GlblcntUsage",  ctypes.wintypes.DWORD),
        ("ProccntUsage",  ctypes.wintypes.DWORD),
        ("modBaseAddr",   ctypes.POINTER(ctypes.c_byte)),
        ("modBaseSize",   ctypes.wintypes.DWORD),
        ("hModule",       ctypes.wintypes.HMODULE),
        ("szModule",      ctypes.c_char * 256),
        ("szExePath",     ctypes.c_char * 260),
    ]

def find_sc_pid():
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32()
    pe.dwSize = ctypes.sizeof(pe)
    if kernel32.Process32First(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.lower() == b"starcraft.exe":
                kernel32.CloseHandle(snap)
                return pe.th32ProcessID
            if not kernel32.Process32Next(snap, ctypes.byref(pe)):
                break
    kernel32.CloseHandle(snap)
    return None

def get_module_base(pid):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    me = MODULEENTRY32()
    me.dwSize = ctypes.sizeof(me)
    if kernel32.Module32First(snap, ctypes.byref(me)):
        while True:
            if me.szModule.lower() == b"starcraft.exe":
                base = ctypes.addressof(me.modBaseAddr.contents)
                kernel32.CloseHandle(snap)
                return base
            if not kernel32.Module32Next(snap, ctypes.byref(me)):
                break
    kernel32.CloseHandle(snap)
    return None

def read_mem(hp, addr, size):
    buf = (ctypes.c_ubyte * size)()
    read = ctypes.c_size_t(0)
    kernel32.ReadProcessMemory(hp, ctypes.c_void_p(addr), buf, size, ctypes.byref(read))
    return bytes(buf[:read.value])

SCAN_START = 0x1090000
SCAN_END   = 0x1093000
SCAN_SIZE  = SCAN_END - SCAN_START
IS_IN_GAME = 0x1090612

# 자주 바뀌는 노이즈 오프셋 (타이머 등) 제외용
IGNORE_OFFSETS = set()

def main():
    pid = find_sc_pid()
    if not pid:
        print("StarCraft.exe 를 찾을 수 없습니다.")
        sys.exit(1)

    hp = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    base = get_module_base(pid)
    print(f"SC PID={pid}  베이스=0x{base:X}")
    print(f"스캔 범위: 0x{SCAN_START:X}~0x{SCAN_END:X}")
    print("화면 전환 시 변화를 자동 감지합니다. Ctrl+C로 종료.\n")

    # 노이즈 필터링: 5초간 계속 바뀌는 오프셋은 무시
    print("노이즈 필터링 중 (5초)...")
    baseline = read_mem(hp, base + SCAN_START, SCAN_SIZE)
    noisy = set()
    t_end = time.time() + 5
    while time.time() < t_end:
        snap = read_mem(hp, base + SCAN_START, SCAN_SIZE)
        for i in range(min(len(baseline), len(snap))):
            if baseline[i] != snap[i]:
                noisy.add(i)
        time.sleep(0.2)
    print(f"노이즈 오프셋 {len(noisy)}개 제외\n")

    # 안정된 베이스라인 새로 잡기
    baseline = read_mem(hp, base + SCAN_START, SCAN_SIZE)
    print("베이스라인 저장 완료. 이제 SC에서 화면을 전환하세요.\n")

    # 변화 추적
    # 오프셋별로 처음 변화한 시각과 값 기록
    changed = {}  # offset_idx -> (old_val, new_val, timestamp)

    try:
        while True:
            snap = read_mem(hp, base + SCAN_START, SCAN_SIZE)
            now = time.strftime("%H:%M:%S")
            new_changes = []
            for i in range(min(len(baseline), len(snap))):
                if i in noisy:
                    continue
                if baseline[i] != snap[i]:
                    offset = SCAN_START + i
                    key = i
                    if key not in changed:
                        changed[key] = (baseline[i], snap[i], now)
                        new_changes.append((offset, baseline[i], snap[i]))

            if new_changes:
                print(f"[{now}] 변화 감지 ({len(new_changes)}개):")
                for off, ov, nv in new_changes:
                    marker = " ★IS_IN_GAME" if off == IS_IN_GAME else ""
                    print(f"  0x{off:07X}  {ov:3}→{nv:3}  (0x{ov:02X}→0x{nv:02X}){marker}")
                print()

            time.sleep(0.3)

    except KeyboardInterrupt:
        pass

    print("\n=== 최종 변화 요약 ===")
    print(f"{'오프셋':12} {'초기값':>8} {'최종값':>8}")
    for i, (ov, nv, ts) in sorted(changed.items(), key=lambda x: x[0]):
        offset = SCAN_START + i
        marker = " ★IS_IN_GAME" if offset == IS_IN_GAME else ""
        print(f"0x{offset:07X}   {ov:3}→{nv:3}   [{ts}]{marker}")

    kernel32.CloseHandle(hp)

if __name__ == "__main__":
    main()
