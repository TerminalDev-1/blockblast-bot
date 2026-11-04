# Block Blast bot

Plays Block Blast (Classic) on a USB-connected Android tablet (2880x1800 landscape) via ADB: screenshot → read board/tray → plan → swipe → verify.

## Files
- `bot.py` — screen reading, recovery from menus/ads/game-over, drag aiming, main loop. `python bot.py` = dry run (reads the screen, no input); `--play` plays.
- `planner.py` — bitboard planner (bit `y*8+x` = cell). Searches every order/position of the tray, scores boards (`fast_eval`, `fit_eval`), and samples future trays (`_survival`) when the board fills up.
- `sim.py` — offline benchmark: `python sim.py [games] [max_moves]` (`--light` older lighter look-ahead, `--old` the legacy `bot.choose`). Use it to check planner changes before going live.
- `start-bot.ps1` / `stop-bot.ps1` — run detached; output in `bot.log`, errors in `bot-error.log`, PID in `bot.pid`.

## Calibration facts (measured from screen recordings)
- Board origin `GX,GY=994,321`, cell `STEP=112` px. Tray slots `TRAYS`, tray cells are 50 px.
- A held piece moves `GAIN≈1.47`x the finger distance on both axes. On pickup its centre jumps to y≈`PICKUP_Y=1232`; horizontally it stays at the tray-piece centre. Aim formulas in `main()` depend on this.
- The colour theme changes during a run (dark, pink, mint, white, wood, ice…), and every fixed visual rule broke on some theme (brightness, texture, the blocks' dark bottom "lip"). So `read_board` *learns* the empty-cell colour: empty cells share one colour; everything else is a block. After a move it re-learns from cells known to be empty (`hint_empty`), which survives theme switches; at startup it picks the largest colour group, preferring one without a lip (`cell_stats`). `is_board` only checks that cell centres are flat (menus/pop-ups aren't) and that a colour group exists.
- Tray pieces: background and noise are sampled from the strip just above the tray (y 1290–1325); the threshold adapts (pink-on-pink pieces differ by only ~34), and other thresholds are tried, accepting only shapes that fit the 50 px tray grid. A slot unreadable 20 times in a row saves the screen to `unknown-screens/`.
- If "no legal moves" lasts 30 s without a result screen, the screen is misread: it is saved and normal recovery runs. Any exception is logged and the loop restarts after `adb wait-for-device`.
- New tray pieces animate in; the bot only plans from two consecutive screenshots that agree.
- Fast capture: `screencap | gzip -1` raw (≈0.35 s) with PNG fallback (≈0.75 s). screencap occasionally hangs on the tablet; each attempt has a 4–6 s timeout (the process is killed) and is retried, so a hang costs seconds, not the old 27 s freeze.

## Connection
- `adbwifi.py` picks USB if plugged in, else wireless debugging (one-time `python adbwifi.py pair`). The bot waits for the tablet to come back after errors (e.g. flat battery). Screenshot timeouts are 3x longer over Wi-Fi (`SLOW`). Shared copy of `../new game bot/adbwifi.py`; keep in sync.

## Working on it
- The game is often live: don't stop a running bot unless the user says so. Code changes only take effect after a restart.
- Run live tests detached (`start-bot.ps1`); a foreground run dies when the tool call is interrupted.
- Unrecognised screens are saved to `unknown-screens/` for calibration.
- Public repo (GPL v3). Never commit screenshots (`*.png` is ignored; they can show personal info), logs, or `unknown-screens/`.
- Commit only the files you changed; don't push unless asked.
