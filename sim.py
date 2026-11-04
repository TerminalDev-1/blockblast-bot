"""Offline benchmark: play simulated games with the planner (no tablet needed).

Usage: python sim.py [games] [max_moves] [--old]
--old benchmarks the original planner in bot.py for comparison.
"""
import random, sys, time
import planner


def deal(rng):
    return [planner.SHAPES[k] for k in rng.choices(range(len(planner.SHAPES)), planner.PROBS, k=3)]


def play(seed, max_moves, use_old):
    rng = random.Random(seed); board = 0; moves = 0; lines = 0
    if use_old: import bot
    while moves < max_moves:
        tray = list(enumerate(deal(rng)))
        while tray:
            if use_old:
                m = bot.choose(board, [{'slot': s, 'cells': list(c)} for s, c in tray])
                if not m: return moves, lines
                slot = m[0]['slot']; nb = m[3]
                cells = dict(tray)[slot]
                mask = sum(1 << ((m[2] + y) * 8 + m[1] + x) for x, y in cells)
                lines += ((board | mask).bit_count() - nb.bit_count()) // 8 and planner.drop(board, mask)[1]
                board = nb; tray = [t for t in tray if t[0] != slot]; moves += 1
            else:
                seq = planner.plan(board, tray, seed=seed + moves)
                if not seq: return moves, lines
                for slot, x, y, nb in seq:
                    cells = dict(tray)[slot]
                    mask = sum(1 << ((y + cy) * 8 + x + cx) for cx, cy in cells)
                    lines += planner.drop(board, mask)[1]
                    board = nb; tray = [t for t in tray if t[0] != slot]; moves += 1
                if tray: return moves, lines
    return moves, lines


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    games = int(args[0]) if args else 10; cap = int(args[1]) if len(args) > 1 else 300
    old = '--old' in sys.argv; t = time.time(); results = []
    for g in range(games):
        r = play(1000 + g, cap, old); results.append(r)
        print(f'game {g}: {r[0]} moves, {r[1]} lines', flush=True)
    ms = [r[0] for r in results]
    print(f"{'old' if old else 'new'}: mean {sum(ms)/len(ms):.0f} moves, "
          f"{sum(m >= cap for m in ms)}/{games} reached cap, {(time.time()-t)/max(1,sum(ms))*1000:.0f} ms/move")
