// Reading the game screen: board cells, tray pieces, menus. Port of the Python bot's vision
// (same rules and thresholds; colours in OpenCV's 8-bit Lab/HSV scales).
#pragma once
#include <array>
#include <optional>
#include <stdexcept>
#include <vector>

#include "planner.hpp"
#include "util.hpp"

namespace bb {

constexpr int W = 2880, H = 1800;          // screen in landscape
constexpr int GX = 994, GY = 321, STEP = 112;  // board origin and cell size
constexpr int TRAYS[3][2] = {{990, 1290}, {1290, 1590}, {1590, 1890}};
constexpr int REST_Y = 1479; constexpr int REST_DX[3] = {4, -1, -5};  // where settled tray pieces sit
constexpr u64 FULL = ~0ULL;
// Screen region the bot reads (board, tray, menu buttons).
constexpr int RX0 = 990, RY0 = 321, RX1 = 1890, RY1 = 1610;

struct CellStat { double lip; std::array<double, 3> lab; double texture; };
std::vector<CellStat> cell_stats(const Image& im);  // 64 cells; cached for the last image

bool is_board(const Image& im);
// Board even when pop-up text covers part of it: visible marks the flat (trusted) cells.
struct Visible { bool ok; u64 board, visible; };
Visible visible_board(const Image& im, u64 hint_empty);
u64 read_board(const Image& im, u64 hint_empty = 0);
void forget_theme();  // drop the learned empty-cell colour

struct TrayPiece { int slot; Cells cells; int ox, oy; };  // ox, oy: top-left on screen
struct UnreadableTray : std::runtime_error { using std::runtime_error::runtime_error; };
std::vector<TrayPiece> read_pieces(const Image& im);  // throws UnreadableTray
bool at_rest(const TrayPiece& p);

bool green_button(const Image& im);   // the game's Play/Classic button
bool play_button(const Image& im);    // result screen's play icon
bool rating_popup(const Image& im);

}  // namespace bb
