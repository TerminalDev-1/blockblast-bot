#include "value.hpp"

#include <cmath>
#include <cstdio>
#include <random>

#include "planner.hpp"

namespace bb {

static inline int pc(u64 x) { return __builtin_popcountll(x); }
static const u64 COL0 = 0x0101010101010101ULL, COL7 = COL0 << 7, ROW0 = 0xFFULL, ROW7 = ROW0 << 56;

void nn_features(u64 b, int since, float* x) {
    for (int i = 0; i < 64; i++) x[i] = (b >> i & 1) ? 1.f : 0.f;
    u64 e = ~b;
    u64 left = ((b << 1) & ~COL0) | COL0, right = ((b >> 1) & ~COL7) | COL7;
    u64 up = (b << 8) | ROW0, down = (b >> 8) | ROW7;
    int holes = pc(e & left & right & up & down);
    int pockets = pc(e & ((left & right & (up | down)) | (up & down & (left | right)))) - holes;
    int trans = pc((b ^ (b >> 1)) & ~COL7) + pc((b ^ (b >> 8)) & ~ROW7);
    double risk = 0, room = 0, clear = 0, big = 0;
    for (const auto& s : shapes()) {
        int n = 0; bool clears = false;
        for (u64 m : s.masks) {
            if (b & m) continue;
            n++;
            if (!clears && drop(b, m).second) clears = true;
        }
        if (n == 0) { risk += s.prob; if (s.cells.size() >= 5) big += s.prob; }
        room += s.prob * std::log1p((double)n);
        if (clears) clear += s.prob;
    }
    int rows = 0, cols = 0;  // nearly full lines (6-7 of 8)
    for (int r = 0; r < 8; r++) { int k = pc((b >> (r * 8)) & 0xFF); rows += k >= 6; }
    for (int c = 0; c < 8; c++) { int k = pc(b & (COL0 << c)); cols += k >= 6; }
    float* f = x + 64;
    f[0] = pc(e) / 64.f;
    f[1] = holes / 8.f;
    f[2] = pockets / 8.f;
    f[3] = trans / 64.f;
    f[4] = (float)risk;
    f[5] = (float)(room / 4.0);
    f[6] = (float)clear;
    f[7] = (float)big;
    f[8] = (rows + cols) / 8.f;
    f[9] = (float)(fast_eval(b) / 40.0);
    f[10] = since < COMBO_WINDOW ? (COMBO_WINDOW - since) / (float)COMBO_WINDOW : 0.f;  // combo still alive
}

void ValueNet::init(uint64_t seed) {
    std::mt19937_64 rng(seed);
    auto fill = [&](std::vector<float>& v, size_t n, int fan_in) {
        std::normal_distribution<float> d(0.f, std::sqrt(2.f / fan_in));
        v.resize(n); for (auto& x : v) x = d(rng);
    };
    fill(w1, NN_FEATURES * NN_H1, NN_FEATURES); b1.assign(NN_H1, 0.f);
    fill(w2, NN_H1 * NN_H2, NN_H1); b2.assign(NN_H2, 0.f);
    fill(w3, NN_H2, NN_H2); b3 = 0.f;
}

double nn_raw(const ValueNet& n, u64 board, int since) {
    float x[NN_FEATURES], h1[NN_H1], h2[NN_H2];
    nn_features(board, since, x);
    for (int j = 0; j < NN_H1; j++) h1[j] = n.b1[j];
    // The 64 cell inputs are 0/1: add the weight rows of filled cells only.
    for (u64 bits = board; bits; bits &= bits - 1) {
        const float* w = &n.w1[__builtin_ctzll(bits) * NN_H1];
        for (int j = 0; j < NN_H1; j++) h1[j] += w[j];
    }
    for (int i = 64; i < NN_FEATURES; i++) {
        const float* w = &n.w1[i * NN_H1]; float v = x[i];
        for (int j = 0; j < NN_H1; j++) h1[j] += w[j] * v;
    }
    for (int j = 0; j < NN_H1; j++) h1[j] = h1[j] > 0 ? h1[j] : 0;
    for (int k = 0; k < NN_H2; k++) h2[k] = n.b2[k];
    for (int j = 0; j < NN_H1; j++) {
        const float* w = &n.w2[j * NN_H2]; float v = h1[j];
        if (v == 0) continue;
        for (int k = 0; k < NN_H2; k++) h2[k] += w[k] * v;
    }
    float out = n.b3;
    for (int k = 0; k < NN_H2; k++) out += n.w3[k] * (h2[k] > 0 ? h2[k] : 0);
    return out;
}

// The compiled-in network (value_weights.inc defines NN_SLOT and NN_W1 ... NN_B3), if any.
#if __has_include("value_weights.inc") && !defined(NN_BIG)
#include "value_weights.inc"
#define NN_BUILTIN 1
#endif

static ValueNet& slot(int k) {
    static ValueNet nets[2];
    static bool init = [] {
#ifdef NN_BUILTIN
        ValueNet& n = nets[NN_SLOT];
        n.w1.assign(NN_W1, NN_W1 + NN_FEATURES * NN_H1); n.b1.assign(NN_B1, NN_B1 + NN_H1);
        n.w2.assign(NN_W2, NN_W2 + NN_H1 * NN_H2); n.b2.assign(NN_B2, NN_B2 + NN_H2);
        n.w3.assign(NN_W3, NN_W3 + NN_H2); n.b3 = NN_B3;
#endif
        return true;
    }();
    (void)init;
    return nets[k];
}

bool nn_ready(int k) { return slot(k).loaded(); }
double nn_danger(u64 board) { return 1.0 / (1.0 + std::exp(-nn_raw(slot(0), board, COMBO_WINDOW))); }
double nn_future(u64 board, int since) { return FUTURE_SCALE * nn_raw(slot(1), board, since); }

bool nn_save(const ValueNet& n, const char* path) {
    FILE* f = fopen(path, "wb"); if (!f) return false;
    auto w = [&](const std::vector<float>& v) { fwrite(v.data(), sizeof(float), v.size(), f); };
    w(n.w1); w(n.b1); w(n.w2); w(n.b2); w(n.w3); fwrite(&n.b3, sizeof(float), 1, f);
    fclose(f); return true;
}

bool nn_load(const char* path, int k) {
    FILE* f = fopen(path, "rb"); if (!f) return false;
    ValueNet n; n.init(0);
    auto r = [&](std::vector<float>& v) { return fread(v.data(), sizeof(float), v.size(), f) == v.size(); };
    bool ok = r(n.w1) && r(n.b1) && r(n.w2) && r(n.b2) && r(n.w3) && fread(&n.b3, sizeof(float), 1, f) == 1;
    fclose(f);
    if (ok) slot(k) = n;
    return ok;
}

void nn_write_include(const ValueNet& n, int k, const char* path) {
    FILE* f = fopen(path, "w"); if (!f) return;
    fprintf(f, "// Generated by train.exe: the v5.9 value network's weights.\n#define NN_SLOT %d\n", k);
    auto arr = [&](const char* name, const std::vector<float>& v) {
        fprintf(f, "static const float %s[%zu] = {", name, v.size());
        for (size_t i = 0; i < v.size(); i++) fprintf(f, "%s%.7g", i == 0 ? "\n  " : i % 12 ? "," : ",\n  ", v[i]);
        fprintf(f, "};\n");
    };
    arr("NN_W1", n.w1); arr("NN_B1", n.b1); arr("NN_W2", n.w2); arr("NN_B2", n.b2); arr("NN_W3", n.w3);
    fprintf(f, "static const float NN_B3 = %.7g;\n", n.b3);
    fclose(f);
}

}  // namespace bb
