"""여러 귓말 순서대로 전송"""
import ctypes, ctypes.wintypes, sys, time

LOG = open('E:/bw_auto_ignore/send_log.txt', 'w', encoding='utf-8')
def log(msg): LOG.write(msg + '\n'); LOG.flush()

kernel32 = ctypes.windll.kernel32
user32   = ctypes.windll.user32
TH32CS_SNAPPROCESS = 0x00000002

class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [('dwSize',ctypes.wintypes.DWORD),('cntUsage',ctypes.wintypes.DWORD),
                ('th32ProcessID',ctypes.wintypes.DWORD),('th32DefaultHeapID',ctypes.POINTER(ctypes.c_ulong)),
                ('th32ModuleID',ctypes.wintypes.DWORD),('cntThreads',ctypes.wintypes.DWORD),
                ('th32ParentProcessID',ctypes.wintypes.DWORD),('pcPriClassBase',ctypes.c_long),
                ('dwFlags',ctypes.wintypes.DWORD),('szExeFile',ctypes.c_char*260)]

def find_sc_pid():
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32(); pe.dwSize = ctypes.sizeof(pe)
    if kernel32.Process32First(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.lower() == b'starcraft.exe':
                pid = pe.th32ProcessID; kernel32.CloseHandle(snap); return pid
            if not kernel32.Process32Next(snap, ctypes.byref(pe)): break
    kernel32.CloseHandle(snap); return None

def find_sc_hwnd(pid):
    result = []
    def cb(hwnd, _):
        p = ctypes.wintypes.DWORD(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd): result.append(hwnd)
        return True
    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return result[0] if result else None

WM_KEYDOWN = 0x0100; WM_KEYUP = 0x0101; WM_CHAR = 0x0102; VK_RETURN = 0x0D

def post_enter(hwnd):
    user32.PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0x001C0001); time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_KEYUP,   VK_RETURN, 0xC01C0001); time.sleep(0.05)

def post_text(hwnd, text):
    for ch in text:
        user32.PostMessageW(hwnd, WM_CHAR, ord(ch), 0); time.sleep(0.02)

def send_whisper(hwnd, target, msg):
    cmd = f'/w {target} {msg}'
    log(f'전송: {cmd}')
    post_enter(hwnd); time.sleep(0.5)
    post_text(hwnd, cmd); time.sleep(0.3)
    post_enter(hwnd); time.sleep(2.0)

pid = find_sc_pid()
if not pid: log('SC 없음'); exit()
log(f'SC PID: {pid}')
hwnd = find_sc_hwnd(pid)
if not hwnd: log('SC 창 없음'); exit()
log(f'SC HWND: 0x{hwnd:X}')

TARGET = 'ghfhxhtm'
MESSAGES = ['msg001', 'msg002', 'msg003', 'msg004', 'msg005',
            'msg006', 'msg007', 'msg008', 'msg009', 'msg010']

for msg in MESSAGES:
    send_whisper(hwnd, TARGET, msg)

log('전송 완료')
LOG.close()
