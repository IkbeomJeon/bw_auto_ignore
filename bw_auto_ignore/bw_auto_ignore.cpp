#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
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

// 전역 변수
DWORD g_starcraftPID = 0;
HANDLE g_hProcess = NULL;
std::set<std::string> g_extractedSet;
std::vector<std::string> g_extractedOrder; // 삽입 순서 관리
std::mutex g_mutex;
HHOOK g_hHook = NULL;

HINSTANCE hInst;                    // 현재 인스턴스
WCHAR szWindowClass[] = L"BW_AutoIgnoreWndClass"; // 윈도우 클래스 이름
NOTIFYICONDATA nid = { 0 };           // 시스템 트레이 아이콘 데이터

// 전방 선언
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

//---------------------------------------------------------------------------
// 기존 기능: StarCraft 프로세스 탐색, 메모리 스캔, 문자열 추출/제거, 키보드 후크

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

std::string ReadNullTerminatedString(HANDLE hProcess, ULONGLONG address, size_t maxLength = 512)
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
    // 먼저 엔터 전송
    SendVirtualKey(VK_RETURN);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, NULL, 0);
    std::wstring wcommand(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, &wcommand[0], size_needed);
    SendUnicodeString(wcommand);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // 마지막으로 엔터 전송
    SendVirtualKey(VK_RETURN);
}

// DoExtraction 함수 수정
void DoExtraction() {
    const char targetPrefix[] = u8"/aurora-profile-by-toon/";
    size_t prefixLen = strlen(targetPrefix);
    const char tailChar = '/'; // 다음 '/'를 찾음

    std::vector<ULONGLONG> addresses = FindAllPrefixAddresses(g_hProcess, targetPrefix);
    if (addresses.empty()) {
        std::wcout << L"새로운 값이 발견되지 않았습니다.\n";
        return;
    }

    //std::map<std::string, int> frequencyMap;
    std::vector<std::string> extractedList;

    for (ULONGLONG addr : addresses) {
        std::string fullStr = ReadNullTerminatedString(g_hProcess, addr);
        if (fullStr.compare(0, prefixLen, targetPrefix) == 0) {
            size_t endPos = fullStr.find(tailChar, prefixLen);
            if (endPos != std::string::npos && endPos > prefixLen) {
                std::string extracted = fullStr.substr(prefixLen, endPos - prefixLen);
                if (!extracted.empty()) {
                    //frequencyMap[extracted]++;
                    extractedList.push_back(extracted);
                }
            }
        }
    }

    if (extractedList.size() <= 1) {
        std::wcout << L"추출된 문자열이 하나뿐이므로 추가하지 않습니다.\n";
        return;
    }

    std::string myId = extractedList[0];

    int newCount = 0;
    for (const std::string& extracted : extractedList) {
		if (extracted != myId) {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_extractedSet.find(extracted) == g_extractedSet.end()) {
                g_extractedSet.insert(extracted);
                g_extractedOrder.push_back(extracted);
                int size_needed = MultiByteToWideChar(CP_UTF8, 0, extracted.c_str(), -1, NULL, 0);
                std::wstring wextracted(size_needed, 0);
                MultiByteToWideChar(CP_UTF8, 0, extracted.c_str(), -1, &wextracted[0], size_needed);
                std::wcout << L"추출 값: " << wextracted << L"  -> 추가되었습니다" << std::endl;
                newCount++;
                SendToStarCraft("/ignore " + extracted);
            }
        }
    }

    if (newCount == 0) {
        std::wcout << L"새로운 값이 발견되지 않았습니다.\n";
    }
}

// DoRemoval 함수 수정
void DoRemoval() {
    std::string removedId;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_extractedOrder.empty()) {
            std::wcout << L"제거할 id가 없습니다.\n";
            return;
        }
        removedId = g_extractedOrder.back();
        g_extractedOrder.pop_back();
        g_extractedSet.erase(removedId);
    }
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, removedId.c_str(), -1, NULL, 0);
    std::wstring wRemovedId(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, removedId.c_str(), -1, &wRemovedId[0], size_needed);
    std::wcout << L"제거된 id: " << wRemovedId << std::endl;
    SendToStarCraft("/unignore " + removedId);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
    {
        KBDLLHOOKSTRUCT* pKbd = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        HWND hForeground = GetForegroundWindow();
        DWORD foregroundPID = 0;
        GetWindowThreadProcessId(hForeground, &foregroundPID);
        if (pKbd->vkCode == VK_F9)
        {
            if (foregroundPID == g_starcraftPID)
            {
                std::thread extractionThread(DoExtraction);
                extractionThread.detach();
            }
        }
        else if (pKbd->vkCode == VK_F8)
        {
            if (foregroundPID == g_starcraftPID)
            {
                std::thread removalThread(DoRemoval);
                removalThread.detach();
            }
        }
    }
    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}

// 후크 시작/중지 함수
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

//---------------------------------------------------------------------------
// Win32 GUI 애플리케이션 부분

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // 중복 실행 방지: 고유 이름의 뮤텍스 생성
    HANDLE hMutex = CreateMutex(NULL, TRUE, L"Local\\bw_auto_ignoreMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // 이미 실행 중이면 종료
        return 0;
    }

    // 콘솔 입출력 모드 설정 (디버깅용)
    _setmode(_fileno(stdout), _O_U16TEXT);
    setlocale(LC_ALL, "");

    MyRegisterClass(hInstance);
    if (!InitInstance(hInstance, nCmdShow))
    {
        return FALSE;
    }

    // StarCraft 프로세스 찾기 및 프로세스 핸들 열기
    const wchar_t* processName = L"StarCraft.exe";
    g_starcraftPID = GetProcessID(processName);
    if (g_starcraftPID == 0)
    {
        std::wcout << L"StarCraft를 먼저 실행해 주세요.\n";
		//메시지 박스 추가
		MessageBox(NULL, L"StarCraft를 먼저 실행해 주세요.", L"Error", MB_OK | MB_ICONERROR);
		return 0;
    }
    else
    {
        std::wcout << L"StarCraft PID: " << g_starcraftPID << std::endl;
        g_hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, g_starcraftPID);
        if (g_hProcess == NULL)
        {
            std::wcout << L"OpenProcess 실패. 오류 코드: " << GetLastError() << L"\n";
            MessageBox(NULL, L"StarCraft 프로세스를 찾을 수 없습니다.", L"Error", MB_OK | MB_ICONERROR);
			return 0;

        }
    }

    // 항상 활성 상태이므로 후크 설치 (g_enabled 관련 토글 제거)
    if (g_hProcess)
    {
        StartKeyboardHook();
    }

    // 메시지 루프
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 종료 시 후크 제거 및 프로세스 핸들 닫기, 트레이 아이콘 제거
    StopKeyboardHook();
    if (g_hProcess)
        CloseHandle(g_hProcess);
    Shell_NotifyIcon(NIM_DELETE, &nid);

    // 뮤텍스 해제
    if (hMutex)
        CloseHandle(hMutex);

    return (int)msg.wParam;
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

    // 창은 보이지 않도록 숨김
    ShowWindow(hWnd, SW_HIDE);
    UpdateWindow(hWnd);

    // 시스템 트레이 아이콘 등록 (메뉴 항목은 Help와 Exit만 있음)
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

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_APP + 1: // 트레이 아이콘 관련 메시지
        if (lParam == WM_RBUTTONUP)
        {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            // Help 메뉴 항목 추가
            AppendMenu(hMenu, MF_STRING, 1002, L"Help");
            // Exit 메뉴 항목 추가
            AppendMenu(hMenu, MF_STRING, 1003, L"Exit");
            SetForegroundWindow(hWnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(hMenu);
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == 1002) // Help
        {
            MessageBox(hWnd,
                L"F9 : 사용자 무시 \nF8 : 사용자 무시 해제",
                L"Help", MB_OK | MB_ICONINFORMATION);
        }
        else if (LOWORD(wParam) == 1003) // Exit
        {
            DestroyWindow(hWnd);
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
