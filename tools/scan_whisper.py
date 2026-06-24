"""
귓말 발신자 메모리 주소 스캐너
사용법:
  1. python scan_whisper.py before   <- 귓말 받기 전
  2. 자신에게 귓말 전송 (/w 아이디 할말)
  3. python scan_whisper.py after <발신자아이디>  <- 귓말 받은 후, 발신자 이름 지정
"""
import ctypes, ctypes.wintypes, sys, os, json

PROCESS_VM_READ           = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS        = 0x00000002
TH32CS_SNAPMODULE         = 0x00000008
TH32CS_SNAPMODULE32       = 0x00000010
kernel32 = ctypes.windll.kernel32

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

def find_sc():
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32(); pe.dwSize = ctypes.sizeof(pe)
    if kernel32.Process32First(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.lower() == b"starcraft.exe":
                pid = pe.th32ProcessID; kernel32.CloseHandle(snap); return pid
            if not kernel32.Process32Next(snap, ctypes.byref(pe)): break
    kernel32.CloseHandle(snap); return None

def get_base_and_size(pid):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32, pid)
    me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(me)
    if kernel32.Module32First(snap, ctypes.byref(me)):
        while True:
            if me.szModule.lower() == b"starcraft.exe":
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

SNAP_DIR = os.path.join(os.path.dirname(__file__), "snaps")

def cmd_before():
    os.makedirs(SNAP_DIR, exist_ok=True)
    pid = find_sc()
    if not pid: print("SC 못 찾음"); return
    hp = kernel32.OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_INFORMATION, False, pid)
    base, size = get_base_and_size(pid)
    data = list(read_mem(hp, base, size))
    kernel32.CloseHandle(hp)
    path = os.path.join(SNAP_DIR, "whisper_before.json")
    json.dump({"base": base, "data": data}, open(path, "w"))
    print(f"저장 완료: {path}  ({len(data)} bytes)")
    print("이제 자신에게 귓말을 보내세요: /w 아이디 할말")

def cmd_after(sender_id):
    pid = find_sc()
    if not pid: print("SC 못 찾음"); return
    hp = kernel32.OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_INFORMATION, False, pid)
    base, size = get_base_and_size(pid)
    after_data = list(read_mem(hp, base, size))
    kernel32.CloseHandle(hp)

    path = os.path.join(SNAP_DIR, "whisper_before.json")
    before = json.load(open(path))
    before_data = before["data"]

    n = min(len(before_data), len(after_data))

    # 발신자 이름을 바이트로 변환 (ASCII/CP949)
    name_bytes = sender_id.encode("utf-8")
    name_len   = len(name_bytes)

    # after 데이터에서 발신자 이름 문자열 검색
    after_bytes = bytes(after_data[:n])
    print(f"\n발신자 '{sender_id}' 검색 중...\n")
    found = []
    pos = 0
    while True:
        idx = after_bytes.find(name_bytes, pos)
        if idx == -1: break
        found.append(idx)
        pos = idx + 1

    if found:
        print(f"발신자 이름 발견: {len(found)}개 위치")
        for idx in found:
            offset = idx  # 모듈 내 오프셋
            # before에서도 있었는지 확인
            in_before = before_data[idx:idx+name_len] == list(name_bytes)
            status = "기존" if in_before else "★새로생김"
            # 주변 바이트
            ctx = after_bytes[max(0,idx-8):idx+name_len+8]
            print(f"  0x{offset:07X}  {status}  주변: {ctx}")
    else:
        print("발신자 이름을 찾지 못했습니다.")
        print("귓말을 받은 직후 실행했는지, 이름이 정확한지 확인하세요.")

if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "before":
        cmd_before()
    elif len(sys.argv) >= 3 and sys.argv[1] == "after":
        cmd_after(sys.argv[2])
    else:
        print(__doc__)
