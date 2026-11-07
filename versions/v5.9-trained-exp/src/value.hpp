// v5.9-trained-exp: small neural networks that judge boards, trained by self-play in the
// simulator (datagen.cpp, train.cpp). Two kinds:
//   danger (slot 0): chance the game is lost within the next HORIZON trays.
//   future (slot 1): what v5.8's Monte Carlo look-ahead would conclude about a board
//                    ("distillation"): its judgement without playing 64 sampled futures.
// Input: the 64 cells, a few hand-made measurements (holes, rough edges, how many pieces still
// fit, ...) and the combo state. The weights are compiled in (value_weights.inc).
#pragma once
#include <cstdint>
#include <vector>

namespace bb {

constexpr int NN_FEATURES = 64 + 11;
#ifdef NN_BIG  // experiment: a wider network (build with -DNN_BIG)
constexpr int NN_H1 = 128;
constexpr int NN_H2 = 64;
#else
constexpr int NN_H1 = 64;
constexpr int NN_H2 = 32;
#endif
constexpr int HORIZON = 10;          // trays (danger net)
constexpr double FUTURE_SCALE = 100;  // future net output unit

struct ValueNet {
    std::vector<float> w1, b1, w2, b2, w3;  // w1: [NN_FEATURES][NN_H1] row-major
    float b3 = 0;
    void init(uint64_t seed);
    bool loaded() const { return !w1.empty(); }
};

struct FutureSample { uint64_t board; int8_t since; float future; };

void nn_features(uint64_t board, int since, float* x);
double nn_raw(const ValueNet& net, uint64_t board, int since);
double nn_danger(uint64_t board);              // 0..1
double nn_future(uint64_t board, int since);   // look-ahead units
bool nn_ready(int slot);
bool nn_load(const char* path, int slot);
bool nn_save(const ValueNet& net, const char* path);
void nn_write_include(const ValueNet& net, int slot, const char* path);

}  // namespace bb
