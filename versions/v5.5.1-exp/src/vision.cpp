#include "vision.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace bb {

using Col = std::array<double, 3>;

static double dist(const Col& a, const Col& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// OpenCV's 8-bit RGB -> Lab (sRGB gamma, D65), cached per colour.
static uint32_t lab_of(uint8_t r, uint8_t g, uint8_t b) {
    static std::unordered_map<uint32_t, uint32_t> cache;
    uint32_t key = r << 16 | g << 8 | b;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    auto lin = [](double v) { v /= 255.0; return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    double R = lin(r), G = lin(g), B = lin(b);
    double X = (0.412453 * R + 0.357580 * G + 0.180423 * B) / 0.950456;
    double Y = 0.212671 * R + 0.715160 * G + 0.072169 * B;
    double Z = (0.019334 * R + 0.119193 * G + 0.950227 * B) / 1.088754;
    auto f = [](double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116.0; };
    double L = Y > 0.008856 ? 116.0 * std::cbrt(Y) - 16.0 : 903.3 * Y;
    double A = 500.0 * (f(X) - f(Y)) + 128.0, Bb = 200.0 * (f(Y) - f(Z)) + 128.0;
    auto sat = [](double v) { return (uint32_t)std::clamp((int)std::lround(v), 0, 255); };
    uint32_t v = sat(L * 255.0 / 100.0) << 16 | sat(A) << 8 | sat(Bb);
    if (cache.size() > 2000000) cache.clear();
    return cache[key] = v;
}

static inline int gray(const uint8_t* p) { return (p[0] * 4899 + p[1] * 9617 + p[2] * 1868 + 8192) >> 14; }

static double median(std::vector<double> v) {
    if (v.empty()) return 0;
    size_t n = v.size(); std::sort(v.begin(), v.end());
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

static Col median_col(const std::vector<Col>& cs) {
    Col out;
    for (int c = 0; c < 3; c++) { std::vector<double> v; for (auto& x : cs) v.push_back(x[c]); out[c] = median(v); }
    return out;
}

std::vector<CellStat> cell_stats(const Image& im) {
    static uint64_t cached_serial = 0; static std::vector<CellStat> cached;
    if (im.serial && im.serial == cached_serial) return cached;
    std::vector<CellStat> stats(64);
    for (int cy = 0; cy < 8; cy++)
        for (int cx = 0; cx < 8; cx++) {
            int x0 = GX + cx * STEP, y0 = GY + cy * STEP;
            // Blocks in many themes are raised with a darker bottom edge ("lip"); the row
            // profile is smoothed over 6 px to ignore grid lines.
            double rows[STEP];
            for (int r = 0; r < STEP; r++) {
                int s = 0; for (int x = x0 + 30; x < x0 + 82; x++) s += gray(im.px(x, y0 + r));
                rows[r] = s / 52.0;
            }
            double top = 0; for (int r = 30; r < 70; r++) top += rows[r];
            double low = 1e9;
            for (int i = 76; i < STEP - 6; i++) { double s = 0; for (int k = 0; k < 6; k++) s += rows[i + k]; low = std::min(low, s / 6); }
            Col lab = {0, 0, 0};
            for (int y = y0 + 30; y < y0 + 82; y++)
                for (int x = x0 + 30; x < x0 + 82; x++) {
                    const uint8_t* p = im.px(x, y); uint32_t v = lab_of(p[0], p[1], p[2]);
                    lab[0] += v >> 16; lab[1] += (v >> 8) & 255; lab[2] += v & 255;
                }
            for (auto& c : lab) c /= 52.0 * 52.0;
            // Texture from the middle 32 px only: some themes bevel their blocks.
            double s = 0, s2 = 0;
            for (int y = y0 + 40; y < y0 + 72; y++)
                for (int x = x0 + 40; x < x0 + 72; x++) {
                    const uint8_t* p = im.px(x, y); double L = lab_of(p[0], p[1], p[2]) >> 16;
                    s += L; s2 += L * L;
                }
            double mean = s / 1024.0;
            stats[cy * 8 + cx] = {top / 40.0 - low, lab, std::sqrt(std::max(0.0, s2 / 1024.0 - mean * mean))};
        }
    cached_serial = im.serial; cached = stats;
    return stats;
}

// Greedy clusters of near-identical colours, largest first (indices into cs).
static std::vector<std::vector<int>> colour_groups(const std::vector<Col>& cs, double radius = 20) {
    size_t n = cs.size();
    std::vector<std::vector<char>> near(n, std::vector<char>(n));
    for (size_t i = 0; i < n; i++) for (size_t j = 0; j < n; j++) near[i][j] = dist(cs[i], cs[j]) < radius;
    std::vector<char> left(n, 1); std::vector<std::vector<int>> groups;
    for (size_t remaining = n; remaining;) {
        int seed = -1, bestc = -1;
        for (size_t i = 0; i < n; i++) {
            if (!left[i]) continue;
            int c = 0; for (size_t j = 0; j < n; j++) c += left[j] && near[i][j];
            if (c > bestc) { bestc = c; seed = (int)i; }
        }
        std::vector<int> g;
        for (size_t j = 0; j < n; j++) if (left[j] && near[seed][j]) { g.push_back((int)j); left[j] = 0; remaining--; }
        groups.push_back(g);
    }
    std::stable_sort(groups.begin(), groups.end(), [](auto& a, auto& b) { return a.size() > b.size(); });
    return groups;
}

bool is_board(const Image& im) {
    auto st = cell_stats(im);
    // Board cells have flat centres; menus and pop-ups don't. A few textured cells are
    // allowed for line-clear sparkles and combo text (full-screen ads have 8, menus 13+).
    int textured = 0; for (auto& s : st) textured += s.texture > 12;
    if (textured > 6) return false;
    std::vector<Col> cs; for (auto& s : st) cs.push_back(s.lab);
    return colour_groups(cs)[0].size() >= 4;
}

static std::optional<Col> EMPTY_COLOUR;
void forget_theme() { EMPTY_COLOUR.reset(); }

// Empty cells all share one colour; everything else is a block. The colour is learned
// (the theme changes during a run); hint_empty marks cells known to be empty.
u64 read_board(const Image& im, u64 hint_empty) {
    auto st = cell_stats(im);
    std::vector<Col> cs; for (auto& s : st) cs.push_back(s.lab);
    std::optional<Col> empty;
    std::vector<Col> known; for (int i = 0; i < 64; i++) if (hint_empty >> i & 1) known.push_back(cs[i]);
    if (known.size() >= 3) {
        Col cand = median_col(known); int close = 0;
        for (auto& c : known) close += dist(c, cand) < 20;
        if (close >= 0.8 * known.size()) empty = cand;
    }
    if (!empty && EMPTY_COLOUR) {
        int close = 0; for (auto& c : cs) close += dist(c, *EMPTY_COLOUR) < 20;
        if (close >= 3) empty = EMPTY_COLOUR;
    }
    if (!empty) {
        // Fresh start or new theme: prefer a large group without raised edges.
        auto groups = colour_groups(cs);
        std::vector<std::vector<int>> big; for (auto& g : groups) if (g.size() >= 3) big.push_back(g);
        if (big.empty()) big = groups;
        auto key = [&](const std::vector<int>& g) { double l = 0; for (int i : g) l += st[i].lip; return std::make_pair(l / g.size() > 25, -(int)g.size()); };
        auto best = *std::min_element(big.begin(), big.end(), [&](auto& a, auto& b) { return key(a) < key(b); });
        std::vector<Col> gc; for (int i : best) gc.push_back(cs[i]);
        empty = median_col(gc);
    }
    std::vector<Col> empties; u64 board = 0;
    for (int i = 0; i < 64; i++) {
        if (dist(cs[i], *empty) < 22) empties.push_back(cs[i]);
        else board |= 1ULL << i;
    }
    EMPTY_COLOUR = empties.empty() ? *empty : median_col(empties);
    return board;
}

Visible visible_board(const Image& im, u64 hint_empty) {
    auto st = cell_stats(im);
    std::vector<int> flat; for (int i = 0; i < 64; i++) if (st[i].texture <= 12) flat.push_back(i);
    if (flat.size() < 44) return {false, 0, 0};
    std::vector<Col> fc; for (int i : flat) fc.push_back(st[i].lab);
    if (colour_groups(fc)[0].size() < 4) return {false, 0, 0};
    u64 visible = 0; for (int i : flat) visible |= 1ULL << i;
    return {true, read_board(im, hint_empty & visible), visible};
}

// ---- connected components (8-connected) ----
struct Comp { int x, y, w, h, area; };
static std::vector<Comp> components(const std::vector<uint8_t>& mask, int w, int h) {
    std::vector<int> label(mask.size(), 0); std::vector<Comp> out; std::vector<int> stack;
    for (int i = 0; i < w * h; i++) {
        if (!mask[i] || label[i]) continue;
        int minx = w, miny = h, maxx = -1, maxy = -1, area = 0;
        stack.push_back(i); label[i] = 1;
        while (!stack.empty()) {
            int k = stack.back(); stack.pop_back();
            int x = k % w, y = k / w; area++;
            minx = std::min(minx, x); maxx = std::max(maxx, x); miny = std::min(miny, y); maxy = std::max(maxy, y);
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    int j = ny * w + nx;
                    if (mask[j] && !label[j]) { label[j] = 1; stack.push_back(j); }
                }
        }
        out.push_back({minx, miny, maxx - minx + 1, maxy - miny + 1, area});
    }
    return out;
}

// One tray slot at one threshold: 0 = empty, 1 = piece, -1 = not a clean piece.
static int slot_piece(const Image& im, int slot, double threshold, const Col& bg, TrayPiece& out) {
    int left = TRAYS[slot][0], right = TRAYS[slot][1], cw = right - left, ch = 1610 - 1320;
    std::vector<uint8_t> mask(cw * ch);
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) {
            const uint8_t* p = im.px(left + x, 1320 + y);
            mask[y * cw + x] = dist({(double)p[0], (double)p[1], (double)p[2]}, bg) > threshold;
        }
    std::vector<Comp> comps;
    for (auto& c : components(mask, cw, ch)) if (c.area > 300 && c.w >= 20 && c.h >= 20) comps.push_back(c);
    if (comps.empty()) return 0;
    int x0 = 9999, y0 = 9999, x1 = 0, y1 = 0;
    for (auto& c : comps) { x0 = std::min(x0, c.x); y0 = std::min(y0, c.y); x1 = std::max(x1, c.x + c.w); y1 = std::max(y1, c.y + c.h); }
    if (x1 - x0 > 275 || y1 - y0 > 275) return -1;
    // Tray cells are 50 px; a real piece's box (plus a small shadow) fits that grid.
    for (int d : {x1 - x0, y1 - y0}) if (std::min(d % 50, 50 - d % 50) > 14) return -1;
    Cells cells;
    int ny = (int)std::lround((y1 - y0) / 50.0), nx = (int)std::lround((x1 - x0) / 50.0);
    for (int y = 0; y < ny; y++)
        for (int x = 0; x < nx; x++) {
            int py0 = y0 + y * 50 + 5, py1 = std::min(ch, y0 + y * 50 + 45), px0 = x0 + x * 50 + 5, px1 = std::min(cw, x0 + x * 50 + 45);
            if (py1 <= py0 || px1 <= px0) continue;
            int on = 0; for (int yy = py0; yy < py1; yy++) for (int xx = px0; xx < px1; xx++) on += mask[yy * cw + xx];
            if (on > 0.40 * (py1 - py0) * (px1 - px0)) cells.push_back({x, y});
        }
    if (cells.empty() || cells.size() > 9) return -1;
    out = {slot, cells, left + x0, 1320 + y0};
    return 1;
}

std::vector<TrayPiece> read_pieces(const Image& im) {
    std::vector<TrayPiece> pieces;
    for (int slot = 0; slot < 3; slot++) {
        int left = TRAYS[slot][0], right = TRAYS[slot][1];
        // Background and its noise come from the strip just above the tray, which pieces
        // never reach. Some themes draw pieces close to the background colour (pink on pink),
        // others are textured (wood), so the threshold adapts and others are tried.
        std::vector<Col> strip;
        for (int y = 1290; y < 1325; y++) for (int x = left; x < right; x++) { const uint8_t* p = im.px(x, y); strip.push_back({(double)p[0], (double)p[1], (double)p[2]}); }
        Col bg = median_col(strip);
        std::vector<double> d; for (auto& c : strip) d.push_back(dist(c, bg));
        std::sort(d.begin(), d.end());
        double idx = 0.99 * (d.size() - 1); size_t lo = (size_t)idx;
        double noise = d[lo] + (d[std::min(lo + 1, d.size() - 1)] - d[lo]) * (idx - lo);
        double adaptive = std::min(45.0, std::max(18.0, noise * 1.6 + 6));
        // Wood grain can add cells at a low threshold, giving a shape the game doesn't
        // have; keep trying until a real piece shape comes out (else the first reading).
        int result = -1, fallback = -1; TrayPiece rp{}, fp{};
        for (double t : {adaptive, 45.0, 30.0, 24.0, 60.0, 18.0, 80.0, 100.0, 120.0}) {
            if (t < noise * 1.2) continue;
            TrayPiece p{}; int r = slot_piece(im, slot, t, bg, p);
            if (r == -1) continue;
            if (r == 0 || shape_index(p.cells) >= 0) { result = r; rp = p; break; }
            if (fallback == -1) { fallback = r; fp = p; }
        }
        if (result == -1) { result = fallback; rp = fp; }
        if (result == -1) throw UnreadableTray("Unrecognized piece in tray " + std::to_string(slot + 1) + ".");
        if (result == 1) pieces.push_back(rp);
    }
    return pieces;
}

bool at_rest(const TrayPiece& p) {
    int w = 0, h = 0; for (auto [x, y] : p.cells) { w = std::max(w, x + 1); h = std::max(h, y + 1); }
    double dx = p.ox + w * 25 - (TRAYS[p.slot][0] + TRAYS[p.slot][1]) / 2.0, cy = p.oy + h * 25;
    return std::abs(cy - REST_Y) <= 6 && std::abs(dx - REST_DX[p.slot]) <= 6;
}

// OpenCV 8-bit HSV: H 0-180, S and V 0-255.
static void hsv(const uint8_t* p, int& H, int& S, int& V) {
    int r = p[0], g = p[1], b = p[2], mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    V = mx; S = mx ? (int)std::lround(255.0 * d / mx) : 0;
    double h = 0;
    if (d) { if (mx == r) h = 60.0 * (g - b) / d; else if (mx == g) h = 120.0 + 60.0 * (b - r) / d; else h = 240.0 + 60.0 * (r - g) / d; }
    if (h < 0) h += 360;
    H = (int)std::lround(h / 2) % 180;
}

bool green_button(const Image& im) {
    int on = 0, n = 0;
    for (int y = 1270; y < 1460; y++)
        for (int x = 1140; x < 1740; x++) {
            int Hh, S, V; hsv(im.px(x, y), Hh, S, V); n++;
            on += Hh > 35 && Hh < 90 && S > 100 && V > 110;
        }
    return on > 0.30 * n;
}

bool play_button(const Image& im) {
    // The results screen uses a white triangular play icon.
    int w = 1630 - 1250, h = 1460 - 1260; std::vector<uint8_t> mask(w * h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int Hh, S, V; hsv(im.px(1250 + x, 1260 + y), Hh, S, V); mask[y * w + x] = V > 200 && S < 60; }
    for (auto& c : components(mask, w, h))
        if (c.w >= 65 && c.w <= 110 && c.h >= 75 && c.h <= 110 && c.area >= 3000 && c.area <= 7000) return true;
    return false;
}

bool rating_popup(const Image& im) {
    double a[3] = {0, 0, 0}, b = 0; int na = 0, nb = 0;
    for (int y = 490; y < 505; y++) for (int x = 1080; x < 1200; x++) { const uint8_t* p = im.px(x, y); for (int c = 0; c < 3; c++) a[c] += p[c]; na++; }
    for (int y = 510; y < 550; y++) for (int x = 1750; x < 1800; x++) { const uint8_t* p = im.px(x, y); b += p[0] + p[1] + p[2]; nb += 3; }
    double red = a[0] / na, blue = a[2] / na;  // RGB order here
    return blue > 150 && blue > red * 1.15 && b / nb > 110 && !is_board(im);
}

}  // namespace bb
