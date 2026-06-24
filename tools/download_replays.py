"""
SC:R 리플레이 다운로더 (cwal.gg 기반)
- SC:R 실행 불필요
- 사용법: python download_replays.py <아이디> <개수> [저장폴더]
  예) python download_replays.py ghfhxhtm 20
      python download_replays.py ghfhxhtm 20 C:/replays
"""
import sys, os, urllib.request, urllib.parse, json
sys.stdout.reconfigure(encoding='utf-8')

SUPABASE_URL = "https://xmploueumzkrdvapbyfs.supabase.co"
ANON_KEY     = ("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"
                ".eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InhtcGxvdWV1bXprcmR2YXBieWZzIiwicm9sZSI6ImFub24iLCJpYXQiOjE2NzI4ODY5MTQsImV4cCI6MTk4ODQ2MjkxNH0"
                ".p8Jkm2fnFzzy7YYdCs0NVjBdqLmUzvBFJjdf3V0bHuo")
GATEWAY      = 30   # KR

PAGE_SIZE = 100  # Supabase PostgREST 최대 페이지 크기

def fetch_replays(toon, count):
    """cwal.gg Supabase에서 최신순 리플레이 목록 조회 (페이지네이션)"""
    results = []
    offset = 0
    while len(results) < count:
        batch = min(PAGE_SIZE, count - len(results))
        params = urllib.parse.urlencode({
            "alias":      f"eq.{toon}",
            "gateway":    f"eq.{GATEWAY}",
            "replay_url": "not.is.null",
            "order":      "timestamp.desc",
            "limit":      batch,
            "offset":     offset,
            "select":     "id,timestamp,replay_url,alias,opponent_alias,map_name,result",
        })
        url = f"{SUPABASE_URL}/rest/v1/player_matches_distinct_v2?{params}"
        req = urllib.request.Request(url, headers={
            "apikey":        ANON_KEY,
            "Authorization": f"Bearer {ANON_KEY}",
        })
        with urllib.request.urlopen(req, timeout=15) as r:
            page = json.loads(r.read().decode('utf-8'))
        if not page:
            break
        results.extend(page)
        offset += len(page)
        if len(page) < batch:  # 더 이상 없음
            break
    return results

def download(replay_url, out_path):
    try:
        req = urllib.request.Request(replay_url,
            headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=30) as r:
            data = r.read()
        with open(out_path, 'wb') as f:
            f.write(data)
        return len(data)
    except Exception as e:
        print(f"    다운로드 실패: {e}")
        return 0

def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)

    toon    = sys.argv[1]
    count   = int(sys.argv[2])
    out_dir = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.getcwd(), f"replays_{toon}")

    print(f"아이디: {toon}  |  요청 개수: {count}  |  게이트웨이: KR")
    print(f"저장 폴더: {out_dir}")

    print("\n리플레이 목록 조회 중...")
    try:
        replays = fetch_replays(toon, count)
    except Exception as e:
        print(f"조회 실패: {e}"); sys.exit(1)

    if not replays:
        print("리플레이를 찾지 못했습니다.")
        print(f"  - cwal.gg에 '{toon}' (KR) 기록이 없거나 아이디가 다를 수 있습니다.")
        sys.exit(1)

    print(f"총 {len(replays)}개 발견\n")
    os.makedirs(out_dir, exist_ok=True)

    # 이미 받은 파일의 match ID 목록 (파일명 끝 _XXXXXXXX.rep에서 추출)
    existing_ids = {f[-12:-4] for f in os.listdir(out_dir) if f.endswith(".rep") and len(f) >= 12}

    for i, row in enumerate(replays, 1):
        ts      = (row.get("timestamp") or "")[:19].replace(":", "").replace("-", "").replace("T", "_")
        opp     = row.get("opponent_alias") or "unknown"
        result  = row.get("result") or ""
        rid     = row.get("id", "")[3:11]   # MM-XXXXXXXX-... 에서 앞 8자리 (고유)
        url     = row["replay_url"]
        fname   = f"{i:03d}_{ts}_{result}_vs_{opp}_{rid}.rep"
        out_path = os.path.join(out_dir, fname)

        if rid in existing_ids:
            print(f"[{i}/{len(replays)}] 이미 존재: {fname}")
            continue

        size = download(url, out_path)
        if size:
            print(f"[{i}/{len(replays)}] {fname}  ({size//1024}KB)")
        else:
            if os.path.exists(out_path):
                os.remove(out_path)

    print(f"\n완료! 저장 위치: {out_dir}")

if __name__ == "__main__":
    main()
