// The windows of Windows programs on the desktop: their display driver (wineps4.drv) sends where
// they are and their pixels (port/include/ps4_display.h); they're drawn over the desktop's own
// windows, and the pointer's input over them goes to the wineserver. A window Windows gives a title
// bar gets the desktop's own drawn over it (its look, its buttons), as Windows 11's.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ps4_canvas.h"

namespace shell {

// Listens for programs (call once, from the desktop's thread)
void winwin_start();
// Something to draw again (a window moved, showed, changed its pixels)
bool winwin_changed();

// What the desktop draws of a program's window itself: its shadow, title bar and border
struct WinFrame {
    uint64_t hwnd = 0;
    int pid = 0;
    int x = 0, y = 0, w = 0, h = 0;      // the window on the screen
    int caption_bottom = 0;              // its title bar ends here (0: none: no frame of ours)
    int border = 0;                      // the width of Windows' frame around the client area
    bool active = false, maximized = false, can_minimize = false, can_maximize = false;
    std::string title;
};
// Full-screen windows (no title bar, the whole screen) go over the taskbar, the others under it
enum class WinLayer { Normal, FullScreen };
// Draws the layer's visible windows bottom to top: under() before each one (its shadow), over()
// after it (the title bar and border over Windows' own)
void winwin_draw(ps4ui::Canvas &c, WinLayer layer, const std::function<void(ps4ui::Canvas &, const WinFrame &)> &under,
                 const std::function<void(ps4ui::Canvas &, const WinFrame &)> &over);
// The window under (x, y), 0 if none; *full_screen: it covers the screen (the taskbar too)
uint64_t winwin_at(float x, float y, bool *full_screen = nullptr);

// The title bar's buttons, from the right: 0 close, 1 maximize or restore, 2 minimize
constexpr int WIN_BUTTON_W = 46;
struct WinRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
WinRect winwin_button_rect(const WinFrame &f, int button);
// The desktop's quick frame, when since the last winwin_draw nothing but programs' pixels changed (no
// window came, went, moved or was raised): their changed pixels copied into the scene as they are, when
// each change lies in its window's own part (below the title bar, inside the frame and the bottom
// corners, above taskbar_top unless the window is full-screen) and no other window, or the shadow
// shadow px around one, is over it; out gets the screen rects copied. false: nothing copied, the
// whole scene must be drawn (winwin_draw).
bool winwin_patch(ps4ui::Canvas &c, int taskbar_top, int shadow, std::vector<WinRect> &out);
// The button of a title bar of ours at (x, y) (with its window, and whether that is maximized), or -1
int winwin_button_at(float x, float y, uint64_t *hwnd, bool *maximized = nullptr);
// A command for a window, as its title bar's buttons send it: SC_* or PS4_SC_ACTIVATE
void winwin_command(uint64_t hwnd, uint32_t command);

struct WinTask {
    uint64_t hwnd;
    int pid;
    std::string title;
    bool active;     // the top one
    bool minimized;
};
// The windows a taskbar button is for (shown, not menus or dialogs), oldest first
std::vector<WinTask> winwin_tasks();

// What takes text in a program's window now, from its display driver: a caret shown or moved there
// (Windows' edit boxes), or the field Chromium's text services focus (Steam's web pages). The desktop
// opens the PS4's keyboard for a text field clicked.
struct WinTextEvent {
    uint64_t hwnd = 0;   // the top-level window (0: none had the process's focus)
    int pid = 0;
    uint32_t flags = 0;  // PS4_TEXT_* (port/include/ps4_display.h)
    int left = 0, top = 0, right = 0, bottom = 0;  // the caret on the screen (PS4_TEXT_CARET)
};
// The ones that came since the last call, oldest first
std::vector<WinTextEvent> winwin_text_events();
// The window's process (0: no such window)
int winwin_pid(uint64_t hwnd);
// The title of a shown window of the process (its active one's if it has it)
std::string winwin_title(int pid);

// Input for the wineserver: the pointer (screen pixels), buttons (MOUSEEVENTF_* flags), text, keys
void winwin_mouse(float x, float y, uint32_t flags, int wheel = 0);
void winwin_text(const std::string &utf8);
void winwin_key(uint16_t vkey, bool down, uint16_t scan = 0, bool extended = false);

} // namespace shell
