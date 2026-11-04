# Block Blast bot

Plays Block Blast (Classic) on a USB-connected Android tablet (2880x1800 landscape) via ADB: screenshot → read board/tray → plan → swipe → verify.

## Files
- `bot.py` — screen reading, recovery from menus/ads/game-over, drag aiming, main loop. `python bot.py` = dry run (reads the screen, no input); `--play` plays.
- `planner.py` — bitboard planner (bit `y*8+x` = cell). Searches every order/position of the tray, scores boards (`fast_eval`, `fit_eval`), and samples future trays (`_survival`) when the board fills up.
- `sim.py` — offline benchmark: `python sim.py [games] [max_moves]` (`--old` runs the legacy `bot.choose`). Use it to check planner changes before going live.
- `start-bot.ps1` / `stop-bot.ps1` — run detached; output in `bot.log`, errors in `bot-error.log`, PID in `bot.pid`.
- `bot_next.py` — an older experiment; not used.

## Calibration facts (measured from screen recordings)
- Board origin `GX,GY=994,321`, cell `STEP=112` px. Tray slots `TRAYS`, tray cells are 50 px.
- A held piece moves `GAIN≈1.47`x the finger distance on both axes. On pickup its centre jumps to y≈`PICKUP_Y=1232`; horizontally it stays at the tray-piece centre. Aim formulas in `main()` depend on this.
- Cells are classified by texture (blocks are bevelled, empty cells are flat), not colour, because the game changes colour theme as the score rises (dark pink, mint, ...). `is_board` requires every cell to be clearly flat or textured.
- New tray pieces animate in; the bot only plans from two consecutive screenshots that agree.
- Fast capture: `screencap | gzip -1` raw (≈0.35 s) with PNG fallback (≈0.75 s).

## Working on it
- The game is often live: don't stop a running bot unless the user says so. Code changes only take effect after a restart.
- Run live tests detached (`start-bot.ps1`); a foreground run dies when the tool call is interrupted.
- Unrecognised screens are saved to `unknown-screens/` for calibration.
- Commit only the files you changed; don't push unless asked.
