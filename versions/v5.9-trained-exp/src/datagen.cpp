// v5.9-trained-exp self-play data: plays simulated games and writes every end-of-tray board
// with its label: 1 if the game was lost within HORIZON trays, else 0. Boards from the last
// HORIZON trays of a game that hit the move cap are left out (their outcome is unknown).
// usage: datagen out.bin games cap seed [nn=weights.bin] [name=value ...]  (same names as sim.exe)
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "planner.hpp"
#include "value.hpp"

using namespace bb;

struct Rec { uint64_t board; uint8_t label; };
static std::string g_record;  // distillation output file (look-ahead verdicts)

static bool set_param(Params& p, const std::string& kv) {
    auto eq = kv.find('='); if (eq == std::string::npos) return false;
    std::string k = kv.substr(0, eq); double v = atof(kv.c_str() + eq + 1);
    if (k == "nn") return nn_load(kv.c_str() + eq + 1, 0);
    if (k == "nnf") { p.nn_future = true; return nn_load(kv.c_str() + eq + 1, 1); }
    if (k == "record") { g_record = kv.substr(eq + 1); return true; }
    if (k == "width") p.width = (int)v; else if (k == "lookahead") p.lookahead = (int)v;
    else if (k == "candidates") p.candidates = (int)v; else if (k == "danger_from") p.danger_from = (int)v;
    else if (k == "danger_lookahead") p.danger_lookahead = (int)v; else if (k == "danger_candidates") p.danger_candidates = (int)v;
    else if (k == "look_width") p.look_width = (int)v; else if (k == "danger_depth") p.danger_depth = (int)v;
    else if (k == "nn_weight") p.nn_weight = v; else if (k == "nn_record_only") p.nn_record_only = v != 0; else if (k == "nn_prune") p.nn_prune = (int)v; else if (k == "tail") p.tail = v;
    else return false;
    return true;
}

int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "usage: datagen out.bin games cap seed [nn=file] [name=value...]\n"); return 1; }
    const char* out = argv[1];
    int games = atoi(argv[2]), cap = atoi(argv[3]);
    uint64_t seed0 = strtoull(argv[4], nullptr, 10);
    Params p; p.threads = 1;
    for (int i = 5; i < argc; i++) if (!set_param(p, argv[i])) { fprintf(stderr, "bad option %s\n", argv[i]); return 1; }

    std::vector<Rec> all; std::vector<FutureSample> fut; std::mutex m;
    std::atomic<int> next{0}, lost{0}; std::atomic<long long> moves_total{0};
    auto worker = [&] {
        Params lp = p;  // per worker: its own recording buffer
        std::vector<FutureSample> mine;
        if (!g_record.empty()) lp.record = &mine;
        for (int g; (g = next++) < games;) {
            std::mt19937_64 rng((seed0 + g) * 7919 + 13);
            const auto& shp = shapes();
            std::vector<double> probs; for (auto& s : shp) probs.push_back(s.prob);
            std::discrete_distribution<int> pick(probs.begin(), probs.end());
            u64 board = 0; int since = COMBO_WINDOW, moves = 0; bool dead = false;
            std::vector<u64> boards;
            while (moves < cap) {
                std::vector<Piece> tray;
                for (int i = 0; i < 3; i++) tray.push_back({i, shp[pick(rng)].cells});
                auto seq = plan(board, tray, lp, {}, since, seed0 + g * 100003 + moves);
                for (const Move& mv : seq) {
                    const Cells* cells = nullptr;
                    for (auto& q : tray) if (q.slot == mv.slot) cells = &q.cells;
                    u64 mask = 0; for (auto [x, y] : *cells) mask |= 1ULL << ((mv.y + y) * 8 + mv.x + x);
                    int n = drop(board, mask).second;
                    board = mv.after; moves++;
                    if (n) since = 0; else since++;
                }
                if (seq.size() < tray.size()) { dead = true; break; }
                boards.push_back(board);
            }
            moves_total += moves; if (dead) lost++;
            std::vector<Rec> recs;
            int T = (int)boards.size();
            for (int t = 0; t < T; t++) {
                int left = T - t;  // trays until the end
                if (!dead && left <= HORIZON) continue;
                recs.push_back({boards[t], (uint8_t)(dead && left <= HORIZON)});
            }
            std::lock_guard<std::mutex> l(m);
            all.insert(all.end(), recs.begin(), recs.end());
            fut.insert(fut.end(), mine.begin(), mine.end());
            mine.clear();
        }
    };
    int n = std::max(1u, std::thread::hardware_concurrency() - 1);
    if (const char* procs = getenv("SIM_PROCS")) n = std::max(1, atoi(procs));
    std::vector<std::thread> ts; for (int t = 0; t < n; t++) ts.emplace_back(worker);
    for (auto& t : ts) t.join();
    FILE* f = fopen(out, "wb");
    for (auto& r : all) { fwrite(&r.board, 8, 1, f); fwrite(&r.label, 1, 1, f); }
    fclose(f);
    if (!g_record.empty()) {
        FILE* r = fopen(g_record.c_str(), "wb");
        for (auto& x : fut) { fwrite(&x.board, 8, 1, r); fwrite(&x.since, 1, 1, r); fwrite(&x.future, 4, 1, r); }
        fclose(r);
        printf("%zu look-ahead verdicts recorded\n", fut.size());
    }
    long long pos = 0; for (auto& r : all) pos += r.label;
    printf("%d games, %.0f moves avg, %d lost; %zu boards (%lld danger)\n", games, (double)moves_total / games, lost.load(), all.size(), pos);
}
