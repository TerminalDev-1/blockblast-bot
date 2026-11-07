// Talking to the tablet: adb, the tablet helper (fast frames and touches), screenshots,
// swipes, the navigation-bar layout, focused app and accessible ad buttons.
#pragma once
#include <stdexcept>
#include <string>
#include <vector>

#include "util.hpp"

namespace bb {

struct Timeout : std::runtime_error { using std::runtime_error::runtime_error; };
struct AdbError : std::runtime_error { using std::runtime_error::runtime_error; };

// Runs a program, returns its stdout; throws Timeout (the process is killed) or AdbError
// on a non-zero exit when check is set.
std::string run(const std::vector<std::string>& args, double timeout, bool check = true, int* code = nullptr);
std::string adb(const std::vector<std::string>& args, double timeout = 20, bool check = true);

// Picks the tablet (USB first, then Wi-Fi via mDNS) and waits for it as long as it takes.
std::string ensure_device();
bool wireless();
void open_app(const std::string& package);
std::string focused_package();  // "" while switching
std::string focused_window();   // "package/activity" (or just the package for dialogs; "" while switching)
bool close_accessible_ad();     // taps an ad's close button (or a safe dialog answer)
void key_back();

extern double SCALE;  // game height / 1800 (the navigation bar shrinks the game)
void update_layout();

// Screenshot of the region the bot reads, in game coordinates (full = whole screen).
Image capture(bool full = false);
// Screenshot of one rectangle in game coordinates (helper: only that part is sent).
Image capture_rect(int x0, int y0, int x1, int y1);
// v5.8: a frame of the rectangle once its pixels haven't changed for hold_ms (the tablet
// waits, so no Wi-Fi round trips while it does); timed_out if timeout_ms passed first.
Image capture_still(int x0, int y0, int x1, int y1, int hold_ms, int timeout_ms, bool& timed_out);
struct Drag { double sx, sy, dx, dy; int ms; };
// v5.8: several drags in one helper command, played back to back on the tablet.
void swipes(const std::vector<Drag>& drags);
extern int SHOTS;  // screenshots since the counter was reset

void swipe(double sx, double sy, double dx, double dy, int ms);
void tap(double x, double y);

// Tablet helper control (started by ensure_helper; failures fall back to screencap/input).
bool helper_ok();
void helper_restart();
void release_touch();  // ends a touch the game may still think is down
extern int GAP_MS;        // pause between one helper drag's lift and the next drag
extern int BATCH_GAP_MS;  // pause between drags sent together (on the tablet)
extern int HOLD_MS;  // finger rest on the target before lifting
std::string helper_dex_path();  // defined by the program (next to its executable)

}  // namespace bb
