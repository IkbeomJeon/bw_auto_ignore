"""
matchmaker-gameinfo-by-toon API 응답의 첫 번째 매치 전체 구조 출력
- 게임 실행 중 실행
- 사용법: python dump_gameinfo.py <toon이름> [gateway=30] [season=19]
"""
import sys, urllib.request, json, subprocess, re
sys.stdout.reconfigure(encoding='utf-8')

def find_port():
    try:
        out = subprocess.check_output("netstat -ano", shell=True, text=True, errors='ignore')
        pids = set(); ports = []
        for line in out.splitlines():
            if "StarCraft" in line: continue
            if "127.0.0.1" in line and "LISTEN" in line:
                m = re.search(r'127\.0\.0\.1:(\d+)', line)
                if m: ports.append(int(m.group(1)))
    except: pass
    # 포트 범위 5000~65000 탐색
    for p in range(5000, 65000):
        try:
            req = urllib.request.Request(f"http://127.0.0.1:{p}/web-api/",
                headers={"User-Agent":"StarCraft/1.0"})
            with urllib.request.urlopen(req, timeout=0.5) as r:
                if r.status in (200, 404): return p
        except: pass
    return None

toon    = sys.argv[1] if len(sys.argv) > 1 else "ghfhxhtm"
gateway = int(sys.argv[2]) if len(sys.argv) > 2 else 30
season  = int(sys.argv[3]) if len(sys.argv) > 3 else 19

print(f"포트 탐색 중...")
port = find_port()
if not port: print("포트 탐색 실패 - SC:R 실행 중인지 확인"); exit()
print(f"포트: {port}")

url = f"http://127.0.0.1:{port}/web-api/v1/matchmaker-gameinfo-by-toon/{toon}/{gateway}/1/{season}?limit=3"
print(f"URL: {url}\n")

req = urllib.request.Request(url, headers={"User-Agent":"StarCraft/1.0"})
with urllib.request.urlopen(req, timeout=5) as r:
    raw = r.read().decode('utf-8')

data = json.loads(raw)
if not isinstance(data, list) or not data:
    print("응답이 리스트가 아니거나 비어있음:")
    print(raw[:500])
    exit()

print(f"총 {len(data)}개 매치 (limit=3 요청)")
print(f"\n=== 첫 번째 매치 전체 구조 ===")
print(json.dumps(data[0], indent=2, ensure_ascii=False))

# game_result 키 목록
gr = data[0].get("game_result", {})
print(f"\n=== game_result 키 목록 ===")
for pname, pdata in gr.items():
    print(f"\n  플레이어: {pname!r}")
    print(f"  {json.dumps(pdata, indent=4, ensure_ascii=False)}")

# 최상위 키 목록
print(f"\n=== 매치 최상위 키 ===")
for k, v in data[0].items():
    print(f"  {k}: {type(v).__name__} = {str(v)[:80]}")
