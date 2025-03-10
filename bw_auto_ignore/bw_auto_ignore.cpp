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
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
DWORD GetProcessID(const wchar_t* processName);

// 전역 변수 추가
bool g_swapSpaceAndControl = true; // 체크박스 설정에 따라 키 리매핑 여부 결정

// 전역 변수: 마지막 업데이트 시각 (예: std::chrono::steady_clock)
std::chrono::steady_clock::time_point g_lastUpdate = std::chrono::steady_clock::now();

void UpdateStarCraftProcess()
{
    const wchar_t* processName = L"StarCraft.exe";
    DWORD newPID = GetProcessID(processName);
    if (newPID != 0 && g_starcraftPID != newPID)
    {
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

// 별도 모니터링 스레드 함수
void ProcessMonitorThread()
{
    while (true)
    {
        UpdateStarCraftProcess();
        std::this_thread::sleep_for(std::chrono::seconds(3)); // 3초마다 업데이트
    }
}
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

void DoExtraction() {
    const char targetPrefix[] = u8"/aurora-profile-by-toon/";
    size_t prefixLen = strlen(targetPrefix);
    const char tailChar = '/'; // 다음 '/'를 찾음

    std::vector<ULONGLONG> addresses = FindAllPrefixAddresses(g_hProcess, targetPrefix);
    if (addresses.empty()) {
        std::wcout << L"새로운 값이 발견되지 않았습니다.\n";
        return;
    }
    std::vector<std::string> extractedList;

    for (ULONGLONG addr : addresses) {
        std::string fullStr = ReadNullTerminatedString(g_hProcess, addr);
        if (fullStr.compare(0, prefixLen, targetPrefix) == 0) {
            size_t endPos = fullStr.find(tailChar, prefixLen);
            if (endPos != std::string::npos && endPos > prefixLen) {
                std::string extracted = fullStr.substr(prefixLen, endPos - prefixLen);
                if (!extracted.empty()) {
                    extractedList.push_back(extracted);
                }
            }
        }
    }

    if (extractedList.size() <= 1) {
        std::wcout << L"추출된 문자열이 하나뿐이므로 추가하지 않습니다.\n";
        return;
    }
    std::string string_to_ignore = "\"+encodeURIComponent(r.data.name)+\"";
   
	//extractedList에서 string_to_ignore를 제외하기
	extractedList.erase(std::remove(extractedList.begin(), extractedList.end(), string_to_ignore), extractedList.end());

    std::string myId = extractedList[0];

    int newCount = 0;
    
    for (const std::string& extracted : extractedList) {
		if (extracted != myId && extracted != string_to_ignore) {
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

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        KBDLLHOOKSTRUCT* pKbd = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool blockEvent = false;
        INPUT input = { 0 };
        input.type = INPUT_KEYBOARD;

        // 먼저 포그라운드 창의 프로세스 ID 확인
        HWND hForeground = GetForegroundWindow();
        DWORD foregroundPID = 0;
        GetWindowThreadProcessId(hForeground, &foregroundPID);

        // 기존 F9, F8 처리 (StarCraft 자동 무시/무시 해제)
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
        {
            if (foregroundPID == g_starcraftPID && pKbd->vkCode == VK_F9)
            {
                std::thread extractionThread(DoExtraction);
                extractionThread.detach();
            }
            else if (foregroundPID == g_starcraftPID && pKbd->vkCode == VK_F8)
            {
                std::thread removalThread(DoRemoval);
                removalThread.detach();
            }

            if (foregroundPID == g_starcraftPID && g_swapSpaceAndControl && pKbd->vkCode == VK_KANA)
            {
                // 스페이스바 대신 컨트롤키 다운 전송
                input.ki.wVk = VK_CONTROL;
                input.ki.dwFlags = 0; // key down
                SendInput(1, &input, sizeof(INPUT));
                blockEvent = true;
            }
        }
		if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
		{
			if (foregroundPID == g_starcraftPID && g_swapSpaceAndControl && pKbd->vkCode == VK_KANA)
			{
				// 스페이스바 해제 대신 컨트롤키 해제 전송
				input.ki.wVk = VK_CONTROL;
				input.ki.dwFlags = KEYEVENTF_KEYUP;
				SendInput(1, &input, sizeof(INPUT));
				blockEvent = true;
			}
		}
        if (blockEvent)
            return 1; // 원래 이벤트 차단
    }
    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}


INT_PTR CALLBACK SettingDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_INITDIALOG:
        // 현재 플래그 값에 따라 체크박스 상태를 설정
        CheckDlgButton(hDlg, IDC_SWAP_KEY, g_swapSpaceAndControl ? BST_CHECKED : BST_UNCHECKED);
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            // 체크박스 상태를 확인하여 플래그 업데이트
            g_swapSpaceAndControl = (IsDlgButtonChecked(hDlg, IDC_SWAP_KEY) == BST_CHECKED);
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
    case WM_APP + 1: // 트레이 아이콘 관련 메시지
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
        if (LOWORD(wParam) == 1002) // Help
        {
            MessageBox(hWnd,
                L"게임 시작 후 \nF9 : 사용자 무시 \nF8 : 사용자 무시 해제\n\n",
                L"Help", MB_OK | MB_ICONINFORMATION);
        }
        else if (LOWORD(wParam) == 1003) // Exit
        {
            DestroyWindow(hWnd);
        }
        else if (LOWORD(wParam) == 1004) // Setting 메뉴 선택 시
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

    StartKeyboardHook();

    // ProcessMonitorThread를 별도의 스레드에서 실행
    std::thread monitorThread(ProcessMonitorThread);
    monitorThread.detach(); // 백그라운드 스레드로 실행

    //프로그램 실행 메세지 출력
    MessageBox(NULL, L"bw_auto_ignore 프로그램이 실행되었습니다. 시계 옆 시스템 트레이를 확인하세요 .", L"bw_auto_ignore", MB_OK | MB_ICONINFORMATION);

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