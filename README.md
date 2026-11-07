# Block Blast bot

A C++ bot that plays the mobile puzzle game **Block Blast** (Classic mode) on an Android device over USB or Wi-Fi. It reads the board and tray from the screen, plans moves with a bitboard search, and drags the pieces. It plays continuously: it chases the best score, keeps going after it beats it, and starts a new game when it loses.

The current version is **v5.8-mach1** (C++, with a small Java helper on the tablet). Versions up to v5.4 were Python; they are kept in `versions/` and in the git history.

> Unofficial fan project, not affiliated with the makers of Block Blast.

## Benchmark

![Benchmark of every version](benchmark.jpg)

Every planner version plays the same simulated games (`python bench.py`); live speed is measured on the tablet from `bot.log`.

| Version | What changed |
|---|---|
| v1 original | One piece at a time with a simple board score; fixed waits after every move. |
| v2 bitboard planner | Plans all three tray pieces together on a 64-bit bitboard. |
| v3 look-ahead | Samples likely future trays when the board fills up; learns each colour theme. |
| v3.1 line-clear skip | Doesn't wait for line-clear animations; works with the navigation bar shown. |
| v4 combos | Keeps combo streaks alive; swipes a whole tray and checks the board once per tray. |
| v4.1 faster checks | Reads the board through combo pop-up text, no fixed waits, one long-lived adb shell for swipes (same planner as v4). |
| v4.2 fast drags | 150 ms drags, new trays planned from one screenshot, wood-theme tray fix. Measured late in a ~400k game, where planning and line clears take longer, so it isn't directly comparable with v4.1's early-game 67. |
| v5 deeper look-ahead | Planning ~6x faster (bit tricks + look-ahead in worker processes), so it can sample 32 future trays instead of 16. Missed fast drops are spotted in ~0.8 s and retried with a slow drag; after a miss, long drags slow down. On 20 longer games (600 moves) it survived longer (503 vs 466 moves); on the chart's 24 games it was slightly behind v4 — about even overall. Live speed measured early in a new game. |
| v5.1 fast planner | Back to the lighter look-ahead (16 future trays; the deeper one wasn't clearly better) with work spread evenly over the CPU cores: planning ~0.03 s per tray. Also handles the blue bevelled theme and size changes between menu and board. Live 87/min was measured over 865 moves (a longer run than v5's 302 early-game moves), median tray 100/min. |
| v5.3 flash max | Same planner as v5.1. No check screenshot after each tray (the next tray's screenshot confirms the board), learned wait for the game to deal the next tray, 120 ms drags. 100 moves/min over 302 live moves (median tray 1.3 s). Trade-off: its picture of the board can drift without the per-tray check; one game ended earlier than it should have because of that. |
| v5.3.1 flash highspeed max | Same planner. Stricter board check from the screenshot it already takes: a block the screen shows empty is removed at once; extra blocks are added only when nothing is animating (combo text is drawn in flat letters that look like blocks). Fewer re-reads, so most trays need one screenshot. 80 ms drags. 122 moves/min over 301 live moves (average tray 1.41 s), no missed drops. |
| v5.4 highspeed max | Fast like v5.3.1 while the board is open; careful when it is crowded (28+ cells after a tray): a deeper look-ahead (48 future trays) and a real check screenshot. A board that looks very different from the bot's picture is read carefully before planning. On 20 long games (up to 600 moves) it survived 494 moves vs v5.1's 466 and was never worse; on the chart's 24 shorter games it is about even with v5.1 (296 vs 299). ~100 moves/min live. The last Python version (`versions/v5.4-highspeed-max/`). |
| v5.5 cpp max | **Rewritten in C++**, with a small Java helper on the tablet that mirrors the screen (a frame in 0.04 s instead of 0.37 s) and injects touches directly. Planning takes ~2–5 ms per tray, so the look-ahead is much deeper (64 future trays x 16 candidates) and plays future trays keeping the combo going. Every drop is checked, so the board picture doesn't drift. **139 moves/min** over 688 live moves with no board corrections. Longest combo streak 67 vs v5.1's 45. Survival: on the chart's 24 games it lost 4 before 300 moves (v5.1: 1), but on 96 other games capped at 300 it reached the cap 83 times vs v5.1's 77, and on 96 games up to 1500 moves it survived 1161 moves on average vs v5.1's 945 (`build\sim.exe`); 24 games are too few to separate them. |
| v5.5.1 (experimental) | v5.5 with a planner tuned for **long-horizon survival** (`versions/v5.5.1-exp/`): it weighs the worst quarter of sampled futures (70%) instead of only their average, switches to careful mode from 24 filled cells, and there looks two future trays ahead. On 128 simulated games up to 3000 moves it lasted the full 3000 moves in 54 games vs v5.5's 29 (average 1992 vs 1616 moves), with longer combo streaks. On the chart's 24 games: 23 reach 300 moves (v5.5: 20, v5.1: 23), streak 66. **154 moves/min** over 750 live moves, no board corrections (same drag-and-check code as v5.5; the difference is likely game state). |
| v5.6 hyperspeed | v5.5.1's survival planner with a faster play loop (released by the user; the experimental label dropped on release). Drags no longer slow down for the rest of the run after one miss (v5.5.1 had drifted to 0.21 s drags, ~120 moves/min, after an hour); 70 ms drags, 30 ms between drags; a tray's first drops go straight after each other and the last drop's check verifies the whole board; a stuck touch is released automatically. **156 moves/min** over 757 live moves, 1 board correction. The user's target for it was 170; it was released at 156 by the user's decision. A game with v5.5.1/v5.6 reached a new best score of 2,976,894 (previous best 809,192). |
| v5.7 speedbreaker | Released (`versions/v5.7-speedbreaker/`). **Faster loop:** after a tray's last drop it checks the board at once and then watches only the tray (small frames that arrive much faster) until the new pieces are certain, instead of a blind wait; a missed drop (piece still in the tray) is spotted in ~0.1 s; 45 ms drags with 15 ms between them; a piece the game is still holding is released at once; full-screen ads inside the game are recognised (and closed when they allow it); the all-clear celebration on an emptied board no longer looks like a full board; game-over, menu and unknown screens are re-checked every 0.3–1 s instead of 3–10 s. A new tray is accepted only when three frames in a row agree (a faster two-frame version ran at **211 moves/min** but once misread a growing tray as three 3x3s and lost a game). **165 moves/min** live over Wi-Fi with that safe check (v5.6: 156); USB frames come faster. **Smarter:** board-scoring weights tuned in the simulator (smooth edges and no holes count more, raw empty space less): on 64 fresh games up to 3,000 moves it survived 2,090 moves on average vs 1,788 with v5.6's weights, 28 vs 23 games lasting all 3,000 moves. On short games it is level (96 games capped at 300: 89 vs 90 reach the cap; the chart's 24 games show 20 vs 23, within noise). Longest combo streak on the chart: 78, the best of any version. The user's target was 200 moves/min; released at 165 by the user's decision. |
| v5.8 mach1 | Released (`versions/v5.8-mach1/`), same planner as v5.7; all about speed over Wi-Fi. A tray's drags go to the tablet in one command (40 ms apart, the gap the game needs between a drop and the next drag; shorter gaps made pieces bounce back); a new tray is accepted when two frames agree but never earlier than 0.5 s after the last drop (half-grown pieces once fooled a faster check); the result screen and menu are acted on at once (no 3 s pause before a new game). Board-picture drift fixed (also in v5.7): each new tray's board is checked against the screen, lines expected to clear are re-checked after 1.6 s, and 4+ unexpected blocks trigger a careful re-read (1–3 are combo text). **174 moves/min** over 834 live moves over Wi-Fi (v5.7: 165), 0 board drift, 2 missed drops in 5 minutes. The user's target was 210 moves/min; over Wi-Fi a tray can't go much below ~1.0 s (the game deals a tray ~0.6 s after the last drop, and drags need 40 ms gaps), so it was released at 174 by the user's decision. |
| v5.9 trained (experimental) | Experimental (`versions/v5.9-trained-exp/`), not released. **Machine learning:** a small neural network (C++, compiled into the bot) judges each candidate move instead of playing 64–96 sampled future trays. It was trained by self-play in the simulator to predict what v5.8's slow look-ahead concludes about a board ("distillation", 4.1M examples), then one more round on boards it reached by itself (6.1M in total). On 256 fresh simulated games up to 3,000 moves it survives 2,166 moves on average vs v5.8's 2,190 (a tie within noise) while planning **3.6 ms per tray instead of ~44 ms** (12x less). Live it plays at **175 moves/min** over 889 moves over Wi-Fi (v5.8: 174): planning was never what limits speed (the deal animation and Wi-Fi are). Tools: `datagen.exe` (self-play data), `train.exe` (trainer). |

## How it works

1. **See** – a helper running on the tablet (`helper/BBHelper.java`, started by the bot over adb) keeps a live mirror of the screen in memory, so the latest frame arrives in ~0.04 s; without it the bot falls back to `screencap` (~0.37 s). The game changes colour theme as the score rises (dark, pink, mint, white, wood, ice…), so instead of fixed colours the bot learns what an empty cell looks like: empty cells share one colour, and after each move it re-learns that colour from cells it knows are empty. New tray pieces grow in, so a tray is only read once its pieces hold still.
2. **Plan** – `src/planner.cpp` searches every order and position of the tray pieces on a 64-bit bitboard. Boards are scored for open space, holes, ragged edges, and room for big pieces (3×3, 1×5), plus how likely a random next piece fits. The best candidates then play 64 sampled future trays each (on all CPU cores) and the plan that keeps surviving wins. It also tracks the combo streak (a line cleared within every 3 placements): clears that extend the streak score extra, breaking it costs, and the future trays are played keeping the streak alive.
3. **Act** – the helper drags each piece (80 ms) and the bot checks the drop on the next frame; a missed drop is retried at once (slowly, then one cell over), and small per-piece aim corrections are learned. The game moves a held piece about 1.47× the finger distance, and the aim accounts for that. Line-clear animations aren't waited for: a move counts as done once the rest of the board matches.
4. **Recover** – brings Block Blast back to the front, closes rating prompts and some ads; on the result screen or the menu it starts the next game; presses Back only when another app (e.g. an ad's store page) stays in front. Unknown screens are saved to `unknown-screens/` for calibration.

## Setup

- Windows, Android platform-tools (`adb` on your PATH), USB debugging on the device (or Wireless debugging: pair once with `python adbwifi.py pair`).
- The built `bbbot.exe` and `helper/bbhelper.dex` are in the repo. To rebuild: MinGW-w64 g++ (`winget install BrechtSanders.WinLibs.POSIX.UCRT`) and `.\build.ps1`; the helper needs a JDK and the Android SDK (`.\helper\build-helper.ps1`).
- The screen coordinates are calibrated for a **2880×1800 landscape** display (constants in `src/vision.hpp`). Other resolutions need recalibration. A visible navigation bar is handled automatically.

## Usage

```bash
bbbot.exe                 # dry run: read the screen and print the planned move
bbbot.exe --play          # play (Ctrl+C to stop)
```

Run it in the background (output in `bot.log`, errors in `bot-error.log`):

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File .\start-bot.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\stop-bot.ps1
```

Other versions live in `versions/<name>/` and run with `.\start-bot.ps1 -Version <name>` (Python ones need `python -m pip install -r requirements.txt`).

Options: `--swipe-ms` (drag duration, default 80), `--gap-ms` (pause between drags, default 40), `--pickup-y` (where a lifted piece appears). `BB_DEBUG=1` saves the screen to `debug-frames/` when a check times out.

## Testing the planner offline

```bash
build\sim.exe 96 1500 7000         # 96 simulated games up to 1500 moves (after .\build.ps1)
build\sim.exe 96 1500 7000 v51     # the same games with v5.1's settings, for comparison
python bench.py 24 300             # every version from git history; writes benchmark.jpg
```

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).
