"""
StarCraft 메모리 스냅샷 & 비교 도구
사용법:
  python sc_snapshot.py save A   -> 현재 상태 스냅샷을 snap_A.bin 으로 저장
  python sc_snapshot.py save B   -> 현재 상태 스냅샷을 snap_B.bin 으로 저장
  python sc_snapshot.py diff     -> snap_A.bin 과 snap_B.bin 비교, 바뀐 오프셋 출력
"""

import sys
import ctypes
import ctypes.wintypes
import struct
import os

SNAPSHOT_DIR = r"C:\temp"
SCAN_OFFSET_START = 0x1090000  # 모듈 베이스로부터
SCAN_LENGTH       = 0x10000    # 64KB 범위 스캔

# ---- Win32 API ----
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi    = ctypes.WinDLL("psapi",    use_last_error=True)

PROCESS_ALL_ACCESS = 0x1F0FFF
TH32CS_SNAPPROCESS = 0x2

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

def find_starcraft_pid():
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    entry = PROCESSENTRY32()
    entry.dwSize = ctypes.sizeof(PROCESSENTRY32)
    kernel32.Process32First(snap, ctypes.byref(entry))
    pid = None
    while True:
        if entry.szExeFile.lower() == b"starcraft.exe":
            pid = entry.th32ProcessID
            break
        if not kernel32.Process32Next(snap, ctypes.byref(entry)):
            break
    kernel32.CloseHandle(snap)
    return pid

def get_module_base(pid):
    hProc = kernel32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not hProc:
        return None, None
    buf = (ctypes.c_ulonglong * 1024)()
    needed = ctypes.wintypes.DWORD()
    psapi.EnumProcessModulesEx(hProc, buf, ctypes.sizeof(buf), ctypes.byref(needed), 3)
    count = needed.value // 8
    base = None
    for i in range(count):
        name_buf = ctypes.create_unicode_buffer(260)
        psapi.GetModuleFileNameExW(hProc, ctypes.c_void_p(buf[i]), name_buf, 260)
        if "starcraft.exe" in name_buf.value.lower():
            base = buf[i]
            break
    return hProc, base

def read_memory(hProc, addr, size):
    buf = ctypes.create_string_buffer(size)
    read = ctypes.c_size_t(0)
    ok = kernel32.ReadProcessMemory(hProc, ctypes.c_void_p(addr), buf, size, ctypes.byref(read))
    if not ok or read.value == 0:
        return None
    return bytes(buf[:read.value])

def save_snapshot(label):
    pid = find_starcraft_pid()
    if not pid:
        print("StarCraft 프로세스를 찾을 수 없습니다.")
        return
    print(f"PID: {pid}")
    hProc, base = get_module_base(pid)
    if not base:
        print("모듈 베이스를 찾을 수 없습니다.")
        return
    print(f"모듈 베이스: 0x{base:016X}")
    addr = base + SCAN_OFFSET_START
    data = read_memory(hProc, addr, SCAN_LENGTH)
    if not data:
        print("메모리 읽기 실패")
        return
    kernel32.CloseHandle(hProc)
    path = os.path.join(SNAPSHOT_DIR, f"snap_{label}.bin")
    with open(path, "wb") as f:
        # 헤더: base 주소 저장
        f.write(struct.pack("<Q", base))
        f.write(data)
    print(f"스냅샷 저장: {path}  ({len(data)} bytes, base=0x{base:X}+0x{SCAN_OFFSET_START:X})")

def diff_snapshots():
    path_a = os.path.join(SNAPSHOT_DIR, "snap_A.bin")
    path_b = os.path.join(SNAPSHOT_DIR, "snap_B.bin")
    if not os.path.exists(path_a) or not os.path.exists(path_b):
        print("snap_A.bin 또는 snap_B.bin 이 없습니다.")
        return
    with open(path_a, "rb") as f:
        base_a = struct.unpack("<Q", f.read(8))[0]
        data_a = f.read()
    with open(path_b, "rb") as f:
        base_b = struct.unpack("<Q", f.read(8))[0]
        data_b = f.read()

    length = min(len(data_a), len(data_b))
    print(f"비교 범위: 0x{SCAN_OFFSET_START:X} ~ 0x{SCAN_OFFSET_START+length:X}  ({length} bytes)")
    print(f"베이스: A=0x{base_a:X}  B=0x{base_b:X}")
    print()

    # 바뀐 바이트 그룹화 (인접한 것끼리 묶음)
    diffs = []
    i = 0
    while i < length:
        if data_a[i] != data_b[i]:
            start = i
            while i < length and data_a[i] != data_b[i]:
                i += 1
            diffs.append((start, i))
        else:
            i += 1

    print(f"변경된 영역: {len(diffs)}개")
    print("-" * 70)
    for (s, e) in diffs[:100]:
        mod_offset = SCAN_OFFSET_START + s
        hex_a = data_a[s:e].hex()
        hex_b = data_b[s:e].hex()
        # 1바이트면 값도 출력
        extra = ""
        if e - s == 1:
            extra = f"  ({data_a[s]:3d} -> {data_b[s]:3d})"
        elif e - s <= 4:
            # int로도 해석
            try:
                va = int.from_bytes(data_a[s:e], 'little')
                vb = int.from_bytes(data_b[s:e], 'little')
                extra = f"  ({va} -> {vb})"
            except: pass
        print(f"  offset 0x{mod_offset:08X}  len={e-s:3d}  A={hex_a:<16s} B={hex_b:<16s}{extra}")
    if len(diffs) > 100:
        print(f"  ... (총 {len(diffs)}개 중 100개만 표시)")

def read_current(offset, size=4):
    """특정 오프셋 값 빠르게 읽기"""
    pid = find_starcraft_pid()
    if not pid: return
    hProc, base = get_module_base(pid)
    if not base: return
    addr = base + offset
    data = read_memory(hProc, addr, size)
    kernel32.CloseHandle(hProc)
    if data:
        val = int.from_bytes(data, 'little')
        print(f"offset 0x{offset:X} -> bytes={data.hex()}  uint={val}")
    else:
        print("읽기 실패")

if __name__ == "__main__":
    if len(sys.argv) >= 2:
        cmd = sys.argv[1]
        if cmd == "save" and len(sys.argv) >= 3:
            save_snapshot(sys.argv[2])
        elif cmd == "diff":
            diff_snapshots()
        elif cmd == "read" and len(sys.argv) >= 3:
            offset = int(sys.argv[2], 16)
            size = int(sys.argv[3]) if len(sys.argv) >= 4 else 4
            read_current(offset, size)
        else:
            print(__doc__)
    else:
        print(__doc__)
