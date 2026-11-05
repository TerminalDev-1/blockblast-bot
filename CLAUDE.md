# Block Blast bot

Plays Block Blast (Classic) on a USB-connected Android tablet (2880x1800 landscape) via ADB: screenshot → read board/tray → plan → swipe → verify.

## Files
- `bot.py` — screen reading, recovery from menus/ads/game-over, drag aiming, main loop. `python bot.py` = dry run (reads the screen, no input); `--play` plays.
- `planner.py` — bitboard planner (bit `y*8+x` = cell). Searches every order/position of the tray, scores boards (`fast_eval`, `fit_eval`), and samples future trays (`_survival`) when the board fills up.
- `bench.py` — benchmarks every planner version straight from git history on the same simulated games and draws `benchmark.jpg` (in the README). `python bench.py 24 300 --procs=5` (~15 min; keep procs low while the bot is live).
- `sim.py` — offline benchmark: `python sim.py [games] [max_moves]` (`--light` older lighter look-ahead, `--old` the legacy `bot.choose`). Use it to check planner changes before going live.
- `start-bot.ps1` / `stop-bot.ps1` — run detached; output in `bot.log`, errors in `bot-error.log`, PID in `bot.pid`.

## Calibration facts (measured from screen recordings)
- Board origin `GX,GY=994,321`, cell `STEP=112` px. Tray slots `TRAYS`, tray cells are 50 px.
- A held piece moves `GAIN≈1.47`x the finger distance on both axes. On pickup its centre jumps to y≈`PICKUP_Y=1232`; horizontally it stays at the tray-piece centre. Aim formulas in `main()` depend on this.
- The colour theme changes during a run (dark, pink, mint, white, wood, ice…), and every fixed visual rule broke on some theme (brightness, texture, the blocks' dark bottom "lip"). So `read_board` *learns* the empty-cell colour: empty cells share one colour; everything else is a block. After a move it re-learns from cells known to be empty (`hint_empty`), which survives theme switches; at startup it picks the largest colour group, preferring one without a lip (`cell_stats`). `is_board` only checks that cell centres are flat (menus/pop-ups aren't) and that a colour group exists.
- Tray pieces: background and noise are sampled from the strip just above the tray (y 1290–1325); the threshold adapts (pink-on-pink pieces differ by only ~34), and other thresholds are tried, accepting only shapes that fit the 50 px tray grid. A slot unreadable 20 times in a row saves the screen to `unknown-screens/`.
- If "no legal moves" lasts 30 s without a result screen, the screen is misread: it is saved and normal recovery runs. Any exception is logged and the loop restarts after `adb wait-for-device`.
- New tray pieces animate in; the bot only plans from two consecutive screenshots that agree.
- All coordinates assume the game fills the full 1800 px height. With the tablet's 3-button navigation bar on, the app gets 1682 px and the game shrinks around x=1440; `update_layout` reads `mAppBounds` from `dumpsys window displays` (at startup and on recovery), `capture` scales screenshots back, and `tap`/`drag_piece` scale input with `to_screen`.
- Speed (v4.1, ~67 moves/min): swipes go through one long-lived `adb shell` (`SHELL`), so they cost ~1 ms of overhead instead of ~50 ms. Combo pop-up text covers some cells; `visible_board`/`shows` compare only the flat (visible) cells, so these frames aren't treated as off-board (`e` in the trace = matched with some cells hidden). There are no fixed sleeps on the normal path; a screenshot (~0.4 s) is the only wait. Swipes are 150 ms (`--swipe-ms`). A full new tray at rest (`at_rest`, `REST_Y`/`REST_DX`) on the predicted board is planned from one screenshot. Two screenshots can't overlap: the tablet takes them one at a time (tried, slower). Tray readings that aren't a real piece shape (`KNOWN_SHAPES`, e.g. wood grain adding cells) are retried at other thresholds. The user chose this over 200 ms knowingly: 150 ms sometimes misses a drop (retried, then recalibrated), 200 ms didn't, but 150 is noticeably faster. Don't change it back without asking.
- Planner speed: `fast_eval`/`drop` use bit operations (no loops over positions); the look-ahead (`_survival`) runs in worker processes (`planner.use_workers`, started in `bot.py` `__main__` only with `--play`). ~0.08 s per tray with 32 future trays x 14 candidates. Windows needs the `__main__` guard for workers.
- Missed drops: two unchanged screenshots = unplaced (not a 3 s timeout). Retry order: same target at `RETRY_MS`, then one-cell shifts. A fast miss sets `slow_from`: drags at least that long use `SAFE_MS`.
- Layout (`update_layout`): Android's `mAppBounds` can be wrong, and menus/result screens show the nav bar while the board is often full screen, so the size can change between screens. Candidate scales are tried on a real screenshot; a board whose tray reads cleanly wins, ties keep the current scale. Rerun on recovery and after 3 unreadable trays (then every 10).
- Texture (`cell_stats`) is measured on the middle 32 px of each cell: some themes bevel their blocks, which a wider patch counts as texture. `is_board` rejects more than 6 textured cells (a saved full-screen ad has 8, menus 13+, boards 0-2).
- `stop-bot.ps1` ends the whole process tree (the planner's worker processes too; before, they piled up and locked `bot.log`). `start-bot.ps1` keeps the previous run's log as `bot.prev.log` — check it when a game ended unexpectedly.
- Pieces of one tray are swiped back to back (`carry`); only the tray's last piece is checked with `settle`. A miss earlier in the tray shows up there and triggers a re-read and re-plan.
- Combos: `since` counts placements since the last clear and is passed to `planner.plan`; the streak breaks after 3 placements without a clear (`COMBO_WINDOW`). Combo pop-up text briefly hides the board (`-` in the trace).
- Line clears animate for ~1.5 s. `settle` accepts a move once everything outside the clearing lines matches (`early`), and the main loop plays on from that expected board (`trusted`), ignoring recently cleared cells (`clears`) in the next checks. Each move logs `swipe Xs, check Ys` plus a trace of the reads (E expected, e expected with some cells hidden, B unchanged, o other, - off-board).
- Fast capture: `screencap | gzip -1` raw (≈0.35 s) with PNG fallback (≈0.75 s). screencap occasionally hangs on the tablet; each attempt has a 4–6 s timeout (the process is killed) and is retried, so a hang costs seconds, not the old 27 s freeze.

## Connection
- `adbwifi.py` picks USB if plugged in, else wireless debugging (one-time `python adbwifi.py pair`). The bot waits for the tablet to come back after errors (e.g. flat battery). Screenshot timeouts are 3x longer over Wi-Fi (`SLOW`). Shared copy of `../arrows bot/adbwifi.py`; keep in sync.

## Working on it
- **Every new version must update the benchmark**: add it to `VERSIONS` in `bench.py` (label + commit; the working tree is `None` until committed, then pin the commit), put its measured live moves/min (from `bot.log`, 100+ moves) in `LIVE_SPEED` (a speed-only version with the same planner goes only in `LIVE_SPEED`), rerun `python bench.py`, add a row to the README's version table, and commit `benchmark.jpg` and `bench-results.json` with the change.
- The game is often live: don't stop a running bot unless the user says so. Code changes only take effect after a restart.
- Run live tests detached (`start-bot.ps1`); a foreground run dies when the tool call is interrupted.
- Unrecognised screens are saved to `unknown-screens/` for calibration.
- Public repo (GPL v3). Never commit screenshots (`*.png` is ignored; they can show personal info), logs, or `unknown-screens/`.
- Commit only the files you changed, and always push after committing ("commit" means commit and push).
