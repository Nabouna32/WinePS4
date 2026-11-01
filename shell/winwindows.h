// The windows of Windows programs on the desktop: their display driver (wineps4.drv) sends where
// they are and their pixels (port/include/ps4_display.h); they're drawn over the desktop's own
// windows, and the pointer's input over them goes to the wineserver.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ps4_canvas.h"

namespace shell {

// Listens for programs (call once, from the desktop's thread)
void winwin_start();
// Something to draw again (a window moved, showed, changed its pixels)
bool winwin_changed();
// Draws the visible windows, bottom to top
void winwin_draw(ps4ui::Canvas &c);
// The window under (x, y), 0 if none
uint64_t winwin_at(float x, float y);

struct WinTask {
    uint64_t hwnd;
    std::string title;
    bool active;  // the top one
};
// The windows a taskbar button is for (shown, not menus or dialogs), oldest first
std::vector<WinTask> winwin_tasks();

// Input for the wineserver: the pointer (screen pixels), buttons (MOUSEEVENTF_* flags), text
void winwin_mouse(float x, float y, uint32_t flags, int wheel = 0);
void winwin_text(const std::string &utf8);
void winwin_key(uint16_t vkey, bool down);

} // namespace shell
