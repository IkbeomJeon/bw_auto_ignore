"""현재 SC 메모리에서 귓말 발신자 상태 진단 (빠른 버전)"""
import ctypes, ctypes.wintypes, sys, time
sys.stdout.reconfigure(encoding='utf-8')

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS = 0x00000002
MEM_COMMIT  = 0x1000
MEM_PRIVATE = 0x20000
PAGE_READWRITE = 0x04
PAGE_READONLY  = 0x02
kernel32 = ctypes.windll.kernel32

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

def find_sc():
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

pid = find_sc()
if not pid:
    print('SC 없음'); exit()
print(f'SC PID: {pid}')
hp = kernel32.OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_INFORMATION, False, pid)
mbi = MEMORY_BASIC_INFORMATION()
mbi_size = ctypes.sizeof(mbi)

TERM_INBOUND_RAW  = b'"type":"whisperInbound"}]'
TERM_OUTBOUND_RAW = b'"type":"whisperOutbound"}]'
TERM_INBOUND_ESC  = b'\\"type\\":\\"whisperInbound\\"}]'
TERM_OUTBOUND_ESC = b'\\"type\\":\\"whisperOutbound\\"}]'
NAME_TAG_RAW  = b'legacyToonName":"'
NAME_TAG_ESC  = b'legacyToonName\\":\\"'
CONTENT_TAG_RAW = b'"content":"'
CONTENT_TAG_ESC = b'\\"content\\":\\"'
BACK = 4096

t0 = time.time()
sender_map = {}
regions_read = 0

addr = 0
while kernel32.VirtualQueryEx(hp, ctypes.c_void_p(addr), ctypes.byref(mbi), mbi_size) == mbi_size:
    base = mbi.BaseAddress or 0
    size = mbi.RegionSize
    next_addr = base + size
    # MEM_PRIVATE + READWRITE/READONLY + 64B~8MB (C++와 동일 조건)
    if (mbi.State == MEM_COMMIT and mbi.Type == MEM_PRIVATE and
            mbi.Protect in (PAGE_READWRITE, PAGE_READONLY) and 64 <= size <= 0x800000):
        data = read_mem(hp, base, size)
        if len(data) < 64:
            if next_addr <= addr: break
            addr = next_addr; continue
        regions_read += 1

        for term, is_out, esc in [
            (TERM_INBOUND_RAW,  False, False), (TERM_OUTBOUND_RAW, True,  False),
            (TERM_INBOUND_ESC,  False, True),  (TERM_OUTBOUND_ESC, True,  True),
        ]:
            name_tag    = NAME_TAG_ESC    if esc else NAME_TAG_RAW
            content_tag = CONTENT_TAG_ESC if esc else CONTENT_TAG_RAW
            pos = 0
            while True:
                idx = data.find(term, pos)
                if idx == -1: break
                abs_addr = base + idx
                # terminal 앞 BACK 바이트: 같은 버퍼 내에서 읽기
                ctx_start = max(0, idx - BACK)
                ctx = data[ctx_start:idx]
                # 부족하면 이전 리전에서 추가 읽기
                if len(ctx) < BACK and abs_addr >= BACK:
                    extra = read_mem(hp, abs_addr - BACK, BACK - len(ctx))
                    if extra:
                        ctx = extra + data[ctx_start:idx]
                ni = ctx.rfind(name_tag)
                if ni == -1: pos = idx+1; continue
                ns = ni + len(name_tag)
                ne = ctx.find(b'"', ns)
                if ne == -1 or ne == ns or ne-ns >= 64: pos = idx+1; continue
                sender = ctx[ns:ne].decode('utf-8','replace')
                if sender.endswith('\\'): sender = sender[:-1]
                if not sender or len(sender) >= 64: pos = idx+1; continue
                content = ''
                ci = ctx.rfind(content_tag)
                if ci != -1:
                    cs = ci + len(content_tag)
                    ce = ctx.find(b'"', cs)
                    if ce != -1 and 0 < ce-cs < 256:
                        content = ctx[cs:ce].decode('utf-8','replace')
                        if esc: content = content.replace('\\"','"')
                is_inbound = not is_out
                prev = sender_map.get(sender)
                if prev is None or abs_addr > prev['addr']:
                    sender_map[sender] = {'addr':abs_addr,'inbound':is_inbound,'content':content}
                pos = idx+1
    if next_addr <= addr: break
    addr = next_addr

kernel32.CloseHandle(hp)
elapsed = time.time() - t0
print(f'스캔 완료: {elapsed:.1f}초  리전={regions_read}개')
print(f'\n발견된 대화 상대 ({len(sender_map)}명):')
for s, e in sorted(sender_map.items(), key=lambda x: -x[1]['addr']):
    d = '수신' if e['inbound'] else '발신'
    print(f"  [{d}] {s:30s}  내용={e['content'][:35]!r}")
