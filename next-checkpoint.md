# Next checkpoints (hub)

Queue of checkpoint ideas from the user. New ideas go here while another checkpoint is being
built or tested. Queued checkpoints are done **in version order, lowest number first**
(v5.7 before v5.8); Claude asks the user before starting the next one. See the rules in `CLAUDE.md`.

## In progress

(nothing in progress)

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

(nothing queued)

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
