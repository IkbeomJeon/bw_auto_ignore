#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <string>
#include <iostream>
#include <vector>
#include <set>
#include <io.h>
#include <fcntl.h>
#include <clocale>
#include <cstring>
#include <algorithm>
#include <thread>
#include <chrono>
#include <mutex>
#include <iterator>
#include <map>
#include "resource.h"

// ---------------------------------------------------------------------------
// StarCraft.exe 메모리 오프셋 상수
// ---------------------------------------------------------------------------
const ULONGLONG PLAYER_TABLE_OFFSET = 0x10931B0;
const int PLAYER_SLOT_SIZE  = 104;
const int PLAYER_SLOT_COUNT = 8;
const int PLAYER_NAME_OFFSET = 8;   // 슬롯 내 이름 시작 위치

const ULONGLONG CHAT_MODE_OFFSET  = 0x1094323; // 채팅 입력 중: 1, 아님: 0
const ULONGLONG MAP_NAME_OFFSET   = 0x1091FEE; // 현재 맵 이름 (UTF-8)
const ULONGLONG IS_IN_GAME_OFFSET = 0x1090612; // 인게임: 1, 로비/메뉴: 0

// ---------------------------------------------------------------------------
// 전역 변수
// ---------------------------------------------------------------------------
int KEY_IGNORE = VK_F9;
int KEY_UNIGNORE = VK_F8;
//int KEY_ADDITIONAL_CTRL = VK_APPS;
int KEY_ADDITIONAL_CTRL = VK_SPACE;
int KEY_SWAP_CTRL = VK_F12;

HWND g_hSettingDlg = NULL;
DWORD g_starcraftPID = 0;
HANDLE g_hProcess = NULL;
std::set<std::string> g_extractedSet;
std::mutex g_mutex;
HHOOK g_hHook = NULL;
HINSTANCE hInst;                    // 현재 인스턴스
WCHAR szWindowClass[] = L"BW_AutoIgnoreWndClass"; // 윈도우 클래스 이름
NOTIFYICONDATA nid = { 0 };           // 시스템 트레이 아이콘 데이터

bool g_swapSpaceAndControl = true;    // 키 리매핑 활성화 여부
bool g_chatMode = false;              // 채팅 입력 중 여부
bool g_showMapName = true;            // 맵 이름 오버레이 표시 여부
bool g_autoIgnoreOnGameStart = false; // 게임 시작 시 자동 무시 여부
bool g_isInGame = false;              // 현재 인게임 여부 (타이머에서 갱신)
bool g_wasInGame = false;             // 직전 인게임 여부 (전환 감지용)
ULONGLONG g_scModuleBase = 0;         // StarCraft.exe 모듈 베이스 캐시

HWND g_hOverlay = NULL;               // 오버레이 창
HWND g_hStarCraftWnd = NULL;          // StarCraft 창 핸들
std::wstring g_mapName = L"";         // 현재 맵 이름
const COLORREF OVERLAY_TRANSPARENT = RGB(0, 0, 1); // 투명 처리할 색상 (배경)

// ---------------------------------------------------------------------------
// 함수 선언
// ---------------------------------------------------------------------------
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK LowLevelKeyboardProc(int, WPARAM, LPARAM);
INT_PTR CALLBACK SettingDlgProc(HWND, UINT, WPARAM, LPARAM);

DWORD GetProcessID(const wchar_t* processName);
std::string ReadNullTerminatedString(HANDLE hProcess, ULONGLONG address, size_t maxLength = 512);
std::vector<ULONGLONG> FindAllPrefixAddresses(HANDLE hProcess, const char* targetPrefix);

void SendUnicodeString(const std::wstring& s);
void SendVirtualKey(WORD vk);
void SendToStarCraft(std::string command);

ULONGLONG GetStarCraftModuleBase();
std::wstring ReadMapName();
std::set<std::string> ReadCurrentGamePlayers();

LRESULT CALLBACK OverlayWndProc(HWND, UINT, WPARAM, LPARAM);
void CreateOverlayWindow(HINSTANCE hInstance);

void DoExtraction();
void DoRemoval();

void StartKeyboardHook();
void StopKeyboardHook();

void UpdateStarCraftProcess();
void ProcessMonitorThread();

// ---------------------------------------------------------------------------
// 함수 정의
// ---------------------------------------------------------------------------

DWORD GetProcessID(const wchar_t* processName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        std::wcout << L"스냅샷 생성 실패. 오류 코드: " << GetLastError() << L"\n";
        return 0;
    }
    PROCESSENTRY32W pe32{};
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    if (Process32FirstW(snapshot, &pe32))
    {
        do {
            if (_wcsicmp(pe32.szExeFile, processName) == 0)
            {
                CloseHandle(snapshot);
                return pe32.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &pe32));
    }
    CloseHandle(snapshot);
    std::wcout << L"프로세스를 찾을 수 없습니다.\n";
    return 0;
}

ULONGLONG GetStarCraftModuleBase()
{
    if (g_scModuleBase != 0) return g_scModuleBase;
    if (g_hProcess == NULL) return 0;

    HMODULE hModules[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(g_hProcess, hModules, sizeof(hModules), &needed, LIST_MODULES_ALL))
        return 0;

    int count = needed / sizeof(HMODULE);
    WCHAR modName[MAX_PATH];
    for (int i = 0; i < count; i++)
    {
        if (GetModuleFileNameExW(g_hProcess, hModules[i], modName, MAX_PATH))
        {
            WCHAR* fileName = wcsrchr(modName, L'\\');
            if (fileName && _wcsicmp(fileName + 1, L"StarCraft.exe") == 0)
            {
                g_scModuleBase = reinterpret_cast<ULONGLONG>(hModules[i]);
                return g_scModuleBase;
            }
        }
    }
    return 0;
}

std::wstring ReadMapName()
{
    ULONGLONG base = GetStarCraftModuleBase();
    if (base == 0 || g_hProcess == NULL) return L"";

    BYTE buf[64] = {};
    SIZE_T bytesRead = 0;
    ReadProcessMemory(g_hProcess, reinterpret_cast<LPCVOID>(base + MAP_NAME_OFFSET), buf, sizeof(buf), &bytesRead);

    // null 종료 위치 찾기
    size_t len = 0;
    while (len < bytesRead && buf[len] != 0) len++;

    // UTF-8 → wstring 변환 (포맷 코드 0x01~0x1F 제거)
    std::string utf8(reinterpret_cast<char*>(buf), len);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
    if (wlen <= 0) return L"";
    std::wstring wide(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], wlen);

    // 제어 문자 제거 (StarCraft 색상 코드 등)
    std::wstring result;
    for (wchar_t c : wide)
        if (c >= 0x20) result += c;
    return result;
}

bool IsInGame()
{
    ULONGLONG base = GetStarCraftModuleBase();
    if (base == 0 || g_hProcess == NULL) return false;
    BYTE flag = 0;
    SIZE_T bytesRead = 0;
    ReadProcessMemory(g_hProcess, reinterpret_cast<LPCVOID>(base + IS_IN_GAME_OFFSET), &flag, 1, &bytesRead);
    return flag == 1;
}

struct EnumData { DWORD pid; HWND hwnd; };

BOOL CALLBACK FindWindowByPID(HWND hwnd, LPARAM lParam)
{
    EnumData* data = reinterpret_cast<EnumData*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == data->pid && IsWindowVisible(hwnd))
    {
        data->hwnd = hwnd;
        return FALSE; // 찾으면 중단
    }
    return TRUE;
}

HWND GetStarCraftWindow()
{
    if (g_starcraftPID == 0) return NULL;
    EnumData data = { g_starcraftPID, NULL };
    EnumWindows(FindWindowByPID, reinterpret_cast<LPARAM>(&data));
    return data.hwnd;
}

void UpdateOverlayPosition()
{
    if (!g_hOverlay) return;

    HWND hSC = GetStarCraftWindow();
    g_hStarCraftWnd = hSC;

    if (!hSC || !IsWindow(hSC))
    {
        ShowWindow(g_hOverlay, SW_HIDE);
        return;
    }

    // StarCraft가 포그라운드가 아니거나 인게임이 아니면 숨김
    HWND hFg = GetForegroundWindow();
    if (hFg != hSC || !g_isInGame)
    {
        ShowWindow(g_hOverlay, SW_HIDE);
        return;
    }

    // StarCraft 클라이언트 영역을 스크린 좌표로 변환
    RECT clientRect;
    GetClientRect(hSC, &clientRect);
    POINT topLeft = { clientRect.left, clientRect.top };
    ClientToScreen(hSC, &topLeft);

    int x = topLeft.x;
    int y = topLeft.y;
    int w = clientRect.right - clientRect.left;
    int h = 40;

    SetWindowPos(g_hOverlay, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    ShowWindow(g_hOverlay, SW_SHOW);
}

LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        // 배경을 투명 색상으로 채우기
        HBRUSH hBrush = CreateSolidBrush(OVERLAY_TRANSPARENT);
        FillRect(hdc, &rc, hBrush);
        DeleteObject(hBrush);

        if (g_showMapName && !g_mapName.empty() && g_starcraftPID != 0)
        {
            HFONT hFont = CreateFont(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
            HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

            SetBkMode(hdc, TRANSPARENT);

            // 외곽선 효과 (검정 그림자)
            SetTextColor(hdc, RGB(0, 0, 0));
            RECT shadow = rc;
            for (int dx = -2; dx <= 2; dx++)
                for (int dy = -2; dy <= 2; dy++) {
                    RECT sr = { rc.left + dx, rc.top + dy, rc.right + dx, rc.bottom + dy };
                    DrawText(hdc, g_mapName.c_str(), -1, &sr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }

            // 본문 (흰색)
            SetTextColor(hdc, RGB(255, 255, 255));
            DrawText(hdc, g_mapName.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, hOldFont);
            DeleteObject(hFont);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_TIMER:
    {
        bool inGame = IsInGame();

        // 인게임 진입 감지 (0→1 전환)
        if (!g_wasInGame && inGame && g_autoIgnoreOnGameStart)
        {
            std::thread t(DoExtraction);
            t.detach();
        }
        g_wasInGame = inGame;
        g_isInGame = inGame;

        UpdateOverlayPosition();

        // 맵 이름 갱신 (인게임일 때만)
        std::wstring newName = inGame ? ReadMapName() : L"";
        if (newName != g_mapName)
        {
            g_mapName = newName;
            InvalidateRect(hWnd, NULL, TRUE);
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hWnd, 1);
        return 0;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

void CreateOverlayWindow(HINSTANCE hInstance)
{
    WNDCLASSEX wcex = { 0 };
    wcex.cbSize       = sizeof(WNDCLASSEX);
    wcex.lpfnWndProc  = OverlayWndProc;
    wcex.hInstance    = hInstance;
    wcex.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcex.lpszClassName = L"BW_OverlayWndClass";
    RegisterClassEx(&wcex);

    g_hOverlay = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"BW_OverlayWndClass", L"",
        WS_POPUP,
        0, 0, 100, 40,  // 초기 크기는 임시, UpdateOverlayPosition에서 조정됨
        NULL, NULL, hInstance, NULL
    );

    SetLayeredWindowAttributes(g_hOverlay, OVERLAY_TRANSPARENT, 160, LWA_COLORKEY | LWA_ALPHA); // 반투명 (160/255)
    SetTimer(g_hOverlay, 1, 100, NULL); // 0.1초마다 위치 갱신
    UpdateOverlayPosition();
}

std::set<std::string> ReadCurrentGamePlayers()
{
    ULONGLONG base = GetStarCraftModuleBase();
    if (base == 0)
    {
        std::wcout << L"StarCraft.exe 모듈 베이스를 찾을 수 없습니다.\n";
        return {};
    }

    ULONGLONG tableAddr = base + PLAYER_TABLE_OFFSET;
    std::vector<BYTE> buf(PLAYER_SLOT_SIZE * PLAYER_SLOT_COUNT, 0);
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(g_hProcess, reinterpret_cast<LPCVOID>(tableAddr), buf.data(), buf.size(), &bytesRead))
    {
        std::wcout << L"플레이어 테이블 읽기 실패. 오류: " << GetLastError() << L"\n";
        return {};
    }

    std::set<std::string> players;
    for (int i = 0; i < PLAYER_SLOT_COUNT; i++)
    {
        BYTE* slot = buf.data() + i * PLAYER_SLOT_SIZE;
        if (slot[0] != 0x01) continue;  // 비활성 슬롯 스킵

        char* name = reinterpret_cast<char*>(slot + PLAYER_NAME_OFFSET);
        size_t nameLen = strnlen(name, PLAYER_SLOT_SIZE - PLAYER_NAME_OFFSET);
        if (nameLen > 0)
            players.insert(std::string(name, nameLen));
    }
    return players;
}

std::string ReadNullTerminatedString(HANDLE hProcess, ULONGLONG address, size_t maxLength)
{
    std::vector<char> buffer(maxLength, 0);
    SIZE_T bytesRead = 0;
    if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), buffer.data(), maxLength, &bytesRead))
    {
        size_t actualLength = 0;
        while (actualLength < bytesRead && buffer[actualLength] != '\0')
            actualLength++;
        return std::string(buffer.data(), actualLength);
    }
    return "";
}

std::vector<ULONGLONG> FindAllPrefixAddresses(HANDLE hProcess, const char* targetPrefix)
{
    std::wcout << L"감지를 시작합니다..." << std::endl;
    std::vector<ULONGLONG> addresses;
    size_t prefixLength = strlen(targetPrefix);
    ULONGLONG currentAddress = 0;
    MEMORY_BASIC_INFORMATION mbi;
    std::vector<BYTE> buffer;

    while (VirtualQueryEx(hProcess, reinterpret_cast<LPCVOID>(currentAddress), &mbi, sizeof(mbi)) != 0)
    {
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        {
            buffer.resize(mbi.RegionSize);
            SIZE_T bytesRead = 0;
            if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(currentAddress), buffer.data(), mbi.RegionSize, &bytesRead))
            {
                auto regionStart = buffer.begin();
                auto regionEnd = regionStart + bytesRead;
                auto patternBegin = reinterpret_cast<const BYTE*>(targetPrefix);
                auto patternEnd = patternBegin + prefixLength;
                auto it = std::search(regionStart, regionEnd, patternBegin, patternEnd);
                while (it != regionEnd)
                {
                    ULONGLONG foundAddress = currentAddress + std::distance(regionStart, it);
                    addresses.push_back(foundAddress);
                    it = std::search(it + prefixLength, regionEnd, patternBegin, patternEnd);
                }
            }
        }
        currentAddress += mbi.RegionSize;
    }
    return addresses;
}

void SendUnicodeString(const std::wstring& s)
{
    for (wchar_t ch : s)
    {
        INPUT input = { 0 };
        input.type = INPUT_KEYBOARD;
        input.ki.dwFlags = KEYEVENTF_UNICODE;
        input.ki.wScan = ch;
        SendInput(1, &input, sizeof(input));
        input.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(1, &input, sizeof(input));
    }
}

void SendVirtualKey(WORD vk)
{
    INPUT input = { 0 };
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    SendInput(1, &input, sizeof(input));
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(input));
}

void SendToStarCraft(std::string command)
{
    SendVirtualKey(VK_RETURN);
    //std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, NULL, 0);
    std::wstring wcommand(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, &wcommand[0], size_needed);
    SendUnicodeString(wcommand);
    //std::this_thread::sleep_for(std::chrono::milliseconds(10));
    SendVirtualKey(VK_RETURN);
}

std::set<std::string> ExtractStrings(const char* targetPrefix, size_t prefixLen, char tailChar = '\0')
{
    std::vector<ULONGLONG> addresses = FindAllPrefixAddresses(g_hProcess, targetPrefix);

    if (addresses.empty()) {
        std::wcout << L"새로운 값이 발견되지 않았습니다.\n";
        return {};
    }
    std::set<std::string> extractedIDSet;

    for (ULONGLONG addr : addresses) {
        std::string fullStr = ReadNullTerminatedString(g_hProcess, addr);
        if (fullStr.compare(0, prefixLen, targetPrefix) == 0) {
            std::string extracted;
            if (tailChar == '\0') {
                // tailChar이 전달되지 않은 경우, 접두어 이후 전체 문자열(즉, null까지)을 추출
                extracted = fullStr.substr(prefixLen);
            }
            else {
                size_t endPos = fullStr.find(tailChar, prefixLen);
                if (endPos != std::string::npos && endPos > prefixLen) {
                    extracted = fullStr.substr(prefixLen, endPos - prefixLen);
                }
                else {
                    // tailChar가 발견되지 않으면 null로 종료된 전체 문자열을 추출
                    extracted = fullStr.substr(prefixLen);
                }
            }
            if (!extracted.empty()) {
                extractedIDSet.insert(extracted);
            }
        }
    }
    return extractedIDSet;
}

void DoExtraction() {
    // 현재 게임 플레이어 테이블에서 직접 읽기
    auto currentPlayers = ReadCurrentGamePlayers();

    if (currentPlayers.empty()) {
        std::wcout << L"현재 게임 플레이어를 찾을 수 없습니다 (게임 중이 아닐 수 있습니다).\n";
        return;
    }

    // 본인 ID 제거 (HAT: 접두사로 검색)
    const char targetPrefix_myID[] = u8"HAT:";
    size_t prefixLen = strlen(targetPrefix_myID);
    auto myIDs = ExtractStrings(targetPrefix_myID, prefixLen, '\x10');
    for (const std::string& myID : myIDs)
        currentPlayers.erase(myID);

    int newCount = 0;
    for (const std::string& playerName : currentPlayers) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_extractedSet.find(playerName) == g_extractedSet.end()) {
            g_extractedSet.insert(playerName);
            int size_needed = MultiByteToWideChar(CP_UTF8, 0, playerName.c_str(), -1, NULL, 0);
            std::wstring wName(size_needed, 0);
            MultiByteToWideChar(CP_UTF8, 0, playerName.c_str(), -1, &wName[0], size_needed);
            std::wcout << L"추출 값: " << wName << L"  -> 추가되었습니다" << std::endl;
            newCount++;
            SendToStarCraft("/ignore " + playerName);
        }
    }
    if (newCount == 0)
        std::wcout << L"새로운 값이 발견되지 않았습니다.\n";
}

void DoRemoval() {
    
    
    for (const std::string& removedId : g_extractedSet)
    {
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, removedId.c_str(), -1, NULL, 0);
        std::wstring wRemovedId(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, removedId.c_str(), -1, &wRemovedId[0], size_needed);
        std::wcout << L"제거된 id: " << wRemovedId << std::endl;
        SendToStarCraft("/unignore " + removedId);
    }
	g_extractedSet.clear();
}

void StartKeyboardHook()
{
    if (g_hHook == NULL)
        g_hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
}

void StopKeyboardHook()
{
    if (g_hHook)
    {
        UnhookWindowsHookEx(g_hHook);
        g_hHook = NULL;
    }
}

void UpdateStarCraftProcess()
{
    const wchar_t* processName = L"StarCraft.exe";
    DWORD newPID = GetProcessID(processName);
    if (newPID != 0 && g_starcraftPID != newPID)
    {
        g_extractedSet.clear();
        g_scModuleBase = 0;
        g_starcraftPID = newPID;
        if (g_hProcess)
            CloseHandle(g_hProcess);
        g_hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, g_starcraftPID);
        if (g_hProcess == NULL)
        {
            std::wcout << L"OpenProcess 실패. 오류 코드: " << GetLastError() << L"\n";
        }
    }
}

void ProcessMonitorThread()
{
    while (true)
    {
        UpdateStarCraftProcess();
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
}

// ---------------------------------------------------------------------------
// 후킹 및 창 관련 콜백 함수들
// ---------------------------------------------------------------------------
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        KBDLLHOOKSTRUCT* pKbd = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool blockEvent = false;
        INPUT input = { 0 };
        input.type = INPUT_KEYBOARD;

        // 포그라운드 창의 프로세스 ID 확인
        HWND hForeground = GetForegroundWindow();
        DWORD foregroundPID = 0;
        GetWindowThreadProcessId(hForeground, &foregroundPID);

        // StarCraft 창에서 F9와 F8 키로 추출/제거 실행
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
        {
            if (foregroundPID == g_starcraftPID)
            {
                if (pKbd->vkCode == KEY_IGNORE)
                {
                    std::thread extractionThread(DoExtraction);
                    extractionThread.detach();
                }
                if (pKbd->vkCode == KEY_UNIGNORE)
                {
                    std::thread removalThread(DoRemoval);
                    removalThread.detach();
                }
                if (pKbd->vkCode == VK_RETURN)
                {
                    g_chatMode = !g_chatMode;
                }
                if (pKbd->vkCode == VK_ESCAPE && g_chatMode)
                {
                    g_chatMode = false;
                }
                if(pKbd->vkCode == KEY_SWAP_CTRL)
                {
                    g_swapSpaceAndControl = !g_swapSpaceAndControl;
                    std::wcout << L"Spacebar to Control swap "
                        << (g_swapSpaceAndControl ? L"enabled" : L"disabled")
                        << std::endl;
                    // 설정 대화상자가 열려 있다면 체크박스 상태 업데이트
                    if (g_hSettingDlg)
                    {
                        CheckDlgButton(g_hSettingDlg, IDC_SWAP_KEY, g_swapSpaceAndControl ? BST_CHECKED : BST_UNCHECKED);
                    }
                    return 1; // F12 키 이벤트 차단
                }
            }

            // StarCraft 창에서만, g_swapSpaceAndControl이 활성화된 경우
            // 스페이스바(KEY_ADDITIONAL_CTRL)를 컨트롤키로 리매핑
            if (foregroundPID == g_starcraftPID && g_swapSpaceAndControl && g_isInGame && !g_chatMode && pKbd->vkCode == KEY_ADDITIONAL_CTRL)
            {
                input.ki.wVk = VK_CONTROL;
                input.ki.dwFlags = 0; // key down
                SendInput(1, &input, sizeof(INPUT));
                blockEvent = true;
            }
        }
        else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
        {
            if (foregroundPID == g_starcraftPID && g_swapSpaceAndControl && g_isInGame && !g_chatMode && pKbd->vkCode == KEY_ADDITIONAL_CTRL)
            {
                input.ki.wVk = VK_CONTROL;
                input.ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(1, &input, sizeof(INPUT));
                blockEvent = true;
            }
        }
        if (blockEvent)
            return 1; // 이벤트 차단
    }
    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}


INT_PTR CALLBACK SettingDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_INITDIALOG:
        CheckDlgButton(hDlg, IDC_SWAP_KEY, g_swapSpaceAndControl ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_SHOW_MAP_NAME, g_showMapName ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_AUTO_IGNORE, g_autoIgnoreOnGameStart ? BST_CHECKED : BST_UNCHECKED);
        return (INT_PTR)TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            g_swapSpaceAndControl = (IsDlgButtonChecked(hDlg, IDC_SWAP_KEY) == BST_CHECKED);
            g_showMapName = (IsDlgButtonChecked(hDlg, IDC_SHOW_MAP_NAME) == BST_CHECKED);
            g_autoIgnoreOnGameStart = (IsDlgButtonChecked(hDlg, IDC_AUTO_IGNORE) == BST_CHECKED);
            // 오버레이 즉시 반영
            if (g_hOverlay)
                InvalidateRect(g_hOverlay, NULL, TRUE);
            EndDialog(hDlg, IDOK);
            return (INT_PTR)TRUE;
        }
        else if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, IDCANCEL);
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_APP + 1: // 시스템 트레이 아이콘 메시지 처리
        if (lParam == WM_RBUTTONUP)
        {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenu(hMenu, MF_STRING, 1002, L"Help");
            AppendMenu(hMenu, MF_STRING, 1004, L"Setting");
            AppendMenu(hMenu, MF_STRING, 1003, L"Exit");
            SetForegroundWindow(hWnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(hMenu);
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == 1002) // Help 메뉴
        {
            MessageBox(hWnd,
                L"게임 시작 후 \nF9 : 사용자 무시 \nF8 : 사용자 무시 해제\n\n",
                L"Help", MB_OK | MB_ICONINFORMATION);
        }
        else if (LOWORD(wParam) == 1003) // Exit 메뉴
        {
            DestroyWindow(hWnd);
        }
        else if (LOWORD(wParam) == 1004) // Setting 메뉴
        {
            DialogBox(hInst, MAKEINTRESOURCE(IDD_SETTING_DIALOG), hWnd, SettingDlgProc);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex = { 0 };
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = szWindowClass;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_BWAUTOIGNORE));
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_BWAUTOIGNORE));
    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;
    HWND hWnd = CreateWindowW(szWindowClass, L"bw_auto_ignore", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
        nullptr, nullptr, hInstance, nullptr);
    if (!hWnd)
        return FALSE;
    // 창은 숨김 처리
    ShowWindow(hWnd, SW_HIDE);
    UpdateWindow(hWnd);

    // 시스템 트레이 아이콘 등록
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hWnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP + 1;
    nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_BWAUTOIGNORE));
    wcscpy_s(nid.szTip, L"bw_auto_ignore");
    Shell_NotifyIcon(NIM_ADD, &nid);
    return TRUE;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // 중복 실행 방지 (뮤텍스)
    HANDLE hMutex = CreateMutex(NULL, TRUE, L"Local\\bw_auto_ignoreMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        return 0;
    }

    // 콘솔 입출력 및 로케일 설정 (디버깅용)
    _setmode(_fileno(stdout), _O_U16TEXT);
    setlocale(LC_ALL, "");

    MyRegisterClass(hInstance);
    if (!InitInstance(hInstance, nCmdShow))
    {
        return FALSE;
    }

    StartKeyboardHook();
    CreateOverlayWindow(hInstance);

    // 주기적 프로세스 모니터링 스레드 실행
    std::thread monitorThread(ProcessMonitorThread);
    monitorThread.detach();

    MessageBox(NULL, L"bw_auto_ignore 프로그램이 실행되었습니다. 시계 옆 시스템 트레이를 확인하세요.",
        L"bw_auto_ignore", MB_OK | MB_ICONINFORMATION);

    // 메시지 루프
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    StopKeyboardHook();
    if (g_hProcess)
        CloseHandle(g_hProcess);
    Shell_NotifyIcon(NIM_DELETE, &nid);
    if (hMutex)
        CloseHandle(hMutex);

    return (int)msg.wParam;
}
