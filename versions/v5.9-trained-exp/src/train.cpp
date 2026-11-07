// v5.9-trained-exp trainer (Adam, mini-batches), two modes:
//   train out.bin epochs data.bin...            danger net: board -> lost within HORIZON trays
//                                               (binary cross-entropy, danger boards weighted up)
//   train out.bin epochs --future rec.bin...    future net: (board, combo state) -> v5.8's
//                                               look-ahead verdict (squared error, distillation)
// Writes out.bin (for sim/datagen rounds) and value_weights.inc (compiled into the bot).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "planner.hpp"
#include "value.hpp"

using namespace bb;

struct Sample { uint64_t board; int8_t since; float y; };

static double auc(std::vector<std::pair<double, int>> sc) {
    std::sort(sc.begin(), sc.end());
    double rank = 0, npos = 0, nneg = 0;
    for (size_t i = 0; i < sc.size(); i++) if (sc[i].second) { rank += i; npos++; } else nneg++;
    return npos && nneg ? (rank - npos * (npos - 1) / 2) / (npos * nneg) : 0;
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: train out.bin epochs [--future] data.bin...\n"); return 1; }
    const char* out = argv[1]; int epochs = atoi(argv[2]);
    int first = 3; bool future = false;
    if (!strcmp(argv[3], "--future")) { future = true; first = 4; }
    std::vector<Sample> data;
    for (int i = first; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb"); if (!f) { fprintf(stderr, "can't read %s\n", argv[i]); return 1; }
        Sample s;
        if (future) {
            while (fread(&s.board, 8, 1, f) == 1 && fread(&s.since, 1, 1, f) == 1 && fread(&s.y, 4, 1, f) == 1) {
                s.y = (float)(s.y / FUTURE_SCALE); data.push_back(s);
            }
        } else {
            uint8_t lab;
            while (fread(&s.board, 8, 1, f) == 1 && fread(&lab, 1, 1, f) == 1) { s.since = COMBO_WINDOW; s.y = lab; data.push_back(s); }
        }
        fclose(f);
    }
    std::mt19937_64 rng(12345);
    std::shuffle(data.begin(), data.end(), rng);
    size_t nval = data.size() / 10;
    std::vector<Sample> val(data.begin(), data.begin() + nval), tr(data.begin() + nval, data.end());
    double pw = 1.0;
    if (!future) {
        long long pos = 0; for (auto& r : tr) pos += r.y > 0.5f;
        pw = pos ? (double)(tr.size() - pos) / pos : 1.0;
        printf("%zu train (%lld danger, weight %.1f), %zu validation\n", tr.size(), pos, pw, val.size());
        std::vector<std::pair<double, int>> sc;
        for (auto& r : val) sc.push_back({-fit_eval(r.board), r.y > 0.5f});
        printf("reference: v5.8 fit_eval AUC %.4f\n", auc(sc));
    } else {
        double m = 0, v = 0; for (auto& r : val) m += r.y; m /= val.size();
        for (auto& r : val) v += (r.y - m) * (r.y - m); v /= val.size();
        printf("%zu train, %zu validation; verdict spread (std) %.2f\n", tr.size(), val.size(), std::sqrt(v) * FUTURE_SCALE);
    }

    auto feats = [](const std::vector<Sample>& v) {
        std::vector<float> x(v.size() * NN_FEATURES);
        for (size_t i = 0; i < v.size(); i++) nn_features(v[i].board, v[i].since, &x[i * NN_FEATURES]);
        return x;
    };
    std::vector<float> xtr = feats(tr), xval = feats(val);

    ValueNet net; net.init(7);
    std::vector<float*> P = {net.w1.data(), net.b1.data(), net.w2.data(), net.b2.data(), net.w3.data(), &net.b3};
    std::vector<size_t> S = {net.w1.size(), net.b1.size(), net.w2.size(), net.b2.size(), net.w3.size(), 1};
    std::vector<std::vector<float>> G(6), M1(6), M2(6);
    for (int k = 0; k < 6; k++) { G[k].assign(S[k], 0); M1[k].assign(S[k], 0); M2[k].assign(S[k], 0); }
    const float lr0 = 1e-3f, b1 = 0.9f, b2 = 0.999f, eps = 1e-8f; long long step = 0;
    const int batch = 256;

    auto forward = [&](const float* x, float* h1, float* h2) {
        for (int j = 0; j < NN_H1; j++) { float s = net.b1[j]; for (int i = 0; i < NN_FEATURES; i++) s += x[i] * net.w1[i * NN_H1 + j]; h1[j] = s > 0 ? s : 0; }
        for (int k = 0; k < NN_H2; k++) { float s = net.b2[k]; for (int j = 0; j < NN_H1; j++) s += h1[j] * net.w2[j * NN_H2 + k]; h2[k] = s > 0 ? s : 0; }
        float o = net.b3; for (int k = 0; k < NN_H2; k++) o += h2[k] * net.w3[k];
        return o;
    };
    auto evaluate = [&]() {
        double loss = 0; float h1[NN_H1], h2[NN_H2];
        std::vector<std::pair<double, int>> sc;
        double sxy = 0, sx = 0, sy = 0, sxx = 0, syy = 0;
        for (size_t i = 0; i < val.size(); i++) {
            float o = forward(&xval[i * NN_FEATURES], h1, h2);
            float y = val[i].y;
            if (future) {
                loss += (o - y) * (o - y);
                sxy += o * y; sx += o; sy += y; sxx += o * o; syy += y * y;
            } else {
                float p = 1 / (1 + std::exp(-o));
                loss -= (y > 0.5f ? pw * std::log(p + 1e-7) : std::log(1 - p + 1e-7));
                sc.push_back({p, y > 0.5f});
            }
        }
        double n = (double)val.size();
        double quality = future ? (n * sxy - sx * sy) / std::sqrt((n * sxx - sx * sx) * (n * syy - sy * sy)) : auc(sc);
        return std::make_pair(loss / n, quality);
    };

    std::vector<size_t> idx(tr.size()); for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    float h1[NN_H1], h2[NN_H2], d2[NN_H2], d1[NN_H1];
    for (int ep = 0; ep < epochs; ep++) {
        std::shuffle(idx.begin(), idx.end(), rng);
        float lr = lr0 * (ep < epochs / 2 ? 1.f : ep < epochs * 3 / 4 ? 0.3f : 0.1f);
        for (size_t s0 = 0; s0 < idx.size(); s0 += batch) {
            for (auto& g : G) std::fill(g.begin(), g.end(), 0.f);
            size_t s1 = std::min(idx.size(), s0 + batch);
            for (size_t s = s0; s < s1; s++) {
                const float* x = &xtr[idx[s] * NN_FEATURES]; float y = tr[idx[s]].y;
                float o = forward(x, h1, h2);
                float d;
                if (future) d = 2 * (o - y);
                else { float p = 1 / (1 + std::exp(-o)); d = (float)((p - y) * (y > 0.5f ? pw : 1.0)); }
                G[5][0] += d;
                for (int k = 0; k < NN_H2; k++) { G[4][k] += d * h2[k]; d2[k] = h2[k] > 0 ? d * net.w3[k] : 0; }
                for (int j = 0; j < NN_H1; j++) {
                    float a = 0;
                    for (int k = 0; k < NN_H2; k++) { G[2][j * NN_H2 + k] += h1[j] * d2[k]; a += net.w2[j * NN_H2 + k] * d2[k]; }
                    d1[j] = h1[j] > 0 ? a : 0;
                }
                for (int k = 0; k < NN_H2; k++) G[3][k] += d2[k];
                for (int j = 0; j < NN_H1; j++) G[1][j] += d1[j];
                for (int i = 0; i < NN_FEATURES; i++) { if (x[i] == 0) continue; for (int j = 0; j < NN_H1; j++) G[0][i * NN_H1 + j] += x[i] * d1[j]; }
            }
            step++;
            float n = (float)(s1 - s0);
            float c1 = 1 - std::pow(b1, (float)step), c2 = 1 - std::pow(b2, (float)step);
            for (int k = 0; k < 6; k++)
                for (size_t i = 0; i < S[k]; i++) {
                    float g = G[k][i] / n;
                    M1[k][i] = b1 * M1[k][i] + (1 - b1) * g; M2[k][i] = b2 * M2[k][i] + (1 - b2) * g * g;
                    P[k][i] -= lr * (M1[k][i] / c1) / (std::sqrt(M2[k][i] / c2) + eps);
                }
        }
        auto [loss, q] = evaluate();
        if (future) printf("epoch %d: validation error %.2f (rms), correlation %.4f\n", ep + 1, std::sqrt(loss) * FUTURE_SCALE, q);
        else printf("epoch %d: validation loss %.4f, AUC %.4f\n", ep + 1, loss, q);
        fflush(stdout);
    }
    nn_save(net, out);
    nn_write_include(net, future ? 1 : 0, "value_weights.inc");
    printf("saved %s and value_weights.inc\n", out);
}
