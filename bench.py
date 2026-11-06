"""Benchmark every planner revision on the same simulated games, and chart them.

Usage: python bench.py [games] [max_moves] [--procs=N] [--all]
Writes bench-results.json and benchmark.jpg (shown in the README).

Committed versions never change, so their results are reused from
bench-results.json and only new versions (or the working tree) are simulated.
--all re-simulates everything.

Each revision's planner is loaded straight from git, so old versions are played
exactly as they were. All of them get the same trays (dealt from the current
piece list with the same seeds), so the numbers are directly comparable.
Live speed (moves/min on the tablet) can't be replayed offline; it comes from the
bot's logs and is listed in LIVE_SPEED by hand.
"""
import importlib.util, json, os, subprocess, sys, tempfile, time
from multiprocessing import Pool

# The trays are dealt with the Python planner's piece list (v5.4, the last Python version),
# so every version, Python or C++, plays exactly the same games.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'versions', 'v5.4-highspeed-max'))
import planner as current

# (label, commit or None for the working tree, kind)
#   'choose' = the original one-move-at-a-time bot.choose; 'plan' = planner.plan;
#   'combo' = planner.plan with combo tracking; 'cpp' = a C++ planner (CPP_SRC below),
#   built from that commit with g++ into a DLL.
VERSIONS = [
    ('v1 original', 'cd316c3', 'choose'),
    ('v2 bitboard planner', '9ed7b94', 'plan'),
    ('v3 look-ahead', '0347688', 'plan'),
    ('v4 combos', 'f28798f', 'combo'),
    ('v5 deeper look-ahead', '698ad26', 'combo'),
    ('v5.1 fast planner', 'f16d74a', 'combo'),
    ('v5.4 highspeed max', '85fa355', 'combo'),
    ('v5.5 cpp max', 'e2f0115', 'cpp'),
    ('v5.5.1 (experimental)', '2847186', 'cpp'),
    ('v5.7 speedbreaker', '82bbd9d', 'cpp'),
]
# Where each C++ version's planner source lives in its commit.
CPP_SRC = {'v5.5 cpp max': 'versions/v5.5-cpp-max/src', 'v5.5.1 (experimental)': 'versions/v5.5.1-exp/src',
            'v5.7 speedbreaker': 'versions/v5.7-speedbreaker/src'}
# Measured on the tablet (moves/min from bot.log over 100+ moves). None = not measured.
LIVE_SPEED = {'v1 original': None, 'v2 bitboard planner': None, 'v3 look-ahead': 29,
              'v3.1 line-clear skip': 39, 'v4 combos': 50,
              'v4.1 faster checks': 67, 'v4.2 fast drags': 64,
              'v5 deeper look-ahead': 94, 'v5.1 fast planner': 87,
              'v5.3 flash max': 100,
              'v5.3.1 flash highspeed max': 122,
              'v5.4 highspeed max': 100, 'v5.5 cpp max': 139, 'v5.5.1 (experimental)': 154,
              'v5.6 hyperspeed': 156,  # same planner as v5.5.1: speed only
              'v5.7 speedbreaker': 165,  # released with the safe tray check (211 with the riskier one)
              'v5.8 mach1': 174}  # same planner as v5.7: speed only

WINDOW = current.COMBO_WINDOW


_loaded = {}


def cpp_dll(label, commit):
    """Path of the planner DLL for a C++ version, built from that commit's source."""
    src = CPP_SRC[label]
    out = os.path.join(tempfile.gettempdir(), f'bbplan_{commit or "worktree"}.dll')
    # Committed sources never change; the working tree is built once per run by the main
    # process (BB_DLL_READY), and the worker processes reuse it.
    if os.path.exists(out) and (commit or os.environ.get('BB_DLL_READY')): return out
    files = ['planner.hpp', 'planner.cpp', 'capi.cpp']
    tmp = tempfile.mkdtemp(prefix='bbplan_')
    for f in files:
        data = (open(os.path.join(src, f), 'rb').read() if commit is None else
                subprocess.run(['git', 'show', f'{commit}:{src}/{f}'], check=True, stdout=subprocess.PIPE).stdout)
        open(os.path.join(tmp, f), 'wb').write(data)
    gpp = 'g++'
    if not any(os.path.exists(os.path.join(d, 'g++.exe')) for d in os.environ.get('PATH', '').split(os.pathsep)):
        import glob
        found = glob.glob(os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Microsoft', 'WinGet', 'Packages', '*', 'mingw64', 'bin', 'g++.exe'))
        if found: gpp = found[0]
    subprocess.run([gpp, '-O2', '-mpopcnt', '-std=c++20', '-static', '-shared', '-o', out,
                    os.path.join(tmp, 'capi.cpp'), os.path.join(tmp, 'planner.cpp')], check=True)
    return out


class CppPlanner:
    """planner.plan-like wrapper around bb_plan in a C++ planner DLL."""
    def __init__(self, path):
        import ctypes
        self.lib = ctypes.CDLL(path); self.c = ctypes
        u64 = ctypes.c_ulonglong; ip = ctypes.POINTER(ctypes.c_int)
        self.lib.bb_plan.argtypes = [u64, ctypes.c_int, ip, ip, ip, ctypes.c_int, u64, ip, ctypes.POINTER(u64)]
        self.lib.bb_plan.restype = ctypes.c_int

    def plan(self, board, tray, seed=0, since=None):
        c = self.c; n = len(tray)
        slots = (c.c_int * n)(*[s for s, _ in tray]); counts = (c.c_int * n)(*[len(cells) for _, cells in tray])
        xy = [v for _, cells in tray for cell in cells for v in cell]
        xy = (c.c_int * len(xy))(*xy); out = (c.c_int * 12)(); after = (c.c_ulonglong * 4)()
        k = self.lib.bb_plan(board, n, slots, counts, xy, WINDOW if since is None else since, seed, out, after)
        return [(out[3 * i], out[3 * i + 1], out[3 * i + 2], after[i]) for i in range(k)]


def load(commit, kind, label=None):
    if kind == 'cpp':
        key = ('cpp', commit)
        if key not in _loaded: _loaded[key] = CppPlanner(cpp_dll(label, commit))
        return _loaded[key]
    if commit is None:
        return current
    if commit not in _loaded:
        name = 'bot.py' if kind == 'choose' else 'planner.py'
        src = subprocess.run(['git', 'show', f'{commit}:{name}'], check=True, stdout=subprocess.PIPE).stdout
        # One file per process: the benchmark runs in parallel.
        path = os.path.join(tempfile.gettempdir(), f'bb_{commit}_{os.getpid()}_{name}')
        with open(path, 'wb') as f: f.write(src)
        spec = importlib.util.spec_from_file_location(f'bb_{commit}', path)
        mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
        os.remove(path); _loaded[commit] = mod
    return _loaded[commit]


def deal(rng):
    return [current.SHAPES[k] for k in rng.choices(range(len(current.SHAPES)), current.PROBS, k=3)]


def play(job):
    """One game. Returns moves, lines, and the length of every combo streak."""
    import random
    label, commit, kind, seed, cap = job
    mod = load(commit, kind, label)
    rng = random.Random(seed); board = 0; moves = 0; lines = 0
    since = WINDOW; streak = 0; streaks = []

    def placed(cells, x, y, nb):
        nonlocal board, moves, lines, since, streak
        mask = sum(1 << ((y + cy) * 8 + x + cx) for cx, cy in cells)
        n = current.drop(board, mask)[1]
        lines += n; board = nb; moves += 1
        if n:
            streak = streak + 1 if since < WINDOW else 1; since = 0
        else:
            since += 1
            if since == WINDOW and streak: streaks.append(streak); streak = 0

    while moves < cap:
        tray = list(enumerate(deal(rng)))
        while tray:
            if kind == 'choose':
                m = mod.choose(board, [{'slot': s, 'cells': list(c)} for s, c in tray])
                if not m: tray = None; break
                slot = m[0]['slot']; placed(dict(tray)[slot], m[1], m[2], m[3])
                tray = [t for t in tray if t[0] != slot]
            else:
                extra = {'since': since} if kind in ('combo', 'cpp') else {}
                seq = mod.plan(board, tray, seed=seed + moves, **extra)
                if not seq: tray = None; break
                for slot, x, y, nb in seq:
                    placed(dict(tray)[slot], x, y, nb); tray = [t for t in tray if t[0] != slot]
                if tray: tray = None; break
        if tray is None: break
    if streak: streaks.append(streak)
    return label, moves, lines, streaks


def summarise(results, versions, cap):
    out = {}
    for label, *_ in versions:
        rs = [r for r in results if r[0] == label]
        streaks = [s for r in rs for s in r[3]]
        out[label] = {
            'games': len(rs), 'cap': cap,
            'moves': sum(r[1] for r in rs) / len(rs),
            'reached_cap': sum(r[1] >= cap for r in rs),
            'lines_per_100': 100 * sum(r[2] for r in rs) / max(1, sum(r[1] for r in rs)),
            'best_streak': sum(max(r[3], default=0) for r in rs) / len(rs),
            'mean_streak': sum(streaks) / max(1, len(streaks)),
            'live_moves_per_min': LIVE_SPEED.get(label),
            'commit': next(c for l, c, _ in versions if l == label),
        }
    return out


def chart(summary, path='benchmark.jpg'):
    import matplotlib; matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    labels = list(summary)
    speed = dict(LIVE_SPEED)
    commits = {label: commit for label, commit, _ in VERSIONS}

    def tick_label(name):
        # Experimental checkpoints show when they were made (their commit's date and
        # time), so a stale benchmark of one is easy to spot.
        text = name.replace(' ', '\n')
        if '(experimental)' in name:
            commit = commits.get(name)
            when = 'working tree'
            if commit:
                when = subprocess.run(['git', 'show', '-s', '--format=%cd', '--date=format-local:%d %b %Y %H:%M', commit],
                                      capture_output=True, text=True).stdout.strip()
                when = when[:11] + '\n' + when[12:]  # date, then time below
            text += '\n' + when
        return text
    speed_labels = [k for k, v in speed.items() if v]
    panels = [
        ('Moves survived (avg, cap %d)' % next(iter(summary.values()))['cap'], [summary[k]['moves'] for k in labels], labels),
        ('Longest combo streak per game (avg)', [summary[k]['best_streak'] for k in labels], labels),
        ('Lines cleared per 100 moves', [summary[k]['lines_per_100'] for k in labels], labels),
        ('Live speed on the tablet (moves/min)', [speed[k] for k in speed_labels], speed_labels),
    ]
    bg, panel, text, dim, grid = '#0d1117', '#161b22', '#e6edf3', '#8b949e', '#21262d'
    fig, axes = plt.subplots(2, 2, figsize=(14, 8), dpi=110, facecolor=bg)

    def greens(n):  # dull green for the oldest version, bright green for the newest
        a, b = (0x2d, 0x4a, 0x35), (0x39, 0xd3, 0x53)
        return ['#%02x%02x%02x' % tuple(round(x + (y - x) * i / max(1, n - 1)) for x, y in zip(a, b))
                for i in range(n)]

    for ax, (title, values, names) in zip(axes.flat, panels):
        ax.set_facecolor(panel)
        ax.grid(axis='y', color=grid, linewidth=1); ax.set_axisbelow(True)
        bars = ax.bar(range(len(values)), values, color=greens(len(values)), width=.62)
        ax.set_title(title, fontsize=12, loc='left', fontweight='bold', color=text)
        # Version number on the first line, its name wrapped below.
        ax.set_xticks(range(len(names)), [tick_label(n) for n in names],
                      fontsize=8.5 if len(names) <= 9 else 6.8, color=dim)  # many bars: smaller labels
        for side in ('top', 'right', 'left'): ax.spines[side].set_visible(False)
        ax.spines['bottom'].set_color(grid)
        ax.tick_params(axis='y', labelsize=9, colors=dim, length=0); ax.tick_params(axis='x', length=0)
        for b, v in zip(bars, values):
            ax.text(b.get_x() + b.get_width() / 2, b.get_height(), f'{v:.1f}' if v < 20 else f'{v:.0f}',
                    ha='center', va='bottom', fontsize=10, color=text, fontweight='bold')
    games = next(iter(summary.values()))['games']
    fig.suptitle(f'How each version plays ({games} simulated games each, plus live speed)',
                 fontsize=14, fontweight='bold', color=text)
    fig.tight_layout(rect=(0, 0, 1, .95)); fig.savefig(path, facecolor=bg, pil_kwargs={'quality': 90}); plt.close(fig)


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    games = int(args[0]) if args else 24; cap = int(args[1]) if len(args) > 1 else 300
    try: saved = json.load(open('bench-results.json'))
    except (OSError, ValueError): saved = {}
    reuse = {label: saved[label] for label, commit, _ in VERSIONS
             if commit and '--all' not in sys.argv and label in saved
             and saved[label].get('commit') == commit and saved[label]['games'] == games and saved[label]['cap'] == cap}
    todo = [v for v in VERSIONS if v[0] not in reuse]
    for label, commit, kind in todo:
        if kind == 'cpp': cpp_dll(label, commit)  # build once, before the worker processes
    os.environ['BB_DLL_READY'] = '1'
    if reuse: print('Reusing saved results for:', ', '.join(reuse))
    jobs = [(label, commit, kind, 5000 + g, cap) for label, commit, kind in todo for g in range(games)]
    t = time.time()
    procs = next((int(a[8:]) for a in sys.argv if a.startswith('--procs=')), max(1, (os.cpu_count() or 2) - 2))
    with Pool(procs) as pool:
        results = []
        for r in pool.imap_unordered(play, jobs):
            results.append(r); print(f'{r[0]}: {r[1]} moves, {r[2]} lines, best streak {max(r[3], default=0)}', flush=True)
    fresh = summarise(results, todo, cap) if todo else {}
    summary = {}
    for label, *_ in VERSIONS:
        summary[label] = reuse.get(label) or fresh[label]
        summary[label]['live_moves_per_min'] = LIVE_SPEED.get(label)
    json.dump(summary, open('bench-results.json', 'w'), indent=1)
    for k, v in summary.items(): print(k, {a: round(b, 2) if isinstance(b, float) else b for a, b in v.items()})
    chart(summary); print(f'Done in {time.time() - t:.0f}s; wrote bench-results.json and benchmark.jpg')
