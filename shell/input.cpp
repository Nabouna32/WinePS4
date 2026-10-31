// WinePS4's input (input.h). The pointer moves with the left stick (faster the further it is
// pushed), the touch pad (like a laptop's) and a USB mouse. libSceMouse is loaded when the desktop
// starts, not linked: a module the system doesn't have would stop the app from starting at all.
#include "input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include "screen.h"

extern "C" void ps4_boot_trace(const char *line);

namespace shell {
namespace {

// libSceMouse (from shadPS4's description of it): one merged mouse for the user
struct MouseOpenParam {
    uint8_t behavior_flag;  // 1: every mouse as one
    uint8_t reserved[7];
};
struct MouseData {
    uint64_t timestamp;
    bool connected;
    uint32_t buttons;  // 1 left, 2 right, 4 middle
    int32_t x, y;      // movement since the last read
    int32_t wheel;
    int32_t tilt;
    uint8_t reserved[8];
};
static_assert(sizeof(MouseData) == 40, "libSceMouse's data");

int (*mouse_init)();
int (*mouse_open)(int32_t user, int32_t type, int32_t index, const MouseOpenParam *param);
int (*mouse_read)(int32_t handle, MouseData *data, int32_t count);

Input state;
int32_t user = -1;
int pad = -1;
int mouse = -1;
uint32_t last_pad_buttons = 0;
uint32_t last_mouse_buttons = 0;
float repeat_wait = 0;
bool finger = false;
float finger_x = 0, finger_y = 0;

void trace(const char *what, int value) {
    char line[128];
    std::snprintf(line, sizeof line, "input: %s (%08x)", what, static_cast<unsigned>(value));
    std::fprintf(stderr, "shell: %s\n", line);
    ps4_boot_trace(line);
}

void load_mouse() {
    static const char *const paths[] = {"/system/common/lib/libSceMouse.sprx", "/system/priv/lib/libSceMouse.sprx"};
    for (const char *path : paths) {
        const int32_t module = static_cast<int32_t>(sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr));
        if (module < 0)
            continue;
        sceKernelDlsym(module, "sceMouseInit", reinterpret_cast<void **>(&mouse_init));
        sceKernelDlsym(module, "sceMouseOpen", reinterpret_cast<void **>(&mouse_open));
        sceKernelDlsym(module, "sceMouseRead", reinterpret_cast<void **>(&mouse_read));
        if (mouse_init && mouse_open && mouse_read) {
            trace("mouse library loaded", module);
            mouse_init();
            return;
        }
    }
    trace("no mouse library", 0);
}

void open_devices() {
    if (user < 0) {
        int32_t initial = -1;
        if (sceUserServiceGetInitialUser(&initial) == 0)
            user = initial;
    }
    if (user < 0)
        return;
    if (pad < 0) {
        pad = scePadOpen(user, 0, 0, nullptr);
        if (pad < 0)
            pad = -1;
    }
    if (mouse < 0 && mouse_open) {
        const MouseOpenParam param = {1, {}};
        const int handle = mouse_open(user, 0, 0, &param);
        if (handle >= 0) {
            mouse = handle;
            trace("mouse opened", handle);
        }
    }
}

// a stick's position as -1..1, with a dead zone
float axis(uint8_t value) {
    const float v = (static_cast<float>(value) - 128.f) / 127.f;
    if (std::fabs(v) < 0.14f)
        return 0.f;
    return std::clamp((v - std::copysign(0.14f, v)) / 0.86f, -1.f, 1.f);
}

} // namespace

void input_open() {
    sceUserServiceInitialize(nullptr);
    scePadInit();
    load_mouse();
    open_devices();
}

const Input &input_poll(float dt) {
    static float reopen = 0;
    if ((pad < 0 || (mouse < 0 && mouse_open)) && (reopen -= dt) <= 0) {
        open_devices();
        reopen = 1.f;  // a controller or mouse connected later
    }
    uint32_t held = 0;
    float dx = 0, dy = 0;
    state.scroll = 0;
    state.pad = false;

    OrbisPadData data{};
    if (pad >= 0 && scePadReadState(pad, &data) == 0 && data.connected) {
        state.pad = true;
        const uint32_t b = data.buttons;
        if (b & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_R2)) held |= BTN_CLICK;
        if (b & ORBIS_PAD_BUTTON_SQUARE) held |= BTN_MENU;
        if (b & ORBIS_PAD_BUTTON_CIRCLE) held |= BTN_BACK;
        if (b & ORBIS_PAD_BUTTON_OPTIONS) held |= BTN_START;
        if (b & ORBIS_PAD_BUTTON_TRIANGLE) held |= BTN_SWITCH;
        if (b & ORBIS_PAD_BUTTON_L1) held |= BTN_PREV;
        if (b & ORBIS_PAD_BUTTON_R1) held |= BTN_NEXT;
        if (b & ORBIS_PAD_BUTTON_UP) held |= BTN_UP;
        if (b & ORBIS_PAD_BUTTON_DOWN) held |= BTN_DOWN;
        if (b & ORBIS_PAD_BUTTON_LEFT) held |= BTN_LEFT;
        if (b & ORBIS_PAD_BUTTON_RIGHT) held |= BTN_RIGHT;
        last_pad_buttons = b;

        // left stick: up to ~1500 px/s, gently near the middle for aiming at small things
        const float sx = axis(data.leftStick.x), sy = axis(data.leftStick.y);
        const float speed = 1500.f * dt;
        dx += sx * std::fabs(sx) * speed;
        dy += sy * std::fabs(sy) * speed;
        // right stick: scrolling, up to ~25 lines/s
        const float ry = axis(data.rightStick.y);
        state.scroll += ry * std::fabs(ry) * 25.f * dt;

        // the touch pad as a laptop's: the finger's movement moves the pointer
        if (data.touch.fingers > 0) {
            const float fx = data.touch.touch[0].x, fy = data.touch.touch[0].y;
            if (finger) {
                dx += (fx - finger_x) * 1.1f;
                dy += (fy - finger_y) * 1.1f;
            }
            finger = true;
            finger_x = fx;
            finger_y = fy;
        } else {
            finger = false;
        }
    }

    state.mouse = false;
    if (mouse >= 0) {
        MouseData events[8];
        const int n = mouse_read(mouse, events, 8);
        for (int i = 0; i < n; i++) {
            if (!events[i].connected)
                continue;
            state.mouse = true;
            dx += events[i].x;
            dy += events[i].y;
            state.scroll -= events[i].wheel * 3.f;
            last_mouse_buttons = events[i].buttons;
        }
        if (n > 0 || state.mouse) {
            if (last_mouse_buttons & 1) held |= BTN_CLICK;
            if (last_mouse_buttons & 2) held |= BTN_MENU;
        }
    }

    const float nx = std::clamp(state.x + dx, 0.f, static_cast<float>(SCREEN_W - 1));
    const float ny = std::clamp(state.y + dy, 0.f, static_cast<float>(SCREEN_H - 1));
    state.moved = nx != state.x || ny != state.y;
    state.x = nx;
    state.y = ny;

    state.pressed = held & ~state.held;
    state.released = state.held & ~held;
    // the D-pad again while it's held: 0.4 s, then 12 a second
    constexpr uint32_t REPEATS = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT;
    state.repeat = state.pressed & REPEATS;
    if (held & REPEATS) {
        if (state.pressed & REPEATS)
            repeat_wait = 0.4f;
        else if ((repeat_wait -= dt) <= 0) {
            state.repeat = held & REPEATS;
            repeat_wait = 1.f / 12.f;
        }
    }
    state.held = held;
    return state;
}

} // namespace shell
