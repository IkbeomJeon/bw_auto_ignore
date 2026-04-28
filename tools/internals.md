# StarCraft Remastered 내부 데이터 접근 방법 정리

## 1. 프로세스 접근

```cpp
// StarCraft.exe PID 탐색
HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
// Process32FirstW / Process32NextW 로 "StarCraft.exe" 찾기

// 프로세스 열기
HANDLE hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);

// 모듈 베이스 주소 (ASLR 적용됨, 매 실행마다 다름)
// EnumProcessModulesEx → GetModuleFileNameExW → "StarCraft.exe" 확인
// 실측값 예시: 0x7FF67B140000
```

---

## 2. 메모리 오프셋 (베이스 주소 기준 RVA)

| 항목 | 오프셋 | 타입 | 비고 |
|------|--------|------|------|
| 플레이어 테이블 | `0x10931B0` | 구조체 배열 | 슬롯 8개 × 104바이트 |
| 인게임 여부 | `0x1090612` | BYTE | 1=인게임, 0=아님 |
| 맵 이름 | `0x1091FEE` | UTF-8 문자열 | null 종료 |

### 플레이어 슬롯 구조 (104바이트)
```
offset 0x00 : BYTE  - 슬롯 활성 여부 (0x01 = 활성)
offset 0x08 : char[] - 플레이어 이름 (UTF-8, null 종료)
```

### 읽기 예시
```cpp
ULONGLONG base = GetStarCraftModuleBase(); // 런타임 베이스
ReadProcessMemory(hProcess, (LPCVOID)(base + 0x10931B0), buf, 104*8, &r);

// 인게임 여부
BYTE flag;
ReadProcessMemory(hProcess, (LPCVOID)(base + 0x1090612), &flag, 1, &r);
bool inGame = (flag == 1);

// 맵 이름
BYTE mapBuf[64];
ReadProcessMemory(hProcess, (LPCVOID)(base + 0x1091FEE), mapBuf, 64, &r);
// UTF-8 → UTF-16 변환 후 사용
```

---

## 3. 로컬 웹 API 포트 탐색

SC:R은 로컬에 HTTP 서버를 띄워 전적 정보를 제공함. 포트는 매 실행마다 다름.

### 탐색 방법
1. SC 프로세스 전체 메모리를 `VirtualQueryEx` 로 순회
2. `"127.0.0.1:"` 패턴 검색 → 뒤따르는 숫자 추출 (4~5자리)
3. 후보 포트에 HTTP GET `/web-api/` 요청
4. 응답 status 200 또는 404 → 유효한 포트

```cpp
// 메모리 스캔 조건
mbi.State == MEM_COMMIT
!(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
mbi.RegionSize < 0x40000000

// 포트 유효성 검증 (401은 다른 서버이므로 제외)
status == 200 || status == 404  →  유효
status == 401                   →  무효
```

### 주의
- 인게임 중일 때만 API 서버 활성화됨 (로비/메인메뉴에서는 포트 없음)
- 탐색된 포트는 캐싱, SC 재시작 시 초기화

---

## 4. 전적 조회 API

```
GET http://127.0.0.1:{port}/web-api/v2/aurora-profile-by-toon/{이름}/{gateway}?request_flags=scr_tooninfo
Header: User-Agent: StarCraft/1.0
```

### Gateway 번호
| 번호 | 서버 |
|------|------|
| 10 | US West |
| 11 | US East |
| 12 | Europe |
| 20 | Asia |
| 30 | Korea |

### 다중 게이트웨이 조회
플레이어가 어느 서버 계정인지 모르므로 KR → Asia → USW → USE → EU 순으로 시도, `battle_tag` 있으면 성공.

### 응답 JSON 주요 필드
```json
{
  "battle_tag": "플레이어명",
  "toons": [
    { "toon": "인게임명", "guid": 12345, "gateway_id": 30 }
  ],
  "matchmaked_stats": [
    {
      "toon": "인게임명",
      "toon_guid": 12345,
      "season_id": 20,
      "bucket": 6,
      "rating": 1234
    }
  ],
  "stats": [
    {
      "toon": "인게임명",
      "gateway_id": 30,
      "raw": {
        "zerg_wins_sum": 100,
        "terran_wins_sum": 10,
        "protoss_wins_sum": 5
      }
    }
  ]
}
```

### 파싱 주의사항
- JSON 필드 구분자가 `"key":"value"` 가 아닌 `"key": "value"` (공백 포함)이므로 단순 문자열 검색 불가 → 공백/탭/콜론 스킵 처리 필요
- URL 변환 시 `MultiByteToWideChar` 에 `-1` 대신 `(int)name.size()` 사용 (null 문자 포함 방지)

### 티어 계산
```
bucket 1=F, 2=E, 3=D, 4=C, 5=B, 6=A, 7=S
현재 시즌 = season_id 최대값
역대 최고 = bucket 최대값
gateway = toon_guid → toons[].guid 매핑으로 확인
```

### 종족 계산
`stats[].raw` 의 `zerg/terran/protoss_wins_sum` 중 최대값으로 결정

---

## 5. 코드 보호 (분석 한계)

SC:R은 코드 섹션을 암호화/가상화함 (VMProtect 추정):
- **Ghidra 정적 분석**: 코드 섹션이 암호화되어 의미있는 디스어셈블 불가
- **CE 동적 분석**: 브레이크포인트 설정 시 게임 프리징 (안티디버깅)
- **CE 커널 드라이버 모드**: 동일하게 프리징
- **메모리 직접 읽기**: 코드 섹션은 `??` (접근 불가)

→ 내부 함수 직접 호출은 현실적으로 어려움. 데이터 영역(플레이어 테이블, 맵 이름 등)은 정상 접근 가능.

---

## 6. 채팅/커맨드 전송

현재: `SendInput` 배치로 Enter + 문자열 + Enter 를 단일 호출로 전송

이론적으로 가능한 방법:
- 커맨드 패킷 구조: `0x5C` (Chat), 82바이트 고정
- `send_command(packet, 82)` 내부 함수 호출 필요
- 코드 보호로 인해 함수 주소 탐색 불가 → 미구현
