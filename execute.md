# Plan: our own Android block-puzzle game, built for the agent

Status: **planning** (started 2026-11-07). Nothing built yet. This file is the fail-safe: if a chat ends
mid-planning, read this and continue from "Open questions".

It lives in the Block Blast bot repo for now; it moves to the game's own repo once that exists.

## Why
- The user is tired of Block Blast (not abandoning it: the tablet bot stays and keeps its releases).
- On Block Blast the agent is capped by things we don't control: ADB, Wi-Fi, screenshots, reading
  pixels, the game's ~0.6 s deal animation (~1.0 s per tray over Wi-Fi, 174 moves/min max so far).
- In our own game the agent is the star: it reads the board straight from the game, and the only speed
  limit is the one we choose.

## Decided
- **Android app, written in Kotlin.** (The user is an Android person: no web or desktop versions.)
- **Our own game, not a copy of Block Blast.** Same genre (8x8 board, trays of 3 pieces, clear full
  rows/columns, combos, high scores), but its own name, look, sounds and feel.
- **The agent lives inside the app.** No ADB, no wireless debugging, no Wi-Fi, no screenshots. It reads
  the board from the game's own state and places pieces through the game's own code.
- **Animations and high scores** like a real game: line-clear effects, combo pop-ups, score that counts
  up, high-score table.
- **Agent speed dial**: throttle the agent's moves per minute (e.g. 1x / 2x / 5x / 10x) up to a
  comfortable ceiling where animations still look good, plus an uncapped mode for benchmarks.

- **Open source**, in its own public repo (GPL v3 suggested, same as the Block Blast bot).

## The planner carries over (none of the survival work is wasted)
- The bot's planner (`src/planner.*`, C++) is pure logic: board + tray in, moves out. It has nothing to
  do with ADB or screenshots, so it moves into the app unchanged.
- How: build the C++ planner for Android with the NDK and call it from Kotlin (JNI). Same code, same
  look-ahead, same combo awareness, same `EvalWeights` (empty 0.6, hole 9, trans 0.675).
- **The planner is locked inside the app (rule set by the user).** The app never lets anyone change the
  planner: no settings screen for weights, look-ahead, danger mode or planner versions, and nothing
  else in the app can modify its internal state. Only the developers change those settings, in the source
  code, tested in the simulator, then rebuilt into a new app version. (The speed dial only sets how fast
  the agent plays; it doesn't touch the planner.)
- Tuning still works on the PC: `build\sim.exe` and `bench.py` keep benchmarking planner changes.
- To keep the tuning valid, the game uses the same piece list and dealing rules the simulator uses
  (v5.4's piece list). Changing those later means re-tuning in the simulator.

## Rust (v6.0 idea, still in `next-checkpoint.md`)
- Rust isn't faster than C++; both are native. Kotlin was chosen for the game. The v6.0 Rust-edition
  idea for the tablet bot stays in the hub as a plan.

## Open questions
1. Game name (the user's choice; Claude can suggest some).
2. Build it in this chat or a new session? (Always ask the user; never start a session on your own.)
3. Repo folder: next to `blockblast bot/` in `Desktop\fun` (suggested); license GPL v3 (suggested).
4. Visual style / theme of the game.
5. Speed dial levels and the "comfortable ceiling" number.
6. Does a human also play it (agent on/off), or is it agent-only?

## Rough build order (once planning is done)
1. Game core in Kotlin: board, pieces, dealing, clears, scoring, combos, game over.
2. Screen + animations + high scores.
3. Planner via NDK/JNI; agent plays at the dial's speed.
4. Live test on the tablet: moves/min and scores.
