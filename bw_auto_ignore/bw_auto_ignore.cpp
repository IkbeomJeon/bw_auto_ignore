#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#include <d3d11.h>
#include <dxgi.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
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
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// StarCraft.exe 메모리 오프셋 상수
// ---------------------------------------------------------------------------
const ULONGLONG PLAYER_TABLE_OFFSET = 0x10931B0;
const int PLAYER_SLOT_SIZE  = 104;
const int PLAYER_SLOT_COUNT = 8;
const int PLAYER_NAME_OFFSET = 8;

const ULONGLONG CHAT_MODE_OFFSET  = 0x1094323;
const ULONGLONG MAP_NAME_OFFSET   = 0x1091FEE;
const ULONGLONG IS_IN_GAME_OFFSET = 0x1090612;

// ---------------------------------------------------------------------------
// 전역 변수
// ---------------------------------------------------------------------------
int KEY_IGNORE = VK_F9;
int KEY_UNIGNORE = VK_F8;
int KEY_ADDITIONAL_CTRL = VK_SPACE;
int KEY_SWAP_CTRL = VK_F12;

HWND g_hSettingDlg = NULL;
DWORD g_starcraftPID = 0;
HANDLE g_hProcess = NULL;
std::set<std::string> g_extractedSet;
std::mutex g_mutex;
HHOOK g_hHook = NULL;
HINSTANCE hInst;
WCHAR szWindowClass[] = L"BW_AutoIgnoreWndClass";
NOTIFYICONDATA nid = { 0 };

bool g_swapSpaceAndControl = true;
bool g_chatMode = false;
bool g_showMapName = true;
bool g_autoIgnoreOnGameStart = false;
bool g_copyBattleTagOnGameStart = false;
bool g_isInGame = false;
bool g_wasInGame = false;
ULONGLONG g_scModuleBase = 0;

HWND g_hOverlay = NULL;
HWND g_hStarCraftWnd = NULL;
std::wstring g_mapName = L"";

// D3D11 + imgui
ID3D11Device*           g_pd3dDevice       = NULL;
ID3D11DeviceContext*    g_pd3dContext       = NULL;
IDXGISwapChain*         g_pSwapChain       = NULL;
ID3D11RenderTargetView* g_pRenderTargetView = NULL;
bool                    g_imguiInitialized  = false;
bool                    g_showGui           = false; // ` 키로 토글

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
void CopyOpponentBattleTagsToClipboard();
void SaveSettings();

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
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
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

    size_t len = 0;
    while (len < bytesRead && buf[len] != 0) len++;

    std::string utf8(reinterpret_cast<char*>(buf), len);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
    if (wlen <= 0) return L"";
    std::wstring wide(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], wlen);

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
        return FALSE;
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

    HWND hFg = GetForegroundWindow();
    bool scOrOverlayFg = (hFg == hSC || hFg == g_hOverlay);

    // 인게임이거나 GUI가 열려있을 때만 표시
    if (!scOrOverlayFg || (!g_isInGame && !g_showGui))
    {
        ShowWindow(g_hOverlay, SW_HIDE);
        return;
    }

    RECT clientRect;
    GetClientRect(hSC, &clientRect);
    POINT topLeft = { 0, 0 };
    ClientToScreen(hSC, &topLeft);

    int x = topLeft.x;
    int y = topLeft.y;
    int w = clientRect.right - clientRect.left;
    int h = clientRect.bottom - clientRect.top;

    SetWindowPos(g_hOverlay, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    ShowWindow(g_hOverlay, SW_SHOW);
}

// ---------------------------------------------------------------------------
// D3D11 헬퍼
// ---------------------------------------------------------------------------
static void CreateRenderTarget()
{
    ID3D11Texture2D* pBack = NULL;
    g_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBack);
    if (pBack)
    {
        g_pd3dDevice->CreateRenderTargetView(pBack, NULL, &g_pRenderTargetView);
        pBack->Release();
    }
}

static void CleanupRenderTarget()
{
    if (g_pRenderTargetView) { g_pRenderTargetView->Release(); g_pRenderTargetView = NULL; }
}

static void RenderOverlay()
{
    if (!g_imguiInitialized || !g_pRenderTargetView) return;

    float clear[4] = { 0.0f, 1.0f, 0.0f, 1.0f }; // pure green = 색상 키로 투명 처리됨
    g_pd3dContext->OMSetRenderTargets(1, &g_pRenderTargetView, NULL);
    g_pd3dContext->ClearRenderTargetView(g_pRenderTargetView, clear);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // 맵 이름 (인게임 + 설정 ON)
    if (g_showMapName && !g_mapName.empty() && g_isInGame)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 8.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##mapname", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);

        std::string utf8name(g_mapName.size() * 3 + 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, g_mapName.c_str(), -1,
            &utf8name[0], (int)utf8name.size(), NULL, NULL);

        ImGui::SetWindowFontScale(1.4f);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int dx = -2; dx <= 2; dx++)
            for (int dy = -2; dy <= 2; dy++)
                if (dx || dy)
                    dl->AddText(ImVec2(pos.x + dx, pos.y + dy), IM_COL32(0, 0, 0, 220), utf8name.c_str());
        ImGui::TextColored(ImVec4(1, 1, 1, 1), "%s", utf8name.c_str());
        ImGui::End();
    }

    // 설정 GUI (` 키로 토글)
    if (g_showGui)
    {
        ImGui::SetNextWindowSize(ImVec2(330, 160), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_Once);
        ImGui::Begin(u8"bw_auto_ignore 설정", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);

        bool v;
        v = g_swapSpaceAndControl;
        if (ImGui::Checkbox(u8"스페이스바를 컨트롤로 이용하기 (F12)", &v))
        { g_swapSpaceAndControl = v; SaveSettings(); }

        v = g_showMapName;
        if (ImGui::Checkbox(u8"맵 이름 표시", &v))
        { g_showMapName = v; SaveSettings(); }

        v = g_autoIgnoreOnGameStart;
        if (ImGui::Checkbox(u8"게임 시작 시 채팅 자동 무시", &v))
        { g_autoIgnoreOnGameStart = v; SaveSettings(); }

        v = g_copyBattleTagOnGameStart;
        if (ImGui::Checkbox(u8"상대방 배틀태그를 클립보드로 복사", &v))
        { g_copyBattleTagOnGameStart = v; SaveSettings(); }

        ImGui::Spacing();
        if (ImGui::Button(u8"닫기 (` 키)"))
        {
            g_showGui = false;
            LONG_PTR ex = GetWindowLongPtr(g_hOverlay, GWL_EXSTYLE);
            SetWindowLongPtr(g_hOverlay, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
        }
        ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(0, 0);
}

// ---------------------------------------------------------------------------
// 오버레이 윈도우 프로시저
// ---------------------------------------------------------------------------
LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (g_imguiInitialized && g_showGui)
        if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
            return 1;

    switch (message)
    {
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_TIMER:
    {
        bool inGame = IsInGame();

        if (!g_wasInGame && inGame)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_extractedSet.clear();
        }
        if (!g_wasInGame && inGame && g_autoIgnoreOnGameStart)
        {
            std::thread t(DoExtraction); t.detach();
        }
        if (!g_wasInGame && inGame && g_copyBattleTagOnGameStart)
        {
            std::thread t(CopyOpponentBattleTagsToClipboard); t.detach();
        }
        g_wasInGame = inGame;
        g_isInGame  = inGame;

        UpdateOverlayPosition();

        std::wstring newName = inGame ? ReadMapName() : L"";
        if (newName != g_mapName)
            g_mapName = newName;

        RenderOverlay();
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hWnd, 1);
        return 0;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 오버레이 창 생성 (D3D11 + imgui)
// ---------------------------------------------------------------------------
void CreateOverlayWindow(HINSTANCE hInstance)
{
    WNDCLASSEX wcex = { 0 };
    wcex.cbSize       = sizeof(WNDCLASSEX);
    wcex.lpfnWndProc  = OverlayWndProc;
    wcex.hInstance    = hInstance;
    wcex.lpszClassName = L"BW_OverlayWndClass";
    RegisterClassEx(&wcex);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    g_hOverlay = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"BW_OverlayWndClass", L"",
        WS_POPUP,
        0, 0, sw, sh,
        NULL, NULL, hInstance, NULL
    );

    // 색상 키 투명도: pure green(0,255,0) 픽셀을 투명으로 처리
    // imgui 다크 테마는 순수 초록(0,255,0)을 출력하지 않으므로 안전
    SetLayeredWindowAttributes(g_hOverlay, RGB(0, 255, 0), 0, LWA_COLORKEY);

    // D3D11 디바이스 + 스왑체인 생성
    // BGRA 포맷 사용 (DWM 합성에 필요)
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = sw;
    sd.BufferDesc.Height                  = sh;
    sd.BufferDesc.Format                  = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = g_hOverlay;
    sd.SampleDesc.Count                   = 1;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        NULL, 0, D3D11_SDK_VERSION, &sd,
        &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dContext);

    if (FAILED(hr))
    {
        MessageBox(NULL, L"D3D11 초기화 실패. 오버레이를 사용할 수 없습니다.", L"오류", MB_OK);
        return;
    }

    CreateRenderTarget();

    // imgui 초기화
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // imgui.ini 비활성화 (창 위치 저장 안 함)

    // 한글 폰트 로드
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 16.0f,
        nullptr, io.Fonts->GetGlyphRangesKorean());

    ImGui::StyleColorsDark();
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 0.85f;

    ImGui_ImplWin32_Init(g_hOverlay);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dContext);
    g_imguiInitialized = true;

    SetTimer(g_hOverlay, 1, 33, NULL); // ~30fps
    UpdateOverlayPosition();
}

// ---------------------------------------------------------------------------
// 플레이어 / 무시 관련 함수
// ---------------------------------------------------------------------------
std::set<std::string> ReadCurrentGamePlayers()
{
    ULONGLONG base = GetStarCraftModuleBase();
    if (base == 0) return {};

    ULONGLONG tableAddr = base + PLAYER_TABLE_OFFSET;
    std::vector<BYTE> buf(PLAYER_SLOT_SIZE * PLAYER_SLOT_COUNT, 0);
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(g_hProcess, reinterpret_cast<LPCVOID>(tableAddr), buf.data(), buf.size(), &bytesRead))
        return {};

    std::set<std::string> players;
    for (int i = 0; i < PLAYER_SLOT_COUNT; i++)
    {
        BYTE* slot = buf.data() + i * PLAYER_SLOT_SIZE;
        if (slot[0] != 0x01) continue;
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
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, NULL, 0);
    std::wstring wcommand(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, &wcommand[0], size_needed);
    SendUnicodeString(wcommand);
    SendVirtualKey(VK_RETURN);
}

std::set<std::string> ExtractStrings(const char* targetPrefix, size_t prefixLen, char tailChar = '\0')
{
    std::vector<ULONGLONG> addresses = FindAllPrefixAddresses(g_hProcess, targetPrefix);
    if (addresses.empty()) return {};

    std::set<std::string> extractedIDSet;
    for (ULONGLONG addr : addresses) {
        std::string fullStr = ReadNullTerminatedString(g_hProcess, addr);
        if (fullStr.compare(0, prefixLen, targetPrefix) == 0) {
            std::string extracted;
            if (tailChar == '\0') {
                extracted = fullStr.substr(prefixLen);
            } else {
                size_t endPos = fullStr.find(tailChar, prefixLen);
                extracted = (endPos != std::string::npos && endPos > prefixLen)
                    ? fullStr.substr(prefixLen, endPos - prefixLen)
                    : fullStr.substr(prefixLen);
            }
            if (!extracted.empty())
                extractedIDSet.insert(extracted);
        }
    }
    return extractedIDSet;
}

void DoExtraction()
{
    auto currentPlayers = ReadCurrentGamePlayers();
    if (currentPlayers.empty()) return;

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
            newCount++;
            SendToStarCraft("/ignore " + playerName);
        }
    }
}

void DoRemoval()
{
    for (const std::string& removedId : g_extractedSet)
        SendToStarCraft("/unignore " + removedId);
    g_extractedSet.clear();
}

// StarCraft 로컬 웹서버 포트를 메모리에서 찾기
static WORD FindLocalWebApiPort()
{
    if (g_hProcess == NULL) return 0;
    const char pattern[] = "127.0.0.1:";
    const char webApiSuffix[] = "/web-api/";
    MEMORY_BASIC_INFORMATION mbi = {};
    ULONGLONG addr = 0;
    WORD foundPort = 0;

    while (VirtualQueryEx(g_hProcess, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)))
    {
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            && mbi.RegionSize > 0 && mbi.RegionSize < 0x20000000)
        {
            std::vector<char> buf(mbi.RegionSize);
            SIZE_T bytesRead = 0;
            if (ReadProcessMemory(g_hProcess, reinterpret_cast<LPCVOID>(addr), buf.data(), mbi.RegionSize, &bytesRead))
            {
                size_t i = 0;
                while (i + sizeof(pattern) + 10 < bytesRead)
                {
                    if (memcmp(buf.data() + i, pattern, sizeof(pattern) - 1) == 0)
                    {
                        size_t portStart = i + sizeof(pattern) - 1;
                        size_t portEnd = portStart;
                        while (portEnd < bytesRead && buf[portEnd] >= '0' && buf[portEnd] <= '9')
                            portEnd++;
                        size_t portLen = portEnd - portStart;
                        if (portLen >= 4 && portLen <= 5 &&
                            portEnd + sizeof(webApiSuffix) - 1 < bytesRead &&
                            memcmp(buf.data() + portEnd, webApiSuffix, sizeof(webApiSuffix) - 1) == 0)
                        {
                            int port = atoi(buf.data() + portStart);
                            if (port > 1024 && port < 65536)
                            {
                                foundPort = (WORD)port;
                                addr += (mbi.RegionSize > 0 ? mbi.RegionSize : 1);
                                goto done;
                            }
                        }
                    }
                    i++;
                }
            }
        }
        addr += (mbi.RegionSize > 0 ? mbi.RegionSize : 1);
        if (addr == 0) break;
    }
done:
    return foundPort;
}

static std::string LocalWebApiGet(const std::string& inGameName, WORD port)
{
    int wlen = MultiByteToWideChar(CP_UTF8, 0, inGameName.c_str(), -1, NULL, 0);
    std::wstring wname(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, inGameName.c_str(), -1, &wname[0], wlen);
    std::wstring path = L"/web-api/v2/aurora-profile-by-toon/" + wname + L"/30?request_flags=scr_tooninfo";

    HINTERNET hSession = WinHttpOpen(L"StarCraft/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";
    HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1", port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    std::string result;
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL))
    {
        DWORD dwSize = 0;
        do {
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
            std::vector<char> buf(dwSize + 1, 0);
            DWORD dwRead = 0;
            WinHttpReadData(hRequest, buf.data(), dwSize, &dwRead);
            result.append(buf.data(), dwRead);
        } while (dwSize > 0);
    }
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

static std::string JsonStringVal(const std::string& json, const std::string& key, size_t from = 0)
{
    std::string fullKey = "\"" + key + "\":\"";
    size_t pos = json.find(fullKey, from);
    if (pos == std::string::npos) return "";
    size_t start = pos + fullKey.size();
    size_t end   = json.find('"', start);
    if (end == std::string::npos) return "";
    return json.substr(start, end - start);
}

void CopyOpponentBattleTagsToClipboard()
{
    std::this_thread::sleep_for(std::chrono::seconds(5));
    if (g_hProcess == NULL) return;

    auto players = ReadCurrentGamePlayers();
    if (players.empty()) return;

    const char targetPrefix_myID[] = u8"HAT:";
    size_t prefixLen = strlen(targetPrefix_myID);
    auto myIDs = ExtractStrings(targetPrefix_myID, prefixLen, '\x10');
    for (const std::string& myID : myIDs)
        players.erase(myID);

    WORD port = FindLocalWebApiPort();
    std::wstring clipText;

    for (const auto& name : players)
    {
        std::string battleTag;
        if (port > 0)
        {
            std::string json = LocalWebApiGet(name, port);
            battleTag = JsonStringVal(json, "battle_tag");
        }
        if (battleTag.empty()) continue;

        int taglen = MultiByteToWideChar(CP_UTF8, 0, battleTag.c_str(), -1, NULL, 0);
        std::wstring wtag(taglen, 0);
        MultiByteToWideChar(CP_UTF8, 0, battleTag.c_str(), -1, &wtag[0], taglen);

        if (!clipText.empty()) clipText += L"\r\n";
        clipText += wtag + L"#";
    }

    if (clipText.empty()) return;

    if (OpenClipboard(NULL))
    {
        EmptyClipboard();
        size_t bytes = (clipText.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hMem)
        {
            memcpy(GlobalLock(hMem), clipText.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
        CloseClipboard();
    }

    nid.uFlags = NIF_INFO;
    wcscpy_s(nid.szInfoTitle, L"BattleTag Copied");
    wcscpy_s(nid.szInfo, clipText.c_str());
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIcon(NIM_MODIFY, &nid);
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
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
        if (g_hProcess) CloseHandle(g_hProcess);
        g_hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, g_starcraftPID);
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
// 키보드 훅
// ---------------------------------------------------------------------------
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        KBDLLHOOKSTRUCT* pKbd = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool blockEvent = false;
        INPUT input = { 0 };
        input.type = INPUT_KEYBOARD;

        HWND hForeground = GetForegroundWindow();
        DWORD foregroundPID = 0;
        GetWindowThreadProcessId(hForeground, &foregroundPID);

        bool scFg      = (foregroundPID == g_starcraftPID);
        bool overlayFg = (hForeground == g_hOverlay);

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
        {
            // F1 키: GUI 토글 — SC나 overlay가 포그라운드일 때 (SC-only 블록 밖에서 처리)
            if (pKbd->vkCode == VK_F1 && (scFg || overlayFg) && g_hOverlay)
            {
                g_showGui = !g_showGui;
                LONG_PTR ex = GetWindowLongPtr(g_hOverlay, GWL_EXSTYLE);
                if (g_showGui)
                    SetWindowLongPtr(g_hOverlay, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
                else
                    SetWindowLongPtr(g_hOverlay, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
                UpdateOverlayPosition();
                RenderOverlay();
                return 1; // SC에 전달하지 않음
            }

            if (scFg)
            {
                if (pKbd->vkCode == KEY_IGNORE)
                {
                    std::thread t(DoExtraction); t.detach();
                }
                if (pKbd->vkCode == KEY_UNIGNORE)
                {
                    std::thread t(DoRemoval); t.detach();
                }
                if (pKbd->vkCode == VK_RETURN)
                {
                    g_chatMode = !g_chatMode;
                }
                if (pKbd->vkCode == VK_ESCAPE && g_chatMode)
                {
                    g_chatMode = false;
                }
                if (pKbd->vkCode == KEY_SWAP_CTRL)
                {
                    g_swapSpaceAndControl = !g_swapSpaceAndControl;
                    if (g_hSettingDlg)
                        CheckDlgButton(g_hSettingDlg, IDC_SWAP_KEY, g_swapSpaceAndControl ? BST_CHECKED : BST_UNCHECKED);
                    return 1;
                }
            }

            if (scFg && g_swapSpaceAndControl && g_isInGame && !g_chatMode && pKbd->vkCode == KEY_ADDITIONAL_CTRL)
            {
                input.ki.wVk = VK_CONTROL;
                input.ki.dwFlags = 0;
                SendInput(1, &input, sizeof(INPUT));
                blockEvent = true;
            }
        }
        else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
        {
            if (scFg && g_swapSpaceAndControl && g_isInGame && !g_chatMode && pKbd->vkCode == KEY_ADDITIONAL_CTRL)
            {
                input.ki.wVk = VK_CONTROL;
                input.ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(1, &input, sizeof(INPUT));
                blockEvent = true;
            }
        }
        if (blockEvent) return 1;
    }
    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 설정 저장/로드
// ---------------------------------------------------------------------------
static const wchar_t* REG_KEY = L"Software\\bw_auto_ignore";

void SaveSettings()
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return;
    DWORD val;
    val = g_swapSpaceAndControl ? 1 : 0;
    RegSetValueExW(hKey, L"SwapSpaceAndControl", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    val = g_showMapName ? 1 : 0;
    RegSetValueExW(hKey, L"ShowMapName", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    val = g_autoIgnoreOnGameStart ? 1 : 0;
    RegSetValueExW(hKey, L"AutoIgnoreOnGameStart", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    val = g_copyBattleTagOnGameStart ? 1 : 0;
    RegSetValueExW(hKey, L"CopyBattleTagOnGameStart", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    RegCloseKey(hKey);
}

void LoadSettings()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return;
    DWORD val, size = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"SwapSpaceAndControl", NULL, NULL, (BYTE*)&val, &size) == ERROR_SUCCESS)
        g_swapSpaceAndControl = (val != 0);
    size = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"ShowMapName", NULL, NULL, (BYTE*)&val, &size) == ERROR_SUCCESS)
        g_showMapName = (val != 0);
    size = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"AutoIgnoreOnGameStart", NULL, NULL, (BYTE*)&val, &size) == ERROR_SUCCESS)
        g_autoIgnoreOnGameStart = (val != 0);
    size = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"CopyBattleTagOnGameStart", NULL, NULL, (BYTE*)&val, &size) == ERROR_SUCCESS)
        g_copyBattleTagOnGameStart = (val != 0);
    RegCloseKey(hKey);
}

// ---------------------------------------------------------------------------
// 설정 대화상자 (트레이 메뉴에서 접근 가능)
// ---------------------------------------------------------------------------
INT_PTR CALLBACK SettingDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_INITDIALOG:
        CheckDlgButton(hDlg, IDC_SWAP_KEY, g_swapSpaceAndControl ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_SHOW_MAP_NAME, g_showMapName ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_AUTO_IGNORE, g_autoIgnoreOnGameStart ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_COPY_BATTLE_TAG, g_copyBattleTagOnGameStart ? BST_CHECKED : BST_UNCHECKED);
        return (INT_PTR)TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            g_swapSpaceAndControl      = (IsDlgButtonChecked(hDlg, IDC_SWAP_KEY) == BST_CHECKED);
            g_showMapName              = (IsDlgButtonChecked(hDlg, IDC_SHOW_MAP_NAME) == BST_CHECKED);
            g_autoIgnoreOnGameStart    = (IsDlgButtonChecked(hDlg, IDC_AUTO_IGNORE) == BST_CHECKED);
            g_copyBattleTagOnGameStart = (IsDlgButtonChecked(hDlg, IDC_COPY_BATTLE_TAG) == BST_CHECKED);
            SaveSettings();
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

// ---------------------------------------------------------------------------
// 메인 창 / 트레이
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_APP + 1:
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
        if (LOWORD(wParam) == 1002)
        {
            MessageBox(hWnd,
                L"F9 : 사용자 무시\nF8 : 사용자 무시 해제\nF1 : 설정 GUI 토글",
                L"Help", MB_OK | MB_ICONINFORMATION);
        }
        else if (LOWORD(wParam) == 1003)
        {
            DestroyWindow(hWnd);
        }
        else if (LOWORD(wParam) == 1004)
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
    wcex.cbSize        = sizeof(WNDCLASSEX);
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = WndProc;
    wcex.hInstance     = hInstance;
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName  = NULL;
    wcex.lpszClassName = szWindowClass;
    wcex.hIcon         = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_BWAUTOIGNORE));
    wcex.hIconSm       = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_BWAUTOIGNORE));
    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;
    HWND hWnd = CreateWindowW(szWindowClass, L"bw_auto_ignore", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
        nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) return FALSE;
    ShowWindow(hWnd, SW_HIDE);
    UpdateWindow(hWnd);

    nid.cbSize          = sizeof(NOTIFYICONDATA);
    nid.hWnd            = hWnd;
    nid.uID             = 1;
    nid.uFlags          = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP + 1;
    nid.hIcon           = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_BWAUTOIGNORE));
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

    HANDLE hMutex = CreateMutex(NULL, TRUE, L"Local\\bw_auto_ignoreMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    _setmode(_fileno(stdout), _O_U16TEXT);
    setlocale(LC_ALL, "");

    LoadSettings();

    MyRegisterClass(hInstance);
    if (!InitInstance(hInstance, nCmdShow)) return FALSE;

    StartKeyboardHook();
    CreateOverlayWindow(hInstance);

    std::thread monitorThread(ProcessMonitorThread);
    monitorThread.detach();

    MessageBox(NULL, L"bw_auto_ignore 실행 중. 트레이 아이콘을 확인하세요.\n게임 중 ` 키로 설정창을 열 수 있습니다.",
        L"bw_auto_ignore", MB_OK | MB_ICONINFORMATION);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    StopKeyboardHook();
    if (g_imguiInitialized)
    {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    if (g_pRenderTargetView) g_pRenderTargetView->Release();
    if (g_pSwapChain) g_pSwapChain->Release();
    if (g_pd3dContext) g_pd3dContext->Release();
    if (g_pd3dDevice) g_pd3dDevice->Release();
    if (g_hProcess) CloseHandle(g_hProcess);
    Shell_NotifyIcon(NIM_DELETE, &nid);
    if (hMutex) CloseHandle(hMutex);

    return (int)msg.wParam;
}
