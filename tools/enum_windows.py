"""SC 프로세스의 모든 창 열거 (child 포함)"""
import ctypes, ctypes.wintypes

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

def get_window_info(hwnd):
    cls  = ctypes.create_unicode_buffer(256)
    title = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, cls, 256)
    user32.GetWindowTextW(hwnd, title, 256)
    visible = user32.IsWindowVisible(hwnd)
    return cls.value, title.value, visible

LOG = open('E:/bw_auto_ignore/windows_log.txt', 'w', encoding='utf-8')
def log(msg):
    print(msg)
    LOG.write(msg + '\n')
    LOG.flush()

pid = find_sc_pid()
if not pid:
    log('SC 없음'); exit()
log(f'SC PID: {pid}')

WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

sc_windows = []

def top_cb(hwnd, _):
    p = ctypes.wintypes.DWORD(0)
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
    if p.value == pid:
        sc_windows.append(hwnd)
    return True

user32.EnumWindows(WNDENUMPROC(top_cb), 0)
log(f'최상위 창: {len(sc_windows)}개')

for hwnd in sc_windows:
    cls, title, visible = get_window_info(hwnd)
    log(f'\n[TOP] HWND=0x{hwnd:X}  class={cls!r}  title={title!r}  visible={visible}')

    # child 창 열거
    children = []
    def child_cb(hwnd, _):
        children.append(hwnd)
        return True
    user32.EnumChildWindows(hwnd, WNDENUMPROC(child_cb), 0)
    log(f'  child 창: {len(children)}개')
    for ch in children:
        cls2, title2, vis2 = get_window_info(ch)
        log(f'    [CHILD] HWND=0x{ch:X}  class={cls2!r}  title={title2!r}  visible={vis2}')

LOG.close()
