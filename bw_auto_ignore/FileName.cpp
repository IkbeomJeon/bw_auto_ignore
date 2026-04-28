#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <iostream>
#include <vector>
#include <io.h>
#include <fcntl.h>
#include <clocale>
#include <filesystem>

// 대상 프로세스 이름으로 프로세스 ID를 찾는 함수
DWORD GetProcessID(const wchar_t* processName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        std::wcout << L"스냅샷 생성 실패. 오류 코드: " << GetLastError() << L"\n";
        return 0;
    }

    PROCESSENTRY32W pe32{};
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(snapshot, &pe32)) {
        do {
            if (_wcsicmp(pe32.szExeFile, processName) == 0) {
                CloseHandle(snapshot);
                return pe32.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &pe32));
    }

    CloseHandle(snapshot);
    std::wcout << L"프로세스를 찾을 수 없습니다.\n";
    return 0;
}

// DLL을 대상 프로세스에 인젝션하는 함수
bool InjectDLL(DWORD processId, const wchar_t* dllPath) {
    // 전체 DLL 경로 확인
    std::filesystem::path dllFullPath = std::filesystem::absolute(dllPath);
    if (!std::filesystem::exists(dllFullPath)) {
        std::wcout << L"DLL 파일을 찾을 수 없습니다: " << dllFullPath.wstring() << L"\n";
        return false;
    }

    std::wcout << L"DLL 경로: " << dllFullPath.wstring() << L"\n";

    // 프로세스 핸들 열기
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
    if (hProcess == NULL) {
        std::wcout << L"프로세스 열기 실패. 오류 코드: " << GetLastError() << L"\n";
        return false;
    }

    // 대상 프로세스 메모리에 DLL 경로를 저장할 공간 할당
    size_t pathSize = (dllFullPath.wstring().length() + 1) * sizeof(wchar_t);
    LPVOID pRemotePath = VirtualAllocEx(hProcess, NULL, pathSize, MEM_COMMIT, PAGE_READWRITE);
    if (pRemotePath == NULL) {
        std::wcout << L"원격 메모리 할당 실패. 오류 코드: " << GetLastError() << L"\n";
        CloseHandle(hProcess);
        return false;
    }

    // DLL 경로 쓰기
    if (!WriteProcessMemory(hProcess, pRemotePath, dllFullPath.wstring().c_str(), pathSize, NULL)) {
        std::wcout << L"원격 메모리 쓰기 실패. 오류 코드: " << GetLastError() << L"\n";
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // LoadLibraryW 함수의 주소 가져오기
    HMODULE hKernel32 = GetModuleHandleW(L"Kernel32.dll");
    if (hKernel32 == NULL) {
        std::wcout << L"Kernel32.dll 모듈 핸들 획득 실패. 오류 코드: " << GetLastError() << L"\n";
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    FARPROC pLoadLibraryW = GetProcAddress(hKernel32, "LoadLibraryW");
    if (pLoadLibraryW == NULL) {
        std::wcout << L"LoadLibraryW 함수 주소 획득 실패. 오류 코드: " << GetLastError() << L"\n";
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // 원격 스레드 생성
    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0,
        (LPTHREAD_START_ROUTINE)pLoadLibraryW,
        pRemotePath, 0, NULL);
    if (hThread == NULL) {
        std::wcout << L"원격 스레드 생성 실패. 오류 코드: " << GetLastError() << L"\n";
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    std::wcout << L"원격 스레드 생성 성공! DLL 로딩 중...\n";

    // 원격 스레드 종료 대기
    WaitForSingleObject(hThread, INFINITE);

    // 스레드 종료 코드 확인
    DWORD exitCode = 0;
    if (GetExitCodeThread(hThread, &exitCode) && exitCode != 0) {
        std::wcout << L"DLL 로드 성공! 핸들: 0x" << std::hex << exitCode << std::dec << L"\n";
    }
    else {
        std::wcout << L"DLL 로드 실패. 종료 코드: 0x" << std::hex << exitCode << std::dec << L"\n";
        CloseHandle(hThread);
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // 리소스 정리
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    std::wcout << L"DLL 인젝션 성공!\n";
    return true;
}

// 프로세스에 이미 로드된 모듈 목록 출력
void ListProcessModules(DWORD processId) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        std::wcout << L"모듈 스냅샷 생성 실패. 오류 코드: " << GetLastError() << L"\n";
        return;
    }

    MODULEENTRY32W me32{};
    me32.dwSize = sizeof(MODULEENTRY32W);

    std::wcout << L"\n===== 프로세스 로드된 모듈 목록 =====\n";
    if (Module32FirstW(hSnapshot, &me32)) {
        do {
            std::wcout << L"모듈 이름: " << me32.szModule << L"\n";
            std::wcout << L"모듈 경로: " << me32.szExePath << L"\n";
            std::wcout << L"모듈 베이스 주소: 0x" << std::hex << (uintptr_t)me32.modBaseAddr << std::dec << L"\n";
            std::wcout << L"모듈 크기: " << me32.modBaseSize << L" 바이트\n";
            std::wcout << L"------------------------------\n";
        } while (Module32NextW(hSnapshot, &me32));
    }
    else {
        std::wcout << L"모듈 정보 획득 실패. 오류 코드: " << GetLastError() << L"\n";
    }

    CloseHandle(hSnapshot);
}

int main() {
    // 콘솔 출력 스트림을 유니코드 모드로 설정
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stdin), _O_U16TEXT);
    setlocale(LC_ALL, "");

    // 프로세스명 설정
    const wchar_t* processName = L"StarCraft.exe";
    std::wcout << L"대상 프로세스: " << processName << L"\n";

    // 프로세스 ID 찾기
    DWORD processId = GetProcessID(processName);
    if (processId == 0) {
        std::wcout << L"스타크래프트 프로세스를 찾을 수 없습니다. 게임이 실행 중인지 확인하세요.\n";
        std::wcout << L"아무 키나 누르면 종료됩니다...\n";
        std::wcin.get();
        return 1;
    }

    std::wcout << L"프로세스 ID: " << processId << L" (0x" << std::hex << processId << std::dec << L")\n";

    // 인젝션 전 모듈 목록 출력
    std::wcout << L"인젝션 전 모듈 목록:\n";
    ListProcessModules(processId);

    // DLL 경로 입력 받기
    std::wstring dllPath;
    std::wcout << L"인젝션할 DLL 파일 경로를 입력하세요: ";
    std::getline(std::wcin, dllPath);

    // DLL 인젝션 수행
    if (InjectDLL(processId, dllPath.c_str())) {
        // 인젝션 후 모듈 목록 출력
        std::wcout << L"\n인젝션 후 모듈 목록:\n";
        ListProcessModules(processId);
    }

    std::wcout << L"\n아무 키나 누르면 종료됩니다...\n";
    std::wcin.get();
    return 0;
}
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return main();
}