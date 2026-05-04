# SCR Scout

[English](README.en.md) | [한국어](README.md)

> StarCraft: Remastered in-game overlay — auto-lookup opponent stats, block chat, and stay focused on the game.

[Download (.exe)](https://github.com/IkbeomJeon/scr_scout/raw/main/dist/scr_scout.exe) | Or build directly with Visual Studio 2022 x64

---

## Features

![main](img/main.png)

### 1. Auto Stat Lookup

When a game starts, automatically searches for your opponent's **all accounts (including smurfs)** and displays their tier, rating, race, and win/loss streak as an overlay.  
Instantly spot **smurf hunters** who prey on lower-ranked players using alternate accounts.

- Tier table: GW / ID / Race (color-coded) / Current tier / Best tier / Win·Loss streak
- **Highlights** the currently matched opponent ID in green
- Win rate by opponent race table (last 100 games): Total row + per-race (T/P/Z) game count and win rate bar

### 2. Auto Chat Ignore

Press **F9** once to instantly block your opponent's chat. Don't let trash talk or mind games distract you.  
Enable **Auto-ignore on game start** in the tray settings to apply it automatically every game without pressing anything.

### 3. Map Name Display

Shows the map name at the top of the overlay so you can instantly identify **maps where spawn positions are confusing**.

### 4. Space bar → Control Key

Easily assign unit groups **7, 8, 9, 0** using the space bar — no more awkward finger stretching.

### 5. In-game Quick Whisper Reply

Press **Shift+Enter** after receiving a whisper in-game and `/w sendername ` is automatically typed into the chat box.  
No need to manually type the sender's ID — just continue with your reply.

---

## Hotkeys

| Key | Function |
|-----|----------|
| F12 | Toggle stat overlay on/off (stays on if manually activated) |
| F9 | Ignore opponent's chat |
| F8 | Unignore opponent's chat |
| Shift+Enter | Quick reply to last whisper sender (in-game) |

## Settings

Right-click the system tray icon → Setting

| Setting | Description |
|---------|-------------|
| Auto-display stats (10s) | Automatically show the stat overlay for 10 seconds on game start |
| Auto-ignore on game start | Automatically ignore opponent chat on game start |
| Swap Space/Control | Use space bar as control key |
| Whisper reply (Shift+Enter) | Enable quick whisper reply |

---

## About the Windows Security Warning

When running the program, you may see a *"Windows protected your PC"* or *"Unrecognized app"* warning.

This happens because the executable is not signed with a code signing certificate (which requires a paid subscription) — it does not mean the program is harmful.  
**All source code for this program is publicly available in this repository** and can be built by anyone.

To proceed, click **"More info" → "Run anyway"** in the warning dialog.

---

## Contact

📧 jeonikbeom@gmail.com

## License

[MIT License](https://opensource.org/licenses/MIT)
