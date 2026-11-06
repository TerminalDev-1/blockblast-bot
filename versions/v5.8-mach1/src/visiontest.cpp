// Test tool: reads raw 2880x1800 RGB files and prints what the vision code sees, one line
// per file, for comparison with the Python bot. usage: visiontest file.rgb ...
#include <cstdio>
#include <vector>

#include "vision.hpp"

using namespace bb;

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb"); if (!f) { printf("%s: cannot open\n", argv[i]); continue; }
        Image im; im.w = W; im.h = H; im.rgb.resize((size_t)W * H * 3); im.serial = i;
        size_t got = fread(im.rgb.data(), 1, im.rgb.size(), f); fclose(f);
        if (got != im.rgb.size()) { printf("%s: short\n", argv[i]); continue; }
        forget_theme();
        printf("%s board=%d", argv[i], (int)is_board(im));
        printf(" cells=%016llx", (unsigned long long)read_board(im));
        auto v = visible_board(im, 0);
        printf(" vis=%d", (int)v.ok);
        try {
            auto ps = read_pieces(im); printf(" pieces=");
            for (auto& p : ps) { printf("%d:", p.slot); for (auto [x, y] : p.cells) printf("%d%d", x, y); printf("@%d,%d%s;", p.ox, p.oy, at_rest(p) ? "r" : ""); }
        } catch (std::exception& e) { printf(" pieces=ERR(%s)", e.what()); }
        printf(" green=%d play=%d rating=%d\n", (int)green_button(im), (int)play_button(im), (int)rating_popup(im));
    }
}
