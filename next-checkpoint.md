# Next checkpoints (hub)

Queue of checkpoint ideas from the user. New ideas go here while another checkpoint is being
built or tested. Queued checkpoints are done **in version order, lowest number first**
(v5.7 before v5.8); Claude asks the user before starting the next one. See the rules in `CLAUDE.md`.

## In progress

### v5.9-trained-exp
- Name chosen by the user (2026-11-07). Based on v5.8-mach1. Built in `versions/v5.9-trained-exp/`.
- Idea (user): back to Block Blast after the 8x8 Agent side project, and **train** the bot with **machine
  learning**: "the most efficient architecture possible in the least time possible".
- Plan: self-play in the simulator with the v5.8 planner records boards and whether the game died soon
  after; a small neural network (plain C++, no new dependencies) learns to rate a board's danger; the
  planner uses it in place of (or next to) the slow Monte Carlo look-ahead; a few self-play rounds
  retrain it on its own games.
- Targets (proposed, to confirm with the user): longer survival than v5.8 in the simulator (64 fresh
  games up to 3000 moves) with less planning time per tray, and live moves/min at least v5.8's 174.
- Results so far (simulator, 64 games up to 3000 moves, seed 50000; v5.8: 2303 moves avg, 35 capped,
  48 ms/tray):
  - Danger net (lost within 10 trays, from fast-policy games): AUC 0.715 vs 0.692 for v5.8's
    `fit_eval`, but it made play worse at weights 100-300 (only weight 30 was even).
  - Distilled look-ahead net (`nnf`, trained on 4.1M look-ahead verdicts incl. 48 candidates per
    tray; correlation 0.916): alone 2057 moves / 28 capped at 3.6 ms/tray (13x faster planning);
    with the real look-ahead on its best 4: 2159 / 30 at 15 ms; best 8: 2220 / 29 at 24 ms.
  - Not yet better than v5.8. Next ideas: self-play rounds on boards the network itself reaches,
    a bigger network, more games per comparison (64 games is noisy).
  - **Live (2026-11-07, real Block Blast over Wi-Fi, network-alone mode compiled into bbbot.exe):
    174.7 moves/min over 889 moves in 305 s (176.8 after the first 100 moves), no game over or ad
    pauses; planning logs 0.00 s per tray.** v5.8-mach1: 174. Planning was never the bottleneck
    (the deal animation and Wi-Fi are), so the 13x faster planning doesn't raise live speed.
  - **Training round 2 (self-play / DAgger):** the network played 120 games itself while the
    look-ahead (96 futures) graded the boards it reached: 1.9M more verdicts, 6.1M in total; retrained
    (correlation 0.914; a 2x wider net reached 0.920 but played no better, so the small one stays).
    On 256 fresh games (two sets of 128, cap 3000): network alone 2166 moves avg (117 capped) vs
    v5.8 2190 (125 capped), at 3.6 vs ~44 ms/tray: 99% of v5.8's survival, a tie within noise
    (set seed 70000: 2197 vs 2114; set seed 90000: 2135 vs 2265). Hybrid (look-ahead on its best 8)
    was not better. This network (fnet3) is now compiled into the bot.

## Done

### v5.8-mach1 (released)
- 174 moves/min live over Wi-Fi (target was 210; ~1.0 s per tray is the Wi-Fi floor). Released by the user's
  decision, experimental label dropped.

## Plans the user mentioned (2026-11-06)
- Mainline releases continue (the user is **not** abandoning released main bots), but **less often** than so far.
- The user may also produce more **experimental checkpoints that are variations of the bot**. They can be
  based on **v5.8-mach1, v5.7-speedbreaker or v5.6-hyperspeed**; the user **recommends v5.8-mach1** as the
  default base, and the older two are there for variety (to test how a variation would have turned out on them).
  **What each experiment changes is entirely the user's call.** There are no preset kinds (not "speed",
  "planning" or "intelligence" variants) unless the user asks for one; Claude doesn't assume any.
- **Each experimental checkpoint goes in its own folder (`versions/<name>-exp/`) and carries the
  experimental label** (`-exp` in the name, "(experimental)" with date and time in the benchmark).

### v5.7-speedbreaker (released)
- 165 moves/min live over Wi-Fi with the safe tray check (target was 200; 211 with a riskier check that
  lost a game); +17% survival on long simulated games vs v5.6. Released by the user's decision.

### v5.6-hyperspeed (released)
- Was v5.6-hyperspeed-exp; released by the user, experimental label dropped. 156 moves/min live
  (target was 170; released at 156 by the user's decision).

## Queued

### v6 (exact name to confirm) — fully transformer network, from scratch
- The user's idea (2026-11-07, while v5.9-trained-exp was being built): a second machine-learning
  experiment that starts from **nothing**: no hand-made board measurements and no v5.8 planner to
  learn from. The network learns to play purely by self-play in the simulator (reinforcement
  learning), and the experiment also designs **its own training architecture**.
- Goals (user): an **efficient architecture** (small, fast network) and **little training time**, for
  **better results** than the hand-tuned planner.
- Ideas: a small convolutional or bitboard network that sees only the 64 cells, the three pieces and
  the combo state; trained AlphaZero/TD-style with the simulator as the environment; the bit-trick
  move generator and multi-threaded self-play from v5.9 keep training fast.
- **Architecture (user, 2026-11-07): fully transformer**, efficient to train, and it must **at least beat
  v5.9** (v5.9-trained-exp's results). E.g. the 64 cells (plus the three pieces and the combo state) as
  tokens into a small transformer encoder.
- Plays the real Block Blast game on the tablet like every version (the 8x8 Agent client is not used).
- Ask the user for the exact version name before starting (after v5.9-trained-exp is finished; the
  v6.0-rust-edition-exp idea is also still in the hub).

## Planned (not started; the user is still planning)

### v6.0-rust-edition-exp (name to confirm when starting)
- The user's idea (2026-11-07): a **Rust edition** of the bot, a new major version.
- Goals: **higher scores** (beat the best score of 809,192; the planner so far is tuned for survival,
  not points) and **more moves/min over Wi-Fi** than v5.8-mach1 (174).
- Note from planning: Rust and C++ run at about the same speed (both compile to native code), and
  planning is only ~2-5 ms of a ~1.0 s tray. The Wi-Fi gain has to come from the loop's design
  (e.g. dragging pieces while the tray is still dealing, fewer frames per tray), which the rewrite is a
  chance to redo. Switching to Rust changes the language rule in `CLAUDE.md` (C++ primary since v5.5)
  only if the user releases it; the tablet helper stays Java.
- Base: v5.8-mach1 (recommended default).
