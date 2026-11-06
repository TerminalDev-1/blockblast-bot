# Next checkpoints (hub)

Queue of checkpoint ideas from the user. New ideas go here while another checkpoint is being
built or tested. When a checkpoint is finished, Claude asks the user which queued one to work
on next (the user picks; nothing here starts on its own). See the rules in `CLAUDE.md`.

## In progress

### v5.6-hyperspeed-exp
- Built on v5.5.1-exp. Target set by the user: **170 moves/min** (visible gameplay, measured live).
- Status: being tested live. Drags no longer slow down for good after one miss; tighter timings
  (70 ms drags, 30 ms pause, 30 ms tray settle, 15 ms check delay); a tray's first drops go
  straight after each other and the last drop's check verifies the whole board.

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
