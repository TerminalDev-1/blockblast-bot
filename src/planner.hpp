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
    int danger_from = 28;       // a crowded board (games are lost there) gets more look-ahead
    int danger_lookahead = 96;
    int danger_candidates = 20;
    int look_width = 8;         // beam width inside the look-ahead
    bool look_combo = true;     // play future trays keeping the combo streak alive
    double fit_weight = 1.0;    // weight of the board score next to the look-ahead
    int threads = 0;            // look-ahead threads (0 = all cores but one)
};

extern const int COMBO_WINDOW;

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
