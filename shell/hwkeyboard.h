// WinePS4's USB keyboard: a keyboard plugged into the PS4 types straight into Windows programs. Its keys
// come as Windows key events (KEYBDINPUT's virtual key, scan code and flags) for the wineserver. Right
// Shift comes with extended set, as Wine's own X11 driver sends it: Wine's server tells the two Shifts
// apart by that flag, and takes it off again before programs see it.
#pragma once

#include <cstdint>
#include <vector>

namespace shell {

struct KeyEvent {
    uint16_t vkey;   // Windows virtual-key code (VK_*)
    uint16_t scan;   // set-1 scan code, as Windows' KEYBDINPUT.wScan
    bool extended;   // KEYEVENTF_EXTENDEDKEY (E0-prefixed keys: arrows, Insert/Delete/Home/End/PgUp/PgDn, right Ctrl/Alt, Windows keys, Menu, numpad / and Enter, ...)
    bool down;       // pressed (true) or released (false); auto-repeat comes as extra presses
};

// The USB keyboard (libSceIme's keyboard, else libSceKeyboard). Call once after the user service is up
// (shell/input.cpp's input_open()).
void hwkeyboard_open();
// Once per frame (dt in seconds): what changed since the last call, in order, auto-repeat included.
std::vector<KeyEvent> hwkeyboard_poll(float dt);
// The system's keyboard dialog closed: what was typed while it was up went to it, and the keys held
// before are let go (their releases went to it too)
void hwkeyboard_dialog_closed();
// A keyboard is connected now
bool hwkeyboard_connected();

} // namespace shell
