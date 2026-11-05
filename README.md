# Block Blast bot

A Python bot that plays the mobile puzzle game **Block Blast** (Classic mode) on an Android device over USB. It reads the board and tray from screenshots, plans moves with a bitboard search, and drags pieces with ADB. It plays continuously: it chases the best score, keeps going after it beats it, and starts a new game when it loses.

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

## How it works

1. **See** – `adb exec-out screencap` (raw pixels gzipped on the device, ~0.35 s). The game changes colour theme as the score rises (dark, pink, mint, white, wood, ice…), so instead of fixed colours the bot learns what an empty cell looks like: empty cells share one colour, and after each move it re-learns that colour from cells it knows are empty.
2. **Plan** – `planner.py` searches every order and position of the tray pieces on a 64-bit bitboard. Boards are scored for open space, holes, ragged edges, and room for big pieces (3×3, 1×5). When the board gets crowded it also simulates likely future trays and prefers boards that keep surviving. It also tracks the combo streak (a line cleared within every 3 placements): clears that extend the streak score extra, breaking it costs, and when the streak is at risk it prefers boards where the next piece can clear a line.
3. **Act** – one `adb shell input swipe` per piece; the pieces of a tray are swiped back to back and the board is checked once after the last one. The game moves a held piece about 1.47× the finger distance, and the aim accounts for that. Line-clear animations aren't waited for: a move counts as done once the rest of the board matches. A missed drop is retried one cell over, and small per-piece aim corrections are learned.
4. **Recover** – detects the menu, the result screen (taps play again), rating prompts and some ads. Unknown screens are saved to `unknown-screens/` for calibration.

## Setup

- Python 3.10+ and Android platform-tools (`adb` on your PATH).
- `python -m pip install -r requirements.txt`
- Enable USB debugging on the device, connect it, and open a Classic game.
- The screen coordinates are calibrated for a **2880×1800 landscape** display (constants at the top of `bot.py`). Other resolutions need recalibration. A visible navigation bar is handled automatically.

## Usage

```bash
python bot.py                 # dry run: read the screen and print the planned move
python bot.py --play          # play (Ctrl+C to stop)
python bot.py --play --max-moves 1
```

Run it in the background on Windows (output in `bot.log`, errors in `bot-error.log`):

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File .\start-bot.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\stop-bot.ps1
```

Options: `--swipe-ms` (drag duration, default 300) and `--pickup-y` (calibration of where a lifted piece appears).

## Testing the planner offline

`sim.py` plays simulated games with random trays, no device needed:

```bash
python sim.py 16 500           # 16 games, capped at 500 moves each
python sim.py 16 500 --light   # lighter, older look-ahead
python sim.py 16 500 --old     # the original simple planner, for comparison
python bench.py 24 300         # every version from git history; writes benchmark.jpg
```

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).
