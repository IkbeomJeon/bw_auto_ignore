"""
SC:R 리플레이 채팅 추출기
- 폴더 내 모든 .rep 파일의 채팅을 추출해 .txt로 저장
- 사용법: python extract_chat.py <리플레이폴더> [출력파일.txt] [screp경로]
  예) python extract_chat.py C:/replays/_ZeaIot_
      python extract_chat.py C:/replays/_ZeaIot_ chat.txt
      python extract_chat.py C:/replays/_ZeaIot_ chat.txt C:/wd/screp_python/screp.exe
"""
import sys, os, subprocess, json, glob
sys.stdout.reconfigure(encoding='utf-8')

# screp.exe 후보 경로 (우선순위 순)
def find_screp():
    candidates = [
        os.path.join(os.path.dirname(__file__), "screp.exe"),
        os.path.join(os.path.dirname(__file__), "..", "screp_python", "screp.exe"),
        r"C:\wd\screp_python\screp.exe",
        "screp.exe",
        "screp",
    ]
    for c in candidates:
        if os.path.exists(c):
            return os.path.abspath(c)
    return None

SCREP = find_screp()
if not SCREP:
    print("screp.exe를 찾을 수 없습니다.")
    print("사용법: python extract_chat.py <폴더> [출력파일] [screp경로]")
    sys.exit(1)

def frames_to_time(frames):
    total_sec = int(frames / 23.81)
    m, s = divmod(total_sec, 60)
    return f"{m:02d}:{s:02d}"

def parse_chat(rep_path):
    """리플레이에서 채팅 목록 반환: [(time_str, sender, receiver, message), ...]"""
    try:
        result = subprocess.run(
            [SCREP, "-cmds", rep_path],
            capture_output=True, encoding="utf-8", errors="replace", timeout=10
        )
        if not result.stdout:
            return None, None, []
        data = json.loads(result.stdout)
    except Exception as e:
        print(f"  파싱 실패: {e}")
        return None, None, []

    header   = data.get("Header", {})
    players  = header.get("Players", [])
    chat_raw = data.get("Computed", {}).get("ChatCmds") or []

    if not players:
        return None, None, []

    # SlotID → 이름 매핑
    slot_to_name = {p["SlotID"]: p["Name"] for p in players}
    # PlayerID → 이름 매핑 (fallback)
    id_to_name   = {p["ID"]: p["Name"] for p in players}

    chats = []
    for cmd in chat_raw:
        frame      = cmd.get("Frame", 0)
        player_id  = cmd.get("PlayerID", -1)
        sender_slot= cmd.get("SenderSlotID", -1)
        message    = cmd.get("Message", "")

        # 발신자: SenderSlotID 우선, 없으면 PlayerID로 fallback
        sender = slot_to_name.get(sender_slot) or id_to_name.get(player_id) or f"Player{player_id}"

        # 수신자: 발신자 제외 나머지 플레이어 (1v1이면 상대 1명)
        others = [p["Name"] for p in players if p["Name"] != sender]
        receiver = ", ".join(others) if others else "All"

        chats.append((frames_to_time(frame), sender, receiver, message))

    # 플레이어 이름 두 명
    names = [p["Name"] for p in players if not p.get("Observer")]
    return names[0] if len(names) > 0 else "?", names[1] if len(names) > 1 else "?", chats

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)

    rep_dir  = sys.argv[1]
    out_file = sys.argv[2] if len(sys.argv) > 2 else os.path.join(rep_dir, "chat.txt")

    global SCREP
    if len(sys.argv) > 3:
        SCREP = sys.argv[3]

    print(f"screp: {SCREP}")

    rep_files = sorted(glob.glob(os.path.join(rep_dir, "*.rep")))
    if not rep_files:
        print(f"리플레이 파일이 없습니다: {rep_dir}"); sys.exit(1)

    print(f"리플레이 {len(rep_files)}개 처리 중...")

    lines = []
    total_msgs = 0
    skipped = 0

    for rep_path in rep_files:
        fname = os.path.basename(rep_path)
        p1, p2, chats = parse_chat(rep_path)

        if not chats:
            skipped += 1
            continue

        lines.append(f"{'='*60}")
        lines.append(f"[{fname}]  {p1} vs {p2}")
        lines.append(f"{'='*60}")
        for time_str, sender, receiver, msg in chats:
            lines.append(f"[{time_str}] {sender} → {receiver}: {msg}")
        lines.append("")
        total_msgs += len(chats)
        print(f"  {fname}: {len(chats)}개")

    if not lines:
        print("채팅이 있는 리플레이가 없습니다.")
        sys.exit(0)

    with open(out_file, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print(f"\n완료: 총 {total_msgs}개 메시지 (채팅 없는 리플레이 {skipped}개 제외)")
    print(f"저장: {out_file}")

if __name__ == "__main__":
    main()
