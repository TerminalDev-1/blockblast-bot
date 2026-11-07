// C interface to the planner for bench.py (ctypes), so the C++ planner plays the same
// simulated trays as every earlier version.
//   bb_plan(board, n, slots[n], counts[n], xy[2*sum(counts)], since, seed, out[4*3])
// Returns how many moves were planned; out holds (slot, x, y) per move and the boards
// after each move go to after[] (may be NULL).
#include "planner.hpp"
#include "value.hpp"

using namespace bb;

extern "C" __declspec(dllexport) int bb_plan(unsigned long long board, int n, const int* slots, const int* counts,
                                             const int* xy, int since, unsigned long long seed, int* out,
                                             unsigned long long* after) {
    std::vector<Piece> pieces; int k = 0;
    for (int i = 0; i < n; i++) {
        Cells c; for (int j = 0; j < counts[i]; j++, k++) c.push_back({xy[2 * k], xy[2 * k + 1]});
        pieces.push_back({slots[i], c});
    }
    Params p; p.threads = 1;  // bench.py runs games in parallel processes
    p.nn_future = nn_ready(1);  // v5.9: the trained network judges candidates, as in the bot
    auto seq = plan(board, pieces, p, {}, since, seed);
    for (size_t i = 0; i < seq.size(); i++) {
        out[3 * i] = seq[i].slot; out[3 * i + 1] = seq[i].x; out[3 * i + 2] = seq[i].y;
        if (after) after[i] = seq[i].after;
    }
    return (int)seq.size();
}
