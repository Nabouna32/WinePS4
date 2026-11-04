// WinePS4's USB keyboard (hwkeyboard.h). libSceKeyboard tells which keys are held now (USB HID usage
// IDs, the modifiers as bits), so the presses and releases are the difference to the last read, and
// the auto-repeat is made here, as Windows' keyboard driver makes it. libSceKeyboard is linked, but its
// module (system module 0x106) is loaded here: a call into a system library whose module isn't loaded
// ends the app (input.cpp), so the keyboard is only used once it is.
#include "hwkeyboard.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <orbis/UserService.h>

extern "C" {
void ps4_boot_trace(const char *line);
int32_t sceSysmoduleLoadModule(uint16_t id);
}

namespace shell {
namespace {

// libSceKeyboard's state of the keyboard: OpenOrbis' OrbisKeyboardData, with the field names of the PS5
// ports (Kyty's, SharpProspero's), which agree on where things are
struct KeyboardData {
    uint64_t timestamp;   // microseconds
    uint8_t intercepted;  // the system has the keyboard (its own keyboard dialog is up)
    uint8_t reserved1[7];
    uint8_t connected;
    uint8_t reserved2[3];
    int32_t count;        // entries in keys: 1 (a 0) when none is held
    uint32_t lights;      // 1 Num Lock, 2 Caps Lock, 4 Scroll Lock
    uint32_t mods;        // 1 left Ctrl, 2 left Shift, 4 left Alt, 8 left Windows; x16 the right ones
    uint16_t keys[16];    // the other keys held, as USB HID usage IDs
    uint8_t reserved3[32];
};
static_assert(sizeof(KeyboardData) == 96 && offsetof(KeyboardData, connected) == 16 &&
                  offsetof(KeyboardData, count) == 20 && offsetof(KeyboardData, keys) == 32,
              "libSceKeyboard's data");

constexpr uint16_t KEYBOARD_MODULE = 0x106;
extern "C" {
int32_t sceKeyboardInit();
int32_t sceKeyboardOpen(int32_t user, int32_t type, int32_t index, const void *param);
int32_t sceKeyboardClose(int32_t handle);
int32_t sceKeyboardReadState(int32_t handle, KeyboardData *data);
}
bool keyboard_loaded = false;

// A key as Windows gets it (KEYBDINPUT): virtual key, set-1 scan code, E0 prefix
struct Key {
    uint8_t vk = 0;  // 0: no key of Windows' US layout (ignored)
    uint8_t scan = 0;
    bool ext = false;
    uint8_t nav = 0;  // the number pad's keys without Num Lock: Home, Up, ...
};
struct Usage {
    uint8_t usage;
    Key key;
};

// USB HID usage -> Windows, US layout. The scan codes are Microsoft's USB HID to PS/2 translation
// (Keyboard Scan Code Specification), the virtual keys what Wine's kbdus table (dlls/win32u/input.c)
// makes of them.
constexpr Usage USAGES[] = {
    {0x04, {'A', 0x1e}}, {0x05, {'B', 0x30}}, {0x06, {'C', 0x2e}}, {0x07, {'D', 0x20}},
    {0x08, {'E', 0x12}}, {0x09, {'F', 0x21}}, {0x0a, {'G', 0x22}}, {0x0b, {'H', 0x23}},
    {0x0c, {'I', 0x17}}, {0x0d, {'J', 0x24}}, {0x0e, {'K', 0x25}}, {0x0f, {'L', 0x26}},
    {0x10, {'M', 0x32}}, {0x11, {'N', 0x31}}, {0x12, {'O', 0x18}}, {0x13, {'P', 0x19}},
    {0x14, {'Q', 0x10}}, {0x15, {'R', 0x13}}, {0x16, {'S', 0x1f}}, {0x17, {'T', 0x14}},
    {0x18, {'U', 0x16}}, {0x19, {'V', 0x2f}}, {0x1a, {'W', 0x11}}, {0x1b, {'X', 0x2d}},
    {0x1c, {'Y', 0x15}}, {0x1d, {'Z', 0x2c}},
    {0x1e, {'1', 0x02}}, {0x1f, {'2', 0x03}}, {0x20, {'3', 0x04}}, {0x21, {'4', 0x05}},
    {0x22, {'5', 0x06}}, {0x23, {'6', 0x07}}, {0x24, {'7', 0x08}}, {0x25, {'8', 0x09}},
    {0x26, {'9', 0x0a}}, {0x27, {'0', 0x0b}},
    {0x28, {0x0d, 0x1c}},        // Enter (VK_RETURN)
    {0x29, {0x1b, 0x01}},        // Escape
    {0x2a, {0x08, 0x0e}},        // Backspace (VK_BACK)
    {0x2b, {0x09, 0x0f}},        // Tab
    {0x2c, {0x20, 0x39}},        // Space
    {0x2d, {0xbd, 0x0c}},        // - _ (VK_OEM_MINUS)
    {0x2e, {0xbb, 0x0d}},        // = + (VK_OEM_PLUS)
    {0x2f, {0xdb, 0x1a}},        // [ { (VK_OEM_4)
    {0x30, {0xdd, 0x1b}},        // ] } (VK_OEM_6)
    {0x31, {0xdc, 0x2b}},        // \ | (VK_OEM_5)
    {0x32, {0xdc, 0x2b}},        // ISO keyboards' # ~ by Enter: the US \ | key's place
    {0x33, {0xba, 0x27}},        // ; : (VK_OEM_1)
    {0x34, {0xde, 0x28}},        // ' " (VK_OEM_7)
    {0x35, {0xc0, 0x29}},        // ` ~ (VK_OEM_3)
    {0x36, {0xbc, 0x33}},        // , < (VK_OEM_COMMA)
    {0x37, {0xbe, 0x34}},        // . > (VK_OEM_PERIOD)
    {0x38, {0xbf, 0x35}},        // / ? (VK_OEM_2)
    {0x39, {0x14, 0x3a}},        // Caps Lock (VK_CAPITAL)
    {0x3a, {0x70, 0x3b}}, {0x3b, {0x71, 0x3c}}, {0x3c, {0x72, 0x3d}}, {0x3d, {0x73, 0x3e}},  // F1-F4
    {0x3e, {0x74, 0x3f}}, {0x3f, {0x75, 0x40}}, {0x40, {0x76, 0x41}}, {0x41, {0x77, 0x42}},  // F5-F8
    {0x42, {0x78, 0x43}}, {0x43, {0x79, 0x44}}, {0x44, {0x7a, 0x57}}, {0x45, {0x7b, 0x58}},  // F9-F12
    {0x46, {0x2c, 0x37, true}},  // Print Screen (VK_SNAPSHOT)
    {0x47, {0x91, 0x46}},        // Scroll Lock
    {0x48, {0x13, 0x45}},        // Pause: Num Lock's scan code without E0, as Windows has it
    {0x49, {0x2d, 0x52, true}},  // Insert
    {0x4a, {0x24, 0x47, true}},  // Home
    {0x4b, {0x21, 0x49, true}},  // Page Up (VK_PRIOR)
    {0x4c, {0x2e, 0x53, true}},  // Delete
    {0x4d, {0x23, 0x4f, true}},  // End
    {0x4e, {0x22, 0x51, true}},  // Page Down (VK_NEXT)
    {0x4f, {0x27, 0x4d, true}},  // Right
    {0x50, {0x25, 0x4b, true}},  // Left
    {0x51, {0x28, 0x50, true}},  // Down
    {0x52, {0x26, 0x48, true}},  // Up
    {0x53, {0x90, 0x45, true}},  // Num Lock: extended for Windows
    {0x54, {0x6f, 0x35, true}},  // number pad / (VK_DIVIDE)
    {0x55, {0x6a, 0x37}},        // * (VK_MULTIPLY)
    {0x56, {0x6d, 0x4a}},        // - (VK_SUBTRACT)
    {0x57, {0x6b, 0x4e}},        // + (VK_ADD)
    {0x58, {0x0d, 0x1c, true}},  // Enter
    {0x59, {0x61, 0x4f, false, 0x23}},  // 1, End
    {0x5a, {0x62, 0x50, false, 0x28}},  // 2, Down
    {0x5b, {0x63, 0x51, false, 0x22}},  // 3, Page Down
    {0x5c, {0x64, 0x4b, false, 0x25}},  // 4, Left
    {0x5d, {0x65, 0x4c, false, 0x0c}},  // 5, Clear
    {0x5e, {0x66, 0x4d, false, 0x27}},  // 6, Right
    {0x5f, {0x67, 0x47, false, 0x24}},  // 7, Home
    {0x60, {0x68, 0x48, false, 0x26}},  // 8, Up
    {0x61, {0x69, 0x49, false, 0x21}},  // 9, Page Up
    {0x62, {0x60, 0x52, false, 0x2d}},  // 0, Insert
    {0x63, {0x6e, 0x53, false, 0x2e}},  // . (VK_DECIMAL), Delete
    {0x64, {0xe2, 0x56}},        // ISO keyboards' \ | by the left Shift (VK_OEM_102)
    {0x65, {0x5d, 0x5d, true}},  // Application, the menu key (VK_APPS)
    {0x67, {0x0c, 0x59}},        // number pad = (VK_CLEAR in the US layout)
    {0x68, {0x7c, 0x64}}, {0x69, {0x7d, 0x65}}, {0x6a, {0x7e, 0x66}}, {0x6b, {0x7f, 0x67}},  // F13-F16
    {0x6c, {0x80, 0x68}}, {0x6d, {0x81, 0x69}}, {0x6e, {0x82, 0x6a}}, {0x6f, {0x83, 0x6b}},  // F17-F20
    {0x70, {0x84, 0x6c}}, {0x71, {0x85, 0x6d}}, {0x72, {0x86, 0x6e}}, {0x73, {0x87, 0x76}},  // F21-F24
    {0x7f, {0xad, 0x20, true}},  // Mute (VK_VOLUME_MUTE)
    {0x80, {0xaf, 0x30, true}},  // Volume Up
    {0x81, {0xae, 0x2e, true}},  // Volume Down
    {0x85, {0xc2, 0x7e}},        // Brazilian number pad . (VK_ABNT_C2)
    {0x87, {0xc1, 0x73}},        // Brazilian / ?, Japanese \ _ (International1, VK_ABNT_C1)
    // the modifiers, from the bits of mods (usage 0xe0 + bit)
    {0xe0, {0xa2, 0x1d}},        // left Ctrl (VK_LCONTROL)
    {0xe1, {0xa0, 0x2a}},        // left Shift (VK_LSHIFT)
    {0xe2, {0xa4, 0x38}},        // left Alt (VK_LMENU)
    {0xe3, {0x5b, 0x5b, true}},  // left Windows (VK_LWIN)
    {0xe4, {0xa3, 0x1d, true}},  // right Ctrl (VK_RCONTROL)
    // right Shift (VK_RSHIFT) has no E0, but Wine's server tells the Shifts apart by that flag alone
    // (server/queue.c) and takes it off again before programs see it; Wine's X11 driver sends it so
    {0xe5, {0xa1, 0x36, true}},
    {0xe6, {0xa5, 0x38, true}},  // right Alt (VK_RMENU)
    {0xe7, {0x5c, 0x5c, true}},  // right Windows (VK_RWIN)
};

constexpr std::array<Key, 256> KEYS = [] {
    std::array<Key, 256> keys{};
    for (const Usage &u : USAGES)
        keys[u.usage] = u.key;
    return keys;
}();

constexpr uint8_t USAGE_ROLLOVER = 0x01;  // more keys held than the keyboard can tell apart
constexpr uint8_t USAGE_CAPS = 0x39, USAGE_SCROLL = 0x47, USAGE_PAUSE = 0x48, USAGE_NUM = 0x53;
constexpr uint8_t USAGE_MODIFIERS = 0xe0;  // to 0xe7: the bits of mods
constexpr uint32_t NUM_LOCK = 1, CAPS_LOCK = 2, SCROLL_LOCK = 4;
constexpr uint32_t MODS_CTRL = 0x11, MODS_SHIFT = 0x22;
// Windows' defaults: half a second, then about 30 a second (KeyboardDelay 1, KeyboardSpeed 31)
constexpr float REPEAT_DELAY = 0.5f, REPEAT_PERIOD = 1.f / 30.f;
// how long a light must differ from Windows' toggle before it counts (not just behind a press)
constexpr float LIGHTS_SETTLE = 0.3f;

// a key held, as it was pressed: its release is the same key, even if Num Lock changed since
struct Held {
    uint8_t usage;
    KeyEvent press;
};

int32_t user = -1;
int keyboard = -1;
float reopen = 0;
bool connected = false;
bool intercepted = false;
bool gap = true;                  // the last read had no keyboard (or the system had it)
std::vector<Held> held;           // in the order they were pressed
std::array<bool, 256> blocked{};  // held when the keyboard came (back): taken once let go
uint8_t repeating = 0;            // the key auto-repeating (0: none)
float repeat_wait = 0;
uint32_t told = 0;  // the lock toggles Windows got from here (it starts with all off)
uint32_t lit = 0;   // the lights the PS4 has shown on: it keeps those
float lights_wait = 0;

void trace(const char *what, int value) {
    char line[128];
    std::snprintf(line, sizeof line, "hwkeyboard: %s (%08x)", what, static_cast<unsigned>(value));
    std::fprintf(stderr, "shell: %s\n", line);
    ps4_boot_trace(line);
}

// once: hwkeyboard_open comes again when the keyboard is switched on again in the settings (a module
// that didn't load isn't tried again either)
void load_keyboard() {
    static bool tried = false;
    if (tried)
        return;
    tried = true;
    const int loaded = sceSysmoduleLoadModule(KEYBOARD_MODULE);
    if (loaded != 0) {
        trace("the keyboard module did not load", loaded);
        return;
    }
    const int init = sceKeyboardInit();
    trace("keyboard module loaded, sceKeyboardInit", init);
    keyboard_loaded = true;
}

void open_keyboard() {
    if (!keyboard_loaded || keyboard >= 0)
        return;
    if (user < 0) {
        int32_t initial = -1;
        if (sceUserServiceGetInitialUser(&initial) == 0)
            user = initial;
    }
    if (user < 0)
        return;
    static int last_answer = -1;  // (none yet: SCE errors are 0x80xxxxxx)
    const int handle = sceKeyboardOpen(user, 0, 0, nullptr);  // the standard port's first keyboard
    if (handle >= 0)
        keyboard = handle;
    if (handle != last_answer) {
        last_answer = handle;  // (once per answer: this is tried every second)
        trace(handle >= 0 ? "keyboard opened" : "sceKeyboardOpen failed", handle);
    }
}

uint32_t lock_of(unsigned usage) {
    return usage == USAGE_NUM ? NUM_LOCK : usage == USAGE_CAPS ? CAPS_LOCK : usage == USAGE_SCROLL ? SCROLL_LOCK : 0;
}

// Windows repeats the key pressed last; here not the modifiers and locks, nor Pause (Windows never
// repeats it)
bool repeats(unsigned usage) {
    return usage < USAGE_MODIFIERS && !lock_of(usage) && usage != USAGE_PAUSE;
}

// The event a key is pressed as. The number pad types digits with Num Lock on (as Windows has it) and
// no Shift, else it's Home, Up, ...: Wine's server's rule (queue.c). Ctrl+Pause is Break, as Windows'
// keyboard driver makes it.
KeyEvent press_event(unsigned usage, uint32_t mods) {
    const Key &key = KEYS[usage];
    KeyEvent event{key.vk, key.scan, key.ext, true};
    if (key.nav && (!(told & NUM_LOCK) || (mods & MODS_SHIFT)))
        event.vkey = key.nav;
    if (usage == USAGE_PAUSE && (mods & MODS_CTRL))
        event = {0x03, 0x46, true, true};  // VK_CANCEL, E0 46
    return event;
}

void press(unsigned usage, uint32_t mods, std::vector<KeyEvent> &out) {
    const KeyEvent event = press_event(usage, mods);
    out.push_back(event);
    held.push_back({static_cast<uint8_t>(usage), event});
    told ^= lock_of(usage);  // Windows toggles a lock on its press
    // only the key pressed last repeats: a modifier pressed after a letter stops the letter
    repeating = repeats(usage) ? static_cast<uint8_t>(usage) : 0;
    repeat_wait = REPEAT_DELAY;
}

void release(size_t index, std::vector<KeyEvent> &out) {
    KeyEvent event = held[index].press;
    event.down = false;
    out.push_back(event);
    if (held[index].usage == repeating)
        repeating = 0;
    held.erase(held.begin() + static_cast<std::ptrdiff_t>(index));
}

// No keyboard (or the system has it): every key held is let go, the last pressed first. The keys still
// held when it comes back are only taken once let go (such as the Enter that closed the system's
// keyboard dialog).
void lose(std::vector<KeyEvent> &out) {
    while (!held.empty())
        release(held.size() - 1, out);
    gap = true;
    lights_wait = 0;
}

// Windows toggles Caps, Num and Scroll Lock on each press it gets, so its locks only part from the
// keyboard's lights if it started with others, or a tap went by between two reads: then the lock key
// is pressed for it once more. Only lights the PS4 has shown on are followed (one it doesn't keep
// stays off and is left alone).
void follow_lights(uint32_t lights, bool lock_held, float dt, std::vector<KeyEvent> &out) {
    lit |= lights & (NUM_LOCK | CAPS_LOCK | SCROLL_LOCK);
    const uint32_t differ = (lights ^ told) & lit;
    if (!differ || lock_held) {
        lights_wait = 0;
        return;
    }
    if ((lights_wait += dt) < LIGHTS_SETTLE)
        return;
    lights_wait = 0;
    for (const unsigned usage : {USAGE_NUM, USAGE_CAPS, USAGE_SCROLL}) {
        if (!(differ & lock_of(usage)))
            continue;
        KeyEvent event = press_event(usage, 0);
        out.push_back(event);
        event.down = false;
        out.push_back(event);
        told ^= lock_of(usage);
    }
}

// One read against the last: releases (the last pressed first), then presses (the modifiers first,
// then the keys in the keyboard's order), then the repeat
void apply(const KeyboardData &data, float dt, std::vector<KeyEvent> &out) {
    // what's held now, in the order to press it: the modifiers' bits, then the keys Windows knows
    uint8_t now[24];
    size_t n = 0;
    const auto holds = [&](unsigned usage) { return std::find(now, now + n, usage) != now + n; };
    const auto add = [&](unsigned usage) {
        if (n < std::size(now) && !holds(usage))
            now[n++] = static_cast<uint8_t>(usage);
    };
    for (unsigned bit = 0; bit < 8; bit++)
        if (data.mods & (1u << bit))
            add(USAGE_MODIFIERS + bit);
    const uint16_t *keys = data.keys, *end = keys + std::clamp(data.count, 0, 16);
    if (std::find(keys, end, USAGE_ROLLOVER) != end) {
        // too many keys for the keyboard to tell: the keys of the last read again
        for (const Held &h : held)
            if (h.usage < USAGE_MODIFIERS)
                add(h.usage);
        for (unsigned usage = 0; usage < USAGE_MODIFIERS; usage++)
            if (blocked[usage])
                add(usage);
    } else {
        for (const uint16_t *key = keys; key != end; key++)
            if (*key < KEYS.size() && KEYS[*key].vk)
                add(*key);
    }

    if (gap) {
        for (size_t i = 0; i < n; i++)
            blocked[now[i]] = true;
        gap = false;
    }
    for (unsigned usage = 0; usage < blocked.size(); usage++)
        if (blocked[usage] && !holds(usage))
            blocked[usage] = false;  // let go: its next press counts

    for (size_t i = held.size(); i-- > 0;)
        if (!holds(held[i].usage))
            release(i, out);

    follow_lights(data.lights, holds(USAGE_NUM) || holds(USAGE_CAPS) || holds(USAGE_SCROLL), dt, out);

    bool pressed = false;
    for (size_t i = 0; i < n; i++) {
        const unsigned usage = now[i];
        const auto is_it = [&](const Held &h) { return h.usage == usage; };
        if (blocked[usage] || std::any_of(held.begin(), held.end(), is_it))
            continue;
        press(usage, data.mods, out);
        pressed = true;
    }

    // the key pressed last, again while it's held: after REPEAT_DELAY, then every REPEAT_PERIOD
    if (!repeating || pressed)
        return;
    const auto key = std::find_if(held.begin(), held.end(), [](const Held &h) { return h.usage == repeating; });
    if (key == held.end())
        return;
    repeat_wait -= dt;
    for (int i = 0; i < 4 && repeat_wait <= 0; i++) {
        out.push_back(key->press);
        repeat_wait += REPEAT_PERIOD;
    }
    if (repeat_wait <= 0)
        repeat_wait = REPEAT_PERIOD;  // (after a long frame: a few, not a burst)
}

// Settings > Diagnostics > Keyboard log: each new shape of what libSceKeyboard reads (how many keys,
// in which slots, their high bits, how many the table doesn't know, whether modifiers are held) and
// the presses it made; never which keys (passwords are typed with it). 400 lines at most.
void log_shape(const KeyboardData &data, const std::vector<KeyEvent> &events) {
    static const bool on = [] {
        const char *v = std::getenv("WINEPS4_KEY_LOG");
        return !v || std::strcmp(v, "0") != 0;
    }();
    static unsigned lines = 0;
    static uint64_t last = ~0ull;
    if (!on || lines >= 400)
        return;
    const int count = std::clamp(data.count, 0, 16);
    unsigned slots = 0, high = 0, unknown = 0, presses = 0;
    for (int i = 0; i < count; i++) {
        if (!data.keys[i])
            continue;
        slots |= 1u << i;
        high |= data.keys[i] >> 8;
        if (data.keys[i] >= KEYS.size() || !KEYS[data.keys[i]].vk)
            unknown++;
    }
    for (const KeyEvent &e : events)
        presses += e.down;
    const uint64_t shape = static_cast<uint64_t>(static_cast<uint32_t>(data.count)) | static_cast<uint64_t>(slots) << 32 |
                           static_cast<uint64_t>(high & 0xff) << 48 | static_cast<uint64_t>(unknown & 0x1f) << 56 |
                           static_cast<uint64_t>(data.mods != 0) << 61 | static_cast<uint64_t>(data.intercepted != 0) << 62 |
                           static_cast<uint64_t>(data.connected != 0) << 63;
    if (shape == last && !presses)
        return;
    last = shape;
    lines++;
    std::fprintf(stderr,
                 "shell: hwkeyboard: read: count %d, keys in slots %#x, high bits %#x, %u not in the table, modifiers %s, "
                 "%s%s; %u presses\n",
                 data.count, slots, high, unknown, data.mods ? "held" : "none", data.connected ? "connected" : "not connected",
                 data.intercepted ? ", the system's" : "", presses);
}

} // namespace

void hwkeyboard_open() {
    load_keyboard();
    open_keyboard();
}

std::vector<KeyEvent> hwkeyboard_poll(float dt) {
    std::vector<KeyEvent> events;
    if (keyboard < 0 && keyboard_loaded && (reopen -= dt) <= 0) {
        open_keyboard();
        reopen = 1.f;  // a keyboard connected later, or a handle that stopped reading
    }
    KeyboardData data{};
    int result = -1;
    if (keyboard >= 0) {
        result = sceKeyboardReadState(keyboard, &data);
        if (result < 0) {
            static int last_error = 0;
            if (result != last_error) {
                last_error = result;
                trace("sceKeyboardReadState failed", result);
            }
            sceKeyboardClose(keyboard);  // opened again in a second
            keyboard = -1;
            reopen = 1.f;
        }
    }
    const bool now_connected = result >= 0 && data.connected;
    if (now_connected != connected) {
        connected = now_connected;
        trace(connected ? "keyboard connected" : "keyboard disconnected", result);
    }
    const bool now_intercepted = connected && data.intercepted;
    if (now_intercepted != intercepted) {
        intercepted = now_intercepted;
        if (connected)
            trace(intercepted ? "the system has the keyboard" : "the keyboard is back", keyboard);
    }
    if (connected && !intercepted)
        apply(data, dt, events);
    else
        lose(events);
    if (result >= 0)
        log_shape(data, events);
    return events;
}

bool hwkeyboard_connected() {
    return connected;
}

} // namespace shell
