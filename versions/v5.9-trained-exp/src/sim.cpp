// Offline benchmark: plays simulated games with the planner (no tablet needed).
// usage: sim [games] [max_moves] [first_seed] [name=value ...]
//   names: width lookahead candidates min_filled danger_from danger_lookahead
//          danger_candidates look_width look_combo fit_weight
//   presets: v51 (v5.1 settings), v54 (v5.4 settings)
// Games run in parallel; the planner itself is single-threaded here.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "planner.hpp"
#include "value.hpp"

using namespace bb;

struct Result { int moves = 0, lines = 0, best_streak = 0; double plan_s = 0, worst_s = 0; int trays = 0; };

static Result play(uint64_t seed, int cap, const Params& p) {
    std::mt19937_64 rng(seed * 7919 + 13);
    const auto& all = shapes();
    std::vector<double> probs; for (auto& s : all) probs.push_back(s.prob);
    std::discrete_distribution<int> pick(probs.begin(), probs.end());
    Result r; u64 board = 0; int since = COMBO_WINDOW, streak = 0;
    while (r.moves < cap) {
        std::vector<Piece> tray;
        for (int i = 0; i < 3; i++) tray.push_back({i, all[pick(rng)].cells});
        auto t0 = std::chrono::steady_clock::now();
        auto seq = plan(board, tray, p, {}, since, seed + r.moves);
        double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        r.plan_s += dt; r.worst_s = std::max(r.worst_s, dt); r.trays++;
        if (seq.size() < tray.size()) break;  // a piece doesn't fit: game over
        for (const Move& m : seq) {
            const Cells* cells = nullptr;
            for (auto& q : tray) if (q.slot == m.slot) cells = &q.cells;
            u64 mask = 0; for (auto [x, y] : *cells) mask |= 1ULL << ((m.y + y) * 8 + m.x + x);
            int n = drop(board, mask).second;
            r.lines += n; board = m.after; r.moves++;
            if (n) { streak = since < COMBO_WINDOW ? streak + 1 : 1; since = 0; }
            else if (++since == COMBO_WINDOW) streak = 0;
            r.best_streak = std::max(r.best_streak, streak);
        }
    }
    return r;
}

static bool set_param(Params& p, const std::string& kv) {
    auto old = [&] { p.lookahead = 16; p.candidates = 10; p.min_filled = 12; p.look_width = 4; p.look_combo = false; p.fit_weight = 0.5; };
    if (kv == "v51") { old(); p.danger_from = 99; return true; }
    if (kv == "v54") { old(); p.danger_from = 28; p.danger_lookahead = 48; p.danger_candidates = 16; return true; }
    auto eq = kv.find('='); if (eq == std::string::npos) return false;
    std::string k = kv.substr(0, eq); double v = atof(kv.c_str() + eq + 1);
    if (k == "width") p.width = (int)v; else if (k == "lookahead") p.lookahead = (int)v;
    else if (k == "candidates") p.candidates = (int)v; else if (k == "min_filled") p.min_filled = (int)v;
    else if (k == "danger_from") p.danger_from = (int)v; else if (k == "danger_lookahead") p.danger_lookahead = (int)v;
    else if (k == "danger_candidates") p.danger_candidates = (int)v; else if (k == "look_width") p.look_width = (int)v;
    else if (k == "look_combo") p.look_combo = v != 0; else if (k == "threads") p.threads = (int)v; else if (k == "gameover_penalty") p.gameover_penalty = v;
    else if (k == "combo_scale") p.combo_scale = v; else if (k == "tail") p.tail = v; else if (k == "danger_depth") p.danger_depth = (int)v; else if (k == "look_depth") p.look_depth = (int)v; else if (k == "fit_weight") p.fit_weight = v;
    else if (k == "w_empty") EVAL.empty = v; else if (k == "w_hole") EVAL.hole = v; else if (k == "w_pocket") EVAL.pocket = v;
    else if (k == "w_trans") EVAL.trans = v; else if (k == "w_square3") EVAL.square3 = v; else if (k == "w_line5") EVAL.line5 = v;
    else if (k == "nn_weight") p.nn_weight = v; else if (k == "nn_prune") p.nn_prune = (int)v; else if (k == "nn") return nn_load(kv.c_str() + eq + 1, 0); else if (k == "nnf") { p.nn_future = true; return nn_load(kv.c_str() + eq + 1, 1); }
    else if (k == "w_risk") EVAL.risk = v; else if (k == "w_room") EVAL.room = v;
    else return false;
    return true;
}

int main(int argc, char** argv) {
    int games = argc > 1 ? atoi(argv[1]) : 24, cap = argc > 2 ? atoi(argv[2]) : 1000;
    uint64_t seed0 = argc > 3 ? strtoull(argv[3], nullptr, 10) : 7000;
    Params p; p.threads = 1; std::string label;
    for (int i = 4; i < argc; i++) {
        if (!set_param(p, argv[i])) { fprintf(stderr, "unknown option %s\n", argv[i]); return 1; }
        label += std::string(argv[i]) + " ";
    }
    std::vector<Result> rs(games);
    std::atomic<int> next{0};
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    // Games in parallel with a single-threaded planner; with threads=0 (the bot's setting)
    // one game at a time, the planner using all cores.
    int n = p.threads == 1 ? std::max(1u, std::thread::hardware_concurrency() - 1) : 1;
    if (const char* procs = getenv("SIM_PROCS")) n = std::max(1, atoi(procs));  // leave cores for a live bot
    for (int t = 0; t < n; t++) ts.emplace_back([&] { for (int g; (g = next++) < games;) rs[g] = play(seed0 + g, cap, p); });
    for (auto& t : ts) t.join();
    double moves = 0, lines = 0, streak = 0, plan_s = 0, worst = 0; int trays = 0, capped = 0;
    for (auto& r : rs) { worst = std::max(worst, r.worst_s); moves += r.moves; lines += r.lines; streak += r.best_streak; plan_s += r.plan_s; trays += r.trays; capped += r.moves >= cap; }
    printf("%s: moves %.1f cap %d/%d lines/100 %.2f best streak %.2f plan %.1f ms/tray (slowest %.0f ms) [%.0fs]\n",
           label.empty() ? "default" : label.c_str(), moves / games, capped, games, 100 * lines / moves, streak / games,
           1000 * plan_s / trays, 1000 * worst, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}
