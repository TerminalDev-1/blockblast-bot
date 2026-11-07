// Block Blast move planner on a 64-bit bitboard (bit y*8+x is cell (x, y)).
// C++ port of the Python planner (v5.4) with a deeper, combo-aware look-ahead that the
// speed of C++ makes affordable.
#pragma once
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace bb {

using u64 = uint64_t;
using Cells = std::vector<std::pair<int, int>>;  // (x, y), normalised: min x = min y = 0

struct Piece { int slot; Cells cells; };
struct Move { int slot, x, y; u64 after; };

struct Params {
    int width = 300;            // beam width while placing this tray's pieces
    // Tuned with sim.exe (96 games up to 1500 moves): v5.1's settings survived 945 moves
    // on average with a best combo streak of 56; these survive 1161 with 107.
    int lookahead = 64;         // future trays sampled for each candidate
    int candidates = 16;        // best plans that get the look-ahead
    int min_filled = 0;         // look-ahead once the board has this many blocks
    int danger_from = 24;       // a crowded board (games are lost there) gets more look-ahead (v5.5: 28)
    int danger_lookahead = 96;
    int danger_candidates = 20;
    int look_width = 8;         // beam width inside the look-ahead
    bool look_combo = true;     // play future trays keeping the combo streak alive
    double fit_weight = 1.0;    // weight of the board score next to the look-ahead
    // v5.5.1-exp survival knobs. Tuned with sim.exe on 64 games up to 3000 moves, twice
    // (seeds 20000 and 30000): v5.5's settings reached 3000 moves in 29 of 128 games,
    // these in 54 (average 1992 moves vs 1616), with longer combo streaks too.
    double gameover_penalty = 150;  // per piece that doesn't fit in a sampled future
    double combo_scale = 1.0;       // weight of combo bonuses/penalties against safety
    double tail = 0.7;              // weight of the worst quarter of futures (v5.5: 0, plain average)
    int look_depth = 1;             // future trays played per sample on an open board
    int danger_depth = 2;           // future trays played per sample when the board is crowded (v5.5: 1)
    int threads = 0;            // look-ahead threads (0 = all cores but one)
    // v5.9-flash: weight of the trained value network's danger (chance of losing within
    // the next few trays) in a candidate's score. 0 = off (plays exactly like v5.8).
    double nn_weight = 0;
    // v5.9: judge candidates with the distilled look-ahead network instead of playing sampled
    // futures (much faster). Needs the future net (slot 1).
    bool nn_future = false;
    // v5.9 hybrid: with nn_future, keep the network's best nn_prune candidates and let the real
    // look-ahead decide between them (0 = the network decides alone).
    int nn_prune = 0;
    // v5.9 training (DAgger): with nn_future and nn_prune, the look-ahead still runs on the
    // network's best nn_prune candidates (so its verdicts get recorded), but the network's own
    // choice is played. Teaches the network about the boards it reaches by itself.
    bool nn_record_only = false;
    // v5.9 training: when set, every look-ahead verdict (board, combo state, value) is recorded.
    std::vector<struct FutureSample>* record = nullptr;
};

extern const int COMBO_WINDOW;

// Board score weights (fast_eval / fit_eval). v5.7 tuned them with sim.exe (one at a time at
// 0.6x / 1.5x, then combined): smooth edges and no holes matter more, raw empty space less.
// On 64 fresh games up to 3000 moves: 2090 moves on average vs 1788 with the old weights
// (empty 1.0, hole 6.0, trans 0.45), 28 vs 23 games lasting all 3000 moves.
struct EvalWeights {
    double empty = 0.6, hole = 9.0, pocket = 1.5, trans = 0.675, square3 = 7.0, line5 = 4.0;
    double risk = 120.0, room = 6.0;
};
extern EvalWeights EVAL;

// Shapes the game deals, with their probabilities.
struct Shape { Cells cells; std::vector<u64> masks; std::vector<std::pair<int, int>> xy; double prob; };
const std::vector<Shape>& shapes();
int shape_index(const Cells& cells);  // -1 if the game has no such shape
Cells normalize(const Cells& cells);

std::pair<u64, int> drop(u64 board, u64 mask);  // place + clear full lines: (board, lines)
double fast_eval(u64 b);
double fit_eval(u64 b);

// Best full sequence for the tray. avoid: first moves (slot, x, y) to skip.
// since: placements since the last line clear (-1 = ignore combos).
std::vector<Move> plan(u64 board, const std::vector<Piece>& pieces, const Params& p = Params(),
                       const std::vector<std::tuple<int, int, int>>& avoid = {}, int since = -1,
                       uint64_t seed = 0);

}  // namespace bb
