#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#define HAVE_REMOTE
#include <pcap.h>
#pragma comment(lib, "wpcap.lib")
#pragma comment(lib, "Packet.lib")
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
#include <queue>
#include <condition_variable>
#include <atomic>
#include <fstream>
#include <sstream>
#include "resource.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#include <mmdeviceapi.h>
#include <audiopolicy.h>

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
const ULONGLONG CHAT_MODE_OFFSET      = 0x10B6BD8; // 1=채팅 입력 중, 0=그 외
const ULONGLONG CHAT_LOG_SCAN_START   = 0x1060000; // 귓말 로그 버퍼 스캔 시작
const ULONGLONG CHAT_LOG_SCAN_END     = 0x1080000; // 귓말 로그 버퍼 스캔 끝

// ---------------------------------------------------------------------------
// 언어 설정
// ---------------------------------------------------------------------------
bool g_isKorean = true;

// 한/영 문자열 선택 헬퍼
#define S(ko, en) (g_isKorean ? (ko) : (en))
#define WS(ko, en) (g_isKorean ? (ko) : (en))

static void DetectLanguage(LPWSTR lpCmdLine)
{
    if (lpCmdLine) {
        std::wstring cmd(lpCmdLine);
        if (cmd.find(L"--lang=en") != std::wstring::npos) { g_isKorean = false; return; }
        if (cmd.find(L"--lang=ko") != std::wstring::npos) { g_isKorean = true;  return; }
    }
    LANGID lang = GetUserDefaultUILanguage();
    g_isKorean = (PRIMARYLANGID(lang) == LANG_KOREAN);
}

// ---------------------------------------------------------------------------
// 버전
// ---------------------------------------------------------------------------
#define APP_VERSION L"1.0.0"

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
bool g_autoShowStats = true;   // 게임 시작 후 5초 대기, 5초간 전적 오버레이 자동 표시
bool g_autoFetchChatOnGameStart = false; // 게임 시작 시 채팅 자동 조회
bool g_showGuiManual = false;  // F12로 수동 활성화 여부
bool g_whisperReply = true;    // Shift+Enter: 귓말 발신자에게 /w 입력

std::string g_cachedWhisperSender;
std::mutex  g_whisperMutex;
bool g_muteOtherAudio = false; // Pause 키: 외부 오디오 음소거
bool g_detectBadManner = false; // 비매너 사용자 검출
bool g_otherAudioMuted = false; // 현재 외부 오디오 음소거 상태
bool g_fastJoin = false;      // 공개방 빠른 입장 (현재 비활성)
bool g_fastJoinActive = false;
bool g_isInGame = false;
bool g_wasInGame = false;
ULONGLONG g_scModuleBase = 0;

HWND g_hOverlay = NULL;
HWND g_hStarCraftWnd = NULL;
HHOOK g_hMouseHook = NULL;

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL && g_hOverlay) {
        MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lParam;
        POINT pt = ms->pt;
        RECT rc;
        if (GetWindowRect(g_hOverlay, &rc) &&
            pt.x >= rc.left && pt.x < rc.right &&
            pt.y >= rc.top  && pt.y < rc.bottom) {
            PostMessage(g_hOverlay, WM_MOUSEWHEEL,
                MAKEWPARAM(0, (SHORT)HIWORD(ms->mouseData)), MAKELPARAM(pt.x, pt.y));
        }
    }
    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
}
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
// 종족별 매치업 통계 (상대 종족 기준)
struct RaceMatchStats {
    int games = 0, wins = 0, dodge = 0, disc = 0;
    float WinRate()   const { return games > 0 ? (float)wins  / games : -1.f; }
    float DodgeRate() const { return games > 0 ? (float)dodge / games :  0.f; }
    float DiscRate()  const { return games > 0 ? (float)disc  / games :  0.f; }
};
// T=0, Z=1, P=2
struct MatchHistoryStats {
    int total = 0, wins = 0, dodge = 0, disc = 0;
    RaceMatchStats vs[3];
    bool fetched = false;
    float WinRate()   const { return total > 0 ? (float)wins  / total : -1.f; }
    float DodgeRate() const { return total > 0 ? (float)dodge / total :  0.f; }
    float DiscRate()  const { return total > 0 ? (float)disc  / total :  0.f; }
};

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
    MatchHistoryStats hist; // 매치 기록 통계
};

struct DisplayProfile {
    std::string          battleTag;
    std::vector<ToonStat> toons;
    std::string          queryName; // 조회에 사용된 인게임 이름
    bool valid    = false;
    bool fetching = false;
    std::string statusMsg;
};

static std::vector<DisplayProfile> g_autoProfiles;
static bool          g_autoFetching = false;

// 상대방 IP (npcap 캡처)
struct PeerIPInfo {
    std::string ip;
    std::string country;
    std::string region;
    std::string city;
    std::string org;
    bool fetched = false;
};
static std::vector<PeerIPInfo> g_peerIPs;
static std::mutex              g_peerMutex;
static bool                    g_pcapRunning = false;
static DisplayProfile g_selfProfile;
static bool          g_selfFetching = false;
static bool          g_selfFetched  = false;
static HANDLE        g_selfLastProcess = NULL;
static ULONGLONG     g_selfToonAddr = 0;  // HAT: 문자열 주소 캐시 (재스캔 방지)
static std::mutex    g_profileMutex;

static bool g_showReplayViewer    = false;
static RECT g_replayBtnScreenRect    = {};
static RECT g_replayViewerScreenRect = {};

// ---------------------------------------------------------------------------
// 리플레이 채팅 조회
// ---------------------------------------------------------------------------
struct ReplayChatLine {
    std::string time;    // "MM:SS"
    std::string sender;
    std::string msg;
    bool isTarget = false;
};
struct ReplayGame {
    std::string filename;   // rep 파일명
    std::string opponent;   // 상대방 이름
    std::string timestamp;  // "YYYYMMDD_HHMMSS"
    std::string result;     // win/loss/disconnect
    std::vector<ReplayChatLine> lines;
};
enum class ReplayFetchStatus { IDLE, LOADING_LIST, DOWNLOADING, PARSING, DONE, FETCH_ERROR };
struct ReplayFetchState {
    std::string              toon;
    int                      gateway = 0;
    ReplayFetchStatus        status  = ReplayFetchStatus::IDLE;
    std::string              statusMsg;
    std::atomic<int>         dlDone{0}, dlTotal{0};
    std::atomic<int>         parseDone{0}, parseTotal{0};
    std::vector<ReplayGame>  games;   // newest first
    std::mutex               mtx;
};
static ReplayFetchState g_replayFetch;
static bool g_bmDetected = false; // 비매너 채팅 검출 결과

// 비속어 필터 로드 (exe 경로의 "비속어 필터.txt")
static std::vector<std::string> LoadProfanityFilter()
{
    std::vector<std::string> words;
    char selfPath[MAX_PATH] = {};
    GetModuleFileNameA(NULL, selfPath, MAX_PATH);
    std::string dir = selfPath;
    size_t sl = dir.rfind('\\');
    if (sl != std::string::npos) dir = dir.substr(0, sl + 1);
    std::string path = dir + "\xeb\xb9\x84\xec\x86\x8d\xec\x96\xb4 \xed\x95\x84\xed\x84\xb0.txt"; // "비속어 필터.txt" UTF-8

    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "rb");
    if (!f) return words;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::string buf(sz, '\0');
    fread(&buf[0], 1, sz, f);
    fclose(f);
    // BOM 제거
    if (buf.size() >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
        buf = buf.substr(3);
    // 쉼표로 분리
    size_t start = 0;
    while (start < buf.size()) {
        size_t end = buf.find(',', start);
        if (end == std::string::npos) end = buf.size();
        std::string w = buf.substr(start, end - start);
        // trim whitespace/newlines
        while (!w.empty() && (w.front() == ' ' || w.front() == '\r' || w.front() == '\n')) w.erase(w.begin());
        while (!w.empty() && (w.back() == ' ' || w.back() == '\r' || w.back() == '\n')) w.pop_back();
        if (!w.empty()) words.push_back(w);
        start = end + 1;
    }
    return words;
}

// ── 비매너 검출 파라미터 ──
static constexpr float BM_PROFANITY_RATIO = 0.30f; // 채팅 있는 게임 중 욕설 게임 비율 임계값

// 비매너 검사: 채팅 있는 게임 중 30% 이상에서 조회 대상이 욕설 사용 → 검출
static bool CheckBadManner(const std::vector<ReplayGame>& games)
{
    std::vector<std::string> filter = LoadProfanityFilter();
    if (filter.empty()) return false;

    int chatGames = 0;
    int profanityGames = 0;

    for (auto& game : games) {
        if (game.lines.empty()) continue;
        chatGames++;

        for (auto& cl : game.lines) {
            if (!cl.isTarget) continue;
            bool found = false;
            for (auto& w : filter) {
                if (cl.msg.find(w) != std::string::npos) { found = true; break; }
            }
            if (found) { profanityGames++; break; }
        }
    }

    return chatGames > 0 && (float)profanityGames / chatGames >= BM_PROFANITY_RATIO;
}

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
std::string GetLastWhisperSender();
void TypeText(const std::string& text);
void SendWhisperReply();
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

// ---------------------------------------------------------------------------
// npcap UDP 6112 캡처
// ---------------------------------------------------------------------------
struct EthHeader  { uint8_t dst[6], src[6]; uint16_t type; };
struct IPv4Header { uint8_t ver_ihl, tos; uint16_t len, id, frag; uint8_t ttl, proto; uint16_t cksum; uint32_t src, dst; };
struct UDPHeader  { uint16_t sport, dport, len, cksum; };

static bool IsPublicIP(uint32_t ip) {
    uint8_t a = (ip >> 24) & 0xFF;
    uint8_t b = (ip >> 16) & 0xFF;
    uint8_t c = (ip >>  8) & 0xFF;
    if (a == 0 || a == 10 || a == 127) return false;
    if (a == 192 && b == 168) return false;
    if (a == 172 && b >= 16 && b <= 31) return false;
    if (a >= 224) return false;
    // 블리자드 서버 대역 제외
    if (a == 158 && b == 115 && c == 203) return false; // 블리자드 KR 게임서버
    if (a == 137 && b == 221) return false;              // 블리자드 배틀넷
    if (a == 117 && b ==  52) return false;              // 블리자드 KR
    if (a ==  37 && b == 244) return false;              // 블리자드 STUN
    if (a ==  59 && b == 153) return false;              // 블리자드 STUN Asia
    if (a == 144 && b ==  95) return false;              // 블리자드 릴레이 EU
    if (a ==  13) return false;                          // AWS (블리자드 CDN)
    if (a ==  34) return false;                          // GCP (블리자드 CDN)
    return true;
}

static void DbgLog(const char* fmt, ...) {
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat_s(path, "scr_debug.txt");
    FILE* f = nullptr; fopen_s(&f, path, "a");
    if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fclose(f);
}

static void FetchIPInfo(std::string ip) {
    // ipinfo.io/json/{ip} 호출
    std::wstring wip(ip.begin(), ip.end());
    std::wstring path = L"/" + wip;

    HINTERNET hS = WinHttpOpen(L"SCRScout/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return;
    HINTERNET hC = WinHttpConnect(hS, L"ipinfo.io", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hC) { WinHttpCloseHandle(hS); return; }
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path.c_str(), NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return; }

    std::string json;
    BOOL sent = WinHttpSendRequest(hR, L"Accept: application/json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    BOOL rcvd = sent && WinHttpReceiveResponse(hR, NULL);
    DWORD statusCode = 0, scLen = sizeof(statusCode);
    if (rcvd) {
        WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            NULL, &statusCode, &scLen, NULL);
        DWORD sz = 0;
        do {
            if (!WinHttpQueryDataAvailable(hR, &sz) || !sz) break;
            std::vector<char> b(sz+1, 0); DWORD rd = 0;
            WinHttpReadData(hR, b.data(), sz, &rd);
            json.append(b.data(), rd);
        } while (sz > 0);
    }
    DWORD lastErr = GetLastError();
    DbgLog("fetch: IP=%s sent=%d rcvd=%d status=%lu err=%lu json_len=%zu json=%.300s\n",
        ip.c_str(), (int)sent, (int)rcvd, statusCode, lastErr, json.size(), json.c_str());
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);

    auto extract = [&](const std::string& key) -> std::string {
        std::string k = "\"" + key + "\"";
        size_t p = json.find(k);
        if (p == std::string::npos) return "";
        p = json.find('"', p + k.size()); // 값의 첫 따옴표
        if (p == std::string::npos) return "";
        p++;
        size_t e = json.find('"', p);
        return e == std::string::npos ? "" : json.substr(p, e - p);
    };

    std::lock_guard<std::mutex> lk(g_peerMutex);
    for (auto& info : g_peerIPs) {
        if (info.ip == ip) {
            info.country = extract("country");
            info.region  = extract("region");
            info.city    = extract("city");
            info.org     = extract("org");
            info.fetched = true;
            break;
        }
    }
}

static void PcapPacketHandler(u_char*, const struct pcap_pkthdr* hdr, const u_char* pkt) {
    if (hdr->caplen < sizeof(EthHeader) + sizeof(IPv4Header) + sizeof(UDPHeader)) return;
    auto* eth = (EthHeader*)pkt;
    if (ntohs(eth->type) != 0x0800) return; // IPv4만
    auto* ip  = (IPv4Header*)(pkt + sizeof(EthHeader));
    if (ip->proto != 17) return; // UDP만
    auto* udp = (UDPHeader*)(pkt + sizeof(EthHeader) + sizeof(IPv4Header));
    uint16_t sp = ntohs(udp->sport), dp = ntohs(udp->dport);
    // 로컬 포트 6112로 오가는 트래픽 → 상대방은 반대쪽
    bool toLocal   = (dp == 6112); // 상대→나: remote는 src
    bool fromLocal = (sp == 6112); // 나→상대: remote는 dst
    if (!toLocal && !fromLocal) return;

    uint32_t remote = toLocal ? ntohl(ip->src) : ntohl(ip->dst);
    uint32_t remoteRaw = toLocal ? ip->src : ip->dst;

    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
        (remote >> 24) & 0xFF, (remote >> 16) & 0xFF,
        (remote >> 8)  & 0xFF,  remote & 0xFF);

    DbgLog("pkt: sp=%u dp=%u remote=%s public=%d\n", sp, dp, buf, (int)IsPublicIP(remote));

    if (!IsPublicIP(remote)) return;

    {
        std::lock_guard<std::mutex> lk(g_peerMutex);
        for (auto& info : g_peerIPs)
            if (info.ip == buf) return; // 이미 있음
        g_peerIPs.push_back({buf});
    }
    std::thread(FetchIPInfo, std::string(buf)).detach();
}

static void PcapCaptureOnDev(std::string devName) {
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t* handle = pcap_open_live(devName.c_str(), 65536, 0, 100, errbuf);
    if (!handle) return;
    struct bpf_program fp;
    pcap_compile(handle, &fp, "udp port 6112", 1, PCAP_NETMASK_UNKNOWN);
    pcap_setfilter(handle, &fp);
    pcap_freecode(&fp);
    while (g_pcapRunning)
        pcap_dispatch(handle, 32, PcapPacketHandler, nullptr);
    pcap_close(handle);
}

static void StartPcapCapture() {
    if (g_pcapRunning) return;
    g_pcapRunning = true;
    std::thread([]() {
        char errbuf[PCAP_ERRBUF_SIZE];
        pcap_if_t* alldevs = nullptr;
        if (pcap_findalldevs(&alldevs, errbuf) != 0 || !alldevs) {
            g_pcapRunning = false; return;
        }
        // loopback 제외 모든 어댑터에서 캡처
        std::vector<std::string> devNames;
        for (pcap_if_t* d = alldevs; d; d = d->next) {
            if (!(d->flags & PCAP_IF_LOOPBACK))
                devNames.push_back(d->name);
        }
        pcap_freealldevs(alldevs);
        // 각 어댑터마다 스레드
        for (size_t i = 1; i < devNames.size(); i++)
            std::thread(PcapCaptureOnDev, devNames[i]).detach();
        if (!devNames.empty())
            PcapCaptureOnDev(devNames[0]); // 첫 번째는 현재 스레드
        g_pcapRunning = false;
    }).detach();
}

static void StopPcapCapture() {
    g_pcapRunning = false;
}

// SC 프로세스가 연결된 IP 목록 반환 (TCP ESTABLISHED 기준)
std::vector<std::string> GetSCConnectedIPs()
{
    std::vector<std::string> result;
    if (!g_starcraftPID) return result;

    DWORD size = 0;
    GetExtendedTcpTable(NULL, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size == 0) return result;

    std::vector<BYTE> buf(size);
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR)
        return result;

    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
    std::set<std::string> seen;
    for (DWORD i = 0; i < table->dwNumEntries; i++) {
        auto& row = table->table[i];
        if (row.dwOwningPid != g_starcraftPID) continue;
        if (row.dwState != MIB_TCP_STATE_ESTAB) continue;
        DWORD ip = row.dwRemoteAddr;
        char ipStr[32];
        snprintf(ipStr, sizeof(ipStr), "%lu.%lu.%lu.%lu",
            ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
        DWORD remotePort = ((row.dwRemotePort & 0xFF) << 8) | ((row.dwRemotePort >> 8) & 0xFF);
        // 로컬호스트, HTTPS(443) 제외
        if (row.dwRemoteAddr == 0x0100007F) continue; // 127.0.0.1
        if (remotePort == 443) continue;
        char full[64]; snprintf(full, sizeof(full), "%s:%lu", ipStr, remotePort);
        if (seen.insert(full).second)
            result.push_back(full);
    }
    return result;
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

// SC:R MAPPED 채팅 링버퍼에서 귓말 발신자를 추적한다.
//
// 구조:
//   - 0x7FF639270000 + 0x106E0CB = 슬롯 0 시작
//   - 슬롯 크기 218바이트(0xDA), 총 10슬롯 = 2180바이트
//   - 각 슬롯: null-terminated ASCII, "sender> content" 형식이면 귓말
//   - SC:R ASLR 비활성화 → 모듈 베이스는 재시작해도 고정
static const ULONGLONG WHISPER_RING_RVA  = 0x106E0CBULL;  // 슬롯 0 RVA
static const int       WHISPER_SLOT_SIZE = 0xDA;           // 218바이트
static const int       WHISPER_SLOT_COUNT = 10;

void ScanWhisperSenderFromRingBuffer()
{
    if (!g_hProcess) return;

    ULONGLONG modBase = GetStarCraftModuleBase();
    if (!modBase) return;

    ULONGLONG slot0 = modBase + WHISPER_RING_RVA;

    // 이전 스냅샷 (프로세스 핸들 변경 시 재초기화)
    static char    prevSlots[WHISPER_SLOT_COUNT][WHISPER_SLOT_SIZE];
    static bool    initialized  = false;
    static HANDLE  lastProcess  = NULL;

    if (lastProcess != g_hProcess)
    {
        initialized = false;
        lastProcess = g_hProcess;
    }

    // 10슬롯 한 번에 읽기 (2180바이트)
    char buf[WHISPER_SLOT_COUNT * WHISPER_SLOT_SIZE];
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(g_hProcess, (LPCVOID)slot0, buf, sizeof(buf), &bytesRead)
        || (int)bytesRead < WHISPER_SLOT_COUNT * WHISPER_SLOT_SIZE)
        return;

    if (!initialized)
    {
        memcpy(prevSlots, buf, sizeof(prevSlots));
        initialized = true;
        // 기존 슬롯에서 귓말 발신자 초기값 설정 (역순 = 최근 우선 추정)
        for (int i = WHISPER_SLOT_COUNT - 1; i >= 0; i--)
        {
            char text[WHISPER_SLOT_SIZE + 1];
            memcpy(text, buf + i * WHISPER_SLOT_SIZE, WHISPER_SLOT_SIZE);
            text[WHISPER_SLOT_SIZE] = '\0';
            const char* sep = strstr(text, "> ");
            if (sep && sep > text)
            {
                int prefixLen = (int)(sep - text);
                bool allAscii = true;
                for (int k = 0; k < prefixLen; k++)
                    if ((unsigned char)text[k] < 33 || (unsigned char)text[k] > 126)
                    { allAscii = false; break; }
                if (allAscii && prefixLen > 0 && prefixLen < 64)
                {
                    std::lock_guard<std::mutex> lk(g_whisperMutex);
                    g_cachedWhisperSender = std::string(text, prefixLen);
                    break;
                }
            }
        }
        return;
    }

    // 변경된 슬롯에서 "sender> content" 파싱
    std::string latestSender;
    for (int i = 0; i < WHISPER_SLOT_COUNT; i++)
    {
        const char* slot = buf + i * WHISPER_SLOT_SIZE;
        if (memcmp(prevSlots[i], slot, WHISPER_SLOT_SIZE) == 0) continue;

        // 변경된 슬롯 파싱
        char text[WHISPER_SLOT_SIZE + 1];
        memcpy(text, slot, WHISPER_SLOT_SIZE);
        text[WHISPER_SLOT_SIZE] = '\0';

        // 수신 귓말 형식: "sender> content" (순수 ASCII, 한국어 없음)
        // 발신 귓말 형식: "recipient 님에게> content" ("> " 앞에 한국어 UTF-8 포함) → 무시
        const char* sep = strstr(text, "> ");
        if (sep && sep > text)
        {
            int prefixLen = (int)(sep - text);
            // "> " 앞이 모두 ASCII printable인 경우만 수신 귓말
            bool allAscii = true;
            for (int k = 0; k < prefixLen; k++)
                if ((unsigned char)text[k] < 33 || (unsigned char)text[k] > 126)
                { allAscii = false; break; }

            if (allAscii && prefixLen > 0 && prefixLen < 64)
                latestSender = std::string(text, prefixLen);
        }

        memcpy(prevSlots[i], slot, WHISPER_SLOT_SIZE);
    }

    if (!latestSender.empty())
    {
        std::lock_guard<std::mutex> lk(g_whisperMutex);
        g_cachedWhisperSender = latestSender;
    }
}

std::string GetLastWhisperSender()
{
    std::lock_guard<std::mutex> lk(g_whisperMutex);
    return g_cachedWhisperSender;
}

// 채팅창에 텍스트만 입력 (전송 없이, 딜레이 없음)
void TypeText(const std::string& text)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, NULL, 0);
    std::wstring wc(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wc[0], n);

    for (wchar_t ch : wc) {
        if (!ch) continue;
        INPUT inputs[2] = {};
        inputs[0].type = INPUT_KEYBOARD; inputs[0].ki.dwFlags = KEYEVENTF_UNICODE; inputs[0].ki.wScan = ch;
        inputs[1] = inputs[0]; inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, inputs, sizeof(INPUT));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Shift+Enter: 마지막 귓말 발신자에게 /w 입력
void SendWhisperReply()
{
    std::string sender = GetLastWhisperSender();
    if (sender.empty()) return;

    std::string cmd = "/w " + sender + " ";
    int n = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, NULL, 0);
    std::wstring wc(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, &wc[0], n);

    // Shift 릴리즈 + Enter(채팅창 열기) + 문자들을 단일 배치로 전송
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

    pushVK(VK_SHIFT, true);   // Shift 릴리즈
    pushVK(VK_RETURN, false); // Enter down (채팅창 열기)
    pushVK(VK_RETURN, true);  // Enter up
    for (wchar_t ch : wc) if (ch) pushChar(ch);

    SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
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

    if (!g_isInGame && !g_showGui) { ShowWindow(g_hOverlay, SW_HIDE); return; }

    RECT cr; GetClientRect(hSC, &cr);
    POINT tl = {0, 0}; ClientToScreen(hSC, &tl);
    int w = cr.right - cr.left, h = cr.bottom - cr.top;

    // SC 창 바로 위 z-order에 배치 → 다른 창이 SC를 가리면 오버레이도 같이 가려짐
    HWND insertAfter = GetWindow(hSC, GW_HWNDPREV); // SC 위에 있는 창
    if (!insertAfter || insertAfter == g_hOverlay)
        insertAfter = HWND_TOP;
    SetWindowPos(g_hOverlay, insertAfter, tl.x, tl.y, w, h, SWP_NOACTIVATE);
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
    switch (gw) {
        case 10: return "USW"; case 11: return "USE";
        case 12: return "EU";  case 20: return "Asia"; case 30: return "KR";
        case  1: return "US";  case  2: return "EU";   case  3: return "KR"; case 4: return "Asia";
        case 45: return "EU";
        default: { static char buf[8]; snprintf(buf, sizeof(buf), "?%d", gw); return buf; }
    }
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

// 임의 경로 GET (UTF-8 path → wstring 변환)
static std::string LocalWebApiGetPath(const std::string& path, WORD port) {
    int wl = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), NULL, 0);
    std::wstring wpath(wl, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), &wpath[0], wl);

    HINTERNET hS = WinHttpOpen(L"StarCraft/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return "";
    HINTERNET hC = WinHttpConnect(hS, L"127.0.0.1", port, 0);
    if (!hC) { WinHttpCloseHandle(hS); return ""; }
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", wpath.c_str(), NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return ""; }

    std::string result;
    if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hR, NULL)) {
        DWORD sz = 0;
        do {
            if (!WinHttpQueryDataAvailable(hR, &sz) || !sz) break;
            std::vector<char> b(sz + 1, 0); DWORD rd = 0;
            WinHttpReadData(hR, b.data(), sz, &rd);
            result.append(b.data(), rd);
        } while (sz > 0);
    }
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
    return result;
}

// matchmaker-gameinfo JSON 파싱 → MatchHistoryStats
// toonName: 조회 대상 플레이어 이름
static MatchHistoryStats ParseMatchHistory(const std::string& json, const std::string& toonName, int vsLimit = 100) {
    MatchHistoryStats stats;
    if (json.empty() || json[0] != '[') return stats;
    int vsCount = 0; // vsLimit까지만 종족별 집계

    auto raceIdx = [](const std::string& r) -> int {
        if (r.empty()) return -1;
        char c = (char)toupper((unsigned char)r[0]);
        if (c == 'T') return 0;
        if (c == 'Z') return 1;
        if (c == 'P') return 2;
        return -1;
    };

    // 배열 원소 순회
    size_t cur = 1;
    while (cur < json.size()) {
        size_t matchStart = json.find('{', cur);
        if (matchStart == std::string::npos) break;
        size_t matchEnd = FindMatchingBrace(json, matchStart);
        if (matchEnd == std::string::npos) break;

        std::string matchObj = json.substr(matchStart, matchEnd - matchStart + 1);

        // game_result 섹션에서 toonName 항목 탐색
        std::string myResult, myLeft, oppRace;
        size_t grSearch = 0;
        while ((grSearch = matchObj.find("\"game_result\"", grSearch)) != std::string::npos) {
            size_t grStart = matchObj.find('{', grSearch + 13);
            if (grStart == std::string::npos) break;
            size_t grEnd = FindMatchingBrace(matchObj, grStart);
            if (grEnd == std::string::npos) break;
            std::string grJson = matchObj.substr(grStart, grEnd - grStart + 1);

            // 이 game_result가 내 toon을 포함하는지 확인
            std::string myKey = "\"" + toonName + "\"";
            size_t myPos = grJson.find(myKey);
            if (myPos == std::string::npos) { grSearch = grEnd + 1; continue; }

            // 내 결과 추출
            size_t myEntryStart = grJson.find('{', myPos + myKey.size());
            if (myEntryStart != std::string::npos) {
                size_t myEntryEnd = FindMatchingBrace(grJson, myEntryStart);
                if (myEntryEnd != std::string::npos) {
                    std::string myEntry = grJson.substr(myEntryStart, myEntryEnd - myEntryStart + 1);
                    myResult = JsonStringVal(myEntry, "result");
                    myLeft   = JsonStringVal(myEntry, "left");
                }
            }

            // 상대 종족 추출: game_result의 다른 키(type=player) 탐색
            size_t p = 1;
            while (p < grJson.size() - 1 && oppRace.empty()) {
                size_t qs = grJson.find('"', p);
                if (qs == std::string::npos) break;
                size_t qe = grJson.find('"', qs + 1);
                if (qe == std::string::npos) break;
                std::string key = grJson.substr(qs + 1, qe - qs - 1);

                size_t colonPos = grJson.find(':', qe);
                if (colonPos == std::string::npos) break;
                size_t valStart = grJson.find('{', colonPos);
                if (valStart == std::string::npos) break;
                size_t valEnd = FindMatchingBrace(grJson, valStart);
                if (valEnd == std::string::npos) break;

                if (!key.empty() && key != toonName) {
                    std::string valObj = grJson.substr(valStart, valEnd - valStart + 1);
                    std::string r = JsonStringVal(valObj, "race");
                    if (!r.empty()) oppRace = r;
                }
                p = valEnd + 1;
            }
            break;
        }

        if (!myResult.empty()) {
            int ri = raceIdx(oppRace);
            bool countVs = (ri >= 0 && vsCount < vsLimit);
            stats.total++;
            if (myResult == "Win") {
                stats.wins++;
                if (countVs) { stats.vs[ri].games++; stats.vs[ri].wins++; }
            } else if (myResult == "Disconnect") {
                stats.disc++;
                if (countVs) { stats.vs[ri].games++; stats.vs[ri].disc++; }
            } else if (myResult == "Loss") {
                if (myLeft == "1") {
                    stats.dodge++;
                    if (countVs) { stats.vs[ri].games++; stats.vs[ri].dodge++; }
                } else {
                    if (countVs) stats.vs[ri].games++;
                }
            } else { // Undecided
                if (countVs) stats.vs[ri].games++;
            }
            if (ri >= 0) vsCount++;
        }

        cur = matchEnd + 1;
    }

    stats.fetched = true;
    return stats;
}

// 전적 조회 (스레드에서 호출)
static DisplayProfile FetchProfileData(const std::string& queryName)
{
    DisplayProfile result;
    result.queryName = queryName;
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
    std::string fallbackJson; // battle_tag 없어도 응답 온 경우 보관
    for (int gw : gateways) {
        std::string gwJson = LocalWebApiGet(queryName, port, gw);
        if (gwJson.empty()) continue;
        if (!JsonStringVal(gwJson, "battle_tag").empty()) {
            json = gwJson;
            break;
        }
        if (fallbackJson.empty()) fallbackJson = gwJson; // 첫 번째 응답 보관
    }
    if (json.empty()) json = fallbackJson; // battle_tag 없는 응답이라도 사용
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

    // 5. 각 toon의 match history 통계 조회
    int curSeason = JsonIntVal(json, "matchmaked_current_season");
    // --- 디버그 로그 ---
    FILE* dbg = nullptr; fopen_s(&dbg, "C:\\wd\\bw_auto_ignore\\hist_debug.log", "w");
    if (dbg) { fprintf(dbg, "curSeason=%d toons=%d\n", curSeason, (int)result.toons.size()); fflush(dbg); }
    for (auto& t : result.toons) {
        if (t.cur_season <= 0 && curSeason <= 0) {
            if (dbg) fprintf(dbg, "SKIP %s gw=%d cur_season=%d\n", t.name.c_str(), t.gateway, t.cur_season);
            continue;
        }
        int season = (t.cur_season > 0) ? t.cur_season : curSeason;
        std::string path = "/web-api/v1/matchmaker-gameinfo-by-toon/" + t.name
            + "/" + std::to_string(t.gateway > 0 ? t.gateway : 30)
            + "/1/" + std::to_string(season) + "?limit=1000";
        if (dbg) fprintf(dbg, "FETCH %s  path=%s\n", t.name.c_str(), path.c_str());
        std::string histJson = LocalWebApiGetPath(path, port);
        if (dbg) { fprintf(dbg, "  resp_len=%d  first50=[%s]\n", (int)histJson.size(), histJson.substr(0, 50).c_str()); fflush(dbg); }
        if (!histJson.empty()) {
            t.hist = ParseMatchHistory(histJson, t.name);
            if (dbg) fprintf(dbg, "  fetched=%d total=%d vsT=%d vsZ=%d vsP=%d\n",
                t.hist.fetched, t.hist.total, t.hist.vs[0].games, t.hist.vs[1].games, t.hist.vs[2].games);
        }
    }
    if (dbg) fclose(dbg);

    result.valid = true;
    return result;
}

static void DoFetchReplayChat(std::string toon, int gateway); // 전방 선언

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

    {
        std::lock_guard<std::mutex> lk(g_profileMutex);
        g_autoProfiles = results;
        g_autoFetching = false;
    }

    if (g_autoShowStats && !results.empty() && !g_showGuiManual) {
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (g_showGuiManual) return;
            g_showGui = true;
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!g_showGuiManual) g_showGui = false;
        }).detach();
    }

    // 게임 시작 시 채팅 자동 조회 (로드만, show는 사용자가 직접)
    if (g_autoFetchChatOnGameStart && !results.empty()) {
        std::string opToon = results[0].queryName;
        int opGw = 30; // 기본값; DoFetchReplayChat 내부에서 gateway fallback 처리
        // 이미 같은 toon 로딩 중이면 스킵
        bool skip = false;
        {
            std::lock_guard<std::mutex> lk(g_replayFetch.mtx);
            auto st = g_replayFetch.status;
            if (g_replayFetch.toon == opToon &&
                (st == ReplayFetchStatus::LOADING_LIST ||
                 st == ReplayFetchStatus::DOWNLOADING  ||
                 st == ReplayFetchStatus::PARSING      ||
                 st == ReplayFetchStatus::DONE))
                skip = true;
        }
        if (!skip) {
            {
                std::lock_guard<std::mutex> lk(g_replayFetch.mtx);
                g_replayFetch.toon      = opToon;
                g_replayFetch.gateway   = opGw;
                g_replayFetch.status    = ReplayFetchStatus::LOADING_LIST;
                g_replayFetch.statusMsg = "";
                g_replayFetch.games.clear();
                g_replayFetch.dlDone = g_replayFetch.dlTotal = 0;
                g_replayFetch.parseDone = g_replayFetch.parseTotal = 0;
            }
            std::thread([opToon, opGw]() { DoFetchReplayChat(opToon, opGw); }).detach();
        }
    }
}

// ---------------------------------------------------------------------------
// Pause 키: StarCraft 외 모든 오디오 세션 mute/unmute 토글
// ---------------------------------------------------------------------------
static void ToggleMuteOtherAudio()
{
    bool newMute = !g_otherAudioMuted;
    CoInitialize(NULL);

    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (FAILED(hr) || !pEnum) { CoUninitialize(); return; }

    IMMDevice* pDevice = nullptr;
    hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr) || !pDevice) { pEnum->Release(); CoUninitialize(); return; }

    IAudioSessionManager2* pMgr = nullptr;
    hr = pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&pMgr);
    if (FAILED(hr) || !pMgr) { pDevice->Release(); pEnum->Release(); CoUninitialize(); return; }

    IAudioSessionEnumerator* pSessions = nullptr;
    hr = pMgr->GetSessionEnumerator(&pSessions);
    if (FAILED(hr) || !pSessions) { pMgr->Release(); pDevice->Release(); pEnum->Release(); CoUninitialize(); return; }

    int count = 0;
    pSessions->GetCount(&count);
    for (int i = 0; i < count; i++) {
        IAudioSessionControl* pCtl = nullptr;
        if (FAILED(pSessions->GetSession(i, &pCtl)) || !pCtl) continue;

        IAudioSessionControl2* pCtl2 = nullptr;
        if (SUCCEEDED(pCtl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pCtl2))) {
            DWORD pid = 0;
            pCtl2->GetProcessId(&pid);
            // StarCraft PID가 아니고 시스템 사운드(pid==0)도 아닌 세션만 mute
            if (pid != 0 && pid != g_starcraftPID) {
                ISimpleAudioVolume* pVol = nullptr;
                if (SUCCEEDED(pCtl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pVol))) {
                    pVol->SetMute(newMute, NULL);
                    pVol->Release();
                }
            }
            pCtl2->Release();
        }
        pCtl->Release();
    }

    pSessions->Release();
    pMgr->Release();
    pDevice->Release();
    pEnum->Release();
    CoUninitialize();

    g_otherAudioMuted = newMute;
}

static void FetchSelfProfile()
{
    const char myPfx[] = "HAT:";
    size_t plen = strlen(myPfx);
    // 전체 메모리 스캔 (최초 1회)
    auto addrs = FindAllPrefixAddresses(g_hProcess, myPfx);
    std::string curID;
    ULONGLONG foundAddr = 0;
    for (ULONGLONG a : addrs) {
        std::string s = ReadNullTerminatedString(g_hProcess, a);
        if (s.compare(0, plen, myPfx) != 0) continue;
        size_t e = s.find('\x10', plen);
        std::string id = (e != std::string::npos && e > plen) ? s.substr(plen, e - plen) : s.substr(plen);
        if (!id.empty()) { curID = id; foundAddr = a; break; }
    }
    {
        std::lock_guard<std::mutex> lk(g_profileMutex);
        g_selfToonAddr = foundAddr;
    }
    DisplayProfile p;
    if (!curID.empty())
        p = FetchProfileData(curID);
    {
        std::lock_guard<std::mutex> lk(g_profileMutex);
        g_selfProfile  = p;
        g_selfFetching = false;
        g_selfFetched  = true;
    }
}

// ---------------------------------------------------------------------------
// 리플레이 채팅 조회 구현
// ---------------------------------------------------------------------------

// screp.exe 경로 찾기
static std::string FindScrepExe()
{
    // 1) 실행파일 옆
    char selfPath[MAX_PATH] = {};
    GetModuleFileNameA(NULL, selfPath, MAX_PATH);
    std::string dir = selfPath;
    size_t sl = dir.rfind('\\');
    if (sl != std::string::npos) dir = dir.substr(0, sl + 1);
    std::string cand = dir + "screp.exe";
    if (GetFileAttributesA(cand.c_str()) != INVALID_FILE_ATTRIBUTES) return cand;
    // 2) fallback
    const char* fb = "C:\\wd\\screp_python\\screp.exe";
    if (GetFileAttributesA(fb) != INVALID_FILE_ATTRIBUTES) return fb;
    return "";
}

// WinHTTP HTTPS GET (임의 호스트)
static std::string HttpsGet(const std::wstring& host, const std::wstring& path,
                            const std::wstring& extraHeaders = L"")
{
    HINTERNET hS = WinHttpOpen(L"SCRScout/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return "";
    WinHttpSetTimeouts(hS, 10000, 10000, 20000, 20000);
    HINTERNET hC = WinHttpConnect(hS, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hC) { WinHttpCloseHandle(hS); return ""; }
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path.c_str(), NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return ""; }

    const wchar_t* hdrs = extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extraHeaders.c_str();
    DWORD hdrLen = extraHeaders.empty() ? 0 : (DWORD)-1L;
    std::string result;
    if (WinHttpSendRequest(hR, hdrs, hdrLen, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hR, NULL)) {
        DWORD sz = 0;
        do {
            if (!WinHttpQueryDataAvailable(hR, &sz) || !sz) break;
            std::vector<char> b(sz + 1, 0); DWORD rd = 0;
            WinHttpReadData(hR, b.data(), sz, &rd);
            result.append(b.data(), rd);
        } while (sz > 0);
    }
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
    return result;
}

// URL에서 파일 다운로드 (HTTP/HTTPS 자동 판별)
static bool DownloadFile(const std::string& url, const std::string& outPath)
{
    // URL 파싱
    bool isHttps = (url.find("https://") == 0);
    std::string rest = url.substr(isHttps ? 8 : 7);
    size_t pathStart = rest.find('/');
    std::string hostStr = (pathStart != std::string::npos) ? rest.substr(0, pathStart) : rest;
    std::string pathStr = (pathStart != std::string::npos) ? rest.substr(pathStart) : "/";

    int whl = MultiByteToWideChar(CP_UTF8, 0, hostStr.c_str(), -1, NULL, 0);
    std::wstring wHost(whl, 0); MultiByteToWideChar(CP_UTF8, 0, hostStr.c_str(), -1, &wHost[0], whl);
    int wpl = MultiByteToWideChar(CP_UTF8, 0, pathStr.c_str(), -1, NULL, 0);
    std::wstring wPath(wpl, 0); MultiByteToWideChar(CP_UTF8, 0, pathStr.c_str(), -1, &wPath[0], wpl);

    HINTERNET hS = WinHttpOpen(L"SCRScout/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return false;
    WinHttpSetTimeouts(hS, 10000, 10000, 30000, 30000);
    INTERNET_PORT port = isHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    DWORD flags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hC = WinHttpConnect(hS, wHost.c_str(), port, 0);
    if (!hC) { WinHttpCloseHandle(hS); return false; }
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", wPath.c_str(), NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return false; }

    bool ok = false;
    if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hR, NULL)) {
        DWORD status = 0, ssz = sizeof(status);
        WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            NULL, &status, &ssz, NULL);
        if (status == 200) {
            std::vector<char> data;
            DWORD sz = 0;
            do {
                if (!WinHttpQueryDataAvailable(hR, &sz) || !sz) break;
                size_t off = data.size(); data.resize(off + sz);
                DWORD rd = 0; WinHttpReadData(hR, data.data() + off, sz, &rd);
                data.resize(off + rd);
            } while (sz > 0);
            if (!data.empty()) {
                FILE* f = nullptr; fopen_s(&f, outPath.c_str(), "wb");
                if (f) { fwrite(data.data(), 1, data.size(), f); fclose(f); ok = true; }
            }
        }
    }
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
    return ok;
}

// screp.exe 실행 → stdout 캡처
static std::string RunScrep(const std::string& screpPath, const std::string& repPath)
{
    // 파이프 생성
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    SECURITY_ATTRIBUTES sa = {}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return "";

    // 자식 프로세스가 읽기 끝을 상속하지 않도록
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = "\"" + screpPath + "\" -cmds \"" + repPath + "\"";
    STARTUPINFOA si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError  = hWritePipe;
    PROCESS_INFORMATION pi = {};

    std::string result;
    if (CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hWritePipe); hWritePipe = NULL;
        char buf[4096];
        DWORD rd = 0;
        while (ReadFile(hReadPipe, buf, sizeof(buf), &rd, NULL) && rd > 0)
            result.append(buf, rd);
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    if (hWritePipe) CloseHandle(hWritePipe);
    CloseHandle(hReadPipe);
    return result;
}

// 최상위 레벨의 string 값만 추출 (중첩 객체 내 동명 키 무시)
// screp의 Player 객체는 "Type":{"Name":"Human",...} 이 먼저 나오므로
// 일반 JsonStringVal로는 "Human"이 잡힘 → 깊이 1에서만 검색
static std::string JsonTopLevelString(const std::string& obj, const std::string& key)
{
    std::string fk = "\"" + key + "\"";
    int depth = 0;
    for (size_t i = 0; i < obj.size(); i++) {
        char c = obj[i];
        if (c == '{' || c == '[') { depth++; continue; }
        if (c == '}' || c == ']') { depth--; continue; }
        if (depth == 1 && obj.compare(i, fk.size(), fk) == 0) {
            size_t p = i + fk.size();
            while (p < obj.size() && (obj[p]==' '||obj[p]=='\t'||obj[p]=='\n'||obj[p]=='\r'||obj[p]==':')) p++;
            if (p < obj.size() && obj[p] == '"') {
                p++;
                size_t e = p;
                while (e < obj.size() && obj[e] != '"') { if (obj[e]=='\\') e++; e++; }
                return obj.substr(p, e - p);
            }
        }
    }
    return "";
}

// screp JSON에서 채팅 추출 → ReplayGame에 채워 넣기
static void ParseScrepJson(const std::string& json, const std::string& toon, ReplayGame& game)
{
    if (json.empty() || json[0] != '{') return;

    // Players: SlotID → Name 맵 (깊이 인식으로 올바른 Name 추출)
    std::map<int, std::string> slotToName;
    size_t playersPos = json.find("\"Players\"");
    if (playersPos != std::string::npos) {
        size_t arr = json.find('[', playersPos);
        size_t arrEnd = FindMatchingBrace(json, arr);
        size_t cur = arr + 1;
        while (cur < arrEnd) {
            size_t oS = json.find('{', cur); if (oS == std::string::npos || oS >= arrEnd) break;
            size_t oE = FindMatchingBrace(json, oS); if (oE == std::string::npos) break;
            std::string obj = json.substr(oS, oE - oS + 1);
            int slotId = JsonIntVal(obj, "SlotID");
            std::string name = JsonTopLevelString(obj, "Name"); // 최상위 Name만
            if (!name.empty()) slotToName[slotId] = name;
            cur = oE + 1;
        }
    }

    // ChatCmds
    size_t cmdsPos = json.find("\"ChatCmds\"");
    if (cmdsPos == std::string::npos) return;
    size_t arr = json.find('[', cmdsPos);
    if (arr == std::string::npos) return;
    size_t arrEnd = FindMatchingBrace(json, arr);
    if (arrEnd == std::string::npos) return;

    auto framesToTime = [](int frames) -> std::string {
        int s = (int)(frames / 23.81);
        char buf[8]; snprintf(buf, sizeof(buf), "%02d:%02d", s / 60, s % 60);
        return buf;
    };

    size_t cur = arr + 1;
    while (cur < arrEnd) {
        size_t oS = json.find('{', cur); if (oS == std::string::npos || oS >= arrEnd) break;
        size_t oE = FindMatchingBrace(json, oS); if (oE == std::string::npos) break;
        std::string obj = json.substr(oS, oE - oS + 1);

        int frame     = JsonIntVal(obj, "Frame");
        int slotId    = JsonIntVal(obj, "SenderSlotID");
        std::string msg = JsonStringVal(obj, "Message");

        if (!msg.empty()) {
            auto it = slotToName.find(slotId);
            std::string sender = (it != slotToName.end()) ? it->second : "";
            ReplayChatLine cl;
            cl.time     = framesToTime(frame);
            cl.sender   = sender;
            cl.msg      = msg;
            cl.isTarget = (sender == toon);
            game.lines.push_back(cl);
        }
        cur = oE + 1;
    }
}

// Supabase에서 리플레이 목록 조회 (특정 게이트웨이)
static const char* SUPABASE_HOST_U8 = "xmploueumzkrdvapbyfs.supabase.co";
static const char* SUPABASE_ANON    =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"
    ".eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InhtcGxvdWV1bXprcmR2YXBieWZzIiwicm9sZSI6ImFub24iLCJpYXQiOjE2NzI4ODY5MTQsImV4cCI6MTk4ODQ2MjkxNH0"
    ".p8Jkm2fnFzzy7YYdCs0NVjBdqLmUzvBFJjdf3V0bHuo";

struct ReplayRecord {
    std::string id, timestamp, replayUrl, opponentAlias, result;
};

static std::vector<ReplayRecord> FetchReplayList(const std::string& toon, int gateway, int count = 100)
{
    std::vector<ReplayRecord> results;
    // URL 인코딩 (이름에 공백/특수문자 대응)
    std::string encodedToon;
    for (unsigned char c : toon) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') encodedToon += c;
        else { char esc[4]; snprintf(esc, sizeof(esc), "%%%02X", c); encodedToon += esc; }
    }

    int offset = 0;
    while ((int)results.size() < count) {
        int batch = std::min(100, count - (int)results.size());
        std::string path =
            "/rest/v1/player_matches_distinct_v2"
            "?alias=eq."    + encodedToon +
            "&gateway=eq."  + std::to_string(gateway) +
            "&replay_url=not.is.null"
            "&order=timestamp.desc"
            "&limit="       + std::to_string(batch) +
            "&offset="      + std::to_string(offset) +
            "&select=id,timestamp,replay_url,opponent_alias,result";

        int wpl = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, NULL, 0);
        std::wstring wPath(wpl, 0); MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wPath[0], wpl);
        std::wstring hdrs = L"apikey: ";
        int wal = MultiByteToWideChar(CP_UTF8, 0, SUPABASE_ANON, -1, NULL, 0);
        std::wstring wAnon(wal, 0); MultiByteToWideChar(CP_UTF8, 0, SUPABASE_ANON, -1, &wAnon[0], wal);
        hdrs += wAnon + L"\r\nAuthorization: Bearer " + wAnon + L"\r\n";

        std::string json = HttpsGet(L"xmploueumzkrdvapbyfs.supabase.co", wPath, hdrs);
        if (json.empty() || json[0] != '[') break;

        // JSON 배열 파싱
        size_t cur = 1;
        int pageCount = 0;
        while (cur < json.size()) {
            size_t oS = json.find('{', cur); if (oS == std::string::npos) break;
            size_t oE = FindMatchingBrace(json, oS); if (oE == std::string::npos) break;
            std::string obj = json.substr(oS, oE - oS + 1);
            ReplayRecord r;
            r.id           = JsonStringVal(obj, "id");
            r.timestamp    = JsonStringVal(obj, "timestamp");
            r.replayUrl    = JsonStringVal(obj, "replay_url");
            r.opponentAlias= JsonStringVal(obj, "opponent_alias");
            r.result       = JsonStringVal(obj, "result");
            if (!r.replayUrl.empty()) results.push_back(r);
            cur = oE + 1; pageCount++;
        }
        if (pageCount < batch) break;
        offset += pageCount;
    }
    return results;
}

// 백그라운드: 리플레이 다운로드 + 채팅 파싱 (병렬)
static void DoFetchReplayChat(std::string toon, int gateway)
{
    auto setMsg = [&](ReplayFetchStatus st, const std::string& msg) {
        std::lock_guard<std::mutex> lk(g_replayFetch.mtx);
        g_replayFetch.status    = st;
        g_replayFetch.statusMsg = msg;
    };

    // screp 위치 확인
    std::string screpPath = FindScrepExe();
    if (screpPath.empty()) {
        setMsg(ReplayFetchStatus::FETCH_ERROR, S("screp.exe 없음 (scr_scout.exe 옆에 두세요)", "screp.exe not found"));
        return;
    }

    // 임시 폴더
    char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
    std::string baseDir = std::string(tmp) + "scrscout\\";
    CreateDirectoryA(baseDir.c_str(), NULL);
    std::string outDir = baseDir + toon + "\\";
    CreateDirectoryA(outDir.c_str(), NULL);

    // ── 1. 리플레이 목록 조회 (게이트웨이 fallback) ──────────────────────────
    setMsg(ReplayFetchStatus::LOADING_LIST, S("목록 조회 중...", "Fetching list..."));
    std::vector<ReplayRecord> records;
    const int gwOrder[] = { gateway, 30, 20, 10, 11, 12 };
    int usedGw = gateway;
    for (int gw : gwOrder) {
        if (gw == 0) continue;
        bool dup = false;
        for (int prev : gwOrder) { if (prev == gw && prev != gwOrder[0]) { dup = true; break; } }
        // 첫 번째(=감지된 gw) 이외엔 이미 시도한 gw 중복 체크
        records = FetchReplayList(toon, gw, 100);
        if (!records.empty()) { usedGw = gw; break; }
    }
    if (records.empty()) {
        setMsg(ReplayFetchStatus::FETCH_ERROR, S("리플레이 없음 (cwal.gg 미등록)", "No replays found"));
        return;
    }

    // ── 2. 병렬 다운로드 (최대 8 워커) ──────────────────────────────────────
    int total = (int)records.size();
    g_replayFetch.dlTotal  = total;
    g_replayFetch.dlDone   = 0;
    setMsg(ReplayFetchStatus::DOWNLOADING, "");

    // 다운로드된 파일 경로 (index → path, 실패면 "")
    std::vector<std::string> repPaths(total);
    {
        std::mutex qMtx;
        std::queue<int> workQ;
        for (int i = 0; i < total; i++) workQ.push(i);

        auto worker = [&]() {
            while (true) {
                int idx;
                { std::lock_guard<std::mutex> lk(qMtx);
                  if (workQ.empty()) return;
                  idx = workQ.front(); workQ.pop(); }

                auto& rec = records[idx];
                // 파일명: "001_20231225_120000_win_vs_opp_ABCD1234.rep"
                std::string ts = rec.timestamp.size() >= 19
                    ? rec.timestamp.substr(0,10) + "_" + rec.timestamp.substr(11,8)
                    : rec.timestamp;
                for (char& c : ts) if (c==':' || c=='-' || c=='T') c = '_';
                std::string rid = rec.id.size() >= 11 ? rec.id.substr(3, 8) : rec.id;
                char fname[256];
                snprintf(fname, sizeof(fname), "%03d_%s_%s_vs_%s_%s.rep",
                    idx + 1, ts.c_str(), rec.result.c_str(),
                    rec.opponentAlias.c_str(), rid.c_str());
                // 특수문자 제거
                for (int ci = 0; fname[ci]; ci++) {
                    char c = fname[ci];
                    if (c=='/'||c=='\\'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>') fname[ci]='_';
                }
                std::string outPath = outDir + fname;

                bool ok = true;
                if (GetFileAttributesA(outPath.c_str()) == INVALID_FILE_ATTRIBUTES)
                    ok = DownloadFile(rec.replayUrl, outPath);
                if (ok) repPaths[idx] = outPath;
                g_replayFetch.dlDone++;
            }
        };

        const int DL_WORKERS = 8;
        std::vector<std::thread> threads;
        for (int i = 0; i < std::min(DL_WORKERS, total); i++)
            threads.emplace_back(worker);
        for (auto& t : threads) t.join();
    }

    // ── 3. 병렬 screp 파싱 (최대 6 워커) ────────────────────────────────────
    g_replayFetch.parseTotal = total;
    g_replayFetch.parseDone  = 0;
    setMsg(ReplayFetchStatus::PARSING, "");

    std::vector<ReplayGame> games(total);
    // 게임 메타데이터 미리 채우기
    for (int i = 0; i < total; i++) {
        games[i].opponent  = records[i].opponentAlias;
        games[i].timestamp = records[i].timestamp;
        games[i].result    = records[i].result;
        games[i].filename  = repPaths[i].empty() ? "" :
            repPaths[i].substr(repPaths[i].rfind('\\') + 1);
    }
    {
        std::mutex qMtx;
        std::queue<int> workQ;
        for (int i = 0; i < total; i++)
            if (!repPaths[i].empty()) workQ.push(i);

        auto worker = [&]() {
            while (true) {
                int idx;
                { std::lock_guard<std::mutex> lk(qMtx);
                  if (workQ.empty()) return;
                  idx = workQ.front(); workQ.pop(); }
                std::string json = RunScrep(screpPath, repPaths[idx]);
                ParseScrepJson(json, toon, games[idx]);
                g_replayFetch.parseDone++;
            }
        };

        const int PARSE_WORKERS = 6;
        std::vector<std::thread> threads;
        int qSize = 0;
        { std::lock_guard<std::mutex> lk(qMtx); qSize = (int)workQ.size(); }
        for (int i = 0; i < std::min(PARSE_WORKERS, qSize); i++)
            threads.emplace_back(worker);
        for (auto& t : threads) t.join();
    }

    // ── 4. 완료 ─────────────────────────────────────────────────────────────
    int chatGames = 0;
    for (auto& g : games) if (!g.lines.empty()) chatGames++;

    // 비매너 검사 (lock 잡기 전에 수행)
    bool bm = g_detectBadManner ? CheckBadManner(games) : false;

    std::lock_guard<std::mutex> lk(g_replayFetch.mtx);
    g_replayFetch.games     = std::move(games);
    g_replayFetch.status    = ReplayFetchStatus::DONE;
    g_replayFetch.statusMsg = "";
    g_replayFetch.gateway   = usedGw;
    g_bmDetected = bm;
}

// ---------------------------------------------------------------------------
// imgui 렌더링
// ---------------------------------------------------------------------------
static void RenderOverlay()
{
    if (!g_imguiInitialized || !g_pRenderTargetView) return;

    // 보여줄 내용이 없으면 Present 건너뜀 (GPU/DWM 부하 제거)
    bool hasContent = g_showGui || (!g_mapName.empty() && g_isInGame) || g_showReplayViewer;
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

    // 인터랙티브 영역 리셋 (매 프레임 갱신)
    g_replayBtnScreenRect    = {};
    g_replayViewerScreenRect = {};

    // 설정 GUI
    if (g_showGui)
    {
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints(ImVec2(200, 60), ImVec2(600, 700));
        ImGui::Begin(u8"SCR Scout", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);

        // 맵 이름 (인게임)
        if (!g_mapName.empty() && g_isInGame) {
            std::string utf8(g_mapName.size()*3+1, 0);
            WideCharToMultiByte(CP_UTF8, 0, g_mapName.c_str(), -1, &utf8[0], (int)utf8.size(), NULL, NULL);
            ImGui::SetWindowFontScale(1.3f);
            ImGui::TextColored(ImVec4(1,1,0.4f,1), S(u8"맵: %s", "Map: %s"), utf8.c_str());
            ImGui::SetWindowFontScale(1.0f);
            ImGui::Separator();
        }

        // ============================================================
        // 전적 조회
        // ============================================================
        ImGui::SeparatorText(S(u8"전적 조회", "Stats"));
        {
            // 셀 내 텍스트 가운데 정렬 헬퍼
            auto CellCenter = [](const char* str) {
                float cw = ImGui::GetColumnWidth();
                float tw = ImGui::CalcTextSize(str).x;
                float off = (cw - tw) * 0.5f;
                if (off > 0.f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + off);
            };

            // 기존 테이블 렌더 람다 (티어 표)
            auto RenderProfileTable = [&CellCenter](const DisplayProfile& prof) {
                if (!prof.statusMsg.empty())
                    ImGui::TextDisabled("%s", prof.statusMsg.c_str());
                if (!prof.valid) return;
                ImGui::TextColored(ImVec4(1,0.85f,0,1), S(u8"배틀태그: %s#", "BattleTag: %s#"), prof.battleTag.c_str());
                // 열 순서: GW, ID, Race, Cur, Best, 연승
                const char* hGW   = "GW";
                const char* hID   = S(u8"아이디", "ID");
                const char* hRace = S(u8"종족",   "Race");
                const char* hCur  = S(u8"현재",   "Curr");
                const char* hBest = S(u8"최고",   "Best");
                const char* hStr  = S(u8"연승",   "Streak");
                float tp = 10.f; // 헤더 잘림 방지용 여유
                float wGW   = std::max(ImGui::CalcTextSize(hGW).x,   ImGui::CalcTextSize("Asia").x)     + tp;
                float wRace = std::max(ImGui::CalcTextSize(hRace).x,  ImGui::CalcTextSize("T").x)        + tp;
                float wCur  = std::max(ImGui::CalcTextSize(hCur).x,  ImGui::CalcTextSize("D1234 S19").x) + tp;
                float wBest = std::max(ImGui::CalcTextSize(hBest).x, ImGui::CalcTextSize("D1234 S19").x) + tp;
                float wStr  = std::max(ImGui::CalcTextSize(hStr).x,  ImGui::CalcTextSize("99W").x)       + tp;
                auto RaceColor = [](char rc) -> ImVec4 {
                    if (rc == 'T') return ImVec4(0.45f, 0.70f, 1.00f, 1.f);
                    if (rc == 'P') return ImVec4(1.00f, 0.85f, 0.25f, 1.f);
                    if (rc == 'Z') return ImVec4(1.00f, 0.38f, 0.38f, 1.f);
                    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
                };
                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3, 2));
                if (ImGui::BeginTable("##toons", 6,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX))
                {
                    ImGui::TableSetupColumn(hGW,   ImGuiTableColumnFlags_WidthFixed, wGW);
                    ImGui::TableSetupColumn(hID,   ImGuiTableColumnFlags_WidthFixed, 0);
                    ImGui::TableSetupColumn(hRace, ImGuiTableColumnFlags_WidthFixed, wRace);
                    ImGui::TableSetupColumn(hCur,  ImGuiTableColumnFlags_WidthFixed, wCur);
                    ImGui::TableSetupColumn(hBest, ImGuiTableColumnFlags_WidthFixed, wBest);
                    ImGui::TableSetupColumn(hStr,  ImGuiTableColumnFlags_WidthFixed, wStr);
                    // 헤더 중앙 정렬
                    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                    for (int col = 0; col < 6; col++) {
                        ImGui::TableSetColumnIndex(col);
                        const char* name = ImGui::TableGetColumnName(col);
                        CellCenter(name);
                        ImGui::TableHeader(name);
                    }
                    for (auto& t : prof.toons) {
                        bool isQuery = (t.name == prof.queryName);
                        ImGui::TableNextRow();
                        if (isQuery)
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                ImGui::ColorConvertFloat4ToU32(ImVec4(0.3f, 0.5f, 0.2f, 0.35f)));

                        // col 0: GW
                        char gwStr[8]; snprintf(gwStr, sizeof(gwStr), "%s", GatewayName(t.gateway));
                        ImGui::TableSetColumnIndex(0); CellCenter(gwStr); ImGui::TextDisabled("%s", gwStr);

                        // col 1: ID
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%s", t.name.c_str());

                        // col 2: Race (종족별 색상)
                        ImGui::TableSetColumnIndex(2);
                        char rc = t.cur_race; char rcStr[4]; snprintf(rcStr, sizeof(rcStr), "%c", (rc=='U')?'-':rc);
                        CellCenter(rcStr);
                        if (rc != 'U') ImGui::TextColored(RaceColor(rc), "%s", rcStr);
                        else ImGui::TextDisabled("%s", rcStr);

                        // col 3: Cur
                        ImGui::TableSetColumnIndex(3);
                        if (t.cur_tier != 'U') {
                            char buf[32]; snprintf(buf, sizeof(buf), "%c%d S%d", t.cur_tier, t.cur_rating, t.cur_season);
                            CellCenter(buf); ImGui::TextColored(TierColor(t.cur_tier), "%s", buf);
                        } else {
                            const char* u = "-";
                            CellCenter(u); ImGui::TextDisabled("%s", u);
                        }

                        // col 4: Best
                        ImGui::TableSetColumnIndex(4);
                        if (t.best_tier != 'U') {
                            char buf[32]; snprintf(buf, sizeof(buf), "%c%d S%d", t.best_tier, t.best_rating, t.best_season);
                            CellCenter(buf); ImGui::TextColored(TierColor(t.best_tier), "%s", buf);
                        } else { CellCenter("-"); ImGui::TextDisabled("-"); }

                        // col 5: 연승/연패
                        ImGui::TableSetColumnIndex(5);
                        if (t.win_streak > 0) {
                            char buf[16]; snprintf(buf, sizeof(buf), "%dW", t.win_streak);
                            CellCenter(buf); ImGui::TextColored(ImVec4(0.3f,0.6f,1.f,1.f), "%s", buf);
                        } else if (t.loss_streak > 0) {
                            char buf[16]; snprintf(buf, sizeof(buf), "%dL", t.loss_streak);
                            CellCenter(buf); ImGui::TextColored(ImVec4(1.f,0.3f,0.3f,1.f), "%s", buf);
                        } else { CellCenter("-"); ImGui::TextDisabled("-"); }
                    }
                    ImGui::EndTable();
                }
                ImGui::PopStyleVar(); // CellPadding
            };

            // queryName toon의 hist 카드 (테이블 형태)
            auto RenderHistCard = [&CellCenter](const DisplayProfile& prof, bool showVsRace) {
                if (!prof.valid) return;
                const ToonStat* t = nullptr;
                for (auto& ts : prof.toons)
                    if (ts.name == prof.queryName) { t = &ts; break; }
                if (!t && !prof.toons.empty()) t = &prof.toons[0];
                if (!t || !t->hist.fetched) return;

                float wr = t->hist.WinRate();
                if (wr < 0.f) { ImGui::TextDisabled(S(u8"기록 없음", "No records")); return; }

                // T=0, Z=1, P=2 → 표시 순서 T, P, Z
                const int      kRaceOrder[3] = { 0, 2, 1 };
                const char*    kRaceLabel[3] = { "T", "P", "Z" };
                const ImVec4   kRaceColor[3] = {
                    ImVec4(0.45f, 0.70f, 1.00f, 1.f),  // T 파랑
                    ImVec4(1.00f, 0.85f, 0.25f, 1.f),  // P 노랑
                    ImVec4(1.00f, 0.38f, 0.38f, 1.f),  // Z 빨강
                };
                const ImVec4   kBarColor[3] = {
                    ImVec4(0.25f, 0.50f, 0.90f, 0.85f),
                    ImVec4(0.80f, 0.65f, 0.10f, 0.85f),
                    ImVec4(0.85f, 0.20f, 0.20f, 0.85f),
                };

                bool hasAny = false;
                for (int i = 0; i < 3; ++i) if (t->hist.vs[i].games > 0) { hasAny = true; break; }

                ImGui::Spacing();
                if (showVsRace && hasAny)
                    ImGui::TextDisabled(S(u8"최근 100경기 상대 종족별 승률", "Win rate by opp. race (last 100)"));
                const char* hdrVs   = "vs";
                const char* hdrGame = S(u8"경기", "Game");
                const char* hdrWin  = S(u8"승률", "Win");
                const char* hdrDisc = S(u8"디스", "Disconnect");
                float pad = 6.f;
                float wVs   = std::max(ImGui::CalcTextSize(hdrVs).x,   ImGui::CalcTextSize("Total").x) + pad;
                float wGame = std::max(ImGui::CalcTextSize(hdrGame).x,  ImGui::CalcTextSize("9999").x)  + pad;
                float wDisc = std::max(ImGui::CalcTextSize(hdrDisc).x,  ImGui::CalcTextSize("99.9%").x) + pad;

                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3, 2));
                // Total행: vs/Game/Win/Disconnect(4열), 종족행: vs/Game/Win(3열)
                // 테이블은 4열 기준, 종족행은 Disconnect 열 비움
                if (ImGui::BeginTable("##hist", 4,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX))
                {
                    ImGui::TableSetupColumn(hdrVs,   ImGuiTableColumnFlags_WidthFixed, wVs);
                    ImGui::TableSetupColumn(hdrGame,  ImGuiTableColumnFlags_WidthFixed, wGame);
                    ImGui::TableSetupColumn(hdrWin,   ImGuiTableColumnFlags_WidthFixed, 68);
                    ImGui::TableSetupColumn(hdrDisc,  ImGuiTableColumnFlags_WidthFixed, wDisc);
                    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                    for (int col = 0; col < 4; col++) {
                        ImGui::TableSetColumnIndex(col);
                        const char* name = ImGui::TableGetColumnName(col);
                        CellCenter(name);
                        ImGui::TableHeader(name);
                    }

                    // Total 행 (흰색, Disconnect 포함)
                    {
                        float dcr = t->hist.DiscRate() * 100.f;
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        CellCenter("Total"); ImGui::Text("Total");

                        ImGui::TableSetColumnIndex(1);
                        char gStr[8]; snprintf(gStr, sizeof(gStr), "%d", t->hist.total);
                        CellCenter(gStr); ImGui::Text("%s", gStr);

                        ImGui::TableSetColumnIndex(2);
                        char barLabel[16]; snprintf(barLabel, sizeof(barLabel), "%.0f%%", wr * 100.f);
                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(1.f, 1.f, 1.f, 0.85f));
                        ImGui::ProgressBar(wr, ImVec2(-1, 0), barLabel);
                        ImGui::PopStyleColor();

                        ImGui::TableSetColumnIndex(3);
                        char dcStr[16]; snprintf(dcStr, sizeof(dcStr), "%.1f%%", dcr);
                        ImVec4 dcCol = (dcr >= 10.f) ? ImVec4(1.f,0.3f,0.3f,1.f)
                                     : (dcr >=  5.f) ? ImVec4(1.f,0.6f,0.1f,1.f)
                                     :                 ImGui::GetStyleColorVec4(ImGuiCol_Text);
                        CellCenter(dcStr); ImGui::TextColored(dcCol, "%s", dcStr);
                    }

                    if (showVsRace) {
                        for (int i = 0; i < 3; ++i) {
                            int ri = kRaceOrder[i];
                            const auto& vs = t->hist.vs[ri];
                            if (vs.games == 0) continue;
                            float vwr = vs.WinRate();
                            ImGui::TableNextRow();

                            ImGui::TableSetColumnIndex(0);
                            char lbl[4]; snprintf(lbl, sizeof(lbl), "%s", kRaceLabel[i]);
                            CellCenter(lbl); ImGui::TextColored(kRaceColor[i], "%s", lbl);

                            ImGui::TableSetColumnIndex(1);
                            char gStr[8]; snprintf(gStr, sizeof(gStr), "%d", vs.games);
                            CellCenter(gStr); ImGui::Text("%s", gStr);

                            ImGui::TableSetColumnIndex(2);
                            char barLabel[16]; snprintf(barLabel, sizeof(barLabel), "%.0f%%", vwr >= 0.f ? vwr * 100.f : 0.f);
                            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kBarColor[i]);
                            ImGui::ProgressBar(vwr >= 0.f ? vwr : 0.f, ImVec2(-1, 0), barLabel);
                            ImGui::PopStyleColor();
                            // col 3 (Disconnect): 종족별 행은 비움
                        }
                    }

                    ImGui::EndTable();
                }
                ImGui::PopStyleVar();
            };

            // --- 자동 조회 결과 (게임 중 상대방) ---
            bool autoFetching; std::vector<DisplayProfile> autoProfs;
            {
                std::lock_guard<std::mutex> lk(g_profileMutex);
                autoFetching = g_autoFetching;
                autoProfs    = g_autoProfiles;
            }
            if (autoFetching)
                ImGui::TextDisabled(S(u8"상대방 조회 중...", "Fetching stats..."));
            else if (!g_isInGame && autoProfs.empty()) {
                // 로비: 내 전적 표시
                bool selfFetching, selfFetched; DisplayProfile selfProf;
                {
                    std::lock_guard<std::mutex> lk(g_profileMutex);
                    // 프로세스 바뀌면 재조회
                    if (g_selfLastProcess != g_hProcess) {
                        g_selfFetched  = false;
                        g_selfFetching = false;
                        g_selfLastProcess = g_hProcess;
                    }
                    selfFetching = g_selfFetching;
                    selfFetched  = g_selfFetched;
                    selfProf     = g_selfProfile;
                    if (!selfFetched && !selfFetching && g_hProcess) {
                        g_selfFetching = true;
                        std::thread(FetchSelfProfile).detach();
                    }
                }
                if (selfFetching)
                    ImGui::TextDisabled(S(u8"내 전적 조회 중...", "Fetching my stats..."));
                else if (selfFetched) {
                    RenderProfileTable(selfProf);
                    RenderHistCard(selfProf, true);
                }
                else
                    ImGui::TextDisabled(S(u8"게임 대기 중...", "Waiting for game..."));
            }
            for (auto& prof : autoProfs) {
                RenderProfileTable(prof);
                RenderHistCard(prof, autoProfs.size() == 1);

                // 인게임 + 유효한 프로필에만 로드/보기 버튼 표시
                if (g_isInGame && prof.valid) {
                    ReplayFetchStatus fetchSt;
                    std::string fetchToon;
                    {
                        std::lock_guard<std::mutex> lk(g_replayFetch.mtx);
                        fetchSt   = g_replayFetch.status;
                        fetchToon = g_replayFetch.toon;
                    }
                    bool isThis    = (fetchToon == prof.queryName);
                    bool isLoading = (fetchSt == ReplayFetchStatus::LOADING_LIST ||
                                      fetchSt == ReplayFetchStatus::DOWNLOADING  ||
                                      fetchSt == ReplayFetchStatus::PARSING);
                    bool isDone    = (isThis && fetchSt == ReplayFetchStatus::DONE);
                    // 로드 버튼: 현재 로딩 중이거나 이미 이 toon이 완료면 비활성
                    bool loadDis = isLoading || isDone;
                    // 보기 버튼: 이 toon의 데이터가 완료된 경우만 활성
                    bool showEn  = isDone;

                    char loadId[80]; snprintf(loadId, sizeof(loadId), S(u8"로드##a%s", "Load##a%s"), prof.queryName.c_str());
                    char showId[80]; snprintf(showId, sizeof(showId), S(u8"보기##a%s", "Show##a%s"), prof.queryName.c_str());
                    float hw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

                    if (loadDis) ImGui::BeginDisabled();
                    if (ImGui::Button(loadId, ImVec2(hw, 0))) {
                        int gw = 30;
                        for (auto& t : prof.toons)
                            if (t.name == prof.queryName) { gw = t.gateway > 0 ? t.gateway : 30; break; }
                        {
                            std::lock_guard<std::mutex> lk(g_replayFetch.mtx);
                            g_replayFetch.toon = prof.queryName;
                            g_replayFetch.gateway = 0;
                            g_replayFetch.status  = ReplayFetchStatus::LOADING_LIST;
                            g_replayFetch.statusMsg = "";
                            g_replayFetch.games.clear();
                            g_replayFetch.dlDone = g_replayFetch.dlTotal = 0;
                            g_replayFetch.parseDone = g_replayFetch.parseTotal = 0;
                        }
                        std::string qn = prof.queryName;
                        std::thread([qn, gw]() { DoFetchReplayChat(qn, gw); }).detach();
                    }
                    if (loadDis) ImGui::EndDisabled();

                    ImGui::SameLine();
                    if (!showEn) ImGui::BeginDisabled();
                    if (ImGui::Button(showId, ImVec2(hw, 0)))
                        g_showReplayViewer = !g_showReplayViewer;
                    if (!showEn) ImGui::EndDisabled();
                }
            }


        } // 전적 조회

        // ============================================================
        // 상대방 IP (npcap)
        // ============================================================
        ImGui::SeparatorText(S(u8"상대 IP", "Peer IPs"));
        {
            std::vector<PeerIPInfo> ips;
            { std::lock_guard<std::mutex> lk(g_peerMutex); ips = g_peerIPs; }
            if (ips.empty()) {
                ImGui::TextDisabled(S(u8"없음", "None"));
            } else {
                float lineH = ImGui::GetTextLineHeightWithSpacing();
                float ipsH = 0;
                for (auto& info : ips) {
                    ipsH += lineH;
                    if (info.fetched) {
                        ipsH += lineH;
                        if (!info.org.empty()) ipsH += lineH;
                    }
                }
                ipsH = std::min(ipsH + 4.f, 150.f);
                ImGui::BeginChild("##peerips", ImVec2(0, ipsH), false, ImGuiWindowFlags_HorizontalScrollbar);
                for (auto& info : ips) {
                    ImGui::TextUnformatted(info.ip.c_str());
                    if (!info.fetched) {
                        ImGui::SameLine(); ImGui::TextDisabled("...");
                    } else {
                        char loc[128];
                        snprintf(loc, sizeof(loc), "  %s / %s / %s",
                            info.country.c_str(), info.region.c_str(), info.city.c_str());
                        ImGui::TextDisabled("%s", loc);
                        if (!info.org.empty())
                            ImGui::TextDisabled("  %s", info.org.c_str());
                    }
                }
                ImGui::EndChild();
            }
        }



        ImGui::End();
    } // g_showGui

    // ============================================================
    // 리플레이 채팅 뷰어 (별도 ImGui 창)
    // ============================================================
    if (g_showReplayViewer) {
        // 상태 스냅샷
        ReplayFetchStatus fetchSt; std::string fetchToon, fetchMsg;
        int dlDone, dlTotal, parseDone, parseTotal;
        static std::vector<ReplayGame> s_viewGames;
        static ReplayFetchStatus s_lastSt = ReplayFetchStatus::IDLE;
        static std::string s_lastToon;
        {
            std::lock_guard<std::mutex> lk(g_replayFetch.mtx);
            fetchSt    = g_replayFetch.status;
            fetchToon  = g_replayFetch.toon;
            fetchMsg   = g_replayFetch.statusMsg;
            dlDone     = g_replayFetch.dlDone;
            dlTotal    = g_replayFetch.dlTotal;
            parseDone  = g_replayFetch.parseDone;
            parseTotal = g_replayFetch.parseTotal;
            // 완료 시점에만 게임 데이터 복사 (매 프레임 복사 방지)
            if ((fetchSt == ReplayFetchStatus::DONE || fetchSt == ReplayFetchStatus::FETCH_ERROR)
                && (fetchSt != s_lastSt || fetchToon != s_lastToon)) {
                s_viewGames = g_replayFetch.games;
                s_lastSt   = fetchSt;
                s_lastToon = fetchToon;
            }
        }

        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
            ImGuiCond_Once, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(560, 620), ImGuiCond_Once);
        bool viewerOpen = true;
        const char* viewerTitle = S(u8"리플레이 채팅", "Replay Chat");
        ImGui::Begin(viewerTitle, &viewerOpen, ImGuiWindowFlags_NoSavedSettings);
        if (!viewerOpen) g_showReplayViewer = false;

        // 헤더
        if (!fetchToon.empty()) {
            ImGui::TextColored(ImVec4(1,0.85f,0,1), S(u8"대상: %s", "Target: %s"), fetchToon.c_str());
            ImGui::SameLine();
        }

        // 진행 상태
        if (fetchSt == ReplayFetchStatus::LOADING_LIST) {
            ImGui::TextDisabled(S(u8"목록 조회 중...", "Fetching list..."));
        } else if (fetchSt == ReplayFetchStatus::DOWNLOADING) {
            char prog[64]; snprintf(prog, sizeof(prog),
                S(u8"다운로드 중... (%d/%d)", "Downloading... (%d/%d)"), dlDone, dlTotal);
            ImGui::TextDisabled("%s", prog);
            if (dlTotal > 0)
                ImGui::ProgressBar((float)dlDone / dlTotal, ImVec2(-1, 6));
        } else if (fetchSt == ReplayFetchStatus::PARSING) {
            char prog[64]; snprintf(prog, sizeof(prog),
                S(u8"채팅 파싱 중... (%d/%d)", "Parsing... (%d/%d)"), parseDone, parseTotal);
            ImGui::TextDisabled("%s", prog);
            if (parseTotal > 0)
                ImGui::ProgressBar((float)parseDone / parseTotal, ImVec2(-1, 6));
        } else if (fetchSt == ReplayFetchStatus::FETCH_ERROR) {
            ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "%s", fetchMsg.c_str());
        } else if (fetchSt == ReplayFetchStatus::DONE) {
            int chatGames = 0;
            for (auto& g : s_viewGames) if (!g.lines.empty()) chatGames++;
            ImGui::TextDisabled(S(u8"완료: %d게임 / 채팅 %d게임", "Done: %d games / chat in %d"),
                (int)s_viewGames.size(), chatGames);
            if (g_detectBadManner && g_bmDetected)
                ImGui::TextColored(ImVec4(1.f, 0.2f, 0.2f, 1.f), S(u8"⚠ 비매너 채팅 검출", "⚠ Bad manner chat detected"));
        } else {
            ImGui::TextDisabled(S(u8"대기 중", "Idle"));
        }
        ImGui::Separator();

        // 채팅 목록 (newest first = s_viewGames 순서대로)
        ImGui::BeginChild("##chatscroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
        for (auto& game : s_viewGames) {
            if (game.lines.empty()) continue;
            // 게임 헤더
            char hdr[256];
            std::string ts = game.timestamp.size() >= 16
                ? game.timestamp.substr(0,10) + " " + game.timestamp.substr(11,5) : game.timestamp;
            snprintf(hdr, sizeof(hdr), S(u8"── %s vs %s [%s] %s ──",
                                         "── %s vs %s [%s] %s ──"),
                fetchToon.c_str(), game.opponent.c_str(), game.result.c_str(), ts.c_str());
            ImGui::TextColored(ImVec4(0.55f,0.55f,0.55f,1), "%s", hdr);
            // 채팅 라인
            for (auto& cl : game.lines) {
                char line[512];
                snprintf(line, sizeof(line), "[%s] %s: %s",
                    cl.time.c_str(), cl.sender.c_str(), cl.msg.c_str());
                if (cl.isTarget)
                    ImGui::TextColored(ImVec4(1,1,0,1), "%s", line);
                else
                    ImGui::TextUnformatted(line);
            }
            ImGui::Spacing();
        }
        ImGui::EndChild();
        ImGui::End();

        // 뷰어 영역 화면 좌표 추적 (NCHITTEST용)
        ImGuiWindow* vw = ImGui::FindWindowByName(viewerTitle);
        if (vw && vw->Size.x > 0) {
            POINT o = {0, 0}; ClientToScreen(g_hOverlay, &o);
            g_replayViewerScreenRect = {
                o.x + (LONG)vw->Pos.x,
                o.y + (LONG)vw->Pos.y,
                o.x + (LONG)(vw->Pos.x + vw->Size.x),
                o.y + (LONG)(vw->Pos.y + vw->Size.y)
            };
        }
    }

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(0, 0);

    // GUI 영역만 윈도우 리전으로 설정
    // - 리전 밖: 윈도우가 존재하지 않음 → SC가 마우스/커서 직접 처리
    // - 리전 안, HTTRANSPARENT: SC로 전달 / HTCLIENT: 오버레이가 처리
    {
        HRGN combined = CreateRectRgn(0, 0, 0, 0); // 빈 시작
        if (g_showGui) {
            ImGuiWindow* w = ImGui::FindWindowByName(u8"SCR Scout");
            if (w && w->Size.x > 0) {
                HRGN r = CreateRectRgn((LONG)w->Pos.x, (LONG)w->Pos.y,
                    (LONG)(w->Pos.x + w->Size.x), (LONG)(w->Pos.y + w->Size.y));
                CombineRgn(combined, combined, r, RGN_OR);
                DeleteObject(r);
            }
        }
        if (g_showReplayViewer) {
            const char* vname = S(u8"리플레이 채팅", "Replay Chat");
            ImGuiWindow* vw = ImGui::FindWindowByName(vname);
            if (vw && vw->Size.x > 0) {
                HRGN r = CreateRectRgn((LONG)vw->Pos.x, (LONG)vw->Pos.y,
                    (LONG)(vw->Pos.x + vw->Size.x), (LONG)(vw->Pos.y + vw->Size.y));
                CombineRgn(combined, combined, r, RGN_OR);
                DeleteObject(r);
            }
        }
        SetWindowRgn(g_hOverlay, combined, FALSE);
        // SetWindowRgn이 combined 소유권을 가져가므로 DeleteObject 불필요
    }
}

// ---------------------------------------------------------------------------
// 오버레이 윈도우 프로시저
// ---------------------------------------------------------------------------
LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg)
    {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        RenderOverlay(); // WantTextInput 즉시 갱신 (Ctrl+V 지연 방지)
        return 0;
    case WM_MOUSEWHEEL:
        return 0; // 훅에서 전달된 휠 이벤트 - ImGui가 위에서 처리함
    case WM_NCHITTEST: {
        // 오버레이 ImGui 창 또는 뷰어 창 영역 내 → HTCLIENT (클릭 수신)
        // 그 외 → HTTRANSPARENT (StarCraft로 통과)
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        // 메인 오버레이 창 영역
        if (g_showGui) {
            RECT rc; GetWindowRect(g_hOverlay, &rc);
            ImGuiWindow* w = ImGui::FindWindowByName(u8"SCR Scout");
            if (w && w->Size.x > 0) {
                POINT o = {0,0}; ClientToScreen(g_hOverlay, &o);
                RECT wr = { o.x+(LONG)w->Pos.x, o.y+(LONG)w->Pos.y,
                            o.x+(LONG)(w->Pos.x+w->Size.x), o.y+(LONG)(w->Pos.y+w->Size.y) };
                if (PtInRect(&wr, pt)) return HTCLIENT;
            }
        }
        // 뷰어 창 영역
        if (g_showReplayViewer &&
            g_replayViewerScreenRect.right > g_replayViewerScreenRect.left &&
            PtInRect(&g_replayViewerScreenRect, pt))
            return HTCLIENT;
        return HTTRANSPARENT;
    }
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
            { std::lock_guard<std::mutex> lk(g_peerMutex); g_peerIPs.clear(); }
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
        if (g_wasInGame && !inGame) {
            // 게임 종료 → 로비: IP 목록 초기화
            { std::lock_guard<std::mutex> lk(g_peerMutex); g_peerIPs.clear(); }
            // 게임 종료 → 로비: 상대 전적 초기화 + 내 전적 재조회
            std::lock_guard<std::mutex> lk(g_profileMutex);
            g_autoProfiles.clear();
            g_autoFetching = false;
            g_selfFetched  = false;
            g_selfFetching = false;
        }
        g_wasInGame = inGame;
        g_isInGame  = inGame;

        // 로비에서 ID 변경 감지:
        // 평상시: 캐시 주소 1개만 읽음 (거의 무비용)
        // 계정 전환 시: 캐시 주소가 무효화되면 백그라운드에서 전체 스캔 1회
        if (!inGame && g_hProcess) {
            ULONGLONG addr; bool fetched;
            {
                std::lock_guard<std::mutex> lk(g_profileMutex);
                addr = g_selfToonAddr; fetched = g_selfFetched;
            }
            if (addr && fetched) {
                const char myPfx[] = "HAT:"; size_t plen = strlen(myPfx);
                std::string s = ReadNullTerminatedString(g_hProcess, addr);
                if (s.compare(0, plen, myPfx) == 0) {
                    // 주소 유효 - ID가 바뀌었는지 확인
                    size_t e = s.find('\x10', plen);
                    std::string curID = (e != std::string::npos && e > plen) ? s.substr(plen, e-plen) : s.substr(plen);
                    if (!curID.empty()) {
                        std::lock_guard<std::mutex> lk(g_profileMutex);
                        if (!g_selfFetching && g_selfProfile.queryName != curID) {
                            g_selfFetched = false; g_selfFetching = false; g_selfToonAddr = 0;
                        }
                    }
                } else {
                    // 주소 무효 (계정 전환으로 메모리 변경) → 백그라운드 전체 스캔 1회
                    {
                        std::lock_guard<std::mutex> lk(g_profileMutex);
                        if (!g_selfFetching) { g_selfFetched = false; g_selfFetching = false; g_selfToonAddr = 0; }
                    }
                }
            }
        }

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
        WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
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
    g_hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, NULL, 0);
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

void WhisperScanThread()
{
    while (true)
    {
        if (g_hProcess && g_whisperReply)
            ScanWhisperSenderFromRingBuffer();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;

        // ImGui가 텍스트 입력 중이면 키보드 이벤트를 오버레이로 전달
        // (WS_EX_NOACTIVATE로 포커스가 없어도 입력 가능하게)
        if (g_hOverlay && g_imguiInitialized && ImGui::GetIO().WantTextInput) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                LPARAM lp = 1 | ((LPARAM)kb->scanCode << 16);
                if (kb->flags & LLKHF_EXTENDED) lp |= (1 << 24);
                PostMessage(g_hOverlay, (UINT)wParam, kb->vkCode, lp);
                // 인쇄 가능한 문자는 WM_CHAR도 생성
                BYTE ks[256] = {};
                GetKeyboardState(ks);
                wchar_t buf[8] = {};
                int n = ToUnicode(kb->vkCode, kb->scanCode, ks, buf, 8, 0);
                for (int i = 0; i < n; i++)
                    PostMessage(g_hOverlay, WM_CHAR, buf[i], lp);
                return 1; // SC로 전달 차단
            }
            if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                LPARAM lp = 1 | ((LPARAM)kb->scanCode << 16) | (0xC0000000L);
                if (kb->flags & LLKHF_EXTENDED) lp |= (1 << 24);
                PostMessage(g_hOverlay, (UINT)wParam, kb->vkCode, lp);
                return 1;
            }
        }

        HWND hFg = GetForegroundWindow();
        DWORD fgPID = 0; GetWindowThreadProcessId(hFg, &fgPID);
        bool scFg = (fgPID == g_starcraftPID), ovFg = (hFg == g_hOverlay);

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
        {
            // F1: GUI 토글
            if (kb->vkCode == VK_F12 && (scFg || ovFg) && g_hOverlay)
            {
                g_showGuiManual = !g_showGuiManual;
                g_showGui = g_showGuiManual;
                if (!g_showGui) g_showReplayViewer = false; // 오버레이 숨길 때 채팅창도 닫기
                UpdateOverlayPosition();
                RenderOverlay();
                return 1;
            }
            if ((scFg || ovFg) && g_muteOtherAudio && kb->vkCode == VK_PAUSE) {
                std::thread(ToggleMuteOtherAudio).detach();
                return 1;
            }
            if (scFg) {
                if (kb->vkCode == KEY_IGNORE)    { std::thread(DoExtraction).detach(); }
                if (kb->vkCode == KEY_UNIGNORE)  { std::thread(DoRemoval).detach(); }
                if (g_whisperReply && kb->vkCode == VK_RETURN && (GetKeyState(VK_SHIFT) & 0x8000)) {
                    std::thread(SendWhisperReply).detach();
                    return 1;
                }
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
static const wchar_t* REG_KEY = L"Software\\scr_scout";

void SaveSettings() {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) != ERROR_SUCCESS) return;
    DWORD v;
    v = g_swapSpaceAndControl;   RegSetValueExW(hKey, L"SwapSpaceAndControl",   0, REG_DWORD, (BYTE*)&v, sizeof(v));
    v = g_autoIgnoreOnGameStart; RegSetValueExW(hKey, L"AutoIgnoreOnGameStart",  0, REG_DWORD, (BYTE*)&v, sizeof(v));
    v = g_autoShowStats;         RegSetValueExW(hKey, L"AutoShowStats",          0, REG_DWORD, (BYTE*)&v, sizeof(v));
    v = g_whisperReply;          RegSetValueExW(hKey, L"WhisperReply",           0, REG_DWORD, (BYTE*)&v, sizeof(v));
    v = g_fastJoin;              RegSetValueExW(hKey, L"FastJoin",               0, REG_DWORD, (BYTE*)&v, sizeof(v));
    v = g_autoFetchChatOnGameStart; RegSetValueExW(hKey, L"AutoFetchChatOnGameStart", 0, REG_DWORD, (BYTE*)&v, sizeof(v));
    v = g_muteOtherAudio;           RegSetValueExW(hKey, L"MuteOtherAudio",           0, REG_DWORD, (BYTE*)&v, sizeof(v));
    v = g_detectBadManner;          RegSetValueExW(hKey, L"DetectBadManner",          0, REG_DWORD, (BYTE*)&v, sizeof(v));
    RegCloseKey(hKey);
}

void LoadSettings() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) return;
    DWORD v, sz = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"SwapSpaceAndControl",   NULL, NULL, (BYTE*)&v, &sz) == ERROR_SUCCESS) g_swapSpaceAndControl   = v != 0; sz = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"AutoIgnoreOnGameStart",  NULL, NULL, (BYTE*)&v, &sz) == ERROR_SUCCESS) g_autoIgnoreOnGameStart  = v != 0; sz = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"AutoShowStats",          NULL, NULL, (BYTE*)&v, &sz) == ERROR_SUCCESS) g_autoShowStats          = v != 0; sz = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"WhisperReply",           NULL, NULL, (BYTE*)&v, &sz) == ERROR_SUCCESS) g_whisperReply           = v != 0; sz = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"FastJoin",               NULL, NULL, (BYTE*)&v, &sz) == ERROR_SUCCESS) g_fastJoin               = v != 0; sz = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"AutoFetchChatOnGameStart", NULL, NULL, (BYTE*)&v, &sz) == ERROR_SUCCESS) g_autoFetchChatOnGameStart = v != 0; sz = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"MuteOtherAudio",           NULL, NULL, (BYTE*)&v, &sz) == ERROR_SUCCESS) g_muteOtherAudio           = v != 0; sz = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"DetectBadManner",          NULL, NULL, (BYTE*)&v, &sz) == ERROR_SUCCESS) g_detectBadManner          = v != 0;
    RegCloseKey(hKey);
}

INT_PTR CALLBACK SettingDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG:
        CheckDlgButton(hDlg, IDC_SWAP_KEY,        g_swapSpaceAndControl   ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_AUTO_IGNORE,      g_autoIgnoreOnGameStart ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_AUTO_SHOW_STATS,   g_autoShowStats              ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_AUTO_FETCH_CHAT,  g_autoFetchChatOnGameStart   ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_WHISPER_REPLY,    g_whisperReply               ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_FAST_JOIN,        g_fastJoin              ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_MUTE_OTHER_AUDIO, g_muteOtherAudio        ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_DETECT_BM,      g_detectBadManner       ? BST_CHECKED : BST_UNCHECKED);
        if (!g_isKorean) {
            SetWindowTextW(hDlg, L"Settings");
            SetDlgItemTextW(hDlg, IDC_SWAP_KEY,        L"Use Space bar as Control key");
            SetDlgItemTextW(hDlg, IDC_AUTO_IGNORE,     L"Auto-ignore opponent's chat on game start");
            SetDlgItemTextW(hDlg, IDC_AUTO_SHOW_STATS,  L"Auto-display stats 5 seconds after game start");
            SetDlgItemTextW(hDlg, IDC_AUTO_FETCH_CHAT,  L"Auto-load replay chat on game start");
            SetDlgItemTextW(hDlg, IDC_WHISPER_REPLY,    L"Shift+Enter: Quick reply to last whisper");
            SetDlgItemTextW(hDlg, IDC_FAST_JOIN,        L"Quick join public games");
            SetDlgItemTextW(hDlg, IDC_MUTE_OTHER_AUDIO, L"Pause key: Mute other audio");
            SetDlgItemTextW(hDlg, IDC_DETECT_BM,       L"Detect bad manner chat in replays");
            SetDlgItemTextW(hDlg, IDC_STATIC,          L"You can change these settings anytime by right-clicking the system tray icon.");
        }
        return (INT_PTR)TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            g_swapSpaceAndControl   = IsDlgButtonChecked(hDlg, IDC_SWAP_KEY)        == BST_CHECKED;
            g_autoIgnoreOnGameStart = IsDlgButtonChecked(hDlg, IDC_AUTO_IGNORE)     == BST_CHECKED;
            g_autoShowStats              = IsDlgButtonChecked(hDlg, IDC_AUTO_SHOW_STATS) == BST_CHECKED;
            g_autoFetchChatOnGameStart   = IsDlgButtonChecked(hDlg, IDC_AUTO_FETCH_CHAT) == BST_CHECKED;
            g_whisperReply               = IsDlgButtonChecked(hDlg, IDC_WHISPER_REPLY)   == BST_CHECKED;
            g_fastJoin              = IsDlgButtonChecked(hDlg, IDC_FAST_JOIN)       == BST_CHECKED;
            g_muteOtherAudio        = IsDlgButtonChecked(hDlg, IDC_MUTE_OTHER_AUDIO) == BST_CHECKED;
            g_detectBadManner       = IsDlgButtonChecked(hDlg, IDC_DETECT_BM)         == BST_CHECKED;
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
            AppendMenu(hM, MF_STRING, 1005, L"About");
            AppendMenu(hM, MF_STRING, 1003, L"Exit");
            SetForegroundWindow(hWnd);
            TrackPopupMenu(hM, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(hM);
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == 1002) MessageBox(hWnd, WS(L"F9: 채팅 무시\nF8: 무시 해제\nF12: 전적 오버레이", L"F9: Ignore chat\nF8: Unignore\nF12: Stats overlay"), L"Help", MB_OK | MB_ICONINFORMATION);
        else if (LOWORD(wParam) == 1003) DestroyWindow(hWnd);
        else if (LOWORD(wParam) == 1004) DialogBox(hInst, MAKEINTRESOURCE(IDD_SETTING_DIALOG), hWnd, SettingDlgProc);
        else if (LOWORD(wParam) == 1005) {
            std::wstring msg = std::wstring(L"SCR Scout v") + APP_VERSION +
                L"\n\nhttps://github.com/IkbeomJeon/scr_scout\njeonikbeom@gmail.com";
            MessageBox(hWnd, msg.c_str(), L"About SCR Scout", MB_OK | MB_ICONINFORMATION);
        }
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
    HWND hWnd = CreateWindowW(szWindowClass, L"SCR Scout", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) return FALSE;
    ShowWindow(hWnd, SW_HIDE); UpdateWindow(hWnd);
    nid.cbSize = sizeof(nid); nid.hWnd = hWnd; nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP+1;
    nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_BWAUTOIGNORE));
    wcscpy_s(nid.szTip, L"SCR Scout");
    Shell_NotifyIcon(NIM_ADD, &nid);
    return TRUE;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
    HANDLE hMutex = CreateMutex(NULL, TRUE, L"Local\\scr_scoutMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    DetectLanguage(lpCmdLine);
    _setmode(_fileno(stdout), _O_U16TEXT);
    setlocale(LC_ALL, "");
    LoadSettings();

    MyRegisterClass(hInstance);
    if (!InitInstance(hInstance, nCmdShow)) return FALSE;

    StartKeyboardHook();
    CreateOverlayWindow(hInstance);

    std::thread(ProcessMonitorThread).detach();
    std::thread(WhisperScanThread).detach();
    StartPcapCapture();

    DialogBox(hInst, MAKEINTRESOURCE(IDD_SETTING_DIALOG), nid.hWnd, SettingDlgProc);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }

    StopKeyboardHook();
    DisableFastJoin();
    if (g_imguiInitialized) {
        if (g_hMouseHook) { UnhookWindowsHookEx(g_hMouseHook); g_hMouseHook = NULL; }
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
