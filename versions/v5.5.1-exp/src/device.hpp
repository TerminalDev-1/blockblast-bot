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
bool close_accessible_ad();     // taps an ad's close button (or a safe dialog answer)
void key_back();

extern double SCALE;  // game height / 1800 (the navigation bar shrinks the game)
void update_layout();

// Screenshot of the region the bot reads, in game coordinates (full = whole screen).
Image capture(bool full = false);
extern int SHOTS;  // screenshots since the counter was reset

void swipe(double sx, double sy, double dx, double dy, int ms);
void tap(double x, double y);

// Tablet helper control (started by ensure_helper; failures fall back to screencap/input).
bool helper_ok();
void helper_restart();
extern int GAP_MS;   // pause between one helper drag's lift and the next drag
extern int HOLD_MS;  // finger rest on the target before lifting
std::string helper_dex_path();  // defined by the program (next to its executable)

}  // namespace bb
