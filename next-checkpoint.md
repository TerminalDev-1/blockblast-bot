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
- The user may also produce more **experimental checkpoints that are variations of the bot**, likely
  **based on v5.8-mach1**: anything goes, e.g. a slower or faster play loop, a slower (deeper) or
  faster planner.
- **Each experimental checkpoint goes in its own folder (`versions/<name>-exp/`) and carries the
  experimental label** (`-exp` in the name, "(experimental)" with date and time in the benchmark).

### v5.7-speedbreaker (released)
- 165 moves/min live over Wi-Fi with the safe tray check (target was 200; 211 with a riskier check that
  lost a game); +17% survival on long simulated games vs v5.6. Released by the user's decision.

### v5.6-hyperspeed (released)
- Was v5.6-hyperspeed-exp; released by the user, experimental label dropped. 156 moves/min live
  (target was 170; released at 156 by the user's decision).

## Queued

### v5.7-speedbreaker
- Name chosen by the user. Next in order (v5.6-hyperspeed is released).
- Goal (raised by the user): **200 moves/min**, clearly faster than v5.6-hyperspeed (156), breaking the
  ceiling of the v5.5/v5.6 loop. The fastest checkpoint so far.
- **A much lighter, more efficient architecture.** Ideas to explore:
  - plan while the next tray's deal animation is still finishing (the deal, ~0.5 s per tray, is
    the one cost the game imposes);
  - overlap each drag with the previous drop's check (a second helper connection);
  - a leaner main loop: fewer screenshots per tray, nothing that waits without a reason.
- **Far more intelligent than v5.6 ("ridiculously" more, in the user's words):** excel at long-horizon survival (beat v5.5.1-exp's
  simulator results: 54 of 128 games lasting 3000 moves) while keeping breakneck speed. Use the
  spare planning time (planning is ~3 ms of a ~1 s tray) for a deeper search.
- Provisional until the user approves it (one yes to release, as its name has no `-exp`).

### v5.8-mach1-exp
- Name chosen by the user ("v5.8-Mach 1"); `-exp` added (new checkpoints are experimental by default from v5.8 on). Only after v5.7-speedbreaker is done.
- Goal: **run at the speed ceiling: 210 moves/min** (the user said "210 moves per second"; per
  minute is the ceiling estimated for this game: the deal animation alone takes ~0.5 s per tray,
  so a tray of 3 moves can't take less than ~0.85 s). Confirm the unit with the user when starting.
- Builds on whatever v5.7-speedbreaker achieves; every part of a tray except the game's own deal
  animation has to be close to zero (drags, checks, reading, planning all overlapped).
- Provisional until the user approves it (one yes to release, as its name has no `-exp`).
