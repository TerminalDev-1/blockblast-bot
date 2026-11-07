// Small self-contained helpers: zlib/gzip decompression (MinGW ships no zlib), a PNG writer
// for saving unrecognised screens, and an RGB image type that keeps screen coordinates.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace bb {

// Decompresses a zlib stream (2-byte header) or a gzip file; throws std::runtime_error.
std::vector<uint8_t> inflate_zlib(const uint8_t* data, size_t n);
std::vector<uint8_t> inflate_gzip(const uint8_t* data, size_t n);

// A rectangle of the screen. Pixel access uses screen coordinates (x0, y0 = top left);
// pixels outside the rectangle read as black.
struct Image {
    int x0 = 0, y0 = 0, w = 0, h = 0;
    std::vector<uint8_t> rgb;  // w*h*3, row by row
    long long frame = -1;      // helper frame number (-1: from screencap)
    uint64_t serial = 0;       // unique per capture (0: unknown), lets readers cache results
    bool empty() const { return w == 0; }
    const uint8_t* px(int x, int y) const {
        static const uint8_t black[3] = {0, 0, 0};
        if (x < x0 || y < y0 || x >= x0 + w || y >= y0 + h) return black;
        return &rgb[((size_t)(y - y0) * w + (x - x0)) * 3];
    }
};

bool write_png(const std::string& path, const Image& im);

double now();                   // seconds (steady clock)
void sleep_s(double seconds);
std::string clock_text(bool tenths = true);  // "HH:MM:SS.t"
void say(const std::string& text);           // print a line and flush
void log(const std::string& text);           // print with a time stamp

}  // namespace bb
