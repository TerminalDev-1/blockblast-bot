#include "util.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <thread>

namespace bb {

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void sleep_s(double s) { if (s > 0) std::this_thread::sleep_for(std::chrono::duration<double>(s)); }

std::string clock_text(bool tenths) {
    auto t = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm; localtime_s(&tm, &tt);
    char buf[32];
    int ds = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count() % 1000 / 100);
    if (tenths) snprintf(buf, sizeof buf, "%02d:%02d:%02d.%d", tm.tm_hour, tm.tm_min, tm.tm_sec, ds);
    else snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

void say(const std::string& text) { fputs(text.c_str(), stdout); fputc('\n', stdout); fflush(stdout); }
void log(const std::string& text) { say(clock_text() + " " + text); }

// ---- inflate (RFC 1951), a straightforward canonical-Huffman decoder ----
namespace {
struct Bits {
    const uint8_t* p; size_t n, pos = 0; uint32_t buf = 0; int cnt = 0;
    int bit() {
        if (!cnt) { if (pos >= n) throw std::runtime_error("inflate: out of data"); buf = p[pos++]; cnt = 8; }
        int b = buf & 1; buf >>= 1; cnt--; return b;
    }
    int bits(int k) { int v = 0; for (int i = 0; i < k; i++) v |= bit() << i; return v; }
};
struct Huff {
    short count[16] = {0}, symbol[320] = {0};
    void build(const uint8_t* len, int n) {
        memset(count, 0, sizeof count);
        for (int i = 0; i < n; i++) count[len[i]]++;
        count[0] = 0;
        short offs[16]; offs[1] = 0;
        for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + count[i];
        for (int i = 0; i < n; i++) if (len[i]) symbol[offs[len[i]]++] = (short)i;
    }
    int decode(Bits& s) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= s.bit();
            int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c; first += c; first <<= 1; code <<= 1;
        }
        throw std::runtime_error("inflate: bad code");
    }
};
const short LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const short DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void codes(Bits& s, std::vector<uint8_t>& out, const Huff& lc, const Huff& dc) {
    for (;;) {
        int sym = lc.decode(s);
        if (sym < 256) out.push_back((uint8_t)sym);
        else if (sym == 256) return;
        else {
            sym -= 257; if (sym >= 29) throw std::runtime_error("inflate: bad length");
            int len = LBASE[sym] + s.bits(LEXT[sym]);
            int ds = dc.decode(s); if (ds >= 30) throw std::runtime_error("inflate: bad distance");
            size_t dist = DBASE[ds] + s.bits(DEXT[ds]);
            if (dist > out.size()) throw std::runtime_error("inflate: distance too far");
            size_t from = out.size() - dist;
            for (int i = 0; i < len; i++) out.push_back(out[from + i]);
        }
    }
}

std::vector<uint8_t> inflate_raw(const uint8_t* data, size_t n, size_t hint) {
    Bits s{data, n};
    std::vector<uint8_t> out; out.reserve(hint);
    static Huff fixl, fixd; static bool fixed = false;
    if (!fixed) {
        uint8_t l[288];
        for (int i = 0; i < 144; i++) l[i] = 8;
        for (int i = 144; i < 256; i++) l[i] = 9;
        for (int i = 256; i < 280; i++) l[i] = 7;
        for (int i = 280; i < 288; i++) l[i] = 8;
        fixl.build(l, 288);
        for (int i = 0; i < 30; i++) l[i] = 5;
        fixd.build(l, 30); fixed = true;
    }
    int last;
    do {
        last = s.bit();
        int type = s.bits(2);
        if (type == 0) {
            s.cnt = 0;  // byte align
            if (s.pos + 4 > n) throw std::runtime_error("inflate: bad stored block");
            unsigned len = data[s.pos] | data[s.pos + 1] << 8; s.pos += 4;
            if (s.pos + len > n) throw std::runtime_error("inflate: stored block too long");
            out.insert(out.end(), data + s.pos, data + s.pos + len); s.pos += len;
        } else if (type == 1) {
            codes(s, out, fixl, fixd);
        } else if (type == 2) {
            int nlen = s.bits(5) + 257, ndist = s.bits(5) + 1, ncode = s.bits(4) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lengths[320] = {0};
            for (int i = 0; i < ncode; i++) lengths[order[i]] = (uint8_t)s.bits(3);
            Huff lencode; lencode.build(lengths, 19);
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = lencode.decode(s);
                if (sym < 16) lengths[idx++] = (uint8_t)sym;
                else {
                    int len = 0, rep;
                    if (sym == 16) { if (!idx) throw std::runtime_error("inflate: repeat with no length"); len = lengths[idx - 1]; rep = 3 + s.bits(2); }
                    else if (sym == 17) rep = 3 + s.bits(3);
                    else rep = 11 + s.bits(7);
                    if (idx + rep > nlen + ndist) throw std::runtime_error("inflate: too many lengths");
                    while (rep--) lengths[idx++] = (uint8_t)len;
                }
            }
            Huff lc, dc; lc.build(lengths, nlen); dc.build(lengths + nlen, ndist);
            codes(s, out, lc, dc);
        } else throw std::runtime_error("inflate: bad block type");
    } while (!last);
    return out;
}
}  // namespace

std::vector<uint8_t> inflate_zlib(const uint8_t* data, size_t n) {
    if (n < 2 || (data[0] & 0x0F) != 8) throw std::runtime_error("not a zlib stream");
    return inflate_raw(data + 2, n - 2, n * 8);
}

std::vector<uint8_t> inflate_gzip(const uint8_t* data, size_t n) {
    if (n < 18 || data[0] != 0x1F || data[1] != 0x8B) throw std::runtime_error("not gzip data");
    int flags = data[3]; size_t pos = 10;
    if (flags & 4) pos += 2 + (data[pos] | data[pos + 1] << 8);
    if (flags & 8) while (pos < n && data[pos++]) {}
    if (flags & 16) while (pos < n && data[pos++]) {}
    if (flags & 2) pos += 2;
    if (pos >= n) throw std::runtime_error("bad gzip header");
    return inflate_raw(data + pos, n - pos, n * 10);
}

// ---- PNG writer (uncompressed deflate blocks: simple and fast) ----
static uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
    static uint32_t table[256]; static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320 ^ (v >> 1) : v >> 1; table[i] = v; }
        ready = true;
    }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

bool write_png(const std::string& path, const Image& im) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(im.w * 3 + 1) * im.h);
    for (int y = 0; y < im.h; y++) { raw.push_back(0); raw.insert(raw.end(), &im.rgb[(size_t)y * im.w * 3], &im.rgb[(size_t)(y + 1) * im.w * 3]); }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t i = 0; i < raw.size(); i += 65535) {
        size_t len = std::min<size_t>(65535, raw.size() - i);
        z.push_back(i + len >= raw.size() ? 1 : 0);
        z.push_back(len & 0xFF); z.push_back(len >> 8); z.push_back(~len & 0xFF); z.push_back((~len >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + i, raw.begin() + i + len);
    }
    uint32_t ad = b << 16 | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back((ad >> s) & 0xFF);
    FILE* f = fopen(path.c_str(), "wb"); if (!f) return false;
    auto chunk = [&](const char* type, const std::vector<uint8_t>& d) {
        uint8_t len[4] = {uint8_t(d.size() >> 24), uint8_t(d.size() >> 16), uint8_t(d.size() >> 8), uint8_t(d.size())};
        fwrite(len, 1, 4, f);
        std::vector<uint8_t> td(type, type + 4); td.insert(td.end(), d.begin(), d.end());
        fwrite(td.data(), 1, td.size(), f);
        uint32_t c = crc32(td.data(), td.size());
        uint8_t cb[4] = {uint8_t(c >> 24), uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)};
        fwrite(cb, 1, 4, f);
    };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr = {uint8_t(im.w >> 24), uint8_t(im.w >> 16), uint8_t(im.w >> 8), uint8_t(im.w),
                                 uint8_t(im.h >> 24), uint8_t(im.h >> 16), uint8_t(im.h >> 8), uint8_t(im.h), 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr); chunk("IDAT", z); chunk("IEND", {});
    fclose(f);
    return true;
}

}  // namespace bb
