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
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
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
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// StarCraft.exe 메모리 오프셋 상수
// ---------------------------------------------------------------------------
const ULONGLONG PLAYER_TABLE_OFFSET = 0x10931B0;
const int PLAYER_SLOT_SIZE  = 104;
const int PLAYER_SLOT_COUNT = 8;
const int PLAYER_NAME_OFFSET = 8;

const ULONGLONG MAP_NAME_OFFSET    = 0x1091FEE;
const ULONGLONG IS_IN_GAME_OFFSET  = 0x1090612;
const ULONGLONG LOBBY_STATE_OFFSET = 0x1091F9C; // 20=로비, 0=그 외
const ULONGLONG CREATE_MODE_OFFSET = 0x1092011; // 1=방 생성 화면, 0=그 외
const ULONGLONG CHAT_MODE_OFFSET   = 0x10B6BD8; // 1=채팅 입력 중, 0=그 외

// ---------------------------------------------------------------------------
// 전역 변수
// ---------------------------------------------------------------------------
int KEY_IGNORE = VK_F9;
int KEY_UNIGNORE = VK_F8;
int KEY_ADDITIONAL_CTRL = VK_SPACE;

HWND g_hSettingDlg = NULL;
DWORD g_starcraftPID = 0;
HANDLE g_hProcess = NULL;
std::set<std::string> g_extractedSet;
std::mutex g_mutex;
HHOOK g_hHook = NULL;
HINSTANCE hInst;
WCHAR szWindowClass[] = L"BW_AutoIgnoreWndClass";
NOTIFYICONDATA nid = { 0 };

bool g_swapSpaceAndControl = false;
bool g_autoIgnoreOnGameStart = false;
bool g_fastJoin = false;      // 공개방 빠른 입장 (현재 비활성)
bool g_fastJoinActive = false;
bool g_isInGame = false;
bool g_wasInGame = false;
ULONGLONG g_scModuleBase = 0;

HWND g_hOverlay = NULL;
HWND g_hStarCraftWnd = NULL;
WORD g_cachedApiPort = 0;
std::wstring g_mapName = L"";

// D3D11 + imgui
ID3D11Device*           g_pd3dDevice       = NULL;
ID3D11DeviceContext*    g_pd3dContext       = NULL;
IDXGISwapChain*         g_pSwapChain       = NULL;
ID3D11RenderTargetView* g_pRenderTargetView = NULL;
bool                    g_imguiInitialized  = false;
bool                    g_showGui           = false;

// ---------------------------------------------------------------------------
// 전적 조회 데이터 구조
// ---------------------------------------------------------------------------
struct ToonStat {
    std::string name;       // 인게임 이름 (UTF-8)
    int  gateway    = 0;    // 10=USW 11=USE 12=EU 20=Asia 30=KR
    char cur_tier   = 'U'; // 현재 시즌 티어
    int  cur_rating = 0;   // 현재 시즌 점수
    int  cur_season = 0;   // 현재 시즌 번호
    char cur_race   = 'U'; // 현재 시즌 주력 종족 Z/T/P/U
    char best_tier  = 'U'; // 역대 최고 티어
    int  best_rating = 0;  // 역대 최고 점수
    int  best_season = 0;  // 역대 최고 달성 시즌 번호
    int  win_streak  = 0;  // 현재 연승
    int  loss_streak = 0;  // 현재 연패
};

struct DisplayProfile {
    std::string          battleTag;
    std::vector<ToonStat> toons;
    bool valid    = false;
    bool fetching = false;
    std::string statusMsg;
};

static std::vector<DisplayProfile> g_autoProfiles;
static bool          g_autoFetching = false;
static std::mutex    g_profileMutex;

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

std::set<std::string> ExtractStrings(const char* prefix, size_t plen, char tail = '\0');
void DoExtraction();
void DoRemoval();
void SaveSettings();
void UpdateOverlayPosition();
static void RenderOverlay();

bool IsCreateScreen();
void EnableFastJoin();
void DisableFastJoin();

void StartKeyboardHook();
void StopKeyboardHook();

void UpdateStarCraftProcess();
void ProcessMonitorThread();

// ---------------------------------------------------------------------------
// 기본 유틸리티
// ---------------------------------------------------------------------------
DWORD GetProcessID(const wchar_t* processName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe32{}; pe32.dwSize = sizeof(pe32);
    if (Process32FirstW(snapshot, &pe32)) {
        do {
            if (_wcsicmp(pe32.szExeFile, processName) == 0) {
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
    if (g_scModuleBase) return g_scModuleBase;
    if (!g_hProcess) return 0;
    HMODULE mods[1024]; DWORD needed = 0;
    if (!EnumProcessModulesEx(g_hProcess, mods, sizeof(mods), &needed, LIST_MODULES_ALL)) return 0;
    WCHAR name[MAX_PATH];
    for (int i = 0; i < (int)(needed / sizeof(HMODULE)); i++) {
        if (GetModuleFileNameExW(g_hProcess, mods[i], name, MAX_PATH)) {
            WCHAR* fn = wcsrchr(name, L'\\');
            if (fn && _wcsicmp(fn + 1, L"StarCraft.exe") == 0) {
                g_scModuleBase = (ULONGLONG)mods[i];
                return g_scModuleBase;
            }
        }
    }
    return 0;
}

std::wstring ReadMapName()
{
    ULONGLONG base = GetStarCraftModuleBase();
    if (!base || !g_hProcess) return L"";
    BYTE buf[64] = {}; SIZE_T r = 0;
    ReadProcessMemory(g_hProcess, (LPCVOID)(base + MAP_NAME_OFFSET), buf, sizeof(buf), &r);
    size_t len = 0; while (len < r && buf[len]) len++;
    std::string utf8((char*)buf, len);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
    if (wlen <= 0) return L"";
    std::wstring wide(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], wlen);
    std::wstring result;
    for (wchar_t c : wide) if (c >= 0x20) result += c;
    return result;
}

bool IsInGame()
{
    ULONGLONG base = GetStarCraftModuleBase();
    if (!base || !g_hProcess) return false;
    BYTE flag = 0; SIZE_T r = 0;
    ReadProcessMemory(g_hProcess, (LPCVOID)(base + IS_IN_GAME_OFFSET), &flag, 1, &r);
    return flag == 1;
}

bool IsChatMode()
{
    ULONGLONG base = GetStarCraftModuleBase();
    if (!base || !g_hProcess) return false;
    BYTE flag = 0; SIZE_T r = 0;
    ReadProcessMemory(g_hProcess, (LPCVOID)(base + CHAT_MODE_OFFSET), &flag, 1, &r);
    return flag == 1;
}

bool IsCreateScreen()
{
    ULONGLONG base = GetStarCraftModuleBase();
    if (!base || !g_hProcess) return false;
    BYTE v = 0; SIZE_T r = 0;
    ReadProcessMemory(g_hProcess, (LPCVOID)(base + CREATE_MODE_OFFSET), &v, 1, &r);
    return v == 1;
}

static std::wstring GetScDataDir()
{
    WCHAR docs[MAX_PATH] = {};
    SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docs);
    return std::wstring(docs) + L"\\StarCraft";
}

static void MergeDir(const std::wstring& srcDir, const std::wstring& dstDir)
{
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((srcDir + L"\\*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        MoveFileW((srcDir + L"\\" + fd.cFileName).c_str(),
                  (dstDir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
}

void EnableFastJoin()
{
    if (g_fastJoinActive) return;
    std::wstring base  = GetScDataDir();
    std::wstring maps  = base + L"\\maps";
    std::wstring dlSrc = maps + L"\\Download";
    std::wstring dlDst = base + L"\\Download_bwai";
    if (GetFileAttributesW(dlSrc.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(dlDst.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MoveFileW(dlSrc.c_str(), dlDst.c_str());
        CreateDirectoryW(dlSrc.c_str(), NULL);
    }
    std::wstring rpSrc = maps + L"\\Replay";
    std::wstring rpDst = base + L"\\Replay_bwai";
    if (GetFileAttributesW(rpSrc.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(rpDst.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MoveFileW(rpSrc.c_str(), rpDst.c_str());
        CreateDirectoryW(rpSrc.c_str(), NULL);
    }
    g_fastJoinActive = true;
}

void DisableFastJoin()
{
    if (!g_fastJoinActive) return;
    std::wstring base  = GetScDataDir();
    std::wstring maps  = base + L"\\maps";
    std::wstring dlSrc = maps + L"\\Download";
    std::wstring dlDst = base + L"\\Download_bwai";
    if (GetFileAttributesW(dlDst.c_str()) != INVALID_FILE_ATTRIBUTES) {
        MergeDir(dlSrc, dlDst);
        RemoveDirectoryW(dlSrc.c_str());
        MoveFileW(dlDst.c_str(), dlSrc.c_str());
    }
    std::wstring rpSrc = maps + L"\\Replay";
    std::wstring rpDst = base + L"\\Replay_bwai";
    if (GetFileAttributesW(rpDst.c_str()) != INVALID_FILE_ATTRIBUTES) {
        MergeDir(rpSrc, rpDst);
        RemoveDirectoryW(rpSrc.c_str());
        MoveFileW(rpDst.c_str(), rpSrc.c_str());
    }
    g_fastJoinActive = false;
}

struct EnumData { DWORD pid; HWND hwnd; };
BOOL CALLBACK FindWindowByPID(HWND hwnd, LPARAM lParam) {
    EnumData* d = (EnumData*)lParam; DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == d->pid && IsWindowVisible(hwnd)) { d->hwnd = hwnd; return FALSE; }
    return TRUE;
}
HWND GetStarCraftWindow() {
    if (!g_starcraftPID) return NULL;
    EnumData d = { g_starcraftPID, NULL };
    EnumWindows(FindWindowByPID, (LPARAM)&d);
    return d.hwnd;
}

void UpdateOverlayPosition()
{
    if (!g_hOverlay) return;
    if (!g_hStarCraftWnd || !IsWindow(g_hStarCraftWnd))
        g_hStarCraftWnd = GetStarCraftWindow();
    HWND hSC = g_hStarCraftWnd;
    if (!hSC) { ShowWindow(g_hOverlay, SW_HIDE); return; }

    HWND hFg = GetForegroundWindow();
    bool scOrOverlayFg = (hFg == hSC || hFg == g_hOverlay);
    if (!scOrOverlayFg || (!g_isInGame && !g_showGui)) { ShowWindow(g_hOverlay, SW_HIDE); return; }

    RECT cr; GetClientRect(hSC, &cr);
    POINT tl = {0, 0}; ClientToScreen(hSC, &tl);
    int w = cr.right - cr.left, h = cr.bottom - cr.top;

    // 위치/크기 변경 시에만 SetWindowPos 호출
    static POINT s_lastTL = {-1,-1}; static SIZE s_lastSz = {-1,-1};
    if (tl.x != s_lastTL.x || tl.y != s_lastTL.y || w != s_lastSz.cx || h != s_lastSz.cy) {
        s_lastTL = tl; s_lastSz = {w, h};
        SetWindowPos(g_hOverlay, HWND_TOPMOST, tl.x, tl.y, w, h, SWP_NOACTIVATE);
    }
    ShowWindow(g_hOverlay, SW_SHOW);
}

// ---------------------------------------------------------------------------
// D3D11 헬퍼
// ---------------------------------------------------------------------------
static void CreateRenderTarget() {
    ID3D11Texture2D* pBack = NULL;
    g_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBack);
    if (pBack) { g_pd3dDevice->CreateRenderTargetView(pBack, NULL, &g_pRenderTargetView); pBack->Release(); }
}
static void CleanupRenderTarget() {
    if (g_pRenderTargetView) { g_pRenderTargetView->Release(); g_pRenderTargetView = NULL; }
}

// ---------------------------------------------------------------------------
// 전적 조회 헬퍼
// ---------------------------------------------------------------------------
static char BucketToTier(int b) {
    switch (b) { case 1: return 'F'; case 2: return 'E'; case 3: return 'D';
                 case 4: return 'C'; case 5: return 'B'; case 6: return 'A';
                 case 7: return 'S'; default: return 'U'; }
}

static ImVec4 TierColor(char t) {
    switch (t) {
    case 'S': return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);       // 노랑
    case 'A': return ImVec4(1.0f, 0.2f, 0.2f, 1.0f);       // 빨강
    case 'B': return ImVec4(0.7f, 0.0f, 1.0f, 1.0f);       // 보라
    case 'C': return ImVec4(0.3f, 0.5f, 1.0f, 1.0f);       // 파랑
    case 'D': return ImVec4(0.0f, 1.0f, 1.0f, 1.0f);       // 시안
    case 'E': return ImVec4(0.0f, 0.82f, 0.0f, 1.0f);      // 초록 (pure green 회피)
    default:  return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);       // 흰색 (F, U)
    }
}

static const char* GatewayName(int gw) {
    switch (gw) { case 10: return "USW"; case 11: return "USE";
                  case 12: return "EU";  case 20: return "Asia"; case 30: return "KR"; default: return "?"; }
}

static std::string JsonStringVal(const std::string& json, const std::string& key, size_t from = 0) {
    std::string fk = "\"" + key + "\"";
    size_t pos = json.find(fk, from); if (pos == std::string::npos) return "";
    pos += fk.size();
    while (pos < json.size() && (json[pos]==' '||json[pos]=='\t'||json[pos]==':')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;
    size_t e = pos;
    while (e < json.size() && json[e] != '"') { if (json[e]=='\\') e++; e++; }
    return json.substr(pos, e - pos);
}

static int JsonIntVal(const std::string& json, const std::string& key, size_t from = 0) {
    std::string fk = "\"" + key + "\"";
    size_t pos = json.find(fk, from); if (pos == std::string::npos) return 0;
    pos += fk.size();
    while (pos < json.size() && (json[pos]==' '||json[pos]=='\t'||json[pos]==':')) pos++;
    return atoi(json.c_str() + pos);
}

// 중첩 괄호 끝 위치 찾기
static size_t FindMatchingBrace(const std::string& s, size_t open) {
    if (open >= s.size()) return std::string::npos;
    char cl = (s[open] == '[') ? ']' : '}';
    char op = s[open]; int depth = 0;
    for (size_t i = open; i < s.size(); i++) {
        if (s[i] == op) depth++;
        else if (s[i] == cl) { if (--depth == 0) return i; }
    }
    return std::string::npos;
}

// 특정 포트에 web-api 서버가 응답하는지 확인
static bool ProbeWebApiPort(WORD port) {
    HINTERNET hS = WinHttpOpen(L"bw/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return false;
    WinHttpSetTimeouts(hS, 1000, 1000, 1000, 1000);
    HINTERNET hC = WinHttpConnect(hS, L"127.0.0.1", port, 0);
    if (!hC) { WinHttpCloseHandle(hS); return false; }
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", L"/web-api/", NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    bool ok = false;
    if (hR &&
        WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hR, NULL)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            NULL, &status, &sz, NULL);
        ok = (status == 200 || status == 404); // 401은 인증 필요한 다른 서버
    }
    if (hR) WinHttpCloseHandle(hR);
    WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
    return ok;
}

// StarCraft 로컬 웹서버 포트 탐색
static WORD FindLocalWebApiPort() {
    if (!g_hProcess) return 0;
    const char pat[] = "127.0.0.1:";
    std::set<WORD> candidates;
    MEMORY_BASIC_INFORMATION mbi = {}; ULONGLONG addr = 0;
    while (VirtualQueryEx(g_hProcess, (LPCVOID)addr, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS|PAGE_GUARD))
            && mbi.RegionSize > 0 && mbi.RegionSize < 0x40000000) {
            std::vector<char> buf(mbi.RegionSize); SIZE_T r = 0;
            if (ReadProcessMemory(g_hProcess, (LPCVOID)addr, buf.data(), mbi.RegionSize, &r)) {
                for (size_t i = 0; i + sizeof(pat) + 5 < r; i++) {
                    if (memcmp(buf.data()+i, pat, sizeof(pat)-1) == 0) {
                        size_t ps = i + sizeof(pat)-1, pe = ps;
                        while (pe < r && buf[pe] >= '0' && buf[pe] <= '9') pe++;
                        size_t pl = pe - ps;
                        if (pl >= 4 && pl <= 5) {
                            int p = atoi(buf.data()+ps);
                            if (p > 1024 && p < 65536) candidates.insert((WORD)p);
                        }
                    }
                }
            }
        }
        addr += (mbi.RegionSize ? mbi.RegionSize : 1);
        if (!addr) break;
    }
    // 후보 포트 중 실제 web-api 응답하는 포트 반환
    for (WORD p : candidates)
        if (ProbeWebApiPort(p)) { g_cachedApiPort = p; return p; }
    return 0;
}

static std::string LocalWebApiGet(const std::string& name, WORD port, int gateway = 30) {
    int wl = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), (int)name.size(), NULL, 0);
    std::wstring wn(wl, 0);
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), (int)name.size(), &wn[0], wl);
    std::wstring path = L"/web-api/v2/aurora-profile-by-toon/" + wn + L"/" + std::to_wstring(gateway) + L"?request_flags=scr_tooninfo";

    HINTERNET hS = WinHttpOpen(L"StarCraft/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return "";
    HINTERNET hC = WinHttpConnect(hS, L"127.0.0.1", port, 0);
    if (!hC) { WinHttpCloseHandle(hS); return ""; }
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path.c_str(), NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return ""; }

    std::string result;
    if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hR, NULL)) {
        DWORD sz = 0;
        do {
            if (!WinHttpQueryDataAvailable(hR, &sz) || !sz) break;
            std::vector<char> b(sz+1, 0); DWORD rd = 0;
            WinHttpReadData(hR, b.data(), sz, &rd);
            result.append(b.data(), rd);
        } while (sz > 0);
    }
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
    return result;
}

// 전적 조회 (스레드에서 호출)
static DisplayProfile FetchProfileData(const std::string& queryName)
{
    DisplayProfile result;
    // 캐시된 포트 우선 사용, 실패 시 재탐색
    WORD port = g_cachedApiPort;
    if (!port || !ProbeWebApiPort(port)) {
        g_cachedApiPort = 0;
        port = FindLocalWebApiPort();
    }
    if (!port) { result.statusMsg = "StarCraft not running"; return result; }

    // 모든 게이트웨이 순서대로 시도 (KR=30, Asia=20, USW=10, USE=11, EU=12)
    const int gateways[] = { 30, 20, 10, 11, 12 };
    std::string json;
    for (int gw : gateways) {
        std::string gwJson = LocalWebApiGet(queryName, port, gw);
        if (!gwJson.empty() && !JsonStringVal(gwJson, "battle_tag").empty()) {
            json = gwJson;
            break;
        }
    }
    if (json.empty()) { result.statusMsg = "No response"; return result; }

    result.battleTag = JsonStringVal(json, "battle_tag");
    if (result.battleTag.empty()) { result.statusMsg = "Not found"; return result; }

    // 1. toons[] 파싱 → guid→(name,gateway) 맵
    struct ToonInfo { std::string name; int gateway; };
    std::map<int, ToonInfo> guidMap; // guid → ToonInfo

    auto parseArray = [&](const std::string& arrayKey, auto callback) {
        size_t kpos = json.find("\"" + arrayKey + "\"");
        if (kpos == std::string::npos) return;
        size_t aStart = json.find('[', kpos);
        if (aStart == std::string::npos) return;
        size_t aEnd = FindMatchingBrace(json, aStart);
        if (aEnd == std::string::npos) aEnd = json.size();
        size_t cur = aStart + 1;
        while (cur < aEnd) {
            size_t oS = json.find('{', cur);
            if (oS == std::string::npos || oS >= aEnd) break;
            size_t oE = FindMatchingBrace(json, oS);
            if (oE == std::string::npos || oE >= aEnd) break;
            std::string obj = json.substr(oS, oE - oS + 1);
            callback(obj);
            cur = oE + 1;
        }
    };

    parseArray("toons", [&](const std::string& obj) {
        std::string name = JsonStringVal(obj, "toon");
        int guid = JsonIntVal(obj, "guid");
        int gw   = JsonIntVal(obj, "gateway_id");
        if (!name.empty() && guid > 0) guidMap[guid] = {name, gw};
    });

    // toon_guid_by_gateway 에서 name→gateway 맵 빌드 (폴백용)
    // 구조: {"30": {"toon_name": guid, ...}, "10": {...}, ...}
    std::map<std::string, int> nameToGw;
    {
        size_t gwPos = json.find("\"toon_guid_by_gateway\"");
        if (gwPos != std::string::npos) {
            size_t blockStart = json.find('{', gwPos);
            if (blockStart != std::string::npos) {
                size_t blockEnd = FindMatchingBrace(json, blockStart);
                std::string block = json.substr(blockStart, blockEnd - blockStart + 1);
                const int knownGws[] = {10, 11, 12, 20, 30};
                for (int gw : knownGws) {
                    std::string gwKey = "\"" + std::to_string(gw) + "\"";
                    size_t kp = block.find(gwKey);
                    if (kp == std::string::npos) continue;
                    size_t ob = block.find('{', kp);
                    if (ob == std::string::npos) continue;
                    size_t oe = FindMatchingBrace(block, ob);
                    std::string inner = block.substr(ob + 1, oe - ob - 1);
                    size_t p = 0;
                    while (p < inner.size()) {
                        size_t qs = inner.find('"', p); if (qs == std::string::npos) break;
                        size_t qe = inner.find('"', qs + 1); if (qe == std::string::npos) break;
                        std::string tname = inner.substr(qs + 1, qe - qs - 1);
                        if (!tname.empty()) nameToGw[tname] = gw;
                        size_t comma = inner.find(',', qe);
                        p = (comma != std::string::npos) ? comma + 1 : inner.size();
                    }
                }
            }
        }
    }

    // 2. stats[] 파싱 → (name,gateway)→종족 맵
    std::map<std::pair<std::string,int>, char> raceMap;
    parseArray("stats", [&](const std::string& obj) {
        std::string name = JsonStringVal(obj, "toon");
        int gw = JsonIntVal(obj, "gateway_id");
        if (name.empty()) return;
        size_t rawP = obj.find("\"raw\"");
        if (rawP == std::string::npos) return;
        int z = JsonIntVal(obj, "zerg_wins_sum",    rawP);
        int t = JsonIntVal(obj, "terran_wins_sum",  rawP);
        int p = JsonIntVal(obj, "protoss_wins_sum", rawP);
        char race = 'U';
        if      (z >= t && z >= p && z > 0) race = 'Z';
        else if (t >= z && t >= p && t > 0) race = 'T';
        else if (p > 0)                     race = 'P';
        raceMap[{name, gw}] = race;
    });

    // 3. matchmaked_stats[] 파싱
    struct StatEntry { int season_id, bucket, rating, win_streak, loss_streak; };
    std::map<std::pair<std::string,int>, std::vector<StatEntry>> allStats;

    parseArray("matchmaked_stats", [&](const std::string& obj) {
        std::string name = JsonStringVal(obj, "toon");
        int guid  = JsonIntVal(obj, "toon_guid");
        int sid   = JsonIntVal(obj, "season_id");
        int bkt   = JsonIntVal(obj, "bucket");
        int rat   = JsonIntVal(obj, "rating");
        int ws    = JsonIntVal(obj, "win_streak");
        int ls    = JsonIntVal(obj, "loss_streak");
        if (name.empty()) return;
        int gw = 0;
        auto it = guidMap.find(guid);
        if (it != guidMap.end()) gw = it->second.gateway;
        if (gw == 0) {
            auto nit = nameToGw.find(name);
            if (nit != nameToGw.end()) gw = nit->second;
        }
        allStats[{name, gw}].push_back({sid, bkt, rat, ws, ls});
    });

    // 4. ToonStat 빌드
    for (auto& [key, entries] : allStats) {
        auto& [toonName, gw] = key;
        ToonStat stat; stat.name = toonName; stat.gateway = gw;

        auto raceIt = raceMap.find({toonName, gw});
        stat.cur_race = (raceIt != raceMap.end()) ? raceIt->second : 'U';

        int maxSid = -1;
        for (auto& e : entries) maxSid = std::max(maxSid, e.season_id);
        for (auto& e : entries) {
            if (e.season_id == maxSid) {
                stat.cur_tier    = BucketToTier(e.bucket);
                stat.cur_rating  = e.rating;
                stat.cur_season  = e.season_id;
                stat.win_streak  = e.win_streak;
                stat.loss_streak = e.loss_streak;
                break;
            }
        }
        int maxBkt = 0;
        for (auto& e : entries) {
            if (e.bucket > maxBkt) {
                maxBkt = e.bucket;
                stat.best_tier   = BucketToTier(e.bucket);
                stat.best_rating = e.rating;
                stat.best_season = e.season_id;
            }
        }
        result.toons.push_back(stat);
    }

    // stats 없는 toon도 표시
    for (auto& [guid, info] : guidMap) {
        bool found = false;
        for (auto& t : result.toons) if (t.name == info.name && t.gateway == info.gateway) { found = true; break; }
        if (!found) { ToonStat s; s.name = info.name; s.gateway = info.gateway; result.toons.push_back(s); }
    }

    std::sort(result.toons.begin(), result.toons.end(), [](const ToonStat& a, const ToonStat& b) {
        return a.gateway != b.gateway ? a.gateway < b.gateway : a.name < b.name;
    });

    result.valid = true;
    return result;
}

// 게임 시작 시 상대방 자동 조회 (스레드에서 호출)
static void AutoFetchOpponents()
{
    auto players = ReadCurrentGamePlayers();
    if (players.empty()) { std::lock_guard<std::mutex> lk(g_profileMutex); g_autoFetching = false; return; }

    const char myPfx[] = u8"HAT:";
    auto myIDs = ExtractStrings(myPfx, strlen(myPfx), '\x10');
    for (auto& id : myIDs) players.erase(id);

    std::vector<DisplayProfile> results;
    for (auto& name : players) {
        DisplayProfile p = FetchProfileData(name);
        if (!p.battleTag.empty()) results.push_back(p);
        else { p.statusMsg = name + ": " + p.statusMsg; results.push_back(p); }
    }

    std::lock_guard<std::mutex> lk(g_profileMutex);
    g_autoProfiles = results;
    g_autoFetching = false;
}

// ---------------------------------------------------------------------------
// imgui 렌더링
// ---------------------------------------------------------------------------
static void RenderOverlay()
{
    if (!g_imguiInitialized || !g_pRenderTargetView) return;

    // 보여줄 내용이 없으면 Present 건너뜀 (GPU/DWM 부하 제거)
    bool hasContent = g_showGui || (!g_mapName.empty() && g_isInGame);
    static bool s_lastHasContent = false;
    if (!hasContent && !s_lastHasContent) return;
    s_lastHasContent = hasContent;

    // 완전 투명: DWM 퍼픽셀 알파로 합성
    float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_pd3dContext->OMSetRenderTargets(1, &g_pRenderTargetView, NULL);
    g_pd3dContext->ClearRenderTargetView(g_pRenderTargetView, clear);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // 설정 GUI
    if (g_showGui)
    {
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints(ImVec2(420, 60), ImVec2(600, 700));
        ImGui::Begin(u8"bw_auto_ignore", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);

        // 맵 이름 (인게임)
        if (!g_mapName.empty() && g_isInGame) {
            std::string utf8(g_mapName.size()*3+1, 0);
            WideCharToMultiByte(CP_UTF8, 0, g_mapName.c_str(), -1, &utf8[0], (int)utf8.size(), NULL, NULL);
            ImGui::SetWindowFontScale(1.3f);
            ImGui::TextColored(ImVec4(1,1,0.4f,1), u8"맵: %s", utf8.c_str());
            ImGui::SetWindowFontScale(1.0f);
            ImGui::Separator();
        }

        // ============================================================
        // 전적 조회
        // ============================================================
        ImGui::SeparatorText(u8"전적 조회");
        {
            // 테이블 렌더 람다
            auto RenderProfileTable = [](const DisplayProfile& prof) {
                if (!prof.statusMsg.empty())
                    ImGui::TextDisabled("%s", prof.statusMsg.c_str());
                if (!prof.valid) return;
                ImGui::TextColored(ImVec4(1,0.85f,0,1), u8"배틀태그: %s#", prof.battleTag.c_str());
                if (ImGui::BeginTable("##toons", 6,
                    ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
                {
                    ImGui::TableSetupColumn("GW",          ImGuiTableColumnFlags_WidthFixed, 32);
                    ImGui::TableSetupColumn(u8"아이디",     ImGuiTableColumnFlags_WidthFixed, 120);
                    ImGui::TableSetupColumn(u8"현재 시즌",  ImGuiTableColumnFlags_WidthFixed, 78);
                    ImGui::TableSetupColumn(u8"종족",       ImGuiTableColumnFlags_WidthFixed, 24);
                    ImGui::TableSetupColumn(u8"역대 최고",  ImGuiTableColumnFlags_WidthFixed, 78);
                    ImGui::TableSetupColumn(u8"연속",       ImGuiTableColumnFlags_WidthFixed, 48);
                    ImGui::TableHeadersRow();
                    for (auto& t : prof.toons) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("%s", GatewayName(t.gateway));
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%s", t.name.c_str());
                        ImGui::TableSetColumnIndex(2);
                        if (t.cur_tier != 'U') ImGui::TextColored(TierColor(t.cur_tier), "%c %4d S%d", t.cur_tier, t.cur_rating, t.cur_season);
                        else ImGui::TextDisabled(u8"미배치");
                        ImGui::TableSetColumnIndex(3);
                        char rc = t.cur_race; ImGui::Text("%c", (rc == 'U') ? '-' : rc);
                        ImGui::TableSetColumnIndex(4);
                        if (t.best_tier != 'U') ImGui::TextColored(TierColor(t.best_tier), "%c %4d S%d", t.best_tier, t.best_rating, t.best_season);
                        else ImGui::TextDisabled("-");
                        ImGui::TableSetColumnIndex(5);
                        if (t.win_streak > 0)
                            ImGui::TextColored(ImVec4(0.3f, 0.6f, 1.0f, 1.0f), u8"%d연승", t.win_streak);
                        else if (t.loss_streak > 0)
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), u8"%d연패", t.loss_streak);
                        else
                            ImGui::TextDisabled("-");
                    }
                    ImGui::EndTable();
                }
            };

            // --- 자동 조회 결과 (게임 중 상대방) ---
            bool autoFetching; std::vector<DisplayProfile> autoProfs;
            {
                std::lock_guard<std::mutex> lk(g_profileMutex);
                autoFetching = g_autoFetching;
                autoProfs    = g_autoProfiles;
            }
            if (autoFetching)
                ImGui::TextDisabled(u8"상대방 조회 중...");
            for (auto& prof : autoProfs)
                RenderProfileTable(prof);


        } // 전적 조회

        ImGui::End();
    } // g_showGui

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(0, 0);

    // GUI 영역만 윈도우 리전으로 설정 (변경 시에만)
    // - 리전 밖: 윈도우가 존재하지 않음 → SC가 마우스/커서 직접 처리
    // - 리전 안: HTTRANSPARENT → SC로 전달 (인터랙티브 요소 없으므로 문제없음)
    {
        static RECT s_lastRgn = {-2,-2,-2,-2};
        RECT newRgn = {0,0,0,0};

        if (g_showGui) {
            ImGuiWindow* w = ImGui::FindWindowByName(u8"bw_auto_ignore");
            if (w && w->Size.x > 0) {
                newRgn.left   = (LONG)w->Pos.x;
                newRgn.top    = (LONG)w->Pos.y;
                newRgn.right  = (LONG)(w->Pos.x + w->Size.x);
                newRgn.bottom = (LONG)(w->Pos.y + w->Size.y);
            }
        }
        // g_showGui==false 또는 창 없으면 newRgn={0,0,0,0} → 빈 리전

        if (memcmp(&newRgn, &s_lastRgn, sizeof(RECT)) != 0) {
            s_lastRgn = newRgn;
            HRGN rgn = CreateRectRgn(newRgn.left, newRgn.top, newRgn.right, newRgn.bottom);
            SetWindowRgn(g_hOverlay, rgn, FALSE);
        }
    }
}

// ---------------------------------------------------------------------------
// 오버레이 윈도우 프로시저
// ---------------------------------------------------------------------------
LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_NCHITTEST:
        return HTTRANSPARENT;  // 마우스 입력 항상 게임으로 통과
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_TIMER:
    {
        bool inGame = IsInGame();
        if (!g_wasInGame && inGame) {
            { std::lock_guard<std::mutex> lk(g_mutex); g_extractedSet.clear(); }
            // 상대방 전적 자동 조회
            {
                std::lock_guard<std::mutex> lk(g_profileMutex);
                g_autoProfiles.clear();
                g_autoFetching = true;
            }
            std::thread(AutoFetchOpponents).detach();
            // 홍보 메시지 (비활성화)
            // std::thread([](){
            //     Sleep(2000);
            //     SendToStarCraft(u8"[bw_auto_ignore] 상대방 전적 자동 조회 + 채팅 무시 프로그램 사용 중");
            // }).detach();
        }
        if (!g_wasInGame && inGame && g_autoIgnoreOnGameStart) {
            // 게임 시작 직후 플레이어 테이블이 채워지기까지 대기
            std::thread([](){
                Sleep(1500);
                DoExtraction();
            }).detach();
        }
        g_wasInGame = inGame;
        g_isInGame  = inGame;

        if (g_fastJoin && g_hProcess) {
            if (IsCreateScreen()) DisableFastJoin();
            else                  EnableFastJoin();
        }
        UpdateOverlayPosition();
        std::wstring nm = inGame ? ReadMapName() : L"";
        if (nm != g_mapName) g_mapName = nm;
        RenderOverlay();
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hWnd, 1);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 오버레이 창 생성
// ---------------------------------------------------------------------------
void CreateOverlayWindow(HINSTANCE hInstance)
{
    WNDCLASSEX wcex = { 0 };
    wcex.cbSize = sizeof(wcex); wcex.lpfnWndProc = OverlayWndProc;
    wcex.hInstance = hInstance; wcex.lpszClassName = L"BW_OverlayWndClass";
    RegisterClassEx(&wcex);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    g_hOverlay = CreateWindowEx(
        WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"BW_OverlayWndClass", L"", WS_POPUP,
        0, 0, sw, sh, NULL, NULL, hInstance, NULL);

    HICON hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_BWAUTOIGNORE));
    SendMessage(g_hOverlay, WM_SETICON, ICON_BIG,   (LPARAM)hIcon);
    SendMessage(g_hOverlay, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);

    // DWM 퍼픽셀 알파: 전체 클라이언트 영역에 적용
    MARGINS m = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_hOverlay, &m);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = sw; sd.BufferDesc.Height = sh;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate = {60, 1};
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hOverlay; sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        NULL, 0, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dContext);
    if (FAILED(hr)) { MessageBox(NULL, L"D3D11 init failed", L"Error", MB_OK); return; }

    CreateRenderTarget();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 15.0f,
        nullptr, io.Fonts->GetGlyphRangesKorean());

    ImGui::StyleColorsDark();
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 0.88f;

    ImGui_ImplWin32_Init(g_hOverlay);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dContext);
    g_imguiInitialized = true;

    SetTimer(g_hOverlay, 1, 200, NULL);
    SetWindowRgn(g_hOverlay, CreateRectRgn(0,0,0,0), FALSE); // 빈 리전으로 시작
    UpdateOverlayPosition();
}

// ---------------------------------------------------------------------------
// 플레이어 / 무시 관련
// ---------------------------------------------------------------------------
std::set<std::string> ReadCurrentGamePlayers()
{
    ULONGLONG base = GetStarCraftModuleBase();
    if (!base) return {};
    std::vector<BYTE> buf(PLAYER_SLOT_SIZE * PLAYER_SLOT_COUNT, 0); SIZE_T r = 0;
    if (!ReadProcessMemory(g_hProcess, (LPCVOID)(base + PLAYER_TABLE_OFFSET), buf.data(), buf.size(), &r)) return {};
    std::set<std::string> players;
    for (int i = 0; i < PLAYER_SLOT_COUNT; i++) {
        BYTE* slot = buf.data() + i * PLAYER_SLOT_SIZE;
        if (slot[0] != 0x01) continue;
        char* name = (char*)(slot + PLAYER_NAME_OFFSET);
        size_t nl = strnlen(name, PLAYER_SLOT_SIZE - PLAYER_NAME_OFFSET);
        if (nl > 0) players.insert(std::string(name, nl));
    }
    return players;
}

std::string ReadNullTerminatedString(HANDLE hProcess, ULONGLONG address, size_t maxLength)
{
    std::vector<char> buf(maxLength, 0); SIZE_T r = 0;
    if (!ReadProcessMemory(hProcess, (LPCVOID)address, buf.data(), maxLength, &r)) return "";
    size_t len = 0; while (len < r && buf[len]) len++;
    return std::string(buf.data(), len);
}

std::vector<ULONGLONG> FindAllPrefixAddresses(HANDLE hProcess, const char* prefix)
{
    std::vector<ULONGLONG> addrs; size_t pl = strlen(prefix);
    ULONGLONG cur = 0; MEMORY_BASIC_INFORMATION mbi; std::vector<BYTE> buf;
    while (VirtualQueryEx(hProcess, (LPCVOID)cur, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS|PAGE_GUARD))) {
            buf.resize(mbi.RegionSize); SIZE_T r = 0;
            if (ReadProcessMemory(hProcess, (LPCVOID)cur, buf.data(), mbi.RegionSize, &r)) {
                auto it = std::search(buf.begin(), buf.begin()+r,
                    (BYTE*)prefix, (BYTE*)prefix+pl);
                while (it != buf.begin()+r) {
                    addrs.push_back(cur + std::distance(buf.begin(), it));
                    it = std::search(it+pl, buf.begin()+r, (BYTE*)prefix, (BYTE*)prefix+pl);
                }
            }
        }
        cur += mbi.RegionSize;
    }
    return addrs;
}

void SendVirtualKey(WORD vk) {
    INPUT in = {}; in.type = INPUT_KEYBOARD; in.ki.wVk = vk;
    SendInput(1, &in, sizeof(in));
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}
void SendToStarCraft(std::string cmd) {
    int n = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, NULL, 0);
    std::wstring wc(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, &wc[0], n);

    // Enter + 모든 글자 + Enter 를 단일 SendInput 배치로 전송
    std::vector<INPUT> inputs;
    auto pushVK = [&](WORD vk, bool up) {
        INPUT in = {}; in.type = INPUT_KEYBOARD; in.ki.wVk = vk;
        if (up) in.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(in);
    };
    auto pushChar = [&](wchar_t ch) {
        INPUT in = {}; in.type = INPUT_KEYBOARD;
        in.ki.dwFlags = KEYEVENTF_UNICODE; in.ki.wScan = ch;
        inputs.push_back(in);
        in.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(in);
    };

    pushVK(VK_RETURN, false);
    pushVK(VK_RETURN, true);
    for (wchar_t ch : wc) if (ch) pushChar(ch);
    pushVK(VK_RETURN, false);
    pushVK(VK_RETURN, true);

    SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
}

std::set<std::string> ExtractStrings(const char* prefix, size_t plen, char tail) {
    auto addrs = FindAllPrefixAddresses(g_hProcess, prefix);
    std::set<std::string> out;
    for (ULONGLONG a : addrs) {
        std::string s = ReadNullTerminatedString(g_hProcess, a);
        if (s.compare(0, plen, prefix) != 0) continue;
        std::string ex;
        if (tail == '\0') ex = s.substr(plen);
        else { size_t e = s.find(tail, plen); ex = (e != std::string::npos && e > plen) ? s.substr(plen, e-plen) : s.substr(plen); }
        if (!ex.empty()) out.insert(ex);
    }
    return out;
}

void DoExtraction() {
    auto players = ReadCurrentGamePlayers();
    if (players.empty()) return;
    const char myPfx[] = u8"HAT:";
    auto myIDs = ExtractStrings(myPfx, strlen(myPfx), '\x10');
    for (auto& id : myIDs) players.erase(id);
    for (auto& name : players) {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_extractedSet.count(name)) {
            g_extractedSet.insert(name);
            SendToStarCraft("/ignore " + name);
        }
    }
}

void DoRemoval() {
    for (auto& id : g_extractedSet) SendToStarCraft("/unignore " + id);
    g_extractedSet.clear();
}

// ---------------------------------------------------------------------------
// 키보드 훅
// ---------------------------------------------------------------------------
void StartKeyboardHook() { if (!g_hHook) g_hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0); }
void StopKeyboardHook()  { if (g_hHook) { UnhookWindowsHookEx(g_hHook); g_hHook = NULL; } }

void UpdateStarCraftProcess() {
    DWORD pid = GetProcessID(L"StarCraft.exe");
    if (pid && pid != g_starcraftPID) {
        // SC 새로 실행됨
        g_extractedSet.clear(); g_scModuleBase = 0; g_starcraftPID = pid; g_cachedApiPort = 0;
        if (g_hProcess) CloseHandle(g_hProcess);
        g_hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    } else if (!pid && g_starcraftPID) {
        if (g_fastJoin) DisableFastJoin();
        g_starcraftPID = 0; g_scModuleBase = 0;
        if (g_hProcess) { CloseHandle(g_hProcess); g_hProcess = NULL; }
    }
}
void ProcessMonitorThread() { while (true) { UpdateStarCraftProcess(); std::this_thread::sleep_for(std::chrono::seconds(3)); } }

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
        HWND hFg = GetForegroundWindow();
        DWORD fgPID = 0; GetWindowThreadProcessId(hFg, &fgPID);
        bool scFg = (fgPID == g_starcraftPID), ovFg = (hFg == g_hOverlay);

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
        {
            // F1: GUI 토글
            if (kb->vkCode == VK_F12 && (scFg || ovFg) && g_hOverlay)
            {
                g_showGui = !g_showGui;
                UpdateOverlayPosition();
                RenderOverlay();
                return 1;
            }
            if (scFg) {
                if (kb->vkCode == KEY_IGNORE)    { std::thread(DoExtraction).detach(); }
                if (kb->vkCode == KEY_UNIGNORE)  { std::thread(DoRemoval).detach(); }
            }
            if (scFg && g_swapSpaceAndControl && g_isInGame && !IsChatMode() && kb->vkCode == KEY_ADDITIONAL_CTRL) {
                INPUT in = {}; in.type = INPUT_KEYBOARD; in.ki.wVk = VK_CONTROL;
                SendInput(1, &in, sizeof(in)); return 1;
            }
        }
        else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
        {
            if (scFg && g_swapSpaceAndControl && g_isInGame && !IsChatMode() && kb->vkCode == KEY_ADDITIONAL_CTRL) {
                INPUT in = {}; in.type = INPUT_KEYBOARD; in.ki.wVk = VK_CONTROL;
                in.ki.dwFlags = KEYEVENTF_KEYUP; SendInput(1, &in, sizeof(in)); return 1;
            }
        }
    }
    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 설정 저장/로드
// ---------------------------------------------------------------------------
static const wchar_t* REG_KEY = L"Software\\bw_auto_ignore";

void SaveSettings() {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) != ERROR_SUCCESS) return;
    DWORD v;
    v = g_swapSpaceAndControl;   RegSetValueExW(hKey, L"SwapSpaceAndControl",   0, REG_DWORD, (BYTE*)&v, sizeof(v));
    v = g_autoIgnoreOnGameStart; RegSetValueExW(hKey, L"AutoIgnoreOnGameStart",  0, REG_DWORD, (BYTE*)&v, sizeof(v));
    RegCloseKey(hKey);
}

void LoadSettings() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) return;
    DWORD v, sz = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"SwapSpaceAndControl",   NULL, NULL, (BYTE*)&v, &sz) == ERROR_SUCCESS) g_swapSpaceAndControl   = v != 0; sz = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"AutoIgnoreOnGameStart",  NULL, NULL, (BYTE*)&v, &sz) == ERROR_SUCCESS) g_autoIgnoreOnGameStart  = v != 0;
    RegCloseKey(hKey);
}

INT_PTR CALLBACK SettingDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG:
        CheckDlgButton(hDlg, IDC_SWAP_KEY,    g_swapSpaceAndControl   ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_AUTO_IGNORE, g_autoIgnoreOnGameStart ? BST_CHECKED : BST_UNCHECKED);
        return (INT_PTR)TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            g_swapSpaceAndControl   = IsDlgButtonChecked(hDlg, IDC_SWAP_KEY)    == BST_CHECKED;
            g_autoIgnoreOnGameStart = IsDlgButtonChecked(hDlg, IDC_AUTO_IGNORE) == BST_CHECKED;
            SaveSettings(); EndDialog(hDlg, IDOK); return (INT_PTR)TRUE;
        } else if (LOWORD(wParam) == IDCANCEL) { EndDialog(hDlg, IDCANCEL); return (INT_PTR)TRUE; }
    }
    return (INT_PTR)FALSE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_APP+1:
        if (lParam == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU hM = CreatePopupMenu();
            AppendMenu(hM, MF_STRING, 1002, L"Help");
            AppendMenu(hM, MF_STRING, 1004, L"Setting");
            AppendMenu(hM, MF_STRING, 1003, L"Exit");
            SetForegroundWindow(hWnd);
            TrackPopupMenu(hM, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(hM);
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == 1002) MessageBox(hWnd, L"F9: ignore\nF8: unignore\nF12: 전적 GUI", L"Help", MB_OK | MB_ICONINFORMATION);
        else if (LOWORD(wParam) == 1003) DestroyWindow(hWnd);
        else if (LOWORD(wParam) == 1004) DialogBox(hInst, MAKEINTRESOURCE(IDD_SETTING_DIALOG), hWnd, SettingDlgProc);
        break;
    case WM_DESTROY: PostQuitMessage(0); break;
    default: return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

ATOM MyRegisterClass(HINSTANCE hInstance) {
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(wcex); wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc; wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszClassName = szWindowClass;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_BWAUTOIGNORE));
    wcex.hIconSm = wcex.hIcon;
    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int) {
    hInst = hInstance;
    HWND hWnd = CreateWindowW(szWindowClass, L"bw_auto_ignore", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) return FALSE;
    ShowWindow(hWnd, SW_HIDE); UpdateWindow(hWnd);
    nid.cbSize = sizeof(nid); nid.hWnd = hWnd; nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP+1;
    nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_BWAUTOIGNORE));
    wcscpy_s(nid.szTip, L"bw_auto_ignore");
    Shell_NotifyIcon(NIM_ADD, &nid);
    return TRUE;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int nCmdShow)
{
    HANDLE hMutex = CreateMutex(NULL, TRUE, L"Local\\bw_auto_ignoreMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    _setmode(_fileno(stdout), _O_U16TEXT);
    setlocale(LC_ALL, "");
    LoadSettings();

    MyRegisterClass(hInstance);
    if (!InitInstance(hInstance, nCmdShow)) return FALSE;

    StartKeyboardHook();
    CreateOverlayWindow(hInstance);

    std::thread(ProcessMonitorThread).detach();

    DialogBox(hInst, MAKEINTRESOURCE(IDD_SETTING_DIALOG), nid.hWnd, SettingDlgProc);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }

    StopKeyboardHook();
    DisableFastJoin();
    if (g_imguiInitialized) {
        ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    }
    if (g_pRenderTargetView) g_pRenderTargetView->Release();
    if (g_pSwapChain)        g_pSwapChain->Release();
    if (g_pd3dContext)       g_pd3dContext->Release();
    if (g_pd3dDevice)        g_pd3dDevice->Release();
    if (g_hProcess)          CloseHandle(g_hProcess);
    Shell_NotifyIcon(NIM_DELETE, &nid);
    if (hMutex) CloseHandle(hMutex);
    return (int)msg.wParam;
}
