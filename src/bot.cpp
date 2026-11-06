// v5.6-hyperspeed: v5.5.1-exp (planner tuned for long-horizon survival)
// with a faster play loop: drags no longer slow down for good after one miss, and tighter,
// adjustable timings (--swipe-ms, --gap-ms, --deal-settle-ms, --check-delay-ms). Target set
// by the user: 170 moves/min. The Block Blast bot in C++. Screenshot -> read board and tray -> plan ->
// swipe -> check, on a USB- or Wi-Fi-connected tablet (2880x1800 landscape) over adb.
// A small helper on the tablet (helper/BBHelper.java) mirrors the screen (a frame in
// ~0.04 s instead of ~0.37 s) and injects touches; without it the bot uses screencap/input.
//   bbbot.exe           dry run: reads the screen and plans, no input
//   bbbot.exe --play    plays
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "device.hpp"
#include "planner.hpp"
#include "vision.hpp"

using namespace bb;

// Measured from screen recordings: a held piece moves ~1.47 px per px of finger movement,
// and when lifted from the tray its centre jumps to this height.
static const double GAIN = 1.47;
static const int RETRY = (int)std::lround(STEP / GAIN);  // finger distance for one cell
static const int SAFE_MS = 200;   // drag time that hasn't missed a drop
static const int RETRY_MS = 300;  // drag time for retries
static const int DANGER_FROM = 28;  // cells filled after a tray from which it is checked for real
static const double UNCHANGED_AFTER = .3;  // helper: an unchanged board is a miss only after this long
static const double UNKNOWN_AFTER = 3.0;   // seconds without a board before recovery taps anything
static const double CONFIRM_GAP = .08;     // helper: time between two reads that must agree
static bool CHECK_EVERY = false;  // --check-every: check every drop (v5.5 behaviour)
static int CHECK_DELAY_MS = 15;  // helper: wait before the first check frame (the game needs a moment to draw a drop)
static int DEAL_SETTLE_MS = 30;  // a new tray's pieces look settled a moment before the game takes drags
static bool DEBUG = getenv("BB_DEBUG") != nullptr;  // save the screen when a check times out

static void save_debug(const std::string& why) {
    if (!DEBUG) return;
    std::filesystem::create_directories("debug-frames");
    std::string name = "debug-frames/" + clock_text() + "-" + why + ".png";
    for (auto& c : name) if (c == ':') c = '-';
    write_png(name, capture(true));
    log("debug: saved " + name);
}

std::string bb::helper_dex_path() {
    char buf[MAX_PATH]; GetModuleFileNameA(nullptr, buf, MAX_PATH);
    auto dir = std::filesystem::path(buf).parent_path();
    for (auto d : {dir, dir.parent_path()})  // bbbot.exe sits in the version folder or in build/
        if (std::filesystem::exists(d / "helper" / "bbhelper.dex")) return (d / "helper" / "bbhelper.dex").string();
    return (dir / "helper" / "bbhelper.dex").string();
}

static std::string TRACE;

static std::string cells_text(const Cells& c) {
    std::string s = "[";
    for (size_t i = 0; i < c.size(); i++) s += (i ? ", (" : "(") + std::to_string(c[i].first) + ", " + std::to_string(c[i].second) + ")";
    return s + "]";
}

static void show(u64 board, const std::vector<TrayPiece>& pieces) {
    std::string out;
    for (int y = 0; y < 8; y++) { for (int x = 0; x < 8; x++) out += board >> (y * 8 + x) & 1 ? '#' : '.'; out += '\n'; }
    for (auto& p : pieces) out += "tray " + std::to_string(p.slot + 1) + " cells " + cells_text(p.cells) + "\n";
    fputs(out.c_str(), stdout); fflush(stdout);
}

static void save_unknown() {
    namespace fs = std::filesystem;
    fs::create_directories("unknown-screens");
    int n = 0; for (auto& e : fs::directory_iterator("unknown-screens")) n += e.is_regular_file();
    if (n >= 30) return;
    Image im = capture(true);
    std::time_t t = std::time(nullptr); std::tm tm; localtime_s(&tm, &t);
    char name[64]; strftime(name, sizeof name, "unknown-screens/%Y%m%d-%H%M%S.png", &tm);
    write_png(name, im);
}

static double LAST_BACK = -1e9;

static void recover_screen(const Image& im) {
    std::string pkg = focused_package();
    if (!pkg.empty() && pkg != "com.block.juggle") {
        // Another app (an ad opened the store or a browser). Back only when it's still
        // there a moment later, and at most once every 15 s: v5.4 pressed Back on every
        // check, which on the game's home screen opened "Leave now?" and kept pressing.
        sleep_s(1.5);
        if (focused_package() != pkg) return;
        if (now() - LAST_BACK < 15) { say("Waiting for " + pkg + " to close."); sleep_s(3); return; }
        LAST_BACK = now();
        say("Returning from another app or ad (" + pkg + ")."); key_back(); sleep_s(2); return;
    }
    double old = SCALE; update_layout();
    if (SCALE != old) return;  // the navigation bar changed; re-read with the new scale
    // The bot never starts a game itself (the user's choice): on the result screen or the
    // menu it waits until the user starts one, then plays it.
    if (play_button(im) || green_button(im)) {
        static double said = -1e9;
        if (now() - said > 60) { say(std::string(play_button(im) ? "Game over" : "Main menu") + ": waiting for you to start a game."); said = now(); }
        sleep_s(3); return;
    }
    if (rating_popup(im)) { say("Closing rating prompt."); tap(1778, 525); sleep_s(2); return; }
    if (close_accessible_ad()) { sleep_s(3); return; }
    say("Unknown screen: waiting to preserve the current game."); sleep_s(10);
}

static bool in_tray(const Image& im, const TrayPiece& piece) {
    try {
        for (auto& q : read_pieces(im)) if (q.slot == piece.slot && q.cells == piece.cells) return true;
        return false;
    } catch (UnreadableTray&) { return true; }  // unreadable: assume it's still there
}

// Capture until the board shows the move's result. Returns the image and the board (nullopt
// off-board); early = lines are still clearing but everything else matches. residual:
// cells of earlier clears that may still be animating. TRACE: E expected, e expected with
// some cells hidden, B unchanged, o other, - off-board, = same helper frame, t piece left the tray.
struct Settled { Image im; std::optional<u64> board; bool early; };
static Settled settle(u64 before, u64 expected, u64 placed, u64 residual, const TrayPiece& piece, double timeout = 3.0) {
    double start = now(), end = start + timeout;
    if (helper_ok()) sleep_s(CHECK_DELAY_MS / 1000.0);
    std::optional<std::pair<u64, u64>> last; int unchanged = 0; long long prev_frame = -2;
    u64 cleared = (before | placed) & ~expected;
    for (;;) {
        Image im = capture();
        if (im.frame >= 0) {
            // Helper frames come faster than the game reacts: a repeat of the same frame
            // says nothing new.
            if (im.frame == prev_frame) { TRACE += '='; sleep_s(.01); if (now() < end) continue; }
            prev_frame = im.frame;
        }
        Visible v = visible_board(im, FULL & ~(before | expected | residual));
        if (!v.ok) {
            TRACE += '-';  // usually a transient effect (clear animation, big combo text)
            if (now() > end) return {im, std::nullopt, false};
            last.reset(); continue;
        }
        u64 b = v.board, seen = v.visible & ~residual, outside = seen & ~cleared;
        bool full = seen == (FULL & ~residual), same = !((b ^ expected) & seen);
        TRACE += same ? (full ? 'E' : 'e') : !((b ^ before) & seen) ? 'B' : 'o';
        if (same) return {im, expected, !full || residual};
        // Two fully visible reads with nothing changed: the drop didn't happen. With helper
        // frames only after the game has had time, and only if the piece is still in the
        // tray (the game sometimes shows a drop late).
        unchanged = full && !((b ^ before) & seen) ? unchanged + 1 : 0;
        if (unchanged >= 2 && (im.frame < 0 || now() - start >= UNCHANGED_AFTER)) {
            if (im.frame < 0 || in_tray(im, piece)) return {im, before, false};
            TRACE += 't';
        }
        if ((cleared | residual) && ((b ^ before) & seen) && !((b ^ expected) & outside)) return {im, expected, true};
        // A changed board read twice in a row is final (clear animations differ from both);
        // with cells hidden the read isn't trustworthy.
        if (((b ^ before) & seen) && last && last->second == seen && !((b ^ last->first) & seen) &&
            (im.frame < 0 || now() - start >= UNCHANGED_AFTER))
            return {im, full ? std::optional<u64>(b) : std::nullopt, false};
        if (now() > end) { save_debug("timeout"); return {im, full ? std::optional<u64>(b) : std::nullopt, false}; }
        last = std::make_pair(b, seen);
    }
}

struct Step { int slot, x, y; u64 after, before; Cells cells; };

struct Args { bool play = false; long long max_moves = 1000; int pickup_y = 1232, swipe_ms = 70; };

static void main_loop(const Args& args) {
    std::map<Cells, std::pair<double, double>> corrections;
    long long turn = 0; double started = now();
    std::optional<u64> failed_board; std::vector<std::tuple<int, int, int>> failed_moves;
    std::vector<Step> plan_seq; std::optional<Image> im;
    std::optional<double> stuck_since, unknown_since, moving_since, dealing;
    bool saved_unknown = false; int tray_errors = 0;
    std::optional<u64> trusted;  // board accepted while lines are still clearing on screen
    std::vector<std::pair<u64, double>> clears;  // recent line clears (they animate ~1.5 s)
    int since = COMBO_WINDOW;  // placements since the last clear
    std::optional<std::vector<TrayPiece>> carry;  // tray pieces left after an unchecked move
    std::optional<u64> predicted;  // board the last checked move should have produced
    // The next tray is dealt a moment after the last piece lands; waiting deal_wait first
    // lets one screenshot read it. Learned: longer after a miss, a bit shorter after a hit.
    double deal_wait = helper_ok() ? .1 : .2; bool probe = false;
    std::optional<std::pair<u64, std::vector<Cells>>> last_state; std::optional<std::tuple<int, int, int>> last_first;
    int repeat = 0; bool danger = false;
    // Slow drags after misses are temporary: v5.5 slowed every later long drag for the rest
    // of the run after one miss (0.21 s per drag instead of 0.09 s). Now two fast misses
    // within 30 drags slow long drags for the next 30 drags only.
    double slow_from = 1e18;  // finger distance from which drags use SAFE_MS
    int slow_left = 0, drags_done = 0;
    std::vector<int> fast_misses;  // drag numbers of recent fast misses
    std::vector<TrayPiece> pieces; std::map<int, TrayPiece> by_slot;
    std::vector<std::string> why;
    Params params;
    double planned_at = 0;
    std::optional<std::pair<std::vector<TrayPiece>, long long>> prev_tray;  // tray in the previous helper frame
    double tray_seen_at = 0, prev_at = 0;  // when the bot started waiting for the tray to hold still

    auto drag_ms = [&](double sx, double sy, double dx, double dy) {
        drags_done++;
        if (slow_left > 0 && --slow_left == 0) slow_from = 1e18;
        return args.swipe_ms < SAFE_MS && std::hypot(dx - sx, dy - sy) >= slow_from ? SAFE_MS : args.swipe_ms;
    };
    auto miss_step = [] { return helper_ok() ? .02 : .04; };
    auto recent_clears = [&](double age) { u64 m = 0; for (auto& [c, t] : clears) if (t > now() - age) m |= c; return m; };

    while (turn < args.max_moves) {
        std::optional<u64> board;
        bool pieces_known = false;
        if (carry) {
            // Mid-tray without a check: the board is known from the plan.
            board = trusted; pieces = *carry; carry.reset(); pieces_known = true;
            by_slot.clear(); for (auto& p : pieces) by_slot[p.slot] = p;
        } else {
            if (!im) im = capture();
            if (trusted) {
                // Known board (lines may still be clearing, text may cover part of it):
                // just confirm the visible cells agree.
                clears.erase(std::remove_if(clears.begin(), clears.end(), [](auto& c) { return c.second <= now() - 2.5; }), clears.end());
                u64 res = recent_clears(2.5), recent = recent_clears(1.5);
                Visible v = visible_board(*im, FULL & ~*trusted & ~res);
                if (!v.ok) { trusted.reset(); why.push_back("hidden"); }
                else {
                    // Pop-up text can't make a filled cell look empty, so a missing block is
                    // real; extra blocks count only when nothing is animating (combo text is
                    // drawn in flat letters that look like blocks).
                    u64 seen = v.visible & ~res, missing = *trusted & ~v.board & seen, extra = v.board & ~*trusted & seen;
                    u64 confirmed = recent ? 0 : extra;
                    if (__builtin_popcountll(missing | confirmed) >= 4) {
                        // The picture or this screenshot is badly off: read the board from two
                        // screenshots that agree before planning again.
                        log("Board very different (" + std::to_string(__builtin_popcountll(missing)) + " gone, " +
                            std::to_string(__builtin_popcountll(confirmed)) + " added); reading carefully.");
                        trusted.reset(); predicted.reset(); why.push_back("big-diff");
                    } else {
                        if (missing | confirmed) {
                            log("Board corrected from the screen (" + std::to_string(__builtin_popcountll(missing)) + " gone, " +
                                std::to_string(__builtin_popcountll(confirmed)) + " added).");
                            trusted = (*trusted & ~missing) | confirmed; predicted = trusted;
                        }
                        board = trusted;
                    }
                }
            }
        }
        if (!board && !is_board(*im)) {
            if (!args.play) { say("Not on the game board."); return; }
            plan_seq.clear(); trusted.reset(); clears.clear(); predicted.reset();
            // Never tap anything until the board has been gone for a while: big combo
            // effects hide it for up to ~1 s.
            if (!unknown_since) unknown_since = now();
            if (now() - *unknown_since < UNKNOWN_AFTER) { im.reset(); sleep_s(.1); continue; }
            if (!saved_unknown) { save_unknown(); saved_unknown = true; }
            recover_screen(capture()); im.reset(); continue;
        }
        unknown_since.reset(); saved_unknown = false;
        if (!pieces_known) {
            if (!board) board = read_board(*im);
            try { pieces = read_pieces(*im); }
            catch (UnreadableTray& e) {
                // Usually new pieces sliding in; a new theme if it lasts.
                tray_errors++;
                if (tray_errors == 20) save_unknown();
                if (tray_errors % 10 == 3) update_layout();  // the game may have been resized
                if (tray_errors > 3) log(std::string(e.what()) + " Waiting for screen to settle.");
                if (tray_errors % 15 == 0) { log("Tray unreadable for a while: releasing any stuck touch."); release_touch(); }
                if (probe) { probe = false; deal_wait = std::min(.6, deal_wait + miss_step()); }
                why.push_back("tray-moving"); im.reset(); continue;
            }
            tray_errors = 0;
            if (probe) {
                probe = false;
                bool dealt = pieces.size() == 3 && std::all_of(pieces.begin(), pieces.end(), at_rest);
                deal_wait = dealt ? std::max(0.0, deal_wait - .01) : std::min(.6, deal_wait + miss_step());
            }
            // A new tray is dealt one piece after another: plan only with all three.
            if (dealing && pieces.size() < 3 && now() - *dealing < 1.0) { why.push_back("dealing"); im.reset(); continue; }
            if (pieces.size() == 3) dealing.reset();
            if (pieces.empty()) { why.push_back("tray-empty"); log("Waiting for pieces or game-over screen."); im.reset(); plan_seq.clear(); continue; }
            by_slot.clear(); for (auto& p : pieces) by_slot[p.slot] = p;
        }
        if (board != failed_board) { failed_board = board; failed_moves.clear(); }
        // Keep following the tray's plan while the board matches it; otherwise re-plan.
        bool following = !plan_seq.empty() && by_slot.count(plan_seq[0].slot) && plan_seq[0].before == *board &&
                         plan_seq[0].cells == by_slot[plan_seq[0].slot].cells;
        if (!following) {
            bool helper = im && im->frame >= 0;
            bool all_rest = std::all_of(pieces.begin(), pieces.end(), at_rest);
            // Sliding pieces: with cheap helper frames, wait for them to come to rest (the
            // drag starts where they lie) instead of planning from a moving tray.
            if (helper && !all_rest) {
                if (!moving_since) moving_since = now();
                if (now() - *moving_since < .6) { why.push_back("tray-moving"); im.reset(); continue; }
            }
            moving_since.reset();
            if (helper) {
                // New pieces grow in around their resting centre, and mid-animation they read
                // as other shapes (a 2x2 as a 3x3). Plan only once the frames show exactly the
                // same known pieces at the same spots for a moment.
                bool same = prev_tray && prev_tray->first.size() == pieces.size();
                for (size_t i = 0; same && i < pieces.size(); i++) {
                    const auto &a = pieces[i], &b = prev_tray->first[i];
                    same = a.slot == b.slot && a.cells == b.cells && std::abs(a.ox - b.ox) <= 1 && std::abs(a.oy - b.oy) <= 1 && shape_index(a.cells) >= 0;
                }
                // The same reading for 40 ms (a still screen produces no new frames at all).
                bool still = same && now() - prev_at >= .04;
                if (!still && (tray_seen_at == 0 || now() - tray_seen_at < 1.0)) {
                    if (!same) { prev_tray = std::make_pair(pieces, im->frame); prev_at = now(); }
                    if (tray_seen_at == 0) tray_seen_at = now();
                    why.push_back("settling"); im.reset(); sleep_s(.01); continue;
                }
                prev_tray.reset(); tray_seen_at = 0;
            }
            else if (!(all_rest && pieces.size() == 3 && board == predicted)) {
                // Otherwise a second screenshot must agree (pieces animate in).
                if (helper) sleep_s(CONFIRM_GAP);
                Image im2 = capture();
                bool same = false;
                try {
                    u64 res = recent_clears(2.5);
                    bool board_same;
                    if (trusted) { Visible v = visible_board(im2, FULL & ~*board & ~res); board_same = v.ok && !((v.board ^ *board) & v.visible & ~res); }
                    else board_same = is_board(im2) && read_board(im2) == *board;
                    auto p2 = read_pieces(im2);
                    same = board_same && p2.size() == pieces.size();
                    for (size_t i = 0; same && i < p2.size(); i++) same = p2[i].slot == pieces[i].slot && p2[i].cells == pieces[i].cells;
                    if (same) { pieces = p2; by_slot.clear(); for (auto& p : pieces) by_slot[p.slot] = p; }
                } catch (UnreadableTray&) { same = false; }
                why.push_back("confirm");
                if (!same) { im = im2; continue; }
            }
            std::vector<Cells> sorted_cells; for (auto& p : pieces) sorted_cells.push_back(p.cells);
            std::sort(sorted_cells.begin(), sorted_cells.end());
            auto state = std::make_pair(*board, sorted_cells);
            repeat = last_state && *last_state == state ? repeat + 1 : 0; last_state = state;
            if (repeat >= 2 && last_first) failed_moves.push_back(*last_first);
            if (repeat) log("Same position again (" + std::to_string(repeat) + "x): a drop missed; dragging slowly.");
            double t = now(); planned_at = t;
            std::vector<Piece> in; for (auto& p : pieces) in.push_back({p.slot, p.cells});
            auto seq = plan(*board, in, params, failed_moves, since, (uint64_t)turn);
            if (!seq.empty()) last_first = std::make_tuple(seq[0].slot, seq[0].x, seq[0].y); else last_first.reset();
            danger = !seq.empty() && __builtin_popcountll(seq.back().after) >= DANGER_FROM;
            plan_seq.clear(); u64 prev = *board;
            for (auto& m : seq) { plan_seq.push_back({m.slot, m.x, m.y, m.after, prev, by_slot[m.slot].cells}); prev = m.after; }
            std::string reasons; for (auto& w : why) reasons += " " + w;
            char buf[160]; snprintf(buf, sizeof buf, "Planned %zu of %zu pieces in %.2fs; %d shots", seq.size(), pieces.size(), now() - t, SHOTS);
            log(buf + reasons); SHOTS = 0; why.clear();
        }
        turn++;
        { char buf[64]; snprintf(buf, sizeof buf, "\nMove %lld  (%.0f moves/min)", turn, turn / std::max(1e-9, now() - started) * 60); say(buf); }
        show(*board, pieces);
        if (plan_seq.empty()) {
            if (trusted || predicted) {
                // Nothing fits on the board the bot expected: that picture may have drifted.
                log("No legal moves on the expected board; reading the real one.");
                im.reset(); trusted.reset(); predicted.reset(); continue;
            }
            if (!failed_moves.empty()) { log("No untried placements left; re-reading the screen."); failed_moves.clear(); sleep_s(1); }
            else {
                // The game normally shows its result screen; if it never comes, the screen is
                // being misread: save it and try the normal recovery.
                if (!stuck_since) stuck_since = now();
                if (now() - *stuck_since > 30) {
                    log("No legal moves for 30s without a game-over screen; trying recovery.");
                    save_unknown(); recover_screen(capture()); stuck_since.reset();
                } else { log("No legal moves; waiting for game-over screen."); sleep_s(2); }
            }
            im.reset(); trusted.reset(); continue;
        }
        stuck_since.reset();
        Step st = plan_seq.front(); plan_seq.erase(plan_seq.begin());
        const TrayPiece p = by_slot[st.slot];
        log("Selected tray " + std::to_string(st.slot + 1) + ", grid column " + std::to_string(st.x + 1) + ", row " + std::to_string(st.y + 1));
        if (!args.play) return;
        int width = 0, height = 0; for (auto [a, b] : p.cells) { width = std::max(width, a + 1); height = std::max(height, b + 1); }
        double sx = p.ox + p.cells[0].first * 50 + 25, sy = p.oy + p.cells[0].second * 50 + 25;
        double target_x = GX + (st.x + width / 2.0) * STEP, tray_center_x = p.ox + width * 25.0;
        auto [adjust_x, adjust_y] = corrections.count(p.cells) ? corrections[p.cells] : std::make_pair(0.0, 0.0);
        double target_y = GY + (st.y + height / 2.0) * STEP;
        double dx = sx + (target_x - tray_center_x) / GAIN + adjust_x, dy = sy + (target_y - args.pickup_y) / GAIN + adjust_y;
        u64 placed = 0; for (auto [a, b] : p.cells) placed |= 1ULL << ((st.y + b) * 8 + st.x + a);
        u64 cleared = (*board | placed) & ~st.after;
        // Which drops get a check. With the helper (v5.6): a tray's last drop always, and its
        // check compares the whole board with the plan, so a missed or misplaced earlier piece
        // is caught on the same tray; the earlier drops go straight after each other (each
        // check cost ~0.11 s), except after a repeated position or with --check-every. Without the helper only the last piece of a careful tray (as in v5.4).
        bool careful = repeat >= 1 || danger;
        // (A crowded board is still checked in full by the last drop; checking each drop there
        // too cost ~0.3 s per tray.)
        bool check = helper_ok() ? (plan_seq.empty() || repeat >= 1 || CHECK_EVERY) : (careful && plan_seq.empty());
        bool first_of_tray = pieces.size() == 3;
        if (first_of_tray && helper_ok()) sleep_s(DEAL_SETTLE_MS / 1000.0 - (now() - planned_at));
        if (!check) {
            double t0 = now(); swipe(sx, sy, dx, dy, repeat >= 1 ? RETRY_MS : drag_ms(sx, sy, dx, dy));
            if (cleared) clears.push_back({cleared, t0});
            since = cleared ? 0 : since + 1;
            trusted = st.after;
            char buf[96];
            if (!plan_seq.empty()) {
                std::vector<TrayPiece> rest; for (auto& q : pieces) if (q.slot != st.slot) rest.push_back(q);
                carry = rest; snprintf(buf, sizeof buf, "swipe %.2fs", now() - t0); log(buf); continue;
            }
            predicted = st.after;
            if (pieces.size() == 1) { sleep_s(deal_wait); probe = true; dealing = now(); }  // a new tray is dealt now
            snprintf(buf, sizeof buf, "swipe %.2fs, last of tray, deal wait %.2fs", now() - t0, deal_wait); log(buf);
            im.reset(); continue;
        }
        std::optional<u64> actual = *board; double retry_x = 0, retry_y = 0;
        // A missed drop is usually a fast drag the game didn't keep up with, so the first
        // retry is the same target dragged slowly; later retries shift it by one cell.
        const double tries[6][3] = {{0, 0, (double)drag_ms(sx, sy, dx, dy)}, {0, 0, RETRY_MS}, {0, (double)RETRY, RETRY_MS},
                                    {0, -(double)RETRY, RETRY_MS}, {(double)RETRY, 0, RETRY_MS}, {-(double)RETRY, 0, RETRY_MS}};
        for (auto& tr : tries) {
            retry_x = tr[0]; retry_y = tr[1]; int ms = (int)tr[2];
            if (st.x == 0 && retry_x < 0) continue;  // never shift the piece off the board
            if (st.x + width == 8 && retry_x > 0) continue;
            if (st.y == 0 && retry_y < 0) continue;
            if (st.y + height == 8 && retry_y > 0) continue;
            double t0 = now(); swipe(sx, sy, dx + retry_x, dy + retry_y, ms);
            clears.erase(std::remove_if(clears.begin(), clears.end(), [&](auto& c) { return c.second <= t0 - 2.5; }), clears.end());
            bool last_of_tray = pieces.size() == 1;
            if (last_of_tray) sleep_s(deal_wait);
            double t1 = now();
            Settled s = settle(*board, st.after, placed, recent_clears(2.5), p);
            im = s.im; actual = s.board;
            if (last_of_tray && actual == st.after) {
                dealing = now();
                bool dealt = false;
                try { auto q = read_pieces(*im); dealt = q.size() == 3 && std::all_of(q.begin(), q.end(), at_rest); } catch (UnreadableTray&) {}
                deal_wait = dealt ? std::max(0.0, deal_wait - .01) : std::min(.6, deal_wait + miss_step());
            }
            trusted = s.early || actual == st.after ? actual : std::nullopt;
            if (actual == st.after && cleared) clears.push_back({cleared, t1});
            char buf[200];
            snprintf(buf, sizeof buf, "swipe %.2fs, check %.2fs %s%s", t1 - t0, now() - t1, TRACE.c_str(), s.early ? " (lines still clearing)" : "");
            std::string line = buf;
            if (last_of_tray) { snprintf(buf, sizeof buf, " deal wait %.2fs", deal_wait); line += buf; }
            log(line); TRACE.clear();
            if (!actual) { log("Screen changed; checking state."); break; }
            if (*actual != *board) break;
            // A miss on a fresh tray's first drag is the game not taking drags yet, not speed.
            if (ms < SAFE_MS && !(first_of_tray && helper_ok())) {
                fast_misses.push_back(drags_done);
                fast_misses.erase(std::remove_if(fast_misses.begin(), fast_misses.end(), [&](int d) { return d < drags_done - 30; }), fast_misses.end());
                if (fast_misses.size() >= 2) {
                    slow_from = std::min(slow_from, std::hypot(dx - sx, dy - sy) * .9); slow_left = 30;
                    snprintf(buf, sizeof buf, "Fast drags missed twice; drags from %.0f px use %d ms for the next 30 drags.", slow_from, SAFE_MS); log(buf);
                } else log("Fast drag missed (once; drags stay fast).");
            }
            log("Retrying unplaced piece.");
        }
        if (!actual) { im.reset(); plan_seq.clear(); trusted.reset(); continue; }
        predicted = actual == st.after ? st.after : *actual;
        if (actual == st.after) since = cleared ? 0 : since + 1;
        if (*actual == *board) {
            failed_moves.push_back({st.slot, st.x, st.y}); plan_seq.clear();
            log("Placement failed; trying a different move."); continue;
        }
        if (*actual != st.after) {
            plan_seq.clear();
            std::vector<std::pair<int, int>> matches;
            for (int yy = 0; yy < 8; yy++)
                for (int xx = 0; xx < 8; xx++) {
                    u64 m = 0; bool fits = true;
                    for (auto [a, b] : p.cells) { if (xx + a >= 8 || yy + b >= 8) { fits = false; break; } m |= 1ULL << ((yy + b) * 8 + xx + a); }
                    if (fits && !(m & *board) && drop(*board, m).first == *actual) matches.push_back({xx, yy});
                }
            if (matches.size() == 1) {
                auto [ax, ay] = matches[0];
                double cx_ = std::clamp(adjust_x + retry_x + (st.x - ax) * STEP / GAIN, -1.5 * RETRY, 1.5 * RETRY);
                double cy_ = std::clamp(adjust_y + retry_y + (st.y - ay) * STEP / GAIN, -1.5 * RETRY, 1.5 * RETRY);
                corrections[p.cells] = {cx_, cy_};
                log("Piece landed at (" + std::to_string(ax + 1) + ", " + std::to_string(ay + 1) + "); recalibrated.");
            } else log("Board differs from the plan; playing on from what the screen shows.");
        }
    }
}

static void pick_device(bool play) {
    ensure_device();
    helper_restart();
    update_layout();
    if (play && focused_package() != "com.block.juggle") { say("Opening Block Blast."); open_app("com.block.juggle"); sleep_s(6); }
}

int main(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&] { return i + 1 < argc ? std::atoll(argv[++i]) : 0LL; };
        if (a == "--play") args.play = true;
        else if (a == "--max-moves") args.max_moves = val();
        else if (a == "--pickup-y") args.pickup_y = (int)val();
        else if (a == "--swipe-ms") args.swipe_ms = (int)val();
        else if (a == "--gap-ms") GAP_MS = (int)val();
        else if (a == "--hold-ms") HOLD_MS = (int)val();
        else if (a == "--deal-settle-ms") DEAL_SETTLE_MS = (int)val();
        else if (a == "--check-delay-ms") CHECK_DELAY_MS = (int)val();
        else if (a == "--check-every") CHECK_EVERY = true;
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    for (;;) {
        try {
            pick_device(args.play);
            main_loop(args);
            return 0;
        } catch (std::exception& e) {
            // Keep playing for hours: log any error, wait for the tablet, and start over.
            fprintf(stderr, "%s Error: %s. Rereading the screen in 5 seconds.\n", clock_text().c_str(), e.what()); fflush(stderr);
            say(clock_text() + " Error: " + e.what() + "; restarting in 5 s.");
            if (!args.play) return 1;
            sleep_s(5);
        }
    }
}
