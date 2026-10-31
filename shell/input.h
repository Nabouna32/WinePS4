// WinePS4's input for the desktop: the controller (pad, touch pad) and a USB mouse, as one pointer
// and a few buttons. Keyboard text comes from the system keyboard for now.
#pragma once

#include <cstdint>

namespace shell {

enum Button : uint32_t {
    BTN_CLICK = 1 << 0,    // left button: Cross, R2, mouse left
    BTN_MENU = 1 << 1,     // right button: Square, mouse right
    BTN_BACK = 1 << 2,     // Circle: close a menu, go up a folder
    BTN_START = 1 << 3,    // OPTIONS: the Start menu
    BTN_SWITCH = 1 << 4,   // Triangle: the next window
    BTN_PREV = 1 << 5,     // L1
    BTN_NEXT = 1 << 6,     // R1
    BTN_UP = 1 << 7,       // D-pad
    BTN_DOWN = 1 << 8,
    BTN_LEFT = 1 << 9,
    BTN_RIGHT = 1 << 10,
};

struct Input {
    float x = 960, y = 540;     // the pointer, in screen pixels
    bool moved = false;         // the pointer moved since the last poll
    uint32_t held = 0;          // Button bits held now
    uint32_t pressed = 0;       // pressed since the last poll
    uint32_t released = 0;      // released since the last poll
    uint32_t repeat = 0;        // D-pad and the pressed keys again while held (lists)
    float scroll = 0;           // lines (positive: down), from the right stick and the wheel
    bool mouse = false;         // a mouse is connected (the pointer follows it)
    bool pad = false;           // a controller is connected
};

void input_open();
// Once per frame, dt in seconds
const Input &input_poll(float dt);

} // namespace shell
