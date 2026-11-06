// Block Blast planner: searches every order and position for the tray's pieces, scores
// the boards, and looks ahead by playing sampled future trays from the best candidates.
// Combos: the game keeps a streak while a line is cleared within every COMBO_WINDOW
// placements; `since` counts placements since the last clear.
#include "planner.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <cmath>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>

namespace bb {

const int COMBO_WINDOW = 3;
static const double COMBO_BONUS = 6.0;   // extra reward for a clear that keeps the streak
static const double COMBO_BREAK = 10.0;  // penalty when a placement breaks the streak
static const double COMBO_READY = 9.0;   // end-of-tray bonus for lines a next piece can clear

static const u64 COL0 = 0x0101010101010101ULL, COL7 = COL0 << 7, ROW0 = 0xFFULL, ROW7 = ROW0 << 56;
static u64 cols_upto(int n) { u64 m = 0; for (int x = 0; x <= n; x++) m |= COL0 << x; return m; }
static const u64 X_LE5 = cols_upto(5), X_LE3 = cols_upto(3);  // where 3- / 5-wide runs can start

Cells normalize(const Cells& cells) {
    int mx = 99, my = 99;
    for (auto [x, y] : cells) { mx = std::min(mx, x); my = std::min(my, y); }
    Cells out;
    for (auto [x, y] : cells) out.push_back({x - mx, y - my});
    std::sort(out.begin(), out.end(), [](auto a, auto b) { return a.second != b.second ? a.second < b.second : a.first < b.first; });
    return out;
}

static std::pair<std::vector<u64>, std::vector<std::pair<int, int>>> placements(const Cells& cells) {
    int w = 0, h = 0;
    for (auto [x, y] : cells) { w = std::max(w, x + 1); h = std::max(h, y + 1); }
    std::vector<u64> masks; std::vector<std::pair<int, int>> xy;
    for (int y = 0; y + h <= 8; y++)
        for (int x = 0; x + w <= 8; x++) {
            u64 m = 0;
            for (auto [cx, cy] : cells) m |= 1ULL << ((y + cy) * 8 + x + cx);
            masks.push_back(m); xy.push_back({x, y});
        }
    return {masks, xy};
}

// Piece families seen in this game, weighted roughly by how often they appeared in the
// bot's logs. Every rotation of a family shares its weight.
const std::vector<Shape>& shapes() {
    static std::vector<Shape> all = [] {
        auto line = [](int n) { Cells c; for (int i = 0; i < n; i++) c.push_back({i, 0}); return c; };
        auto rect = [](int w, int h) { Cells c; for (int x = 0; x < w; x++) for (int y = 0; y < h; y++) c.push_back({x, y}); return c; };
        std::vector<std::pair<Cells, double>> fam = {
            {line(2), 28}, {line(3), 6}, {line(4), 27}, {line(5), 8}, {line(1), 3},
            {rect(2, 2), 11}, {rect(3, 3), 8}, {rect(3, 2), 11},
            {{{1, 0}, {2, 0}, {0, 1}, {1, 1}}, 6}, {{{0, 0}, {1, 0}, {1, 1}, {2, 1}}, 6},
            {{{0, 0}, {1, 0}, {2, 0}, {1, 1}}, 16},
            {{{0, 0}, {1, 0}, {2, 0}, {2, 1}}, 6}, {{{0, 0}, {1, 0}, {2, 0}, {0, 1}}, 6},
            {{{0, 0}, {1, 0}, {2, 0}, {2, 1}, {2, 2}}, 4}, {{{1, 0}, {0, 1}, {1, 1}}, 3},
            {{{0, 0}, {1, 1}}, 2}, {{{0, 0}, {1, 1}, {2, 2}}, 2},
        };
        std::vector<Shape> out; double total = 0;
        for (auto& [cells, w] : fam) {
            std::vector<Cells> rots; Cells cur = normalize(cells);
            for (int r = 0; r < 4; r++) {
                if (std::find(rots.begin(), rots.end(), cur) == rots.end()) rots.push_back(cur);
                Cells next; for (auto [x, y] : cur) next.push_back({-y, x});
                cur = normalize(next);
            }
            std::sort(rots.begin(), rots.end());
            for (auto& r : rots) {
                auto [m, xy] = placements(r);
                out.push_back({r, m, xy, w / rots.size()}); total += w / rots.size();
            }
        }
        for (auto& s : out) s.prob /= total;
        return out;
    }();
    return all;
}

int shape_index(const Cells& cells) {
    Cells n = normalize(cells);
    const auto& all = shapes();
    for (size_t i = 0; i < all.size(); i++) if (all[i].cells == n) return (int)i;
    return -1;
}

std::pair<u64, int> drop(u64 board, u64 mask) {
    u64 nb = board | mask;
    // Full rows leave a bit in column 0, full columns a bit in row 0.
    u64 r = nb & (nb >> 1); r &= r >> 2; r &= r >> 4; r &= COL0;
    u64 c = nb & (nb >> 8); c &= c >> 16; c &= c >> 32; c &= ROW0;
    if (!(r | c)) return {nb, 0};
    u64 cleared = r * 0xFFULL | c * COL0;
    return {nb & ~cleared, __builtin_popcountll(r) + __builtin_popcountll(c)};
}

static inline int pc(u64 x) { return __builtin_popcountll(x); }

// Cheap board score: open space, few single holes, smooth edges, room for big pieces.
double fast_eval(u64 b) {
    u64 e = ~b;
    u64 left = ((b << 1) & ~COL0) | COL0, right = ((b >> 1) & ~COL7) | COL7;
    u64 up = (b << 8) | ROW0, down = (b >> 8) | ROW7;
    int holes = pc(e & left & right & up & down);
    int pockets = pc(e & ((left & right & (up | down)) | (up & down & (left | right)))) - holes;
    int trans = pc((b ^ (b >> 1)) & ~COL7) + pc((b ^ (b >> 8)) & ~ROW7);
    double score = pc(e) * 1.0 - holes * 6.0 - pockets * 1.5 - trans * 0.45;
    u64 h3 = e & (e >> 1) & (e >> 2) & X_LE5;
    if (h3 & (h3 >> 8) & (h3 >> 16)) score += 7;  // room for a 3x3
    if ((e & (e >> 1) & (e >> 2) & (e >> 3) & (e >> 4) & X_LE3) || (e & (e >> 8) & (e >> 16) & (e >> 24) & (e >> 32)))
        score += 4;  // room for a 1x5
    return score;
}

// Fast score plus how likely a random next piece has somewhere to go.
double fit_eval(u64 b) {
    double risk = 0, room = 0;
    for (const auto& s : shapes()) {
        int n = 0;
        for (u64 m : s.masks) if (!(b & m) && ++n >= 12) break;
        if (n == 0) risk += s.prob;
        room += s.prob * std::log1p((double)n);
    }
    return fast_eval(b) - 120.0 * risk + 6.0 * room;
}

static double clear_reward(int lines) { return 3.0 * lines * lines; }

// Probability that a random next piece can complete at least one line.
static double clear_chance(u64 b) {
    double total = 0;
    for (const auto& s : shapes())
        for (u64 m : s.masks)
            if (!(b & m) && drop(b, m).second) { total += s.prob; break; }
    return total;
}

static double combo_eval(u64 b, int since, double scale) {
    if (since >= COMBO_WINDOW) return 0;
    return scale * COMBO_READY * (since + 1) / COMBO_WINDOW * clear_chance(b);
}

// ---- beam search over one tray ----
struct PieceMasks { int slot; const std::vector<u64>* masks; const std::vector<std::pair<int, int>>* xy; };

struct State {
    double reward; u64 board; int since; uint8_t rem; uint8_t n;
    Move moves[3];
};

// One placement step for every state; keeps the best `width` distinct results (same board,
// pieces left and combo state count once, with the best reward). Track = record the moves
// (the look-ahead doesn't need them). A flat open-addressing table instead of a hash map:
// this step runs thousands of times per plan.
template <bool Track, class Eval>
static void expand(const std::vector<State>& states, const std::vector<PieceMasks>& pieces,
                   size_t width, Eval evaluate, bool combo, std::vector<State>& out, double cs = 1.0) {
    thread_local std::vector<State> cand;
    thread_local std::vector<int> table;
    thread_local std::vector<std::pair<double, int>> scored;
    cand.clear();
    size_t need = 0;
    for (const State& st : states) for (size_t i = 0; i < pieces.size(); i++) if (st.rem >> i & 1) need += pieces[i].masks->size();
    size_t cap = 16; while (cap < need * 2) cap <<= 1;
    table.assign(cap, -1);
    for (const State& st : states)
        for (size_t i = 0; i < pieces.size(); i++) {
            if (!(st.rem >> i & 1)) continue;
            const auto& masks = *pieces[i].masks;
            uint8_t rem = st.rem & ~(1 << i);
            for (size_t k = 0; k < masks.size(); k++) {
                u64 m = masks[k];
                if (st.board & m) continue;
                auto [nb, n] = drop(st.board, m);
                double r = st.reward + clear_reward(n); int s2 = st.since;
                if (combo) {
                    if (n) { if (st.since < COMBO_WINDOW) r += cs * COMBO_BONUS; s2 = 0; }
                    else { s2 = st.since + 1; if (s2 == COMBO_WINDOW) r -= cs * COMBO_BREAK; }
                }
                int sk = std::min(s2, COMBO_WINDOW);
                size_t h = (nb * 0x9E3779B97F4A7C15ULL ^ (rem * 31 + sk) * 0xBF58476D1CE4E5B9ULL) >> 7 & (cap - 1);
                while (table[h] >= 0) {
                    const State& c = cand[table[h]];
                    if (c.board == nb && c.rem == rem && std::min(c.since, COMBO_WINDOW) == sk) break;
                    h = (h + 1) & (cap - 1);
                }
                State* slot;
                if (table[h] < 0) { table[h] = (int)cand.size(); cand.push_back(st); slot = &cand.back(); }
                else { slot = &cand[table[h]]; if (slot->reward >= r) continue; *slot = st; }
                slot->reward = r; slot->board = nb; slot->since = s2; slot->rem = rem;
                if constexpr (Track) slot->moves[slot->n++] = {pieces[i].slot, (*pieces[i].xy)[k].first, (*pieces[i].xy)[k].second, nb};
            }
        }
    scored.clear();
    for (size_t i = 0; i < cand.size(); i++) scored.push_back({cand[i].reward + evaluate(cand[i].board), (int)i});
    size_t keep = std::min(width, scored.size());
    std::partial_sort(scored.begin(), scored.begin() + keep, scored.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
    out.clear();
    for (size_t i = 0; i < keep; i++) out.push_back(cand[scored[i].second]);
}

struct OwnedMasks { int slot; std::vector<u64> masks; std::vector<std::pair<int, int>> xy; };

static OwnedMasks masks_for(int slot, const Cells& cells) {
    auto [m, xy] = placements(normalize(cells));
    return {slot, m, xy};
}

// Worker threads created once and reused for every plan: starting threads for each tray
// cost time and, once in a while, a long stall.
static void run_on_pool(const std::function<void()>& job, int nthreads) {
    static std::mutex m; static std::condition_variable wake, done;
    static const std::function<void()>* current = nullptr;
    static long long generation = 0; static int running = 0, workers = 0;
    std::unique_lock<std::mutex> lock(m);
    while (workers < nthreads - 1) {  // the calling thread works too
        std::thread([] {
            long long seen = 0;
            std::unique_lock<std::mutex> l(m);
            for (;;) {
                wake.wait(l, [&] { return generation != seen; });
                seen = generation; const auto* j = current; l.unlock();
                (*j)();
                l.lock(); if (--running == 0) done.notify_all();
            }
        }).detach();
        workers++;
    }
    current = &job; running = workers; generation++;
    wake.notify_all(); lock.unlock();
    job();
    lock.lock(); done.wait(lock, [] { return running == 0; });
}

// Outcome of quickly playing each sampled future tray [from, to) from board b, into out[].
// depth 2 plays a second sampled tray after the first (crowded boards).
static void survival(u64 b, const std::vector<std::vector<int>>& trays, const std::vector<std::vector<int>>& trays2,
                     size_t from, size_t to, int since, const Params& p, int depth, double* out) {
    const auto& all = shapes();
    bool combo = p.look_combo && since < COMBO_WINDOW;
    std::vector<PieceMasks> pieces(3);
    std::vector<State> states, nxt;
    for (size_t t = from; t < to; t++) {
        states.assign(1, State{0.0, b, combo ? since : COMBO_WINDOW, 7, 0, {}});
        double value = 0; bool dead = false;
        for (int tray = 0; tray < depth && !dead; tray++) {
            const auto& tr = tray == 0 ? trays[t] : trays2[t];
            for (int i = 0; i < 3; i++) pieces[i] = {i, &all[tr[i]].masks, &all[tr[i]].xy};
            for (auto& st : states) st.rem = 7;
            for (int d = 0; d < 3; d++) {
                expand<false>(states, pieces, p.look_width, fast_eval, combo, nxt, p.combo_scale);
                if (nxt.empty()) break;
                std::swap(states, nxt);
            }
            // Pieces left: likely game over (a failure in the second tray is less certain).
            if (states[0].rem) { value = -(tray == 0 ? 1.0 : 0.5) * p.gameover_penalty * pc(states[0].rem); dead = true; }
        }
        if (!dead) {
            value = -1e18;
            for (const auto& st : states) value = std::max(value, st.reward + fast_eval(st.board));
        }
        out[t] = value;
    }
}

std::vector<Move> plan(u64 board, const std::vector<Piece>& in, const Params& p,
                       const std::vector<std::tuple<int, int, int>>& avoid, int since, uint64_t seed) {
    std::vector<OwnedMasks> owned;
    for (const auto& q : in) owned.push_back(masks_for(q.slot, q.cells));
    std::vector<PieceMasks> pieces;
    for (auto& o : owned) pieces.push_back({o.slot, &o.masks, &o.xy});
    bool combo = since >= 0;
    std::vector<State> states = {State{0.0, board, combo ? since : COMBO_WINDOW, (uint8_t)((1 << pieces.size()) - 1), 0, {}}};
    std::vector<State> finished;
    for (size_t depth = 0; depth < pieces.size(); depth++) {
        std::vector<State> nxt;
        expand<true>(states, pieces, p.width, fast_eval, combo, nxt, p.combo_scale);
        if (depth == 0 && !avoid.empty())
            nxt.erase(std::remove_if(nxt.begin(), nxt.end(), [&](const State& s) {
                return std::find(avoid.begin(), avoid.end(), std::make_tuple(s.moves[0].slot, s.moves[0].x, s.moves[0].y)) != avoid.end();
            }), nxt.end());
        if (nxt.empty()) break;
        states = nxt; finished = nxt;
    }
    if (finished.empty()) return {};
    int most = 0;
    for (auto& s : finished) most = std::max<int>(most, s.n);
    std::vector<std::pair<double, State>> ranked;
    for (auto& s : finished) if (s.n == most) ranked.push_back({s.reward + fit_eval(s.board), s});
    std::stable_sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
    if (combo) {  // only the leaders get the (slower) combo term
        if (ranked.size() > 40) ranked.resize(40);
        for (auto& [score, s] : ranked) score += combo_eval(s.board, s.since, p.combo_scale);
        std::stable_sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
    }
    int filled = pc(ranked[0].second.board);
    int lookahead = p.lookahead, candidates = p.candidates;
    int depth = 1;
    if (filled >= p.danger_from) { lookahead = std::max(lookahead, p.danger_lookahead); candidates = std::max(candidates, p.danger_candidates); depth = p.danger_depth; }
    if (lookahead > 0 && ranked.size() > 1 && filled >= p.min_filled) {
        if ((int)ranked.size() > candidates) ranked.resize(candidates);
        std::mt19937_64 rng(seed ^ board);
        const auto& all = shapes();
        std::vector<double> probs; for (auto& s : all) probs.push_back(s.prob);
        std::discrete_distribution<int> pick(probs.begin(), probs.end());
        std::vector<std::vector<int>> trays(lookahead, std::vector<int>(3));
        for (auto& t : trays) for (auto& k : t) k = pick(rng);
        std::vector<std::vector<int>> trays2(depth > 1 ? lookahead : 0, std::vector<int>(3));
        for (auto& t : trays2) for (auto& k : t) k = pick(rng);
        // Jobs: (candidate, chunk of future trays), shared by the threads.
        const size_t chunk = 4, nchunks = (trays.size() + chunk - 1) / chunk;
        const size_t n = trays.size();
        std::vector<double> values(ranked.size() * n, 0.0);
        std::atomic<size_t> next{0};
        const size_t jobs = ranked.size() * nchunks;
        auto work = [&] {
            for (size_t j; (j = next++) < jobs;) {
                size_t i = j / nchunks, c = j % nchunks;
                const State& s = ranked[i].second;
                survival(s.board, trays, trays2, c * chunk, std::min(n, (c + 1) * chunk),
                         combo ? s.since : COMBO_WINDOW, p, depth, &values[i * n]);
            }
        };
        int nthreads = p.threads > 0 ? p.threads : std::max(1u, std::thread::hardware_concurrency() - 1);
        nthreads = (int)std::min<size_t>(nthreads, jobs);
        if (nthreads <= 1) work();
        else run_on_pool(work, nthreads);
        for (size_t i = 0; i < ranked.size(); i++) {
            const State& s = ranked[i].second;
            std::vector<double> v(values.begin() + i * n, values.begin() + (i + 1) * n);
            double mean = 0; for (double x : v) mean += x; mean /= n;
            double future = mean;
            if (p.tail > 0) {  // risk aversion: the worst quarter of futures counts extra
                size_t q = std::max<size_t>(1, n / 4);
                std::partial_sort(v.begin(), v.begin() + q, v.end());
                double worst = 0; for (size_t k = 0; k < q; k++) worst += v[k]; worst /= q;
                future = (1 - p.tail) * mean + p.tail * worst;
            }
            ranked[i].first = s.reward + p.fit_weight * fit_eval(s.board) + future + (combo ? combo_eval(s.board, s.since, p.combo_scale) : 0.0);
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
    }
    const State& best = ranked[0].second;
    return std::vector<Move>(best.moves, best.moves + best.n);
}

}  // namespace bb
