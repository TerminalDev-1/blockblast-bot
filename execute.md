# Plan: 8x8 Agent, our own Android block-puzzle game, played by an agent

Status: **planning** (started 2026-11-07). Nothing built yet. This file is the fail-safe: if a chat ends
mid-planning, read this and continue from "Open questions".

It lives in the Block Blast bot repo for now; it moves to the game's own repo once that exists.

## Why
- The user is tired of Block Blast (not abandoning it: the tablet bot stays and keeps its releases).
- On Block Blast the agent is capped by things we don't control: screenshots, reading pixels, the game's
  ~0.6 s deal animation (~1.0 s per tray over Wi-Fi, 174 moves/min max so far).
- In our own game the agent is the star: it gets the board as data from the game, and the only speed
  limit is the one we choose.

## Decided
- **Name: 8x8 Agent** (for now, chosen by the user).
- **Folder: `Desktop\fun\8x8 agent\`**, next to `blockblast bot/`. Its own repo.
- **Open source**, public repo (GPL v3 suggested, same as the Block Blast bot). Others may fork and
  change their own copies; that's fine. What the user protects is **their own copy**: nobody can change
  the agent from inside the app.
- **Android app, written in Kotlin.** (The user is an Android person: no web or desktop versions.)
- **Our own game, not a copy of Block Blast.** Same genre (8x8 board, trays of 3 pieces, clear full
  rows/columns, combos, high scores), but its own name, look, sounds and feel.
- **Agent only: a person can't play at all** (rule set by the user). No touch controls for pieces; the
  app only shows the agent playing.
- **Combos carry over across games** (user's feature): the combo streak is not reset at game over or
  when a new game starts; it continues into the next game.
- **Animations and high scores** like a real game: line-clear effects, combo pop-ups, score that counts
  up, high-score table.
- **Nothing about the agent is configurable in the app** (rule set by the user): not the planner, not its
  speed. No settings screen, no speed dial, and nothing in the app can modify the planner's state.
- **Agent speed is fixed by the user** (moves/min, a comfortable ceiling where animations still look
  good), set in the agent's code. Changing it is a code change by the user, not an app setting.

## The agent lives outside the app (user's decision)
- The game app and the agent are **separate programs**. The game only runs the game: board, pieces,
  animations, scores. The agent connects to it, receives the board and tray as data, and sends back
  moves. The game checks every move against its own rules, so the agent can't break the game.
- **Why (the user's reason):** changing the agent (planner, weights, speed) must not mean shipping a new
  app version every time. The app is shipped rarely; the agent is updated on its own side.
- It is still core gameplay: with no agent connected, the game waits ("waiting for agent") and nothing
  is played.
- Where the agent runs (to decide, see open questions):
  - **On the PC** (suggested): the existing C++ planner, connected over USB or Wi-Fi through ADB
    (`adb forward`, like the Block Blast helper). A move is a few bytes, not a screenshot, so Wi-Fi is
    milliseconds per move, not the old ~1 s per tray. Updating the agent = rebuild on the PC, no app
    update at all.
  - **On the tablet as a separate small agent app**: no PC needed, but updating the agent still means
    reinstalling that agent app.
- **Protection:** the game only accepts the agent on a local socket that is reachable through ADB (not
  open on the network), so other apps or devices can't take over the user's game.
- Built-in agent (planner inside the app) is a possible **future idea, sidelined** by the user.

## The planner carries over (none of the survival work is wasted)
- The bot's planner (`src/planner.*`, C++) is pure logic: board + tray in, moves out. It has nothing to
  do with ADB or screenshots, so the external agent reuses it unchanged: same look-ahead, same combo
  awareness, same `EvalWeights` (empty 0.6, hole 9, trans 0.675).
- Tuning still works on the PC: `build\sim.exe` and `bench.py` keep benchmarking planner changes.
- To keep the tuning valid, the game uses the same piece list and dealing rules the simulator uses
  (v5.4's piece list). Changing those later means re-tuning in the simulator.
- Combo carry-over across games: the simulator/planner should model it the same way (combo state isn't
  reset by a new game).

## Rust (v6.0 idea, still in `next-checkpoint.md`)
- Rust isn't faster than C++; both are native. Kotlin was chosen for the game. The v6.0 Rust-edition
  idea for the tablet bot stays in the hub as a plan.

## Open questions
1. Where the agent runs: on the PC (suggested) or as a separate agent app on the tablet?
2. Build it in this chat or a new session? (Always ask the user; never start a session on your own.)
3. License: GPL v3 (suggested).
4. Visual style / theme of the game.
5. The agent's fixed speed (moves/min), set by the user.
6. Combo carry-over details: does the score multiplier carry over too, or only the streak count?

## Rough build order (once planning is done)
1. Game core in Kotlin: board, pieces, dealing, clears, scoring, combos (carried across games), game over.
2. Screen + animations + high scores; "waiting for agent" screen.
3. Agent connection: local socket in the app, ADB forward, simple message format (board/tray out, moves in).
4. External agent: the C++ planner behind that connection, at the user's fixed speed.
5. Live test on the tablet: moves/min and scores.
