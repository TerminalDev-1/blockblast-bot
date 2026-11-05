"""Block Blast move planner on a 64-bit bitboard.

Bit (y*8 + x) is board cell (x, y). The planner searches every order and
position for the pieces in the tray, scores the resulting boards, and looks
ahead by sampling likely future trays to prefer boards that stay playable.

Combos: the game keeps a combo streak alive while at least one line is cleared
within every COMBO_WINDOW placements. `since` counts placements since the last
clear; the planner rewards clears that extend a streak, penalises breaking it,
and prefers ending a tray with lines ready to clear when the streak is at risk.
"""
import heapq
import math
import random
from functools import lru_cache

FULL = (1 << 64) - 1
ROWS = [0xFF << (8 * y) for y in range(8)]
COLS = [sum(1 << (y * 8 + x) for y in range(8)) for x in range(8)]
LINES = ROWS + COLS
COL0, COL7, ROW0, ROW7 = COLS[0], COLS[7], ROWS[0], ROWS[7]


def normalize(cells):
    mx = min(x for x, y in cells); my = min(y for x, y in cells)
    return tuple(sorted(((x - mx, y - my) for x, y in cells), key=lambda c: (c[1], c[0])))


def rotations(cells):
    out = set(); cur = normalize(cells)
    for _ in range(4):
        out.add(cur); cur = normalize([(-y, x) for x, y in cur])
    return out


@lru_cache(maxsize=None)
def placements(cells):
    """All (x, y, mask) positions where a shape fits inside the 8x8 board."""
    w = max(x for x, y in cells) + 1; h = max(y for x, y in cells) + 1
    return tuple((x, y, sum(1 << ((y + cy) * 8 + x + cx) for cx, cy in cells))
                 for y in range(9 - h) for x in range(9 - w))


def drop(board, mask):
    """Place a mask (caller checks it is free) and clear full lines."""
    nb = board | mask
    # Full rows leave a bit in column 0, full columns a bit in row 0.
    r = nb & (nb >> 1); r &= r >> 2; r &= r >> 4; r &= COL0
    c = nb & (nb >> 8); c &= c >> 16; c &= c >> 32; c &= ROW0
    if not (r | c): return nb, 0
    cleared = r * 0xFF | c * 0x0101010101010101  # spread each bit over its line
    return nb & ~cleared, r.bit_count() + c.bit_count()


# Piece families seen in this game, weighted roughly by how often they appeared
# in the bot's logs. Every rotation of a family shares its weight.
_FAMILIES = [
    ([(0, 0), (1, 0)], 28), ([(0, 0), (1, 0), (2, 0)], 6), ([(0, 0), (1, 0), (2, 0), (3, 0)], 27),
    ([(0, 0), (1, 0), (2, 0), (3, 0), (4, 0)], 8), ([(0, 0)], 3),
    ([(0, 0), (1, 0), (0, 1), (1, 1)], 11), ([(x, y) for x in range(3) for y in range(3)], 8),
    ([(x, y) for x in range(3) for y in range(2)], 11),
    ([(1, 0), (2, 0), (0, 1), (1, 1)], 6), ([(0, 0), (1, 0), (1, 1), (2, 1)], 6),
    ([(0, 0), (1, 0), (2, 0), (1, 1)], 16),
    ([(0, 0), (1, 0), (2, 0), (2, 1)], 6), ([(0, 0), (1, 0), (2, 0), (0, 1)], 6),
    ([(0, 0), (1, 0), (2, 0), (2, 1), (2, 2)], 4), ([(1, 0), (0, 1), (1, 1)], 3),
    ([(0, 0), (1, 1)], 2), ([(0, 0), (1, 1), (2, 2)], 2),
]
SHAPES = []; WEIGHTS = []
for _cells, _w in _FAMILIES:
    _rots = sorted(rotations(_cells))
    for _r in _rots:
        SHAPES.append(_r); WEIGHTS.append(_w / len(_rots))
_total = sum(WEIGHTS); PROBS = [w / _total for w in WEIGHTS]
SHAPE_MASKS = [[m for _, _, m in placements(s)] for s in SHAPES]
SQUARE3 = [m for _, _, m in placements(normalize([(x, y) for x in range(3) for y in range(3)]))]
LINE5 = ([m for _, _, m in placements(normalize([(i, 0) for i in range(5)]))] +
         [m for _, _, m in placements(normalize([(0, i) for i in range(5)]))])


X_LE5 = sum(COLS[:6]); X_LE3 = sum(COLS[:4])  # cells where a 3-wide / 5-wide run can start


@lru_cache(maxsize=500000)
def fast_eval(b):
    """Cheap board score: open space, few single holes, smooth edges."""
    e = ~b & FULL
    left = ((b << 1) & ~COL0 & FULL) | COL0
    right = ((b >> 1) & ~COL7) | COL7
    up = ((b << 8) & FULL) | ROW0
    down = (b >> 8) | ROW7
    holes = (e & left & right & up & down).bit_count()
    pockets = (e & ((left & right & (up | down)) | (up & down & (left | right)))).bit_count() - holes
    trans = ((b ^ (b >> 1)) & ~COL7).bit_count() + ((b ^ (b >> 8)) & ~ROW7).bit_count()
    score = e.bit_count() * 1.0 - holes * 6.0 - pockets * 1.5 - trans * 0.45
    # Room for a 3x3 and for a 1x5, all positions at once: a bit survives the ANDs
    # only if every cell of the shape starting there is empty.
    h3 = e & (e >> 1) & (e >> 2) & X_LE5
    if h3 & (h3 >> 8) & (h3 >> 16): score += 7
    if (e & (e >> 1) & (e >> 2) & (e >> 3) & (e >> 4) & X_LE3) or (e & (e >> 8) & (e >> 16) & (e >> 24) & (e >> 32)):
        score += 4
    return score


@lru_cache(maxsize=200000)
def fit_eval(b):
    """Fast score plus how likely a random next piece has somewhere to go."""
    risk = 0.0; room = 0.0
    for p, masks in zip(PROBS, SHAPE_MASKS):
        n = 0
        for m in masks:
            if not b & m:
                n += 1
                if n >= 12: break
        if n == 0: risk += p
        room += p * math.log1p(n)
    return fast_eval(b) - 120.0 * risk + 6.0 * room


def clear_reward(lines):
    # Multi-line clears score far more in the game; keep it below survival terms.
    return 3.0 * lines * lines


COMBO_WINDOW = 3     # placements allowed without a clear before the streak breaks
COMBO_BONUS = 6.0    # extra reward for a clear that keeps the streak going
COMBO_BREAK = 10.0   # penalty when a placement breaks the streak
COMBO_READY = 9.0    # end-of-tray bonus for lines a likely next piece can clear


def step_combo(since, lines):
    """(reward, new since) for one placement. since >= COMBO_WINDOW means no streak."""
    if lines:
        return (COMBO_BONUS if since < COMBO_WINDOW else 0.0), 0
    since += 1
    return (-COMBO_BREAK if since == COMBO_WINDOW else 0.0), since


@lru_cache(maxsize=200000)
def clear_chance(b):
    """Probability that a random next piece can complete at least one line."""
    total = 0.0
    for p, masks in zip(PROBS, SHAPE_MASKS):
        for m in masks:
            if b & m: continue
            if drop(b, m)[1]:
                total += p; break
    return total


def combo_eval(b, since):
    """End-of-tray combo term: the fewer placements left before the streak breaks,
    the more a board that can clear soon is worth."""
    if since >= COMBO_WINDOW: return 0.0
    return COMBO_READY * (since + 1) / COMBO_WINDOW * clear_chance(b)


def _expand(states, width, evaluate, combo=False):
    """One placement step for every state; keep the best `width` distinct ones.
    States are (reward, board, moves, remaining pieces, since)."""
    best = {}
    for reward, b, moves, rem, since in states:
        for i, (slot, cells) in enumerate(rem):
            rest = rem[:i] + rem[i + 1:]
            for x, y, m in placements(cells):
                if b & m: continue
                nb, n = drop(b, m)
                r = reward + clear_reward(n); s2 = since
                if combo:
                    bonus, s2 = step_combo(since, n); r += bonus
                key = (nb, tuple(s for s, _ in rest), min(s2, COMBO_WINDOW))
                old = best.get(key)
                if old is None or old[0] < r:
                    best[key] = (r, nb, moves + [(slot, x, y, nb)], rest, s2)
    # Same as sorted(...)[:width], without sorting the states that get dropped.
    return heapq.nlargest(width, best.values(), key=lambda s: s[0] + evaluate(s[1]))


def _survival(b, triples):
    """Average outcome of quickly playing sampled future trays from board b."""
    total = 0.0
    for tri in triples:
        states = [(0.0, b, [], tri, COMBO_WINDOW)]
        for _ in range(3):
            nxt = _expand(states, 4, fast_eval)
            if not nxt: break
            states = nxt
        if states[0][3]:
            total -= 150.0 * len(states[0][3])  # pieces left unplaced: likely game over
        else:
            total += max(s[0] + fast_eval(s[1]) for s in states)
    return total / len(triples)


_POOL = None


def use_workers(n):
    """Run the look-ahead for the leading candidates in n worker processes
    (call once, from the main program; 0 turns it off)."""
    global _POOL
    from concurrent.futures import ProcessPoolExecutor
    if _POOL: _POOL.shutdown(cancel_futures=True)
    _POOL = ProcessPoolExecutor(n) if n else None
    if _POOL: list(_POOL.map(abs, range(n)))  # start the workers now


def _survival_all(boards, triples):
    if _POOL is None: return [_survival(b, triples) for b in boards]
    try: return list(_POOL.map(_survival, boards, [triples] * len(boards)))
    except Exception: return [_survival(b, triples) for b in boards]  # a broken pool must not stop play


def plan(board, pieces, avoid=frozenset(), width=300, lookahead=16, seed=0, min_filled=12, candidates=10,
         since=None):
    """Return the best full sequence [(slot, x, y, board_after), ...].

    pieces: list of (slot, cells). avoid: first moves (slot, x, y) to skip.
    since: placements since the last line clear (None = ignore combos).
    Places as many pieces as possible; returns [] if none fit.
    """
    pieces = [(slot, tuple(cells)) for slot, cells in pieces]
    combo = since is not None
    states = [(0.0, board, [], pieces, since if combo else COMBO_WINDOW)]; finished = []
    for depth in range(len(pieces)):
        nxt = _expand(states, width, fast_eval, combo)
        if depth == 0 and avoid:
            nxt = [s for s in nxt if (s[2][0][0], s[2][0][1], s[2][0][2]) not in avoid]
        if not nxt: break
        states = nxt; finished = nxt
    if not finished: return []
    most = max(len(s[2]) for s in finished)
    finished = [s for s in finished if len(s[2]) == most]
    ranked = sorted(finished, key=lambda s: s[0] + fit_eval(s[1]), reverse=True)
    if combo:
        # Only the leaders get the (slower) combo term; the rest can't catch up.
        ranked = sorted(ranked[:40], key=lambda s: s[0] + fit_eval(s[1]) + combo_eval(s[1], s[4]), reverse=True)
    top = ranked[:max(8, candidates)]
    filled = top[0][1].bit_count()
    if lookahead and len(top) > 1 and filled >= min_filled:
        rng = random.Random(seed ^ board)
        triples = [[(i, SHAPES[k]) for i, k in enumerate(rng.choices(range(len(SHAPES)), PROBS, k=3))]
                   for _ in range(lookahead)]
        top = top[:candidates]
        future = dict(zip(range(len(top)), _survival_all([s[1] for s in top], triples)))
        order = sorted(range(len(top)), key=lambda i: top[i][0] + 0.5 * fit_eval(top[i][1]) + future[i]
                       + (combo_eval(top[i][1], top[i][4]) if combo else 0.0), reverse=True)
        top = [top[i] for i in order]
    return [tuple(m) for m in top[0][2]]
