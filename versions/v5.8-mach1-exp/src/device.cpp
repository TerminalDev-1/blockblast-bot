#include "device.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <regex>
#include <sstream>

#include "vision.hpp"

namespace bb {

double SCALE = 1.0;
int SHOTS = 0;
int GAP_MS = 10;
int BATCH_GAP_MS = 40;
int HOLD_MS = 0;
static std::string SERIAL;
static double SLOW = 1;  // timeouts are stretched over Wi-Fi
static const char* HELPER_PORT = "27195";

// ---- processes ----
static std::string quote(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string q = "\"";
    for (size_t i = 0; i < a.size(); i++) {
        size_t bs = 0;
        while (i < a.size() && a[i] == '\\') { bs++; i++; }
        if (i == a.size()) { q.append(bs * 2, '\\'); break; }
        if (a[i] == '"') { q.append(bs * 2 + 1, '\\'); q += '"'; }
        else { q.append(bs, '\\'); q += a[i]; }
    }
    return q + "\"";
}

struct Proc { HANDLE process = nullptr, out = nullptr; };

static Proc spawn(const std::vector<std::string>& args) {
    std::string cmd;
    for (auto& a : args) cmd += (cmd.empty() ? "" : " ") + quote(a);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) throw AdbError("CreatePipe failed");
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOA si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul; si.hStdOutput = wr; si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr); CloseHandle(nul);
    if (!ok) { CloseHandle(rd); throw AdbError("cannot start " + args[0]); }
    CloseHandle(pi.hThread);
    return {pi.hProcess, rd};
}

static void drain(HANDLE h, std::string& out) {
    DWORD avail = 0;
    while (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) && avail) {
        std::string chunk(avail, '\0'); DWORD got = 0;
        if (!ReadFile(h, chunk.data(), avail, &got, nullptr) || !got) break;
        out.append(chunk.data(), got);
    }
}

std::string run(const std::vector<std::string>& args, double timeout, bool check, int* code) {
    Proc p = spawn(args);
    std::string out; double end = now() + timeout;
    // Poll the pipe instead of blocking on it: a background adb server started by this
    // command inherits the pipe and would keep a blocking read open forever.
    while (WaitForSingleObject(p.process, 2) == WAIT_TIMEOUT) {
        drain(p.out, out);
        if (now() > end) {
            TerminateProcess(p.process, 1); CloseHandle(p.process); CloseHandle(p.out);
            throw Timeout("timed out: " + args[0] + " " + (args.size() > 1 ? args[1] : ""));
        }
    }
    drain(p.out, out);
    DWORD ec = 0; GetExitCodeProcess(p.process, &ec);
    CloseHandle(p.process); CloseHandle(p.out);
    if (code) *code = (int)ec;
    if (check && ec) throw AdbError("adb failed (" + std::to_string(ec) + "): " + (args.size() > 1 ? args[1] : ""));
    return out;
}

std::string adb(const std::vector<std::string>& args, double timeout, bool check) {
    std::vector<std::string> full = {"adb"};
    if (!SERIAL.empty()) { full.push_back("-s"); full.push_back(SERIAL); }
    full.insert(full.end(), args.begin(), args.end());
    return run(full, timeout, check);
}

// ---- which tablet ----
static bool is_wireless(const std::string& s) { return s.find(':') != std::string::npos || s.find("._adb") != std::string::npos; }
bool wireless() { return is_wireless(SERIAL); }

std::string ensure_device() {
    bool said = false;
    for (;;) {
        std::vector<std::string> usb, wifi;
        try {
            std::string out = run({"adb", "devices"}, 15, false);
            std::istringstream in(out); std::string line; std::getline(in, line);
            while (std::getline(in, line)) {
                std::istringstream ls(line); std::string serial, state; ls >> serial >> state;
                if (state == "device") (is_wireless(serial) ? wifi : usb).push_back(serial);
            }
        } catch (std::exception&) {}
        std::string pick = !usb.empty() ? usb[0] : !wifi.empty() ? wifi[0] : "";
        if (!pick.empty()) {
            if (pick != SERIAL) say(std::string("Using tablet over ") + (is_wireless(pick) ? "Wi-Fi" : "USB") + " (" + pick + ").");
            SERIAL = pick; SLOW = is_wireless(pick) ? 3 : 1;
            return pick;
        }
        // Connect to any paired tablet that announces itself on the network.
        try {
            std::string out = run({"adb", "mdns", "services"}, 10, false);
            std::regex re(R"(_adb-tls-connect\._tcp\.?\s+(\d+\.\d+\.\d+\.\d+:\d+))");
            for (std::sregex_iterator it(out.begin(), out.end(), re), e; it != e; ++it) run({"adb", "connect", (*it)[1]}, 10, false);
        } catch (std::exception&) {}
        if (!said) { say("Waiting for the tablet (USB cable, or Wireless debugging on the same Wi-Fi)..."); said = true; }
        sleep_s(3);
    }
}

void open_app(const std::string& package) {
    adb({"shell", "monkey", "-p", package, "-c", "android.intent.category.LAUNCHER", "1"}, 20, false);
}

void key_back() { adb({"shell", "input", "keyevent", "BACK"}); }

std::string focused_window() {
    std::string out = adb({"shell", "dumpsys", "window"}, 20, false);
    // The first mCurrentFocus line with a window (dumpsys lists one per display).
    std::smatch m;
    for (size_t at = out.find("mCurrentFocus="); at != std::string::npos; at = out.find("mCurrentFocus=", at + 1)) {
        std::string line = out.substr(at, out.find('\n', at) - at);
        if (std::regex_search(line, m, std::regex(R"( u\d+ ([\w.$/]+)\})"))) return m[1];
    }
    return "";
}

std::string focused_package() {
    // The game's own dialogs (e.g. its "Leave now?" prompt) are named by the package alone,
    // without "/activity"; v5.4 took them for another app and kept pressing Back.
    std::string w = focused_window();
    return w.substr(0, w.find('/'));
}

bool close_accessible_ad() {
    try {
        adb({"shell", "uiautomator", "dump", "/sdcard/bot-window.xml"}, 20);
        std::string xml = adb({"shell", "cat", "/sdcard/bot-window.xml"}, 20);
        static const std::vector<std::string> ok = {"close", "dismiss", "skip", "close ad", "dismiss ad", "close interstitial", "skip ad", "x",
                                                    "cancel", "no", "not now", "stay", "keep playing", "continue playing", "resume"};
        std::regex node(R"(<node [^>]*>)"), attr(R"((text|content-desc|bounds)=\"([^\"]*)\")");
        for (std::sregex_iterator it(xml.begin(), xml.end(), node), e; it != e; ++it) {
            std::string tag = it->str(), text, desc, bounds;
            for (std::sregex_iterator a(tag.begin(), tag.end(), attr); a != e; ++a) {
                std::string k = (*a)[1], v = (*a)[2];
                if (k == "text") text = v; else if (k == "content-desc") desc = v; else bounds = v;
            }
            std::string label = text + " " + desc;
            label.erase(0, label.find_first_not_of(' ')); label.erase(label.find_last_not_of(' ') + 1);
            std::transform(label.begin(), label.end(), label.begin(), ::tolower);
            // Ad close buttons, and the safe answer in the game's own dialogs: never
            // anything that leaves or quits.
            if (label.find("leave") != std::string::npos || label.find("exit") != std::string::npos || label.find("quit") != std::string::npos) continue;
            if (std::find(ok.begin(), ok.end(), label) == ok.end()) continue;
            int x1, y1, x2, y2;
            if (sscanf(bounds.c_str(), "[%d,%d][%d,%d]", &x1, &y1, &x2, &y2) != 4) continue;
            if (x2 > x1 && y2 > y1 && y1 < 1620) {
                say("Closing ad: " + label);
                adb({"shell", "input", "tap", std::to_string((x1 + x2) / 2), std::to_string((y1 + y2) / 2)});
                return true;
            }
        }
    } catch (std::exception&) {}
    return false;
}

// ---- tablet helper ----
static SOCKET SOCK = INVALID_SOCKET;
static Proc HELPER_PROC;
static double HELPER_RETRY_AT = 0;
static double LAST_UP = 0;
static uint64_t SERIAL_NO = 0;

static void helper_stop() {
    if (SOCK != INVALID_SOCKET) { closesocket(SOCK); SOCK = INVALID_SOCKET; }
    if (HELPER_PROC.process) { TerminateProcess(HELPER_PROC.process, 1); CloseHandle(HELPER_PROC.process); CloseHandle(HELPER_PROC.out); HELPER_PROC = {}; }
}

bool helper_ok() { return SOCK != INVALID_SOCKET; }

static void send_line(const std::string& s) {
    std::string l = s + "\n";
    for (size_t sent = 0; sent < l.size();) {
        int n = send(SOCK, l.data() + sent, (int)(l.size() - sent), 0);
        if (n <= 0) throw AdbError("helper connection lost");
        sent += n;
    }
}

static std::string recv_line() {
    std::string l; char c;
    for (;;) {
        int n = recv(SOCK, &c, 1, 0);
        if (n <= 0) throw AdbError("helper connection lost");
        if (c == '\n') return l;
        l += c;
    }
}

static void recv_exact(char* buf, size_t n) {
    for (size_t got = 0; got < n;) {
        int k = recv(SOCK, buf + got, (int)std::min<size_t>(n - got, 1 << 20), 0);
        if (k <= 0) throw AdbError("helper connection lost");
        got += k;
    }
}

static void set_timeout(double s) { DWORD ms = (DWORD)(s * 1000); setsockopt(SOCK, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof ms); }

static std::string call(const std::string& cmd) {
    send_line(cmd);
    std::string l = recv_line();
    if (!l.empty() && l[0] == 'E') throw AdbError("helper: " + l);
    return l;
}

static bool helper_start() {
    if (helper_ok()) return true;
    if (now() < HELPER_RETRY_AT) return false;
    HELPER_RETRY_AT = now() + 60;
    try {
        static bool wsa = false;
        if (!wsa) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); wsa = true; }
        adb({"shell", "pkill -f BBHelper"}, 10, false);
        adb({"push", helper_dex_path(), "/data/local/tmp/bbhelper.dex"}, 30);
        adb({"forward", std::string("tcp:") + HELPER_PORT, "localabstract:bbhelper"}, 10);
        std::vector<std::string> args = {"adb"};
        if (!SERIAL.empty()) { args.push_back("-s"); args.push_back(SERIAL); }
        args.push_back("shell");
        args.push_back("CLASSPATH=/data/local/tmp/bbhelper.dex exec app_process / BBHelper " + std::to_string(W) + " " + std::to_string(H));
        HELPER_PROC = spawn(args);
        std::string out; double end = now() + 15;
        while (out.find('\n') == std::string::npos) {
            drain(HELPER_PROC.out, out);
            if (WaitForSingleObject(HELPER_PROC.process, 20) != WAIT_TIMEOUT) throw AdbError("helper exited: " + out);
            if (now() > end) throw Timeout("helper did not start");
        }
        if (out.rfind("ready", 0) != 0) throw AdbError("helper: " + out);
        addrinfo hints{}, *res = nullptr; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        getaddrinfo("127.0.0.1", HELPER_PORT, &hints, &res);
        SOCK = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (connect(SOCK, res->ai_addr, (int)res->ai_addrlen)) { freeaddrinfo(res); closesocket(SOCK); SOCK = INVALID_SOCKET; throw AdbError("cannot connect to helper"); }
        freeaddrinfo(res);
        int one = 1; setsockopt(SOCK, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
        set_timeout(3 * SLOW);
        call("ping");
        say("Tablet helper running (fast screenshots and swipes).");
        release_touch();
        return true;
    } catch (std::exception& e) {
        say(std::string("Tablet helper unavailable (") + e.what() + "); using screencap and input.");
        helper_stop();
        return false;
    }
}

// A tap on the empty strip left of the game: if a drag was cut off (the bot stopped while
// the finger was down), the game is left holding a piece over the tray and nothing can be
// read; any new touch ends that.
void release_touch() {
    if (!helper_ok()) return;
    try { call("swipe 150 900 150 900 0 20"); } catch (std::exception&) {}
}

void helper_restart() { helper_stop(); HELPER_RETRY_AT = 0; helper_start(); }

static void helper_failed(const std::exception& e) {
    log(std::string("Tablet helper failed (") + e.what() + "); falling back for now.");
    helper_stop();
}

// Screen rectangle from the helper's live mirror.
static Image helper_frame(int x0, int y0, int x1, int y1, int still_hold = -1, int still_timeout = 0, bool* timed_out = nullptr) {
    std::string cmd = still_hold >= 0 ? "still " : "cap ";
    cmd += std::to_string(x0) + " " + std::to_string(y0) + " " + std::to_string(x1 - x0) + " " + std::to_string(y1 - y0);
    if (still_hold >= 0) { cmd += " " + std::to_string(still_hold) + " " + std::to_string(still_timeout); set_timeout(still_timeout / 1000.0 + 3 * SLOW); }
    std::string head = call(cmd);
    if (still_hold >= 0) set_timeout(3 * SLOW);
    int w, h, to = 0; long long n, age, frame;
    if (sscanf(head.c_str(), "F %d %d %lld %lld %lld %d", &w, &h, &n, &age, &frame, &to) < 5) throw AdbError("helper: bad reply " + head);
    if (timed_out) *timed_out = to != 0;
    std::string data(n, '\0'); recv_exact(data.data(), n);
    Image im; im.x0 = x0; im.y0 = y0; im.w = w; im.h = h; im.frame = frame;
    im.rgb = inflate_zlib((const uint8_t*)data.data(), data.size());
    if (im.rgb.size() != (size_t)w * h * 3) throw AdbError("helper: bad frame size");
    return im;
}

// Whole screen via screencap (raw pixels gzipped on the tablet, ~2.5x faster than PNG).
static Image screencap() {
    std::string z = adb({"exec-out", "screencap | gzip -1"}, 4 * SLOW);
    auto data = inflate_gzip((const uint8_t*)z.data(), z.size());
    if (data.size() < 12) throw AdbError("short screenshot");
    uint32_t w, h; memcpy(&w, data.data(), 4); memcpy(&h, data.data() + 4, 4);
    long header = (long)data.size() - (long)w * h * 4;
    if (w != (uint32_t)W || h != (uint32_t)H || header < 12 || header > 32) throw AdbError("unexpected raw screenshot");
    Image im; im.w = W; im.h = H; im.rgb.resize((size_t)W * H * 3);
    const uint8_t* p = data.data() + header;
    for (size_t i = 0; i < (size_t)W * H; i++) { im.rgb[i * 3] = p[i * 4]; im.rgb[i * 3 + 1] = p[i * 4 + 1]; im.rgb[i * 3 + 2] = p[i * 4 + 2]; }
    return im;
}

// Game coordinates -> screen: with the navigation bar the game shrinks around x = W/2.
static std::pair<double, double> to_screen(double x, double y) { return {W / 2.0 + (x - W / 2.0) * SCALE, y * SCALE}; }

// The game-coordinate rectangle [x0,x1)x[y0,y1) sampled from a screen image (like
// cv2.resize of the game area back to 2880x1800, bilinear).
static Image to_game(const Image& screen, double scale, int x0, int y0, int x1, int y1) {
    if (scale == 1.0) return screen;
    int half = (int)std::lround(W / 2.0 * scale), gh = (int)std::lround(H * scale);
    double fx = 2.0 * half / W, fy = (double)gh / H, left = W / 2 - half;
    Image out; out.x0 = x0; out.y0 = y0; out.w = x1 - x0; out.h = y1 - y0; out.frame = screen.frame;
    out.rgb.resize((size_t)out.w * out.h * 3);
    for (int y = y0; y < y1; y++) {
        double sy = std::max(0.0, (y + 0.5) * fy - 0.5); int iy = (int)sy; double ty = sy - iy;
        for (int x = x0; x < x1; x++) {
            double sx = std::max(0.0, left + (x + 0.5) * fx - 0.5); int ix = (int)sx; double tx = sx - ix;
            const uint8_t *a = screen.px(ix, iy), *b = screen.px(ix + 1, iy), *c = screen.px(ix, iy + 1), *d = screen.px(ix + 1, iy + 1);
            uint8_t* o = &out.rgb[((size_t)(y - y0) * out.w + (x - x0)) * 3];
            for (int k = 0; k < 3; k++)
                o[k] = (uint8_t)std::lround((a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty);
        }
    }
    return out;
}

Image capture(bool full) { return full ? capture_rect(0, 0, W, H) : capture_rect(RX0, RY0, RX1, RY1); }

Image capture_rect(int x0, int y0, int x1, int y1) {
    SHOTS++;
    Image im;
    if (helper_ok() || helper_start()) {
        try {
            if (SCALE == 1.0) im = helper_frame(x0, y0, x1, y1);
            else {
                auto [a, b] = to_screen(x0, y0); auto [c, d] = to_screen(x1, y1);
                im = to_game(helper_frame(std::max(0, (int)a - 2), std::max(0, (int)b - 2), std::min(W, (int)c + 3), std::min(H, (int)d + 3)), SCALE, x0, y0, x1, y1);
            }
            im.serial = ++SERIAL_NO;
            return im;
        } catch (std::exception& e) { helper_failed(e); }
    }
    // Screencap occasionally hangs on the tablet: each attempt is time-limited and retried.
    for (int attempt = 1; attempt <= 5; attempt++) {
        try {
            im = to_game(screencap(), SCALE, 0, 0, W, H); im.frame = -1; im.serial = ++SERIAL_NO;
            return im;
        } catch (std::exception& e) {
            say(clock_text(false) + " Screenshot attempt " + std::to_string(attempt) + " failed (" + e.what() + "); retrying.");
        }
    }
    throw AdbError("Screenshots keep failing.");
}

void update_layout() {
    // Android's reported app height isn't reliable (the game sometimes draws full height
    // with the navigation bar on), so each candidate scale is tried on a real screenshot.
    std::string out = adb({"shell", "dumpsys", "window", "displays"}, 20, false);
    std::smatch m; double reported = 1.0;
    if (std::regex_search(out, m, std::regex(R"(mAppBounds=Rect\(0, 0 - (\d+), (\d+)\))")) && std::stoi(m[1]) == W)
        reported = std::stod(m[2]) / H;
    if (!(reported > .7 && reported <= 1)) reported = 1.0;
    double scale = reported;
    try {
        Image raw = helper_ok() ? helper_frame(0, 0, W, H) : screencap();
        int best = -1;
        std::vector<double> cands = {SCALE};
        for (double c : {reported, 1.0}) if (std::find(cands.begin(), cands.end(), c) == cands.end()) cands.push_back(c);
        for (double c : cands) {
            Image im = to_game(raw, c, 0, 0, W, H); im.serial = ++SERIAL_NO;
            int score = 0;
            if (is_board(im)) { score = 1; try { read_pieces(im); score = 2; } catch (UnreadableTray&) {} }
            else if (green_button(im) || play_button(im)) score = 1;
            if (score > best) { best = score; scale = c; }
        }
    } catch (std::exception&) {}
    if (std::abs(scale - SCALE) > 1e-3) {
        char buf[96]; snprintf(buf, sizeof buf, "Game area is %d px tall; scaling by %.3f.", (int)std::lround(scale * H), scale); say(buf);
    }
    SCALE = scale;
}

Image capture_still(int x0, int y0, int x1, int y1, int hold_ms, int timeout_ms, bool& timed_out) {
    timed_out = false;
    if (SCALE == 1.0 && (helper_ok() || helper_start())) {
        try {
            SHOTS++;
            Image im = helper_frame(x0, y0, x1, y1, hold_ms, timeout_ms, &timed_out);
            im.serial = ++SERIAL_NO;
            return im;
        } catch (std::exception& e) { helper_failed(e); }
    }
    return capture_rect(x0, y0, x1, y1);
}

void swipes(const std::vector<Drag>& drags) {
    if (drags.size() == 1 || !helper_ok()) { for (auto& d : drags) swipe(d.sx, d.sy, d.dx, d.dy, d.ms); return; }
    sleep_s(LAST_UP + GAP_MS / 1000.0 - now());
    // On the tablet there is no Wi-Fi delay between drags, and the game ignores a drag that
    // starts less than ~40 ms after the previous drop (measured), so batched drags keep that gap.
    std::string cmd = "swipes " + std::to_string(BATCH_GAP_MS) + " " + std::to_string(drags.size());
    int total = 0;
    for (auto& d : drags) {
        auto [a, b] = to_screen(d.sx, d.sy); auto [c, e] = to_screen(d.dx, d.dy);
        char buf[96]; snprintf(buf, sizeof buf, " %.0f %.0f %.0f %.0f %d %d", a, b, c, e, d.ms, HOLD_MS); cmd += buf;
        total += d.ms + BATCH_GAP_MS + 20;
    }
    try {
        set_timeout(total / 1000.0 + 3 * SLOW); call(cmd); set_timeout(3 * SLOW);
        LAST_UP = now();
        return;
    } catch (std::exception& e) { helper_failed(e); }
    for (auto& d : drags) swipe(d.sx, d.sy, d.dx, d.dy, d.ms);
}

void swipe(double sx, double sy, double dx, double dy, int ms) {
    auto [a, b] = to_screen(sx, sy); auto [c, d] = to_screen(dx, dy);
    if (helper_ok()) {
        // The game ignores a drag that starts right after the previous drop.
        sleep_s(LAST_UP + GAP_MS / 1000.0 - now());
        try {
            char cmd[128]; snprintf(cmd, sizeof cmd, "swipe %.0f %.0f %.0f %.0f %d %d", a, b, c, d, ms, HOLD_MS);
            set_timeout(ms / 1000.0 + 3 * SLOW); call(cmd); set_timeout(3 * SLOW);
            LAST_UP = now();
            return;
        } catch (std::exception& e) { helper_failed(e); }
    }
    char buf[64];
    std::vector<std::string> args = {"shell", "input", "swipe"};
    for (double v : {a, b, c, d}) { snprintf(buf, sizeof buf, "%.0f", v); args.push_back(buf); }
    args.push_back(std::to_string(ms));
    adb(args, ms / 1000.0 + 8);
}

void tap(double x, double y) {
    auto [a, b] = to_screen(x, y);
    if (helper_ok()) {
        try { char cmd[96]; snprintf(cmd, sizeof cmd, "swipe %.0f %.0f %.0f %.0f 0 40", a, b, a, b); call(cmd); return; }
        catch (std::exception& e) { helper_failed(e); }
    }
    adb({"shell", "input", "tap", std::to_string((int)std::lround(a)), std::to_string((int)std::lround(b))});
}

}  // namespace bb
