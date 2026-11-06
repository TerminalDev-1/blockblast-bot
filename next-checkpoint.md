# Next checkpoints (hub)

Queue of checkpoint ideas from the user. New ideas go here while another checkpoint is being
built or tested. When a checkpoint is finished, Claude asks the user which queued one to work
on next (the user picks; nothing here starts on its own). See the rules in `CLAUDE.md`.

## In progress

(nothing: ask the user which queued checkpoint is next)

## Done

### v5.6-hyperspeed (released)
- Was v5.6-hyperspeed-exp; released by the user, experimental label dropped. 156 moves/min live
  (target was 170; released at 156 by the user's decision).

## Queued

### v5.7-speedbreaker
- Name chosen by the user. Start after v5.6-hyperspeed-exp's testing is done (and the user picks it).
- Goal: **the pinnacle of speed: 175–180 moves/min**, breaking the ceiling of the v5.5/v5.6 loop
  (~160–170). The fastest checkpoint so far.
- **A much lighter, more efficient architecture.** Ideas to explore:
  - plan while the next tray's deal animation is still finishing (the deal, ~0.5 s per tray, is
    the one cost the game imposes);
  - overlap each drag with the previous drop's check (a second helper connection);
  - a leaner main loop: fewer screenshots per tray, nothing that waits without a reason.
- **The most intelligent checkpoint yet:** excel at long-horizon survival (beat v5.5.1-exp's
  simulator results: 54 of 128 games lasting 3000 moves) while keeping breakneck speed. Use the
  spare planning time (planning is ~3 ms of a ~1 s tray) for a deeper search.
- Provisional until the user approves it (one yes to release, as its name has no `-exp`).

### v5.8-mach1
- Name chosen by the user ("v5.8-Mach 1"). After v5.7-speedbreaker (when the user picks it).
- Goal: **run at the speed ceiling: 210 moves/min** (the user said "210 moves per second"; per
  minute is the ceiling estimated for this game: the deal animation alone takes ~0.5 s per tray,
  so a tray of 3 moves can't take less than ~0.85 s). Confirm the unit with the user when starting.
- Builds on whatever v5.7-speedbreaker achieves; every part of a tray except the game's own deal
  animation has to be close to zero (drags, checks, reading, planning all overlapped).
- Provisional until the user approves it (one yes to release, as its name has no `-exp`).
