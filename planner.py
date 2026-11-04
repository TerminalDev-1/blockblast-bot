"""Block Blast move planner on a 64-bit bitboard.

Bit (y*8 + x) is board cell (x, y). The planner searches every order and
position for the pieces in the tray, scores the resulting boards, and looks
ahead by sampling likely future trays to prefer boards that stay playable.
"""
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
    nb = board | mask; cleared = 0; n = 0
    for m in LINES:
        if nb & m == m: cleared |= m; n += 1
    return nb & ~cleared, n


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
    if any(not b & m for m in SQUARE3): score += 7
    if any(not b & m for m in LINE5): score += 4
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


def _expand(states, width, evaluate):
    """One placement step for every state; keep the best `width` distinct ones."""
    best = {}
    for reward, b, moves, rem in states:
        for i, (slot, cells) in enumerate(rem):
            rest = rem[:i] + rem[i + 1:]
            for x, y, m in placements(cells):
                if b & m: continue
                nb, n = drop(b, m)
                r = reward + clear_reward(n)
                key = (nb, tuple(s for s, _ in rest))
                old = best.get(key)
                if old is None or old[0] < r:
                    best[key] = (r, nb, moves + [(slot, x, y, nb)], rest)
    out = sorted(best.values(), key=lambda s: s[0] + evaluate(s[1]), reverse=True)
    return out[:width]


def _survival(b, triples):
    """Average outcome of quickly playing sampled future trays from board b."""
    total = 0.0
    for tri in triples:
        states = [(0.0, b, [], tri)]
        for _ in range(3):
            nxt = _expand(states, 4, fast_eval)
            if not nxt: break
            states = nxt
        if states[0][3]:
            total -= 150.0 * len(states[0][3])  # pieces left unplaced: likely game over
        else:
            total += max(s[0] + fast_eval(s[1]) for s in states)
    return total / len(triples)


def plan(board, pieces, avoid=frozenset(), width=300, lookahead=16, seed=0, min_filled=12, candidates=10):
    """Return the best full sequence [(slot, x, y, board_after), ...].

    pieces: list of (slot, cells). avoid: first moves (slot, x, y) to skip.
    Places as many pieces as possible; returns [] if none fit.
    """
    pieces = [(slot, tuple(cells)) for slot, cells in pieces]
    states = [(0.0, board, [], pieces)]; finished = []
    for depth in range(len(pieces)):
        nxt = _expand(states, width, fast_eval)
        if depth == 0 and avoid:
            nxt = [s for s in nxt if (s[2][0][0], s[2][0][1], s[2][0][2]) not in avoid]
        if not nxt: break
        states = nxt; finished = nxt
    if not finished: return []
    most = max(len(s[2]) for s in finished)
    finished = [s for s in finished if len(s[2]) == most]
    ranked = sorted(finished, key=lambda s: s[0] + fit_eval(s[1]), reverse=True)
    top = ranked[:max(8, candidates)]
    filled = top[0][1].bit_count()
    if lookahead and len(top) > 1 and filled >= min_filled:
        rng = random.Random(seed ^ board)
        triples = [[(i, SHAPES[k]) for i, k in enumerate(rng.choices(range(len(SHAPES)), PROBS, k=3))]
                   for _ in range(lookahead)]
        top = sorted(top[:candidates], key=lambda s: s[0] + 0.5 * fit_eval(s[1]) + _survival(s[1], triples), reverse=True)
    return [tuple(m) for m in top[0][2]]
