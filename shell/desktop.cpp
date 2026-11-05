// WinePS4's desktop (shell.h): a Windows 11-style desktop of its own (no Microsoft artwork) drawn on
// the CPU with HENkaku's canvas. Files browses the console and runs programs, Terminal shows what
// the running program prints, Settings holds the diagnostics switches (env.txt). The taskbar and the
// Start menu are see-through over a blurred copy of the wallpaper.
//
// One Windows program per run of the app: Wine can't start another process on the PS4, so running
// a program writes it to launch.txt and restarts the app, which then runs it (ps4_main.c).
#include "shell.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "../port/include/ps4_display.h"
#include "exeicon.h"
#include "fileops.h"
#include "hwkeyboard.h"
#include "input.h"
#include "keyboard.h"
#include "picture.h"
#include "ps4_canvas.h"
#include "screen.h"
#include "winwindows.h"

#ifndef WINEPS4_VERSION
#define WINEPS4_VERSION "00.00"
#endif

extern "C" {
int sceSystemServiceLoadExec(const char *path, const char *args[]);
void ps4_boot_trace(const char *line);
size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAvailableDirectMemorySize(off_t start, off_t end, size_t align, off_t *start_out, size_t *size_out);
int32_t sceKernelIsNeoMode(void);
}

using namespace ps4ui;

namespace shell {
namespace {

constexpr int W = SCREEN_W, H = SCREEN_H;
constexpr int TASKBAR_H = 60;
constexpr int TITLE_H = 40;
constexpr int RADIUS = 8;
constexpr int SHADOW = 28;

// the theme: dark or light (Settings > Personalization), set by apply_theme
Color TEXT, TEXT2, TEXT3, ACCENT, ACCENT_HOT, ACCENT_BORDER, ACCENT_TEXT;
Color WINDOW_BG, CONTENT_BG, BORDER, BORDER_ACTIVE, CONTROL, HOVER, PRESSED, SELECTED, LINE, DISABLED, SCROLLBAR;
Color TASKBAR_TINT, TASK_ACTIVE, TASK_HOVER, START_TINT, START_FOOTER, SEARCH_BG, SEARCH_BORDER;
Color POPUP_BG, POPUP_BORDER, MENU_TINT, CARD_TINT, DIALOG_BG, DIALOG_FOOT;
Color BUTTON, BUTTON_HOT, BUTTON_BORDER, CARD, CARD_HOT, CARD_BORDER, TRACK, ICON_HOLE;
const Color CLOSE_RED = rgb(0xc42b1c);
bool light_theme = false;

// Settings > Bluetooth & devices: the mouse's speed (1-20, as Windows' slider), whether a USB
// keyboard types straight into programs (else the PS4's keyboard opens for text, Triangle), and
// whether the PS4's keyboard opens by itself for a text field clicked
int pointer_speed = 10;
bool direct_keyboard = false;
bool auto_keyboard = true;

void apply_theme(bool light) {
    light_theme = light;
    const auto pick = [&](uint32_t dark, uint32_t lite, uint8_t a = 255) { return rgb(light ? lite : dark, a); };
    const auto over_alpha = [&](uint8_t a_dark, uint8_t a_light) { return light ? rgb(0x000000, a_light) : rgb(0xffffff, a_dark); };
    TEXT = pick(0xffffff, 0x1a1a1a);
    TEXT2 = pick(0xc8c8c8, 0x5c5c5c);
    TEXT3 = pick(0x8a8a8a, 0x7a7a7a);
    ACCENT = pick(0x4cc2ff, 0x005fb8);
    ACCENT_HOT = pick(0x6dd0ff, 0x196ebf);
    ACCENT_BORDER = pick(0x3aa6e0, 0x004a94);
    ACCENT_TEXT = pick(0x000000, 0xffffff);
    WINDOW_BG = pick(0x202020, 0xf3f3f3);
    CONTENT_BG = pick(0x1b1b1b, 0xfafafa);
    BORDER = pick(0x3a3a3a, 0xdcdcdc);
    BORDER_ACTIVE = pick(0x4a4a4a, 0xc4c4c4);
    CONTROL = pick(0x2d2d2d, 0xffffff);
    HOVER = over_alpha(0x12, 0x0a);
    PRESSED = over_alpha(0x0a, 0x06);
    SELECTED = over_alpha(0x1c, 0x10);
    LINE = over_alpha(0x16, 0x14);
    DISABLED = pick(0x6e6e6e, 0xa0a0a0);
    SCROLLBAR = over_alpha(0x50, 0x40);
    TASKBAR_TINT = pick(0x1c1c1c, 0xeeeeee, light ? 0xd6 : 0xc8);
    TASK_ACTIVE = over_alpha(0x1a, 0x10);
    TASK_HOVER = over_alpha(0x10, 0x0a);
    START_TINT = pick(0x242424, 0xf3f3f3, light ? 0xe2 : 0xd8);
    START_FOOTER = pick(0x1c1c1c, 0xe9e9e9, light ? 0xd0 : 0xb0);
    SEARCH_BG = pick(0x1d1d1d, 0xffffff);
    SEARCH_BORDER = pick(0x444444, 0xd0d0d0);
    POPUP_BG = pick(0x2c2c2c, 0xf9f9f9);
    POPUP_BORDER = pick(0x454545, 0xd8d8d8);
    MENU_TINT = pick(0x2c2c2c, 0xf9f9f9, light ? 0xf2 : 0xec);
    CARD_TINT = pick(0x262626, 0xf9f9f9, light ? 0xec : 0xe8);
    DIALOG_BG = pick(0x2b2b2b, 0xf9f9f9);
    DIALOG_FOOT = pick(0x202020, 0xf0f0f0);
    BUTTON = pick(0x333333, 0xfbfbfb);
    BUTTON_HOT = pick(0x3d3d3d, 0xf0f0f0);
    BUTTON_BORDER = pick(0x454545, 0xd0d0d0);
    CARD = pick(0x2b2b2b, 0xffffff);
    CARD_HOT = pick(0x323232, 0xf6f6f6);
    CARD_BORDER = pick(0x1d1d1d, 0xe5e5e5);
    TRACK = over_alpha(0x20, 0x18);
    ICON_HOLE = pick(0x202020, 0xf0f0f0);
}

const char *const DATA_DIR = "/data/WinePS4";
const char *const ENV_FILE = "/data/WinePS4/env.txt";
const char *const RECENT_FILE = "/data/WinePS4/recent.txt";
const char *const C_DRIVE = "/data/WinePS4/prefix/drive_c";

double now_seconds() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

std::string base_name(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string lower(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string &s, const char *suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && lower(s.substr(s.size() - n)) == suffix;
}

bool is_dir(const std::string &path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string &path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// --- what the program's thread reports (any thread) --------------------------------------------

std::mutex program_lock;
std::deque<std::string> program_lines;
std::string program_partial;
std::string program_status;
std::map<int, std::string> programs_running;  // pid -> name
std::string last_ended;                        // "<name> ended (exit code N)"
int last_ended_code = 0;
std::deque<std::string> ended_toasts;          // for the desktop's thread
std::atomic<bool> program_changed{false};

Input last_input;  // this frame's, for drawing

// --- pixels ---------------------------------------------------------------------------------

inline uint32_t pack(Color c) {
    return c.r | (c.g << 8) | (c.b << 16) | (static_cast<uint32_t>(c.a) << 24);
}

// premultiplied s over d
inline uint32_t over(uint32_t d, uint32_t s) {
    const uint32_t sa = s >> 24;
    if (sa == 255)
        return s;
    if (sa == 0)
        return d;
    const uint32_t ia = 255 - sa;
    const auto ch = [&](int shift) {
        const uint32_t dv = (d >> shift) & 0xff, sv = (s >> shift) & 0xff;
        return std::min<uint32_t>(255, sv + (dv * ia + 127) / 255);
    };
    return ch(0) | (ch(8) << 8) | (ch(16) << 16) | 0xff000000u;
}

// coverage of the pixel at (px, py) by a rounded rectangle, antialiased
inline float round_rect_coverage(float px, float py, float x, float y, float w, float h, float r) {
    const float hw = w * 0.5f, hh = h * 0.5f;
    const float qx = std::fabs(px - (x + hw)) - (hw - r), qy = std::fabs(py - (y + hh)) - (hh - r);
    const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
    return std::clamp(0.5f - d, 0.f, 1.f);
}

// --- the wallpaper (made once) and its blurred copy for the see-through panels -----------------

std::vector<uint32_t> wallpaper;
constexpr int BLUR_W = 240, BLUR_H = 135;  // 1/8 of the screen
std::vector<float> blur;                   // BLUR_W x BLUR_H x 3

// The generated backgrounds: a deep base, two soft glows and a ribbon of light sweeping across
struct Style {
    const char *name;
    float base[3];
    float gradient;  // how much lighter towards one side
    float glow1[3], glow2[3], ribbon[3], halo[3];
};
const Style STYLES[] = {
    {"Ribbon", {0.02f, 0.05f, 0.16f}, 1.f, {0.06f, 0.28f, 0.70f}, {0.30f, 0.10f, 0.55f}, {0.55f, 0.78f, 1.00f}, {0.10f, 0.30f, 0.60f}},
    {"Aurora", {0.01f, 0.05f, 0.06f}, 1.f, {0.04f, 0.40f, 0.34f}, {0.10f, 0.18f, 0.45f}, {0.40f, 1.00f, 0.72f}, {0.08f, 0.40f, 0.32f}},
    {"Graphite", {0.05f, 0.05f, 0.06f}, 0.5f, {0.10f, 0.10f, 0.12f}, {0.09f, 0.09f, 0.11f}, {0.40f, 0.42f, 0.48f}, {0.08f, 0.08f, 0.09f}},
    {"Daylight", {0.50f, 0.62f, 0.80f}, 2.f, {0.30f, 0.40f, 0.50f}, {0.40f, 0.35f, 0.50f}, {0.60f, 0.60f, 0.62f}, {0.20f, 0.24f, 0.28f}},
};

uint32_t style_pixel(const Style &st, float u, float v) {
    float r = st.base[0] + 0.03f * u * st.gradient, g = st.base[1] + 0.06f * (1.f - v) * st.gradient,
          bl = st.base[2] + 0.10f * u * st.gradient;
    const float g1 = std::exp(-((u - 0.64f) * (u - 0.64f) * 7.f + (v - 0.52f) * (v - 0.52f) * 10.f));
    const float g2 = std::exp(-((u - 0.36f) * (u - 0.36f) * 9.f + (v - 0.66f) * (v - 0.66f) * 8.f));
    const float d = v - (0.60f + 0.15f * std::sin(u * 3.1f + 0.5f));
    const float ribbon = std::exp(-d * d * 260.f) * (0.35f + 0.65f * std::exp(-(u - 0.55f) * (u - 0.55f) * 4.f));
    const float halo = std::exp(-d * d * 30.f) * 0.35f;
    r += g1 * st.glow1[0] + g2 * st.glow2[0] + ribbon * st.ribbon[0] + halo * st.halo[0];
    g += g1 * st.glow1[1] + g2 * st.glow2[1] + ribbon * st.ribbon[1] + halo * st.halo[1];
    bl += g1 * st.glow1[2] + g2 * st.glow2[2] + ribbon * st.ribbon[2] + halo * st.halo[2];
    const auto tone = [](float c) { return static_cast<uint32_t>(255.f * (1.f - std::exp(-c * 1.6f))); };
    return tone(r) | (tone(g) << 8) | (tone(bl) << 16) | 0xff000000u;
}

// The photos in the package (scripts/make-wallpapers.py)
struct Photo {
    const char *name;
    const char *path;
};
const Photo PHOTOS[] = {
    {"Mountain lake", "/app0/wallpapers/mountain-lake.jpg"}, {"Dunes", "/app0/wallpapers/dunes.jpg"},
    {"Starry night", "/app0/wallpapers/starry-night.jpg"}, {"Ocean sunset", "/app0/wallpapers/ocean-sunset.jpg"},
    {"Forest mist", "/app0/wallpapers/forest-mist.jpg"},
};

// Solid colours to pick from (0xRRGGBB)
const uint32_t SWATCHES[] = {0x0063b1, 0x2d7d9a, 0x00b294, 0x10893e, 0x498205, 0xca5010, 0xda3b01, 0xd13438,
                             0xe3008c, 0x881798, 0x744da9, 0x5c2e91, 0x4c4a48, 0x2d2d30, 0x7a7574, 0xb0b0b0};

enum class BgKind { Style, Photo, Color };
struct Background {
    BgKind kind = BgKind::Style;
    int style = 0;
    std::string photo;
    uint32_t color = 0x0063b1;
};
Background background;
const char *const DESKTOP_FILE = "/data/WinePS4/desktop.ini";

inline uint32_t from_rgb(uint32_t hex) { return ((hex >> 16) & 0xff) | (hex & 0xff00) | ((hex & 0xff) << 16) | 0xff000000u; }

// the blurred copy of the wallpaper, for the see-through panels: 8x8 averages, then three box blurs
void make_blur() {
    blur.assign(BLUR_W * BLUR_H * 3, 0.f);
    for (int by = 0; by < BLUR_H; by++)
        for (int bx = 0; bx < BLUR_W; bx++) {
            float sum[3] = {};
            for (int y = by * 8; y < by * 8 + 8; y++)
                for (int x = bx * 8; x < bx * 8 + 8; x++) {
                    const uint32_t p = wallpaper[static_cast<size_t>(y) * W + x];
                    sum[0] += p & 0xff;
                    sum[1] += (p >> 8) & 0xff;
                    sum[2] += (p >> 16) & 0xff;
                }
            for (int c = 0; c < 3; c++)
                blur[(by * BLUR_W + bx) * 3 + c] = sum[c] / 64.f;
        }
    std::vector<float> tmp(blur.size());
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < BLUR_H; y++)
            for (int x = 0; x < BLUR_W; x++)
                for (int c = 0; c < 3; c++) {
                    float sum = 0;
                    for (int k = -3; k <= 3; k++)
                        sum += blur[(y * BLUR_W + std::clamp(x + k, 0, BLUR_W - 1)) * 3 + c];
                    tmp[(y * BLUR_W + x) * 3 + c] = sum / 7.f;
                }
        for (int y = 0; y < BLUR_H; y++)
            for (int x = 0; x < BLUR_W; x++)
                for (int c = 0; c < 3; c++) {
                    float sum = 0;
                    for (int k = -3; k <= 3; k++)
                        sum += tmp[(std::clamp(y + k, 0, BLUR_H - 1) * BLUR_W + x) * 3 + c];
                    blur[(y * BLUR_W + x) * 3 + c] = sum / 7.f;
                }
    }
}

std::string background_spec(const Background &b) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "%06x", b.color & 0xffffff);
    return b.kind == BgKind::Style ? "style:" + std::to_string(b.style) : b.kind == BgKind::Photo ? "photo:" + b.photo : std::string("color:") + hex;
}

void save_desktop() {
    if (FILE *f = std::fopen(DESKTOP_FILE, "w")) {
        std::fprintf(f, "theme=%s\n", light_theme ? "light" : "dark");
        std::fprintf(f, "background=%s\n", background_spec(background).c_str());
        std::fprintf(f, "pointer=%d\n", pointer_speed);
        std::fprintf(f, "keyboard=%s\n", direct_keyboard ? "direct" : "ps4");
        std::fprintf(f, "keyboard_by_itself=%d\n", auto_keyboard ? 1 : 0);
        std::fclose(f);
    }
}

// The wallpaper (and its blurred copy) for b; false with why in error (a photo that won't load)
bool set_background(const Background &b, std::string *error = nullptr) {
    std::vector<uint32_t> px(static_cast<size_t>(W) * H);
    if (b.kind == BgKind::Photo) {
        Picture pic;
        std::string why;
        if (!load_picture(b.photo, pic, why)) {
            if (error)
                *error = why;
            return false;
        }
        cover(pic, W, H, px.data());
    } else if (b.kind == BgKind::Color) {
        std::fill(px.begin(), px.end(), from_rgb(b.color));
    } else {
        const Style &st = STYLES[std::clamp(b.style, 0, static_cast<int>(std::size(STYLES)) - 1)];
        parallel_rows(0, H, static_cast<size_t>(W) * H, [&](int a, int z) {
            for (int y = a; y < z; y++)
                for (int x = 0; x < W; x++)
                    px[static_cast<size_t>(y) * W + x] = style_pixel(st, x / static_cast<float>(W), y / static_cast<float>(H));
        });
    }
    wallpaper.swap(px);
    make_blur();
    background = b;
    return true;
}

// desktop.ini: theme=dark|light, background=style:<n>|photo:<path>|color:<rrggbb>, pointer=1-20,
// keyboard=ps4|direct
void load_desktop() {
    bool light = false;
    Background b;
    if (FILE *f = std::fopen(DESKTOP_FILE, "r")) {
        char line[1100];
        while (std::fgets(line, sizeof line, f)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
                l.pop_back();
            if (l == "theme=light")
                light = true;
            else if (l.rfind("background=style:", 0) == 0)
                b.kind = BgKind::Style, b.style = std::atoi(l.c_str() + 17);
            else if (l.rfind("background=photo:", 0) == 0)
                b.kind = BgKind::Photo, b.photo = l.substr(17);
            else if (l.rfind("background=color:", 0) == 0)
                b.kind = BgKind::Color, b.color = static_cast<uint32_t>(std::strtoul(l.c_str() + 17, nullptr, 16));
            else if (l.rfind("pointer=", 0) == 0)
                pointer_speed = std::clamp(std::atoi(l.c_str() + 8), 1, 20);
            else if (l == "keyboard=direct")
                direct_keyboard = true;
            else if (l == "keyboard_by_itself=0")
                auto_keyboard = false;
        }
        std::fclose(f);
    }
    input_set_pointer_speed(pointer_speed);
    apply_theme(light);
    if (!set_background(b))
        set_background(Background{});  // (a photo since removed: the default)
}

// the blurred wallpaper at a screen pixel (bilinear)
inline void blurred_at(int x, int y, float out[3]) {
    const float fx = std::clamp((x + 0.5f) / 8.f - 0.5f, 0.f, BLUR_W - 1.001f);
    const float fy = std::clamp((y + 0.5f) / 8.f - 0.5f, 0.f, BLUR_H - 1.001f);
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const float tx = fx - x0, ty = fy - y0;
    for (int c = 0; c < 3; c++) {
        const float a = blur[(y0 * BLUR_W + x0) * 3 + c], b = blur[(y0 * BLUR_W + x0 + 1) * 3 + c];
        const float d = blur[((y0 + 1) * BLUR_W + x0) * 3 + c], e = blur[((y0 + 1) * BLUR_W + x0 + 1) * 3 + c];
        out[c] = (a + (b - a) * tx) + ((d + (e - d) * tx) - (a + (b - a) * tx)) * ty;
    }
}

// A see-through panel: the blurred wallpaper under a tint (its alpha: how much of the tint), in a
// rounded rectangle (radius 0: square)
void acrylic(Canvas &c, int x, int y, int w, int h, int radius, Color tint) {
    const int x0 = std::max(x, 0), x1 = std::min(x + w, W), y0 = std::max(y, 0), y1 = std::min(y + h, H);
    if (x0 >= x1 || y0 >= y1)
        return;
    uint32_t *const pixels = c.data();
    const float ta = tint.a / 255.f;
    parallel_rows(y0, y1, static_cast<size_t>(x1 - x0) * (y1 - y0), [&](int a, int b) {
        for (int yy = a; yy < b; yy++) {
            uint32_t *row = pixels + static_cast<size_t>(yy) * W;
            for (int xx = x0; xx < x1; xx++) {
                const float cov = radius ? round_rect_coverage(xx + 0.5f, yy + 0.5f, x, y, w, h, radius) : 1.f;
                if (cov <= 0.f)
                    continue;
                float bl[3];
                blurred_at(xx, yy, bl);
                const float r = bl[0] + (tint.r - bl[0]) * ta, g = bl[1] + (tint.g - bl[1]) * ta, bb = bl[2] + (tint.b - bl[2]) * ta;
                const uint32_t a8 = static_cast<uint32_t>(cov * 255.f + 0.5f);
                const uint32_t src = (static_cast<uint32_t>(r * cov) & 0xff) | ((static_cast<uint32_t>(g * cov) & 0xff) << 8) |
                                     ((static_cast<uint32_t>(bb * cov) & 0xff) << 16) | (a8 << 24);
                row[xx] = over(row[xx], src);
            }
        }
    });
}

// a rounded rectangle with a 1 px border
void panel(Canvas &c, int x, int y, int w, int h, int radius, Color fill, Color border) {
    c.fill_round_rect(x, y, w, h, radius, border);
    c.fill_round_rect(x + 1, y + 1, w - 2, h - 2, std::max(radius - 1, 0), fill);
}

// --- shadows (one sprite per size) and the pointer ----------------------------------------------

Image make_shadow(int w, int h, int alpha) {
    Image s;
    s.width = w + 2 * SHADOW;
    s.height = h + 2 * SHADOW;
    s.pixels.assign(static_cast<size_t>(s.width) * s.height, 0);
    for (int y = 0; y < s.height; y++)
        for (int x = 0; x < s.width; x++) {
            // distance outside the window (moved down a little: light from above)
            const float px = x + 0.5f - SHADOW, py = y + 0.5f - SHADOW - 8.f;
            const float hw = w * 0.5f, hh = h * 0.5f;
            const float qx = std::fabs(px - hw) - (hw - RADIUS), qy = std::fabs(py - hh) - (hh - RADIUS);
            const float d = std::sqrt(std::max(qx, 0.f) * std::max(qx, 0.f) + std::max(qy, 0.f) * std::max(qy, 0.f)) - RADIUS;
            const float a = d <= 0 ? 1.f : std::exp(-(d * d) / 220.f);
            s.pixels[static_cast<size_t>(y) * s.width + x] = static_cast<uint32_t>(a * alpha) << 24;
        }
    return s;
}

const Image &shadow_for(int w, int h, bool focused) {
    static std::map<std::tuple<int, int, bool>, Image> cache;
    auto key = std::make_tuple(w, h, focused);
    auto it = cache.find(key);
    if (it == cache.end()) {
        if (cache.size() > 24)
            cache.clear();
        it = cache.emplace(key, make_shadow(w, h, focused ? 150 : 90)).first;
    }
    return it->second;
}

// The arrow: white with a dark outline, its tip at (0, 0)
Image make_pointer() {
    static const float pts[][2] = {{0, 0}, {0, 26}, {6.5f, 20.5f}, {10.5f, 29.5f}, {14.5f, 27.8f}, {10.5f, 19}, {18.5f, 19}};
    constexpr int N = sizeof(pts) / sizeof(pts[0]);
    const float scale = 1.25f;
    Image img;
    img.width = 28;
    img.height = 42;
    img.pixels.assign(static_cast<size_t>(img.width) * img.height, 0);
    const auto inside = [&](float x, float y) {
        bool in = false;
        for (int i = 0, j = N - 1; i < N; j = i++) {
            const float xi = pts[i][0] * scale + 1.5f, yi = pts[i][1] * scale + 1.5f;
            const float xj = pts[j][0] * scale + 1.5f, yj = pts[j][1] * scale + 1.5f;
            if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
                in = !in;
        }
        return in;
    };
    const auto edge = [&](float x, float y) {
        float best = 1e9f;
        for (int i = 0, j = N - 1; i < N; j = i++) {
            const float ax = pts[j][0] * scale + 1.5f, ay = pts[j][1] * scale + 1.5f;
            const float bx = pts[i][0] * scale + 1.5f, by = pts[i][1] * scale + 1.5f;
            const float vx = bx - ax, vy = by - ay;
            const float t = std::clamp(((x - ax) * vx + (y - ay) * vy) / (vx * vx + vy * vy), 0.f, 1.f);
            const float dx = x - (ax + vx * t), dy = y - (ay + vy * t);
            best = std::min(best, dx * dx + dy * dy);
        }
        return std::sqrt(best);
    };
    for (int y = 0; y < img.height; y++)
        for (int x = 0; x < img.width; x++) {
            float white = 0, dark = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    const float fx = x + (sx + 0.5f) / 4.f, fy = y + (sy + 0.5f) / 4.f;
                    const float e = edge(fx, fy);
                    if (e < 1.3f)
                        dark += 1;
                    else if (inside(fx, fy))
                        white += 1;
                }
            const float a = (white + dark) / 16.f;
            const uint32_t v = static_cast<uint32_t>(255.f * white / 16.f);
            img.pixels[static_cast<size_t>(y) * img.width + x] = v | (v << 8) | (v << 16) | (static_cast<uint32_t>(a * 255.f) << 24);
        }
    return img;
}

// --- icons (drawn, not pictures) -------------------------------------------------------------

enum class Icon { Logo, Search, Folder, Terminal, Settings, Computer, Drive, Usb, Program, File, Power, Pad, Mouse, Up, User, Package, Picture, Brush, Info,
                  Keyboard, Gauge, Apps, Download, Document, Music, Video, Desktop,
                  Back, Forward, Refresh, Cut, Copy, Paste, Rename, Trash, Plus, Sort, View, Notepad, Hex, Home, Chevron, Check, Save };

void arc(Canvas &c, float cx, float cy, float r, float from, float to, float t, Color col) {
    const int steps = 24;
    for (int i = 0; i < steps; i++) {
        const float a0 = from + (to - from) * i / steps, a1 = from + (to - from) * (i + 1) / steps;
        c.line(cx + std::cos(a0) * r, cy + std::sin(a0) * r, cx + std::cos(a1) * r, cy + std::sin(a1) * r, t, col);
    }
}

void draw_icon(Canvas &c, Icon icon, float cx, float cy, float s) {
    const int x = static_cast<int>(cx - s / 2), y = static_cast<int>(cy - s / 2), is = static_cast<int>(s);
    switch (icon) {
    case Icon::Logo: {  // WinePS4's wine glass
        const Color glass = rgb(0xe9e6f2), wine = rgb(0xc8334f), wine_top = rgb(0xe0566f);
        c.fill_round_rect(x + is * 18 / 100, y + is * 4 / 100, is * 64 / 100, is * 52 / 100, is * 22 / 100, glass);
        c.fill_round_rect(x + is * 22 / 100, y + is * 22 / 100, is * 56 / 100, is * 31 / 100, is * 18 / 100, wine);
        c.fill_rect(x + is * 22 / 100, y + is * 22 / 100, is * 56 / 100, is * 5 / 100, wine_top);
        c.fill_rect(x + is * 46 / 100, y + is * 54 / 100, is * 8 / 100, is * 30 / 100, glass);
        c.fill_round_rect(x + is * 26 / 100, y + is * 84 / 100, is * 48 / 100, is * 10 / 100, is * 5 / 100, glass);
        break;
    }
    case Icon::Search:
        c.ring(cx - s * 0.08f, cy - s * 0.08f, s * 0.27f, s * 0.09f, TEXT);
        c.line(cx + s * 0.13f, cy + s * 0.13f, cx + s * 0.36f, cy + s * 0.36f, s * 0.1f, TEXT);
        break;
    case Icon::Folder:
        c.fill_round_rect(x + is * 6 / 100, y + is * 16 / 100, is * 40 / 100, is * 20 / 100, is * 6 / 100, rgb(0xd29a25));
        c.fill_round_rect(x + is * 6 / 100, y + is * 24 / 100, is * 88 / 100, is * 62 / 100, is * 8 / 100, rgb(0xf2c14b));
        c.fill_rect(x + is * 6 / 100, y + is * 34 / 100, is * 88 / 100, is * 3 / 100, rgb(0xe3ad34));
        break;
    case Icon::Terminal:
        c.fill_round_rect(x + is * 6 / 100, y + is * 12 / 100, is * 88 / 100, is * 76 / 100, is * 10 / 100, rgb(0x4a4a4a));
        c.fill_round_rect(x + is * 9 / 100, y + is * 15 / 100, is * 82 / 100, is * 70 / 100, is * 8 / 100, rgb(0x161616));
        c.line(cx - s * 0.25f, cy - s * 0.12f, cx - s * 0.08f, cy + s * 0.02f, s * 0.07f, TEXT);
        c.line(cx - s * 0.08f, cy + s * 0.02f, cx - s * 0.25f, cy + s * 0.16f, s * 0.07f, TEXT);
        c.line(cx + s * 0.0f, cy + s * 0.18f, cx + s * 0.24f, cy + s * 0.18f, s * 0.07f, ACCENT);
        break;
    case Icon::Settings: {
        const Color col = rgb(0xb8bcc4);
        for (int i = 0; i < 8; i++) {
            const float a = i * 3.14159265f / 4.f;
            c.line(cx + std::cos(a) * s * 0.22f, cy + std::sin(a) * s * 0.22f, cx + std::cos(a) * s * 0.42f,
                   cy + std::sin(a) * s * 0.42f, s * 0.13f, col);
        }
        c.fill_circle(cx, cy, s * 0.31f, col);
        c.fill_circle(cx, cy, s * 0.13f, ICON_HOLE);
        break;
    }
    case Icon::Computer:
        c.fill_round_rect(x + is * 6 / 100, y + is * 12 / 100, is * 88 / 100, is * 58 / 100, is * 7 / 100, rgb(0x9aa4b2));
        c.fill_round_rect(x + is * 10 / 100, y + is * 16 / 100, is * 80 / 100, is * 50 / 100, is * 4 / 100, rgb(0x2f7fd6));
        c.fill_rect(x + is * 44 / 100, y + is * 70 / 100, is * 12 / 100, is * 12 / 100, rgb(0x9aa4b2));
        c.fill_round_rect(x + is * 28 / 100, y + is * 82 / 100, is * 44 / 100, is * 8 / 100, is * 4 / 100, rgb(0x9aa4b2));
        break;
    case Icon::Drive:
        c.fill_round_rect(x + is * 6 / 100, y + is * 30 / 100, is * 88 / 100, is * 42 / 100, is * 8 / 100, rgb(0x9aa4b2));
        c.fill_round_rect(x + is * 10 / 100, y + is * 34 / 100, is * 80 / 100, is * 22 / 100, is * 5 / 100, rgb(0xc9d0da));
        c.fill_rect(x + is * 66 / 100, y + is * 60 / 100, is * 18 / 100, is * 5 / 100, rgb(0x52d16b));
        break;
    case Icon::Usb:
        c.fill_round_rect(x + is * 26 / 100, y + is * 30 / 100, is * 48 / 100, is * 62 / 100, is * 8 / 100, rgb(0x6f7b8c));
        c.fill_rect(x + is * 34 / 100, y + is * 8 / 100, is * 32 / 100, is * 24 / 100, rgb(0xc9d0da));
        c.fill_rect(x + is * 40 / 100, y + is * 14 / 100, is * 6 / 100, is * 6 / 100, rgb(0x6f7b8c));
        c.fill_rect(x + is * 54 / 100, y + is * 14 / 100, is * 6 / 100, is * 6 / 100, rgb(0x6f7b8c));
        break;
    case Icon::Program:
        c.fill_round_rect(x + is * 8 / 100, y + is * 14 / 100, is * 84 / 100, is * 72 / 100, is * 8 / 100, rgb(0xdfe6ef));
        c.fill_round_rect(x + is * 8 / 100, y + is * 14 / 100, is * 84 / 100, is * 18 / 100, is * 8 / 100, rgb(0x3b8ee8));
        c.fill_rect(x + is * 8 / 100, y + is * 24 / 100, is * 84 / 100, is * 8 / 100, rgb(0x3b8ee8));
        c.fill_rect(x + is * 18 / 100, y + is * 44 / 100, is * 40 / 100, is * 6 / 100, rgb(0x9aa4b2));
        c.fill_rect(x + is * 18 / 100, y + is * 58 / 100, is * 56 / 100, is * 6 / 100, rgb(0x9aa4b2));
        break;
    case Icon::Picture:  // a framed landscape
        c.fill_round_rect(x + is * 8 / 100, y + is * 16 / 100, is * 84 / 100, is * 68 / 100, is * 8 / 100, rgb(0x5fa8e8));
        c.fill_circle(x + is * 68 / 100, y + is * 34 / 100, is * 8 / 100, rgb(0xffe08a));
        c.line(x + is * 14 / 100, y + is * 76 / 100, x + is * 40 / 100, y + is * 44 / 100, is * 10 / 100, rgb(0x3d8a4f));
        c.line(x + is * 40 / 100, y + is * 44 / 100, x + is * 58 / 100, y + is * 66 / 100, is * 10 / 100, rgb(0x3d8a4f));
        c.line(x + is * 52 / 100, y + is * 72 / 100, x + is * 70 / 100, y + is * 54 / 100, is * 10 / 100, rgb(0x2f6e3e));
        c.line(x + is * 70 / 100, y + is * 54 / 100, x + is * 86 / 100, y + is * 74 / 100, is * 10 / 100, rgb(0x2f6e3e));
        break;
    case Icon::Brush:  // a brush over a palette
        c.fill_circle(cx - s * 0.06f, cy + s * 0.04f, s * 0.36f, rgb(0xe8c48a));
        c.fill_circle(cx - s * 0.22f, cy - s * 0.06f, s * 0.07f, rgb(0xd13438));
        c.fill_circle(cx - s * 0.05f, cy - s * 0.18f, s * 0.07f, rgb(0x0063b1));
        c.fill_circle(cx + s * 0.12f, cy - s * 0.08f, s * 0.07f, rgb(0x10893e));
        c.line(cx + s * 0.02f, cy + s * 0.30f, cx + s * 0.38f, cy - s * 0.34f, s * 0.08f, rgb(0x7a5230));
        break;
    case Icon::Info:
        c.ring(cx, cy, s * 0.36f, s * 0.07f, TEXT);
        c.fill_circle(cx, cy - s * 0.16f, s * 0.05f, TEXT);
        c.line(cx, cy - s * 0.04f, cx, cy + s * 0.2f, s * 0.08f, TEXT);
        break;
    case Icon::File:
        c.fill_round_rect(x + is * 20 / 100, y + is * 8 / 100, is * 60 / 100, is * 84 / 100, is * 6 / 100, rgb(0xd5dbe4));
        c.fill_rect(x + is * 30 / 100, y + is * 36 / 100, is * 40 / 100, is * 5 / 100, rgb(0x8f99a8));
        c.fill_rect(x + is * 30 / 100, y + is * 50 / 100, is * 40 / 100, is * 5 / 100, rgb(0x8f99a8));
        c.fill_rect(x + is * 30 / 100, y + is * 64 / 100, is * 26 / 100, is * 5 / 100, rgb(0x8f99a8));
        break;
    case Icon::Package:
        c.fill_round_rect(x + is * 12 / 100, y + is * 22 / 100, is * 76 / 100, is * 64 / 100, is * 8 / 100, rgb(0xb98a52));
        c.fill_rect(x + is * 12 / 100, y + is * 40 / 100, is * 76 / 100, is * 6 / 100, rgb(0x8f6638));
        c.fill_rect(x + is * 44 / 100, y + is * 22 / 100, is * 12 / 100, is * 64 / 100, rgb(0xd9b07a));
        break;
    case Icon::Power:
        arc(c, cx, cy + s * 0.04f, s * 0.30f, -1.05f, 4.19f, s * 0.09f, TEXT);
        c.line(cx, cy - s * 0.36f, cx, cy - s * 0.02f, s * 0.09f, TEXT);
        break;
    case Icon::Pad:
        c.fill_round_rect(x + is * 8 / 100, y + is * 28 / 100, is * 84 / 100, is * 44 / 100, is * 20 / 100, TEXT2);
        c.fill_circle(cx - s * 0.2f, cy, s * 0.07f, ICON_HOLE);
        c.fill_circle(cx + s * 0.2f, cy, s * 0.07f, ICON_HOLE);
        break;
    case Icon::Mouse:
        c.fill_round_rect(x + is * 28 / 100, y + is * 10 / 100, is * 44 / 100, is * 80 / 100, is * 22 / 100, TEXT2);
        c.fill_rect(x + is * 49 / 100, y + is * 14 / 100, is * 3 / 100, is * 22 / 100, ICON_HOLE);
        break;
    case Icon::Up:
        c.line(cx, cy - s * 0.3f, cx, cy + s * 0.3f, s * 0.09f, TEXT);
        c.line(cx, cy - s * 0.3f, cx - s * 0.24f, cy - s * 0.06f, s * 0.09f, TEXT);
        c.line(cx, cy - s * 0.3f, cx + s * 0.24f, cy - s * 0.06f, s * 0.09f, TEXT);
        break;
    case Icon::User:
        c.fill_circle(cx, cy, s * 0.5f, rgb(0x5a6170));
        c.fill_circle(cx, cy - s * 0.12f, s * 0.17f, TEXT2);
        c.fill_round_rect(x + is * 24 / 100, y + is * 58 / 100, is * 52 / 100, is * 28 / 100, is * 14 / 100, TEXT2);
        break;
    case Icon::Keyboard:  // a keyboard: three rows of keys and a space bar
        c.fill_round_rect(x + is * 4 / 100, y + is * 24 / 100, is * 92 / 100, is * 54 / 100, is * 9 / 100, TEXT2);
        for (int row = 0; row < 3; row++)
            for (int k = 0; k < 6; k++)
                c.fill_rect(x + is * (12 + k * 13) / 100, y + is * (32 + row * 11) / 100, is * 8 / 100, is * 6 / 100, ICON_HOLE);
        c.fill_rect(x + is * 28 / 100, y + is * 66 / 100, is * 44 / 100, is * 5 / 100, ICON_HOLE);
        break;
    case Icon::Gauge:  // Task Manager: a graph in a frame
        c.fill_round_rect(x + is * 8 / 100, y + is * 12 / 100, is * 84 / 100, is * 76 / 100, is * 10 / 100, rgb(0x2f7fd6));
        c.fill_round_rect(x + is * 13 / 100, y + is * 17 / 100, is * 74 / 100, is * 66 / 100, is * 6 / 100, rgb(0x0f2b4a));
        c.line(cx - s * 0.30f, cy + s * 0.18f, cx - s * 0.12f, cy - s * 0.02f, s * 0.07f, rgb(0x52d1ff));
        c.line(cx - s * 0.12f, cy - s * 0.02f, cx + s * 0.04f, cy + s * 0.12f, s * 0.07f, rgb(0x52d1ff));
        c.line(cx + s * 0.04f, cy + s * 0.12f, cx + s * 0.30f, cy - s * 0.22f, s * 0.07f, rgb(0x52d1ff));
        break;
    case Icon::Apps:  // four tiles
        for (int k = 0; k < 4; k++)
            c.fill_round_rect(x + is * (12 + (k % 2) * 40) / 100, y + is * (12 + (k / 2) * 40) / 100, is * 34 / 100, is * 34 / 100,
                              is * 7 / 100, k == 3 ? ACCENT : TEXT2);
        break;
    case Icon::Download:  // an arrow down onto a tray
        c.line(cx, cy - s * 0.34f, cx, cy + s * 0.14f, s * 0.1f, ACCENT);
        c.line(cx, cy + s * 0.14f, cx - s * 0.2f, cy - s * 0.06f, s * 0.1f, ACCENT);
        c.line(cx, cy + s * 0.14f, cx + s * 0.2f, cy - s * 0.06f, s * 0.1f, ACCENT);
        c.fill_round_rect(x + is * 14 / 100, y + is * 78 / 100, is * 72 / 100, is * 9 / 100, is * 4 / 100, TEXT2);
        break;
    case Icon::Document:  // a page with lines, its corner folded
        c.fill_round_rect(x + is * 20 / 100, y + is * 8 / 100, is * 60 / 100, is * 84 / 100, is * 6 / 100, rgb(0x4f8fe8));
        for (int k = 0; k < 3; k++)
            c.fill_rect(x + is * 30 / 100, y + is * (40 + k * 14) / 100, is * 40 / 100, is * 5 / 100, rgb(0xdbe8fb));
        break;
    case Icon::Music:  // a note
        c.fill_circle(cx - s * 0.14f, cy + s * 0.22f, s * 0.14f, rgb(0xe8508a));
        c.line(cx - s * 0.01f, cy + s * 0.22f, cx - s * 0.01f, cy - s * 0.34f, s * 0.08f, rgb(0xe8508a));
        c.line(cx - s * 0.01f, cy - s * 0.34f, cx + s * 0.26f, cy - s * 0.22f, s * 0.08f, rgb(0xe8508a));
        break;
    case Icon::Video:  // a film frame with a play mark
        c.fill_round_rect(x + is * 8 / 100, y + is * 18 / 100, is * 84 / 100, is * 64 / 100, is * 10 / 100, rgb(0x8a5cd6));
        c.line(cx - s * 0.08f, cy - s * 0.16f, cx - s * 0.08f, cy + s * 0.16f, s * 0.07f, rgb(0xffffff));
        c.line(cx - s * 0.08f, cy - s * 0.16f, cx + s * 0.16f, cy, s * 0.07f, rgb(0xffffff));
        c.line(cx - s * 0.08f, cy + s * 0.16f, cx + s * 0.16f, cy, s * 0.07f, rgb(0xffffff));
        break;
    case Icon::Desktop:  // a screen with a window on it
        c.fill_round_rect(x + is * 6 / 100, y + is * 14 / 100, is * 88 / 100, is * 60 / 100, is * 7 / 100, rgb(0x2f9fd6));
        c.fill_round_rect(x + is * 18 / 100, y + is * 26 / 100, is * 40 / 100, is * 28 / 100, is * 4 / 100, rgb(0xe8f4fb));
        c.fill_round_rect(x + is * 28 / 100, y + is * 80 / 100, is * 44 / 100, is * 8 / 100, is * 4 / 100, rgb(0x9aa4b2));
        break;
    // Files' and the editors' buttons: line drawings in the text colour, as Windows 11's
    case Icon::Back:
    case Icon::Forward: {
        const float d = icon == Icon::Back ? -1.f : 1.f;
        c.line(cx - s * 0.3f * d, cy, cx + s * 0.3f * d, cy, s * 0.08f, TEXT);
        c.line(cx + s * 0.3f * d, cy, cx + s * 0.06f * d, cy - s * 0.24f, s * 0.08f, TEXT);
        c.line(cx + s * 0.3f * d, cy, cx + s * 0.06f * d, cy + s * 0.24f, s * 0.08f, TEXT);
        break;
    }
    case Icon::Refresh:
        arc(c, cx, cy, s * 0.28f, -0.4f, 4.4f, s * 0.08f, TEXT);
        c.line(cx + s * 0.26f, cy - s * 0.1f, cx + s * 0.32f, cy - s * 0.34f, s * 0.08f, TEXT);
        c.line(cx + s * 0.26f, cy - s * 0.1f, cx + s * 0.04f, cy - s * 0.16f, s * 0.08f, TEXT);
        break;
    case Icon::Cut:  // scissors
        c.ring(cx - s * 0.17f, cy + s * 0.22f, s * 0.12f, s * 0.06f, TEXT);
        c.ring(cx + s * 0.17f, cy + s * 0.22f, s * 0.12f, s * 0.06f, TEXT);
        c.line(cx - s * 0.1f, cy + s * 0.12f, cx + s * 0.16f, cy - s * 0.38f, s * 0.06f, TEXT);
        c.line(cx + s * 0.1f, cy + s * 0.12f, cx - s * 0.16f, cy - s * 0.38f, s * 0.06f, TEXT);
        break;
    case Icon::Copy:  // two pages
        c.fill_round_rect(x + is * 30 / 100, y + is * 8 / 100, is * 52 / 100, is * 62 / 100, is * 8 / 100, TEXT);
        c.fill_round_rect(x + is * 35 / 100, y + is * 13 / 100, is * 42 / 100, is * 52 / 100, is * 5 / 100, ICON_HOLE);
        c.fill_round_rect(x + is * 14 / 100, y + is * 28 / 100, is * 52 / 100, is * 62 / 100, is * 8 / 100, TEXT);
        c.fill_round_rect(x + is * 19 / 100, y + is * 33 / 100, is * 42 / 100, is * 52 / 100, is * 5 / 100, ICON_HOLE);
        break;
    case Icon::Paste:  // a clipboard
        c.fill_round_rect(x + is * 16 / 100, y + is * 14 / 100, is * 68 / 100, is * 78 / 100, is * 8 / 100, TEXT);
        c.fill_round_rect(x + is * 21 / 100, y + is * 19 / 100, is * 58 / 100, is * 68 / 100, is * 5 / 100, ICON_HOLE);
        c.fill_round_rect(x + is * 34 / 100, y + is * 6 / 100, is * 32 / 100, is * 16 / 100, is * 5 / 100, TEXT);
        break;
    case Icon::Rename:  // a text box with a cursor
        c.fill_round_rect(x + is * 6 / 100, y + is * 30 / 100, is * 88 / 100, is * 40 / 100, is * 8 / 100, TEXT);
        c.fill_round_rect(x + is * 11 / 100, y + is * 35 / 100, is * 78 / 100, is * 30 / 100, is * 5 / 100, ICON_HOLE);
        c.line(cx + s * 0.12f, cy - s * 0.34f, cx + s * 0.12f, cy + s * 0.34f, s * 0.06f, TEXT);
        c.fill_rect(x + is * 20 / 100, y + is * 47 / 100, is * 22 / 100, is * 6 / 100, TEXT);
        break;
    case Icon::Trash:
        c.fill_round_rect(x + is * 14 / 100, y + is * 18 / 100, is * 72 / 100, is * 8 / 100, is * 4 / 100, TEXT);
        c.fill_round_rect(x + is * 38 / 100, y + is * 8 / 100, is * 24 / 100, is * 12 / 100, is * 4 / 100, TEXT);
        c.fill_round_rect(x + is * 22 / 100, y + is * 30 / 100, is * 56 / 100, is * 62 / 100, is * 8 / 100, TEXT);
        c.fill_round_rect(x + is * 27 / 100, y + is * 30 / 100, is * 46 / 100, is * 56 / 100, is * 5 / 100, ICON_HOLE);
        c.fill_rect(x + is * 40 / 100, y + is * 40 / 100, is * 5 / 100, is * 38 / 100, TEXT);
        c.fill_rect(x + is * 55 / 100, y + is * 40 / 100, is * 5 / 100, is * 38 / 100, TEXT);
        break;
    case Icon::Plus:
        c.line(cx - s * 0.3f, cy, cx + s * 0.3f, cy, s * 0.09f, ACCENT);
        c.line(cx, cy - s * 0.3f, cx, cy + s * 0.3f, s * 0.09f, ACCENT);
        break;
    case Icon::Sort:  // arrows up and down
        c.line(cx - s * 0.16f, cy - s * 0.32f, cx - s * 0.16f, cy + s * 0.32f, s * 0.07f, TEXT);
        c.line(cx - s * 0.16f, cy - s * 0.32f, cx - s * 0.32f, cy - s * 0.14f, s * 0.07f, TEXT);
        c.line(cx + s * 0.16f, cy - s * 0.32f, cx + s * 0.16f, cy + s * 0.32f, s * 0.07f, TEXT);
        c.line(cx + s * 0.16f, cy + s * 0.32f, cx + s * 0.32f, cy + s * 0.14f, s * 0.07f, TEXT);
        break;
    case Icon::View:  // lines of a list
        for (int k = 0; k < 3; k++) {
            c.fill_rect(x + is * 10 / 100, y + is * (22 + k * 22) / 100, is * 12 / 100, is * 9 / 100, TEXT);
            c.fill_rect(x + is * 30 / 100, y + is * (22 + k * 22) / 100, is * 60 / 100, is * 9 / 100, TEXT);
        }
        break;
    case Icon::Notepad:  // a notepad: a page with lines and a binding along the top
        c.fill_round_rect(x + is * 18 / 100, y + is * 10 / 100, is * 64 / 100, is * 82 / 100, is * 7 / 100, rgb(0xf4f6f9));
        c.fill_round_rect(x + is * 18 / 100, y + is * 10 / 100, is * 64 / 100, is * 14 / 100, is * 7 / 100, rgb(0x3b8ee8));
        for (int k = 0; k < 4; k++)
            c.fill_rect(x + is * 28 / 100, y + is * (36 + k * 13) / 100, is * 44 / 100, is * 4 / 100, rgb(0x8f99a8));
        break;
    case Icon::Hex:  // bytes: rows of digit blocks
        c.fill_round_rect(x + is * 8 / 100, y + is * 14 / 100, is * 84 / 100, is * 72 / 100, is * 8 / 100, rgb(0x2e3440));
        for (int row = 0; row < 3; row++)
            for (int k = 0; k < 3; k++)
                c.fill_rect(x + is * (16 + k * 24) / 100, y + is * (26 + row * 18) / 100, is * 16 / 100, is * 9 / 100,
                            k == 2 ? rgb(0x88c0d0) : rgb(0xa3be8c));
        break;
    case Icon::Home:  // a house
        c.line(cx - s * 0.36f, cy - s * 0.02f, cx, cy - s * 0.34f, s * 0.08f, TEXT);
        c.line(cx, cy - s * 0.34f, cx + s * 0.36f, cy - s * 0.02f, s * 0.08f, TEXT);
        c.fill_round_rect(x + is * 24 / 100, y + is * 44 / 100, is * 52 / 100, is * 44 / 100, is * 5 / 100, TEXT);
        c.fill_rect(x + is * 44 / 100, y + is * 62 / 100, is * 12 / 100, is * 26 / 100, ICON_HOLE);
        break;
    case Icon::Chevron:  // >
        c.line(cx - s * 0.12f, cy - s * 0.28f, cx + s * 0.14f, cy, s * 0.09f, TEXT2);
        c.line(cx + s * 0.14f, cy, cx - s * 0.12f, cy + s * 0.28f, s * 0.09f, TEXT2);
        break;
    case Icon::Check:
        c.line(cx - s * 0.3f, cy, cx - s * 0.08f, cy + s * 0.24f, s * 0.11f, ACCENT_TEXT);
        c.line(cx - s * 0.08f, cy + s * 0.24f, cx + s * 0.32f, cy - s * 0.24f, s * 0.11f, ACCENT_TEXT);
        break;
    case Icon::Save:  // a disk
        c.fill_round_rect(x + is * 12 / 100, y + is * 12 / 100, is * 76 / 100, is * 76 / 100, is * 8 / 100, TEXT);
        c.fill_round_rect(x + is * 18 / 100, y + is * 18 / 100, is * 64 / 100, is * 64 / 100, is * 5 / 100, ICON_HOLE);
        c.fill_rect(x + is * 28 / 100, y + is * 18 / 100, is * 40 / 100, is * 22 / 100, TEXT);
        c.fill_round_rect(x + is * 26 / 100, y + is * 54 / 100, is * 48 / 100, is * 28 / 100, is * 4 / 100, TEXT);
        break;
    }
}

// --- files ----------------------------------------------------------------------------------

struct Entry {
    std::string name;
    std::string path;  // the location's root ("" in the list of places)
    bool dir = false;
    long long size = -1;
    std::time_t modified = 0;
    Icon icon = Icon::File;
    std::string note;  // a drive's free space; in the Recycle Bin, where it was
};

// Deleted files wait here until the Recycle Bin is emptied (as on Windows); see "the Recycle Bin"
const char *const RECYCLE_DIR = "/data/WinePS4/Recycle Bin";

// The Windows user's own folders (C:\users\ps4\...), where programs save: browsers' downloads
std::string user_folder(const char *name) {
    const char *user = std::getenv("USER");
    return std::string(C_DRIVE) + "/users/" + (user ? user : "ps4") + "/" + name;
}

bool is_text_name(const std::string &name) {
    static const char *const exts[] = {".txt", ".ini", ".cfg", ".conf", ".log", ".xml", ".json", ".md", ".bat", ".cmd", ".reg",
                                       ".csv", ".yml", ".yaml", ".lua", ".py", ".js", ".c", ".h", ".cpp", ".hpp", ".sh", ".inf",
                                       ".vdf", ".nfo", ".htm", ".html", ".css", ".toml", ".properties", ".sfo.txt", ".gitignore"};
    const std::string l = lower(name);
    return std::any_of(std::begin(exts), std::end(exts), [&](const char *e) { return ends_with(l, e); });
}

Icon icon_for(const std::string &name, bool dir) {
    const std::string l = lower(name);
    if (dir)
        return Icon::Folder;
    if (ends_with(l, ".exe") || ends_with(l, ".msi") || ends_with(l, ".bat") || ends_with(l, ".cmd") || ends_with(l, ".lnk"))
        return Icon::Program;
    if (is_picture_name(name))
        return Icon::Picture;
    if (ends_with(l, ".mp3") || ends_with(l, ".wav") || ends_with(l, ".ogg") || ends_with(l, ".flac") || ends_with(l, ".at9"))
        return Icon::Music;
    if (ends_with(l, ".mp4") || ends_with(l, ".mkv") || ends_with(l, ".avi") || ends_with(l, ".webm"))
        return Icon::Video;
    if (ends_with(l, ".pkg") || ends_with(l, ".zip") || ends_with(l, ".7z") || ends_with(l, ".rar"))
        return Icon::Package;
    if (is_text_name(name))
        return Icon::Notepad;
    return Icon::File;
}

std::string gib(double bytes) {
    char s[32];
    std::snprintf(s, sizeof s, bytes >= 1e12 ? "%.1f TB" : "%.1f GB", bytes >= 1e12 ? bytes / 1099511627776.0 : bytes / 1073741824.0);
    return s;
}

// a drive's (or a folder's file system's) room: "12.3 GB free of 100 GB"
std::string space_note(const std::string &path) {
    struct statvfs fs{};
    if (statvfs(path.c_str(), &fs) != 0 || !fs.f_blocks)
        return {};
    const double unit = fs.f_frsize ? fs.f_frsize : fs.f_bsize;
    return gib(unit * fs.f_bavail) + " free of " + gib(unit * fs.f_blocks);
}

// Quick access: the Windows user's folders, where programs save
std::vector<Entry> quick_access() {
    std::vector<Entry> list;
    const std::pair<const char *, Icon> mine[] = {{"Desktop", Icon::Desktop}, {"Downloads", Icon::Download},
                                                  {"Documents", Icon::Document}, {"Pictures", Icon::Picture},
                                                  {"Music", Icon::Music}, {"Videos", Icon::Video}};
    for (const auto &[name, icon] : mine)
        if (is_dir(user_folder(name)))
            list.push_back({name, user_folder(name), true, -1, 0, icon});
    return list;
}

// This PS4: Windows' C: drive, the console's whole file system and its own folders, USB drives
std::vector<Entry> find_drives() {
    std::vector<Entry> list;
    list.push_back({"Local Disk (C:)", C_DRIVE, true, -1, 0, Icon::Drive});
    list.push_back({"PS4 (/)", "/", true, -1, 0, Icon::Computer});
    for (int i = 0; i < 8; i++) {
        const std::string usb = "/mnt/usb" + std::to_string(i);
        if (is_dir(usb))
            list.push_back({"USB drive " + std::to_string(i + 1), usb, true, -1, 0, Icon::Usb});
    }
    const struct { const char *name, *path; Icon icon; } own[] = {
        {"Data (/data)", "/data", Icon::Drive},
        {"Games and apps (/user/app)", "/user/app", Icon::Apps},
        {"Saved data (/user/home)", "/user/home", Icon::User},
        {"Captures (/user/av_contents)", "/user/av_contents", Icon::Video},
        {"System (/system)", "/system", Icon::Settings},
        {"WinePS4 (/app0)", "/app0", Icon::Package},
    };
    for (const auto &o : own)
        if (is_dir(o.path))
            list.push_back({o.name, o.path, true, -1, 0, o.icon});
    for (Entry &e : list)
        if (e.icon == Icon::Drive || e.icon == Icon::Usb || e.icon == Icon::Computer)
            e.note = space_note(e.path);
    return list;
}

// (the USB drives looked at again every 2 s, not every frame)
const std::vector<Entry> &drives() {
    static std::vector<Entry> list;
    static double when = -10;
    const double t = now_seconds();
    if (t - when > 2) {
        list = find_drives();
        when = t;
    }
    return list;
}

const std::vector<Entry> &quick() {
    static std::vector<Entry> list;
    static double when = -10;
    const double t = now_seconds();
    if (t - when > 2) {
        list = quick_access();
        when = t;
    }
    return list;
}

// The navigation pane's places, top to bottom: Home, Quick access, the Recycle Bin, This PS4's drives
std::vector<Entry> places() {
    std::vector<Entry> list{{"This PS4", "", true, -1, 0, Icon::Computer}};
    for (const Entry &e : quick())
        list.push_back(e);
    list.push_back({"Recycle Bin", RECYCLE_DIR, true, -1, 0, Icon::Trash});
    for (const Entry &e : drives())
        list.push_back(e);
    return list;
}

// where the Recycle Bin's entries were: "name in the bin" -> "where it was"
std::map<std::string, std::string> bin_origins();

std::vector<Entry> list_dir(const std::string &path) {
    std::vector<Entry> list;
    if (path.empty()) {  // Home: Quick access, then the drives
        for (const Entry &e : quick())
            list.push_back(e);
        for (const Entry &e : drives())
            list.push_back(e);
        return list;
    }
    DIR *dir = opendir(path.c_str());
    if (!dir)
        return list;
    const bool bin = path == RECYCLE_DIR;
    const auto origins = bin ? bin_origins() : std::map<std::string, std::string>{};
    while (dirent *e = readdir(dir)) {
        const std::string name = e->d_name;
        if (name == "." || name == ".." || (bin && name == ".index"))
            continue;
        Entry entry;
        entry.name = name;
        entry.path = path == "/" ? "/" + name : path + "/" + name;
        struct stat st{};
        if (stat(entry.path.c_str(), &st) == 0) {
            entry.dir = S_ISDIR(st.st_mode);
            entry.size = entry.dir ? -1 : static_cast<long long>(st.st_size);
            entry.modified = st.st_mtime;
        }
        entry.icon = icon_for(name, entry.dir);
        if (bin) {
            auto it = origins.find(name);
            entry.note = it == origins.end() ? std::string() : it->second;
        }
        list.push_back(entry);
    }
    closedir(dir);
    return list;
}

std::string size_text(long long size) {
    if (size < 0)
        return "";
    char s[32];
    if (size < 1024)
        std::snprintf(s, sizeof s, "%lld bytes", size);
    else if (size < 1024 * 1024)
        std::snprintf(s, sizeof s, "%lld KB", (size + 1023) / 1024);
    else if (size < 1024ll * 1024 * 1024)
        std::snprintf(s, sizeof s, "%.1f MB", size / 1048576.0);
    else
        std::snprintf(s, sizeof s, "%.2f GB", size / 1073741824.0);
    return s;
}

std::string type_text(const Entry &e) {
    if (e.dir)
        return e.icon == Icon::Drive || e.icon == Icon::Usb || e.icon == Icon::Computer ? "Drive"
             : e.icon == Icon::Folder ? "File folder" : "System folder";
    const std::string l = lower(e.name);
    if (ends_with(l, ".exe"))
        return "Application";
    if (ends_with(l, ".msi"))
        return "Windows Installer";
    if (ends_with(l, ".lnk"))
        return "Shortcut";
    if (ends_with(l, ".dll"))
        return "Application extension";
    if (ends_with(l, ".txt"))
        return "Text Document";
    const size_t dot = e.name.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
        return "File";
    std::string ext = e.name.substr(dot + 1);
    for (char &ch : ext)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return ext + " File";
}

std::string date_text(std::time_t t) {
    if (!t)
        return {};
    char when[32];
    std::tm local{};
    localtime_r(&t, &local);
    std::strftime(when, sizeof when, "%d/%m/%Y %H:%M", &local);
    return when;
}

// Files' order: folders first, then by name, date modified, type or size (as the header's columns)
enum SortBy { SORT_NAME, SORT_DATE, SORT_TYPE, SORT_SIZE };
void sort_entries(std::vector<Entry> &list, int by, bool down) {
    std::stable_sort(list.begin(), list.end(), [&](const Entry &a, const Entry &b) {
        if (a.dir != b.dir)
            return a.dir;
        int order = 0;
        if (by == SORT_DATE)
            order = a.modified < b.modified ? -1 : a.modified > b.modified;
        else if (by == SORT_TYPE)
            order = lower(type_text(a)).compare(lower(type_text(b)));
        else if (by == SORT_SIZE)
            order = a.size < b.size ? -1 : a.size > b.size;
        if (order == 0)
            order = lower(a.name).compare(lower(b.name));
        return down ? order > 0 : order < 0;
    });
}

// --- env.txt (Settings) and the recent programs ------------------------------------------------

std::vector<std::string> read_lines(const char *path) {
    std::vector<std::string> lines;
    if (FILE *f = std::fopen(path, "r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
                s.pop_back();
            if (!s.empty())
                lines.push_back(s);
        }
        std::fclose(f);
    }
    return lines;
}

void write_lines(const char *path, const std::vector<std::string> &lines) {
    if (FILE *f = std::fopen(path, "w")) {
        for (const std::string &l : lines)
            std::fprintf(f, "%s\n", l.c_str());
        std::fclose(f);
    }
}

std::string env_get(const char *key) {
    const std::string prefix = std::string(key) + "=";
    for (const std::string &l : read_lines(ENV_FILE))
        if (l.rfind(prefix, 0) == 0)
            return l.substr(prefix.size());
    return {};
}

void env_set(const char *key, const std::string &value) {
    const std::string prefix = std::string(key) + "=";
    std::vector<std::string> lines = read_lines(ENV_FILE), out;
    for (const std::string &l : lines)
        if (l.rfind(prefix, 0) != 0)
            out.push_back(l);
    if (!value.empty())
        out.push_back(prefix + value);
    write_lines(ENV_FILE, out);
}

struct Toggle {
    const char *title;
    const char *detail;
    const char *key;
    const char *on_value;  // "" for "on when the key is absent"
    const char *off_value;
};

const Toggle TOGGLES[] = {
    {"Hang report", "Writes where Wine is stuck to hang.log when a program runs for 20 s and more", "WINEPS4_HANG_DUMP", "20", ""},
    {"Wine debug messages", "Exceptions and DLL loading in wine.log (WINEDEBUG=+seh,+loaddll)", "WINEDEBUG", "+seh,+loaddll", ""},
    {"Memory log", "Every mapping of WinePS4's memory layer in wine.log (slow)", "WINEPS4_VM_LOG", "1", ""},
    {"Budget log", "The PS4's budget for the app (open files, threads, memory) in wine.log, what holds files when they run out", "WINEPS4_FILES_LOG", "", "0"},
    {"Keyboard log", "How the USB keyboard's presses arrive and where they go, in wine.log (never which keys)", "WINEPS4_KEY_LOG", "", "0"},
    {"Socket option log", "The socket options the PS4's kernel refuses a program, each once, in wine.log", "WINEPS4_SOCKET_LOG", "", "0"},
    {"Vulkan log", "Programs' swapchains and frames a second (Vulkan, Direct3D through DXVK) in wine.log", "WINEPS4_VULKAN_LOG", "", "0"},
};

// Settings > Diagnostics > Keyboard log: where the USB keyboard's presses went, a line a second
// while they come (how many, never which: passwords are typed with it)
struct KeyRoutes {
    int desktop = 0, program = 0, held_back = 0, not_sent = 0;
    std::string to;  // the desktop's window that took them
    double since = 0;
} key_routes;

bool toggle_on(const Toggle &t) {
    const std::string v = env_get(t.key);
    return *t.on_value ? v == t.on_value : v != t.off_value;
}

void toggle_flip(const Toggle &t) {
    env_set(t.key, toggle_on(t) ? t.off_value : t.on_value);
}

std::vector<std::string> recent_programs() {
    std::vector<std::string> list = read_lines(RECENT_FILE);
    if (list.size() > 6)
        list.resize(6);
    return list;
}

// --- windows ---------------------------------------------------------------------------------

enum class Kind { Files, Terminal, Settings, TaskManager, Notepad, HexEditor };

// A line of text being typed on a USB keyboard (a name in Files, its address bar, its search box):
// the cursor and the selection's other end, in bytes at character starts
struct LineEdit {
    bool active = false;
    std::string text;
    size_t cursor = 0, anchor = 0;
};

// Notepad's text: its lines (UTF-8), the cursor and the selection's other end
struct TextDoc {
    std::vector<std::string> lines{std::string()};
    bool crlf = false, final_newline = false, modified = false;
    int line = 0, anchor_line = 0;
    size_t col = 0, anchor_col = 0;
    float top = 0;  // the first line shown
    int left = 0;   // pixels scrolled sideways
    struct Step {
        std::vector<std::string> lines;
        int line;
        size_t col;
    };
    std::vector<Step> undo;
    double last_typed = 0;  // (typing within a second is one step to undo)
};

// The hex editor's file: read a page at a time, the changes kept until it is saved
struct HexDoc {
    int fd = -1;
    long long size = 0, cursor = 0, top = 0;  // top: the first row shown
    bool low = false;    // the next digit typed is the byte's second
    bool ascii = false;  // typing into the text column
    std::map<long long, uint8_t> edits;
    std::map<long long, std::vector<uint8_t>> pages;
    ~HexDoc() {
        if (fd >= 0)
            close(fd);
    }
    int byte(long long at) {  // -1 past the end
        if (at < 0 || at >= size)
            return -1;
        auto e = edits.find(at);
        if (e != edits.end())
            return e->second;
        const long long page = at & ~4095ll;
        auto it = pages.find(page);
        if (it == pages.end()) {
            if (pages.size() > 256)
                pages.clear();
            std::vector<uint8_t> data(4096);
            const ssize_t n = fd >= 0 ? pread(fd, data.data(), data.size(), page) : -1;
            data.resize(n > 0 ? static_cast<size_t>(n) : 0);
            it = pages.emplace(page, std::move(data)).first;
        }
        const size_t off = static_cast<size_t>(at - page);
        return off < it->second.size() ? it->second[off] : -1;
    }
};

struct Window {
    int id = 0;
    Kind kind = Kind::Files;
    std::string title;
    int x = 0, y = 0, w = 0, h = 0;
    int nx = 0, ny = 0, nw = 0, nh = 0;  // before maximizing
    bool minimized = false;
    bool maximized = false;
    float scroll = 0;  // lines
    // Files
    std::string path;
    std::vector<Entry> entries;       // as shown: sorted, and only the search box's matches
    std::set<std::string> chosen;     // the selected entries (paths)
    int cursor = -1, anchor = -1;     // the keyboard's entry; the other end of Shift's range
    std::vector<std::string> back, forward;
    int sort_by = SORT_NAME;
    bool sort_down = false;
    LineEdit rename, address, search;
    std::string rename_path;          // the entry rename's text is for
    std::string filter;               // the search box's text, applied
    bool banding = false;             // a selection rectangle being drawn, from:
    float band_x = 0, band_y = 0;     // (band_y: in the list's rows, scroll included)
    // Notepad, Hex editor
    std::string file;
    std::shared_ptr<TextDoc> text;
    std::shared_ptr<HexDoc> hex;
    // Settings
    int page = 0;      // SETTINGS_PAGES
    int bg_tab = 0;    // pictures, colours
    // Task Manager
    int tab = 0;       // Processes, Performance
    int chosen_pid = 0;
};

std::vector<Window> windows;  // back to front
int next_id = 1;
// the desktop's own windows over Windows programs' (one clicked or opened last), or under them
bool desktop_on_top = false;
std::vector<WinTask> program_tasks;  // Windows programs' windows with a taskbar button (this frame's)

enum class Popup { None, Start, Power };
Popup popup = Popup::None;

enum class DialogKind { Message, Delete, EmptyBin, Wallpaper, Unsaved };

struct Dialog {
    bool open = false;
    DialogKind kind = DialogKind::Message;  // [OK], [Delete] [Cancel], [Close without saving] [Cancel], ...
    std::string title, text;                // text: lines split by '\n'
    std::string program;                    // the picture Wallpaper uses
    std::vector<std::string> paths;         // what Delete deletes for good
    int window = 0;                         // the editor Unsaved closes
    bool two() const { return kind != DialogKind::Message; }
    int buttons() const { return kind == DialogKind::Message ? 1 : kind == DialogKind::Unsaved ? 3 : 2; }
};
Dialog dialog;

// The right-click menu
struct MenuItem {
    std::string label;
    std::string action;  // "" for a separator
    bool enabled = true;
};
struct Menu {
    bool open = false;
    int x = 0, y = 0;
    std::vector<MenuItem> items;
    std::string target;  // what was right-clicked (a hit key)
};
Menu menu;
constexpr int MENU_W = 280, MENU_ITEM_H = 38, MENU_SEPARATOR_H = 9;

// Cut or Copy, for Paste; and text copied (Notepad, Copy as path), for Paste in Notepad and the
// address bar, and typed into a Windows program by Paste text on its taskbar button
struct Clipboard {
    std::vector<std::string> paths;
    bool cut = false;
};
Clipboard clipboard;
std::string text_clipboard;

// What the keyboard's text is for (Wine: typed into the program window with the focus; Address and
// Search: a Files window's bars; NoteText: typed into Notepad; HexGoto, HexBytes: the hex editor's)
enum class TextFor { None, Rename, Color, Wine, Address, Search, NoteText, NoteFind, HexGoto, HexBytes };
struct TextRequest {
    TextFor what = TextFor::None;
    std::string path;  // the entry to rename
    int window = 0;    // the Files window showing it
};
TextRequest text_request;
bool input_held_back = false;  // after the keyboard closes: until the buttons that closed it are let go

// The PS4's keyboard for a text field clicked in a program (Settings: auto_keyboard): a caret its display
// driver shows near the click (Windows' text boxes), or Chromium's text services focusing a field (Steam's
// sign-in), within a few seconds of the click; what's typed goes to the program (TextFor::Wine). Not while
// a USB keyboard types straight into programs.
// A field that had the focus before the click (Steam's sign-in focuses its first one) gives no news when
// clicked: one still focused 1.2 s after a click, with no news of the process meanwhile, was clicked again.
struct ProgramClick {
    uint64_t hwnd = 0;
    int pid = 0;
    float x = 0, y = 0;
    double when = -100;
    bool news = false;  // the process's text services told of a field after it
};
ProgramClick program_click;
std::map<int, uint32_t> program_field;  // what Chromium's text services focus, by process (PS4_TEXT_*)

void program_text_events(double t) {
    const auto open_for = [&](int pid, bool password) {
        if (!auto_keyboard || keyboard_active() || (direct_keyboard && hwkeyboard_connected()) || pid != program_click.pid ||
            t - program_click.when > 3)
            return;
        program_click.when = -100;  // (once a click)
        const std::string title = winwin_title(pid);
        if (keyboard_open(title.empty() ? "Type into the program" : "Type into " + title, "", password)) {
            text_request = TextRequest{};
            text_request.what = TextFor::Wine;
        }
    };
    for (const WinTextEvent &e : winwin_text_events()) {
        const uint32_t kind = e.flags & (PS4_TEXT_FIELD | PS4_TEXT_PASSWORD);
        if (!(e.flags & PS4_TEXT_CARET)) {
            uint32_t &before = program_field[e.pid];
            const bool changed = before != kind;
            before = kind;
            if (e.pid == program_click.pid)
                program_click.news = true;
            if (!kind || !changed)
                continue;
        } else if (e.hwnd != program_click.hwnd ||
                   std::fabs((e.top + e.bottom) / 2.f - program_click.y) > std::max(40, e.bottom - e.top + 24)) {
            continue;  // (a caret away from the click: not one it put there)
        }
        open_for(e.pid, kind == PS4_TEXT_PASSWORD);
    }
    if (!program_click.news && t - program_click.when > 1.2 && t - program_click.when < 3) {
        const auto it = program_field.find(program_click.pid);
        if (it != program_field.end() && it->second)
            open_for(program_click.pid, it->second == PS4_TEXT_PASSWORD);
        program_click.news = true;  // (asked once)
    }
}

// A window being resized by an edge: which ones (1 left, 2 right, 4 top, 8 bottom)
int resizing = 0, resize_edges = 0;
float resize_px = 0, resize_py = 0;
int resize_x = 0, resize_y = 0, resize_w = 0, resize_h = 0;
constexpr int MIN_W = 700, MIN_H = 380;

struct Toast {
    std::string text;
    double until = 0;
};
Toast toast;

// what the pointer is over, as a key: hover changes redraw, clicks compare it for double clicks
std::string hover;
std::string pressed_on;
double last_click_time = 0;
std::string last_click_key;
int dragging = 0;  // a window being moved by its title bar
uint32_t wine_buttons = 0;  // buttons pressed over a Windows program's window, not let go yet
float drag_dx = 0, drag_dy = 0;

Window *find_window(int id) {
    for (Window &w : windows)
        if (w.id == id)
            return &w;
    return nullptr;
}

Window *focused() {
    for (auto it = windows.rbegin(); it != windows.rend(); ++it)
        if (!it->minimized)
            return &*it;
    return nullptr;
}

void bring_to_front(int id) {
    auto it = std::find_if(windows.begin(), windows.end(), [&](const Window &w) { return w.id == id; });
    if (it == windows.end())
        return;
    Window w = *it;
    w.minimized = false;
    windows.erase(it);
    windows.push_back(w);
    desktop_on_top = true;
}

// The folder's list again: sorted as the window has it, only what matches its search box
void load_entries(Window &w) {
    std::vector<Entry> list = list_dir(w.path);
    if (!w.path.empty())
        sort_entries(list, w.sort_by, w.sort_down);
    if (!w.filter.empty()) {
        const std::string f = lower(w.filter);
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Entry &e) { return lower(e.name).find(f) == std::string::npos; }),
                   list.end());
    }
    w.entries.swap(list);
}

std::string place_title(const std::string &path) {
    if (path.empty())
        return "This PS4";
    if (path == C_DRIVE)
        return "Local Disk (C:)";
    if (path == RECYCLE_DIR)
        return "Recycle Bin";
    if (path == "/")
        return "PS4 (/)";
    return base_name(path);
}

// To another folder ("" Home); remember: Back comes back here
void navigate(Window &w, const std::string &path, bool remember = true) {
    if (remember && path != w.path) {
        w.back.push_back(w.path);
        if (w.back.size() > 64)
            w.back.erase(w.back.begin());
        w.forward.clear();
    }
    w.path = path;
    w.filter.clear();
    w.search = w.rename = w.address = LineEdit{};
    load_entries(w);
    w.chosen.clear();
    w.cursor = w.anchor = -1;
    w.scroll = 0;
    w.title = place_title(path);
}

Window *open_window(Kind kind, const std::string &path = std::string()) {
    // Terminal, Settings and Task Manager once; Files and the editors as many as wanted
    if (kind != Kind::Files && kind != Kind::Notepad && kind != Kind::HexEditor)
        for (Window &w : windows)
            if (w.kind == kind) {
                bring_to_front(w.id);
                return &windows.back();
            }
    Window w;
    w.id = next_id++;
    w.kind = kind;
    const int n = static_cast<int>(windows.size());
    switch (kind) {
    case Kind::Files:
        w.w = 1240, w.h = 760, w.x = 220 + 30 * (n % 5), w.y = 80 + 30 * (n % 5);
        navigate(w, path, false);
        break;
    case Kind::Notepad:
        w.w = 1120, w.h = 760, w.x = 300 + 30 * (n % 5), w.y = 90 + 30 * (n % 5);
        w.file = path;
        break;
    case Kind::HexEditor:
        w.w = 1260, w.h = 780, w.x = 240 + 30 * (n % 5), w.y = 80 + 30 * (n % 5);
        w.file = path;
        break;
    case Kind::Terminal:
        w.w = 1040, w.h = 600, w.x = 420 + 30 * (n % 5), w.y = 160 + 24 * (n % 5);
        w.title = "Terminal";
        break;
    case Kind::Settings:
        w.w = 1100, w.h = 760, w.x = 410, w.y = 110;
        w.bg_tab = background.kind == BgKind::Color;
        w.title = "Settings";
        break;
    case Kind::TaskManager:
        w.w = 1000, w.h = 680, w.x = 460, w.y = 140;
        w.title = "Task Manager";
        break;
    }
    windows.push_back(w);
    desktop_on_top = true;
    return &windows.back();
}

void close_window(int id) {
    windows.erase(std::remove_if(windows.begin(), windows.end(), [&](const Window &w) { return w.id == id; }), windows.end());
}

void toggle_maximize(Window &w) {
    if (w.maximized) {
        w.x = w.nx, w.y = w.ny, w.w = w.nw, w.h = w.nh;
        w.maximized = false;
    } else {
        w.nx = w.x, w.ny = w.y, w.nw = w.w, w.nh = w.h;
        w.x = 0, w.y = 0, w.w = W, w.h = H - TASKBAR_H;
        w.maximized = true;
    }
}

Icon window_icon(const Window &w) {
    switch (w.kind) {
    case Kind::Files: return Icon::Folder;
    case Kind::Terminal: return Icon::Terminal;
    case Kind::Settings: return Icon::Settings;
    case Kind::TaskManager: return Icon::Gauge;
    case Kind::Notepad: return Icon::Notepad;
    case Kind::HexEditor: return Icon::Hex;
    }
    return Icon::File;
}

// --- Windows programs: their processes, names and icons -----------------------------------------

// A Windows path (C:\..., Z:\...) as the console's (a Unix one stays as it is)
std::string console_path(const std::string &path) {
    if (path.size() < 3 || path[1] != ':' || (path[2] != '\\' && path[2] != '/'))
        return path;
    std::string rest = path.substr(2);
    std::replace(rest.begin(), rest.end(), '\\', '/');
    const char drive = static_cast<char>(std::tolower(static_cast<unsigned char>(path[0])));
    if (drive == 'c')
        return std::string(C_DRIVE) + rest;
    if (drive == 'z')
        return rest;
    return std::string(DATA_DIR) + "/prefix/dosdevices/" + drive + ":" + rest;
}

// The file at path, or the one whose names match it but for case (Windows' paths ignore case, the
// console's files don't: "Steam.exe" is steam.exe)
std::string existing_path(const std::string &path) {
    struct stat st{};
    if (path.empty() || path[0] != '/' || stat(path.c_str(), &st) == 0)
        return path;
    std::string found;
    size_t at = 1;
    while (at <= path.size()) {
        const size_t end = std::min(path.find('/', at), path.size());
        const std::string name = path.substr(at, end - at), dir = found.empty() ? "/" : found;
        std::string match;
        if (DIR *d = opendir(dir.c_str())) {
            while (dirent *e = readdir(d))
                if (lower(e->d_name) == lower(name)) {
                    match = e->d_name;
                    break;
                }
            closedir(d);
        }
        if (match.empty())
            return path;
        found += "/" + match;
        at = end + 1;
    }
    return found;
}

// An .exe's (or .dll's, .ico's) icon, size x size, premultiplied: read on a thread of its own (an exe
// on the console's disk takes a moment, a folder of them longer); nullptr until it's there, or when
// the file has none
std::mutex icons_lock;
std::condition_variable icons_wake;
std::map<std::string, Image> icons_done;       // "size:path"
std::set<std::string> icons_asked;
std::deque<std::pair<std::string, int>> icons_wanted;
std::atomic<bool> icons_changed{false};

void *icon_thread(void *) {
    set_serial(true);  // (the canvas's threads are the desktop's)
    for (;;) {
        std::pair<std::string, int> want;
        {
            std::unique_lock lock(icons_lock);
            icons_wake.wait(lock, [] { return !icons_wanted.empty(); });
            want = icons_wanted.front();
            icons_wanted.pop_front();
        }
        Image icon = file_icon(existing_path(want.first), want.second);
        if (!icon.empty())
            premultiply(icon);
        std::lock_guard lock(icons_lock);
        icons_done[std::to_string(want.second) + ":" + want.first] = std::move(icon);
        icons_changed = true;
    }
    return nullptr;
}

const Image *program_icon(const std::string &path, int size) {
    static bool started = false;
    if (!started) {
        pthread_t thread;
        if (pthread_create(&thread, nullptr, icon_thread, nullptr) == 0)
            pthread_detach(thread);
        started = true;
    }
    const std::string key = std::to_string(size) + ":" + path;
    std::lock_guard lock(icons_lock);
    auto it = icons_done.find(key);
    if (it != icons_done.end())
        return it->second.empty() ? nullptr : &it->second;
    if (icons_asked.insert(key).second) {
        icons_wanted.emplace_back(path, size);
        icons_wake.notify_one();
    }
    return nullptr;
}

bool has_program_icon(const std::string &name) {
    return ends_with(name, ".exe") || ends_with(name, ".dll") || ends_with(name, ".ico") || ends_with(name, ".scr") ||
           ends_with(name, ".cpl");
}

// The program processes, as Task Manager shows them (looked at again once a second)
struct ProcessView {
    int pid = 0;
    std::string image;     // as the process was started: a Windows path or the console's
    std::string name;      // the file's name
    double cpu = 0;        // seconds of CPU so far
    float cpu_percent = 0; // of the console's 7 cores, over the last second
    unsigned long long memory = 0;
    bool windows_own = false;  // C:\windows: Windows' own (services, explorer, ...)
    bool outside = false;      // started outside the PS4's 7 (port/ps4_spawn_desk.c)
};
std::vector<ProcessView> process_views;
std::map<int, double> last_cpu;  // pid: CPU seconds at the last look
double processes_when = -10, desktop_cpu = 0;
float desktop_cpu_percent = 0, cpu_history[60] = {}, memory_history[60] = {};
unsigned long long memory_total = 0, memory_free = 0;
constexpr int APP_CORES = 7;

double process_cpu_seconds() {
    struct rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}

void refresh_processes(bool force = false) {
    const double t = now_seconds();
    if (!force && t - processes_when < 1)
        return;
    const double elapsed = std::max(0.2, t - processes_when);
    processes_when = t;
    ps4_process_info list[48];
    const int n = ps4_process_list(list, 48);
    std::vector<ProcessView> views;
    std::map<int, double> cpu_now;
    float total = 0;
    for (int i = 0; i < n; i++) {
        ProcessView v;
        v.pid = list[i].pid;
        v.image = list[i].image;
        const size_t slash = v.image.find_last_of("\\/");
        v.name = slash == std::string::npos ? v.image : v.image.substr(slash + 1);
        v.cpu = list[i].cpu_seconds;
        v.memory = list[i].memory;
        auto it = last_cpu.find(v.pid);
        if (it != last_cpu.end())
            v.cpu_percent = std::clamp(static_cast<float>((v.cpu - it->second) / elapsed * 100.0 / APP_CORES), 0.f, 100.f);
        v.windows_own = lower(v.image).rfind("c:\\windows\\", 0) == 0;
        v.outside = list[i].outside != 0;
        cpu_now[v.pid] = v.cpu;
        total += v.cpu_percent;
        views.push_back(v);
    }
    const double desktop_now = process_cpu_seconds();
    if (desktop_cpu > 0)
        desktop_cpu_percent = std::clamp(static_cast<float>((desktop_now - desktop_cpu) / elapsed * 100.0 / APP_CORES), 0.f, 100.f);
    desktop_cpu = desktop_now;
    total += desktop_cpu_percent;
    last_cpu.swap(cpu_now);
    process_views.swap(views);
    memory_total = sceKernelGetDirectMemorySize();
    off_t start = 0;
    size_t free_size = 0;
    if (sceKernelAvailableDirectMemorySize(0, static_cast<off_t>(memory_total), 0x4000, &start, &free_size) == 0)
        memory_free = free_size;
    std::memmove(cpu_history, cpu_history + 1, sizeof(cpu_history) - sizeof(cpu_history[0]));
    std::memmove(memory_history, memory_history + 1, sizeof(memory_history) - sizeof(memory_history[0]));
    cpu_history[59] = std::min(total, 100.f);
    memory_history[59] = memory_total ? 100.f * (memory_total - std::min(memory_free, memory_total)) / memory_total : 0.f;
}

// A process's program icon (its .exe's), or nullptr
const Image *process_icon(int pid, int size) {
    for (const ProcessView &v : process_views)
        if (v.pid == pid)
            return program_icon(console_path(v.image), size);
    return nullptr;
}

// --- running a program: straight away, as on Windows ------------------------------------------

void show_message(const std::string &title, const std::string &text) {
    dialog = Dialog{};
    dialog.open = true;
    dialog.title = title;
    dialog.text = text;
}

std::string folder_of(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == 0 || slash == std::string::npos ? "/" : path.substr(0, slash);
}

bool is_program(const std::string &name) {
    const std::string l = lower(name);
    return ends_with(l, ".exe") || ends_with(l, ".msi") || ends_with(l, ".bat") || ends_with(l, ".cmd") || ends_with(l, ".lnk") ||
           ends_with(l, ".com");
}

// A Windows shortcut's (.lnk) program, as the console's path, and its arguments ("" when it names none
// on a drive): its LinkInfo's local path and its StringData, as Windows' Shell Link format has them
std::string lnk_target(const std::string &path, std::string *args = nullptr) {
    std::vector<uint8_t> d(65536);
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return {};
    d.resize(std::fread(d.data(), 1, d.size(), f));
    std::fclose(f);
    const auto u16 = [&](size_t at) { return at + 2 <= d.size() ? static_cast<uint32_t>(d[at] | d[at + 1] << 8) : 0u; };
    const auto u32 = [&](size_t at) { return at + 4 <= d.size() ? u16(at) | u16(at + 2) << 16 : 0u; };
    if (d.size() < 76 || u32(0) != 0x4c)
        return {};
    const uint32_t flags = u32(0x14);
    size_t at = 76;
    if (flags & 1)  // HasLinkTargetIDList
        at += 2 + u16(at);
    std::string target;
    if (flags & 2) {  // HasLinkInfo: its LocalBasePath (ANSI)
        const uint32_t size = u32(at), info_flags = u32(at + 8), base = u32(at + 16);
        if ((info_flags & 1) && base && at + base < d.size())
            for (size_t i = at + base; i < d.size() && d[i]; i++)
                target += static_cast<char>(d[i]);
        at += size;
    }
    // StringData: name, relative path, working folder, arguments (UTF-16 when IsUnicode), ...
    const bool unicode = flags & 0x80;
    for (const uint32_t bit : {0x4u, 0x8u, 0x10u, 0x20u}) {
        if (!(flags & bit))
            continue;
        const uint32_t count = u16(at);
        at += 2;
        std::string text;
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t ch = unicode ? u16(at + i * 2) : (at + i < d.size() ? d[at + i] : 0);
            if (ch < 0x80)
                text += static_cast<char>(ch);
            else if (ch < 0x800)
                text += static_cast<char>(0xc0 | ch >> 6), text += static_cast<char>(0x80 | (ch & 0x3f));
            else
                text += static_cast<char>(0xe0 | ch >> 12), text += static_cast<char>(0x80 | ((ch >> 6) & 0x3f)),
                    text += static_cast<char>(0x80 | (ch & 0x3f));
        }
        at += count * (unicode ? 2 : 1);
        if (bit == 0x20 && args)
            *args = text;
    }
    return target.empty() ? std::string() : existing_path(console_path(target));
}

// A Chromium browser (Chrome, Edge, Steam's web view, CEF and Electron programs) started from the
// desktop gets the switches that keep it in one process, as ntdll gives them to one a program starts
// (the PS4 runs 7 processes for the app, Windows' own 4 among them; Settings > Apps), and keep its GPU
// shader caches off the disk (their files were a quarter of the app's ~250: v00.56's web helper died of it)
void add_one_process_switches(std::vector<std::string> &args) {
    if (args.empty() || env_get("WINEPS4_CHROMIUM_ONE_PROCESS") == "0")
        return;
    const std::string exe = existing_path(console_path(args[0])), name = lower(base_name(exe)), folder = folder_of(exe);
    bool chromium = false;
    for (const char *b : {"chrome.exe", "msedge.exe", "brave.exe", "vivaldi.exe", "opera.exe", "steamwebhelper.exe", "msedgewebview2.exe"})
        chromium |= name == b;
    for (const char *m : {"libcef.dll", "v8_context_snapshot.bin", "chrome_100_percent.pak"})
        chromium |= is_file(folder + "/" + m);
    for (const std::string &a : args)
        if (a.rfind("--type=", 0) == 0 || a == "--single-process")
            chromium = false;
    if (chromium)
        args.insert(args.end(), {"--single-process", "--no-sandbox", "--in-process-gpu", "--disable-gpu", "--disable-breakpad",
                                 "--disable-gpu-shader-disk-cache"});
}

// Starts a program as a process of its own (port/ps4_main.c); "" when it started, else why not
std::string launch(std::vector<std::string> args) {
    add_one_process_switches(args);
    std::vector<char *> argv;
    for (const std::string &a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    const int pid = ps4_launch_program(argv.data());
    if (pid < 0)
        return errno == EAGAIN ? "Windows is still being set up: try again in a moment." : std::string("It could not start: ") + std::strerror(errno);
    {
        std::lock_guard lock(program_lock);
        // its name: the program's, or the installer's for msiexec /i <file>, the script's for cmd /c
        const bool helper = args.size() > 2 && (args[0].find("msiexec") != std::string::npos || args[0].find("cmd.exe") != std::string::npos ||
                                                args[0].find("start.exe") != std::string::npos);
        programs_running[pid] = base_name(helper ? args[2] : args[0]);
    }
    program_changed = true;
    return {};
}

// programs to start once Windows is ready: launch.txt, run.txt, and what's run while it's set up
std::mutex launch_lock;
std::vector<std::vector<std::string>> launch_queue;

void open_terminal();  // (below, with the windows' commands)
void use_background(const Background &b);  // (below, with Settings)

// Runs a program (.exe, .msi, .bat, a shortcut): its window comes up; a console program's output is in
// Terminal, which opens for it (as Windows opens a console window)
void run_program(const std::string &path) {
    std::vector<std::string> args;
    const std::string l = lower(path);
    bool console = false;
    if (ends_with(l, ".msi")) {
        args = {"C:\\windows\\system32\\msiexec.exe", "/i"};
    } else if (ends_with(l, ".bat") || ends_with(l, ".cmd")) {
        args = {"C:\\windows\\system32\\cmd.exe", "/c"};
        console = true;
    } else if (ends_with(l, ".lnk")) {
        // its program straight away (start.exe would take one of the 7 processes for a moment)
        std::string lnk_args;
        const std::string target = lnk_target(path, &lnk_args);
        if (!target.empty() && lnk_args.empty() && ends_with(lower(target), ".exe") && is_file(target)) {
            run_program(target);
            return;
        }
        args = {"C:\\windows\\system32\\start.exe", "/unix"};
    } else {
        console = exe_is_console(existing_path(console_path(path)));
    }
    args.push_back(path);
    std::vector<std::string> recent = read_lines(RECENT_FILE), out{path};
    for (const std::string &r : recent)
        if (r != path && out.size() < 8)
            out.push_back(r);
    write_lines(RECENT_FILE, out);
    if (!ps4_wine_ready()) {
        // (Windows starts with the app: a few seconds): it starts when that's done
        std::lock_guard lock(launch_lock);
        launch_queue.push_back(args);
        toast = Toast{base_name(path) + " starts once Windows has started", now_seconds() + 6};
        return;
    }
    const std::string error = launch(args);
    if (!error.empty()) {
        show_message(base_name(path), error);
        return;
    }
    toast = Toast{"Starting " + base_name(path), now_seconds() + 3};
    if (console)
        open_terminal();
}

// --- Files: the selection --------------------------------------------------------------------------

int files_rows(const Window &w);  // (below, with the layout)

std::vector<std::string> chosen_paths(const Window &w) {  // in the list's order
    std::vector<std::string> out;
    for (const Entry &e : w.entries)
        if (w.chosen.count(e.path))
            out.push_back(e.path);
    return out;
}

void keep_in_view(Window &w, int i) {
    const int rows = files_rows(w);
    if (i < w.scroll)
        w.scroll = static_cast<float>(i);
    else if (i >= w.scroll + rows)
        w.scroll = static_cast<float>(i - rows + 1);
}

void choose_only(Window &w, int i) {
    w.chosen.clear();
    if (i >= 0 && i < static_cast<int>(w.entries.size()))
        w.chosen.insert(w.entries[i].path);
    w.cursor = w.anchor = i;
}

// from the anchor to i (Shift)
void choose_range(Window &w, int i) {
    if (w.anchor < 0)
        w.anchor = w.cursor >= 0 ? w.cursor : i;
    w.chosen.clear();
    for (int k = std::min(w.anchor, i); k <= std::max(w.anchor, i); k++)
        if (k >= 0 && k < static_cast<int>(w.entries.size()))
            w.chosen.insert(w.entries[k].path);
    w.cursor = i;
}

void choose_toggle(Window &w, int i) {  // (Ctrl, or the row's check box)
    if (i < 0 || i >= static_cast<int>(w.entries.size()))
        return;
    const std::string &p = w.entries[i].path;
    if (!w.chosen.erase(p))
        w.chosen.insert(p);
    w.cursor = w.anchor = i;
}

void choose_all(Window &w) {
    for (const Entry &e : w.entries)
        w.chosen.insert(e.path);
}

// The entries a command is for: the chosen ones (the one the pointer is on when it isn't chosen:
// Windows' right click chooses it alone first)
std::vector<std::string> subject(Window &w, int row) {
    if (row >= 0 && row < static_cast<int>(w.entries.size()) && !w.chosen.count(w.entries[row].path))
        choose_only(w, row);
    return chosen_paths(w);
}

// --- Files: commands -----------------------------------------------------------------------------

// The list again (keeping the selection, by path): after a change in the folder; select: the entry to
// have chosen alone
void refresh(Window &w, const std::string &select = std::string()) {
    if (w.kind != Kind::Files)
        return;
    const std::string focus = !select.empty() ? select
                            : w.cursor >= 0 && w.cursor < static_cast<int>(w.entries.size()) ? w.entries[w.cursor].path : std::string();
    const float scroll = w.scroll;
    load_entries(w);
    w.scroll = scroll;
    std::set<std::string> still;
    for (const Entry &e : w.entries)
        if (select.empty() ? w.chosen.count(e.path) > 0 : e.path == select)
            still.insert(e.path);
    w.chosen.swap(still);
    w.cursor = w.anchor = -1;
    for (int i = 0; i < static_cast<int>(w.entries.size()); i++)
        if (w.entries[i].path == focus) {
            w.cursor = w.anchor = i;
            keep_in_view(w, i);
        }
}

void refresh_all(const std::vector<std::string> &folders) {
    for (Window &w : windows)
        if (w.kind == Kind::Files &&
            (folders.empty() || std::find(folders.begin(), folders.end(), w.path) != folders.end()))
            refresh(w);
}

// a path as Windows writes it when it is on C: (C:\users\ps4), else as the console's
std::string shown_path(const std::string &path) {
    if (path.empty())
        return "This PS4";
    const std::string c = C_DRIVE;
    if (path == c || path.rfind(c + "/", 0) == 0) {
        std::string rest = path.substr(c.size());
        std::replace(rest.begin(), rest.end(), '/', '\\');
        return "C:" + (rest.empty() ? std::string("\\") : rest);
    }
    return path;
}

// F2 / Rename: typed in place on a USB keyboard (the name without its extension chosen, as Windows
// does), else on the PS4's keyboard
void start_rename(Window &w, const std::string &path) {
    if (hwkeyboard_connected()) {
        const std::string name = base_name(path);
        const size_t dot = is_dir(path) ? std::string::npos : name.find_last_of('.');
        w.rename = LineEdit{true, name, dot == std::string::npos || dot == 0 ? name.size() : dot, 0};
        w.rename_path = path;
        w.address.active = w.search.active = false;
        return;
    }
    text_request = TextRequest{TextFor::Rename, path, w.id};
    if (!keyboard_open("Rename " + base_name(path), base_name(path))) {
        text_request = TextRequest{};
        show_message("Rename", "The system keyboard did not open.");
    }
}

// The name typed for path: renamed, and still chosen
void finish_rename(Window *w, const std::string &path, std::string name) {
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    while (!name.empty() && name.front() == ' ')
        name.erase(name.begin());
    if (name.empty() || name == base_name(path))
        return;
    // (Windows' rule on its C: drive; the console's file systems refuse only /)
    const bool windows_drive = path.rfind(std::string(C_DRIVE) + "/", 0) == 0;
    if (name.find('/') != std::string::npos || name == "." || name == ".." ||
        (windows_drive && name.find_first_of("\\:*?\"<>|") != std::string::npos)) {
        show_message("Rename", windows_drive ? "A file name can't contain any of these characters:  \\ / : * ? \" < > |"
                                             : "A name can't contain / or be . or ..");
        return;
    }
    const std::string to = folder_of(path) + "/" + name;
    const std::string error = rename_entry(path, to);
    if (!error.empty()) {
        show_message("Rename", error);
        return;
    }
    if (w)
        refresh(*w, to);
    refresh_all({folder_of(path)});
}

// New folder / New text document: made at once, then named (as Windows does)
void make_new(Window &w, bool folder) {
    if (w.path.empty() || w.path == RECYCLE_DIR)
        return;
    const std::string name = folder ? unique_name(w.path, "New folder", "") : unique_name(w.path, "New Text Document", ".txt");
    const std::string path = w.path + "/" + name;
    const std::string error = folder ? make_folder(path) : make_file(path);
    if (!error.empty()) {
        show_message(folder ? "New folder" : "New text document", error);
        return;
    }
    refresh(w, path);
    start_rename(w, path);
}

void cut_copy(const std::vector<std::string> &paths, bool cut) {
    if (paths.empty())
        return;
    clipboard = Clipboard{paths, cut};
    const std::string what = paths.size() == 1 ? base_name(paths[0]) : std::to_string(paths.size()) + " items";
    toast = Toast{std::string(cut ? "Cut " : "Copied ") + what + ": Paste in a folder (Ctrl+V)", now_seconds() + 3};
}

void paste_into(const std::string &folder) {
    if (clipboard.paths.empty() || folder.empty() || folder == RECYCLE_DIR)
        return;
    if (!job_paste(clipboard.paths, clipboard.cut, folder)) {
        show_message("Paste", "Wait for the copy that is running to finish.");
        return;
    }
    if (clipboard.cut)
        clipboard = Clipboard{};  // moved: nothing left to paste
}

// --- the Recycle Bin ---------------------------------------------------------------------------------
// Deleted files move into RECYCLE_DIR under a name of their own, and its .index has a line for each,
// "<its name in the bin>\t<where it was>": Restore puts it back, emptying the bin deletes them for good.
// A file on another drive than /data's (a USB drive's) is deleted for good at once, after a question,
// as Windows does for removable drives.

std::string bin_index() { return std::string(RECYCLE_DIR) + "/.index"; }

std::map<std::string, std::string> bin_origins() {
    std::map<std::string, std::string> map;
    for (const std::string &l : read_lines(bin_index().c_str())) {
        const size_t tab = l.find('\t');
        if (tab != std::string::npos)
            map[l.substr(0, tab)] = l.substr(tab + 1);
    }
    return map;
}

bool in_bin(const std::string &path) { return path.rfind(std::string(RECYCLE_DIR) + "/", 0) == 0; }

bool same_drive(const std::string &a, const std::string &b) {
    struct stat x{}, y{};
    return stat(a.c_str(), &x) == 0 && stat(b.c_str(), &y) == 0 && x.st_dev == y.st_dev;
}

// name split for unique_name: "report", ".txt" (a folder's name whole)
std::pair<std::string, std::string> stem_ext(const std::string &name, bool dir) {
    const size_t dot = dir ? std::string::npos : name.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
        return {name, std::string()};
    return {name.substr(0, dot), name.substr(dot)};
}

std::string recycle(const std::vector<std::string> &paths) {
    mkdir(RECYCLE_DIR, 0777);
    std::vector<std::string> lines = read_lines(bin_index().c_str());
    std::string error;
    for (const std::string &p : paths) {
        if (p == RECYCLE_DIR || in_bin(p))
            continue;
        const auto [stem, ext] = stem_ext(base_name(p), is_dir(p));
        const std::string name = unique_name(RECYCLE_DIR, stem, ext);
        if (rename(p.c_str(), (std::string(RECYCLE_DIR) + "/" + name).c_str()) != 0) {
            error = "Could not move " + base_name(p) + " to the Recycle Bin: " + std::strerror(errno);
            break;
        }
        lines.push_back(name + "\t" + p);
    }
    write_lines(bin_index().c_str(), lines);
    return error;
}

// The bin's entries back where they were (a folder made again if it's gone; a new name if one is there)
std::string restore(const std::vector<std::string> &paths) {
    const auto origins = bin_origins();
    std::set<std::string> done;
    std::string error;
    for (const std::string &p : paths) {
        auto it = origins.find(base_name(p));
        if (it == origins.end()) {
            error = "Where " + base_name(p) + " was is not known: drag it to a folder.";
            continue;
        }
        std::string to = it->second;
        const std::string folder = folder_of(to);
        for (size_t at = 1; at != std::string::npos && at < folder.size(); at = folder.find('/', at + 1))
            mkdir(folder.substr(0, folder.find('/', at + 1)).c_str(), 0777);
        if (is_file(to) || is_dir(to)) {
            const auto [stem, ext] = stem_ext(base_name(to), is_dir(p));
            to = folder + "/" + unique_name(folder, stem, ext);
        }
        if (rename(p.c_str(), to.c_str()) != 0) {
            error = "Could not restore " + base_name(p) + ": " + std::strerror(errno);
            continue;
        }
        done.insert(base_name(p));
    }
    std::vector<std::string> lines;
    for (const std::string &l : read_lines(bin_index().c_str()))
        if (!done.count(l.substr(0, l.find('\t'))))
            lines.push_back(l);
    write_lines(bin_index().c_str(), lines);
    return error;
}

void ask_delete(const std::vector<std::string> &paths) {
    const bool one = paths.size() == 1;
    show_message(one ? "Delete " + base_name(paths[0]) + "?" : "Delete these " + std::to_string(paths.size()) + " items?",
                 std::string(one && is_dir(paths[0]) ? "The folder and everything in it are" : one ? "It is" : "They are") +
                     " deleted for good: they do not go to the Recycle Bin.");
    dialog.kind = DialogKind::Delete;
    dialog.paths = paths;
}

// Delete: into the Recycle Bin on /data's drive, else (and forever: Shift+Delete, the bin's own) for
// good after a question
void delete_paths(const std::vector<std::string> &paths, bool forever) {
    if (paths.empty())
        return;
    bool to_bin = !forever;
    for (const std::string &p : paths)
        if (in_bin(p) || p == RECYCLE_DIR || !same_drive(p, DATA_DIR))
            to_bin = false;
    if (!to_bin) {
        ask_delete(paths);
        return;
    }
    const std::string error = recycle(paths);
    std::vector<std::string> folders{RECYCLE_DIR};
    for (const std::string &p : paths)
        folders.push_back(folder_of(p));
    refresh_all(folders);
    if (!error.empty())
        show_message("Delete", error);
    else
        toast = Toast{(paths.size() == 1 ? base_name(paths[0]) : std::to_string(paths.size()) + " items") + " moved to the Recycle Bin",
                      now_seconds() + 3};
}

void empty_bin() {
    std::vector<std::string> all;
    for (const Entry &e : list_dir(RECYCLE_DIR))
        all.push_back(e.path);
    if (all.empty())
        return;
    if (!job_delete(all)) {
        show_message("Empty Recycle Bin", "Wait for the copy that is running to finish.");
        return;
    }
    write_lines(bin_index().c_str(), {});
}

// --- Files: the address bar's parts --------------------------------------------------------------

struct Crumb {
    std::string label, path;
};

std::vector<Crumb> crumbs(const std::string &path) {
    std::vector<Crumb> list{{"This PS4", ""}};
    if (path.empty())
        return list;
    std::string root, rest;
    const std::string c = C_DRIVE, bin = RECYCLE_DIR;
    if (path == bin || path.rfind(bin + "/", 0) == 0) {
        list = {{"Recycle Bin", bin}};
        rest = path.substr(bin.size());
        root = bin;
    } else if (path == c || path.rfind(c + "/", 0) == 0) {
        list.push_back({"Local Disk (C:)", c});
        rest = path.substr(c.size());
        root = c;
    } else {
        list.push_back({"PS4 (/)", "/"});
        rest = path == "/" ? std::string() : path;
    }
    std::string at = root;
    for (size_t i = 1; i <= rest.size() && !rest.empty();) {
        const size_t end = std::min(rest.find('/', i), rest.size());
        if (end > i) {
            at += "/" + rest.substr(i, end - i);
            list.push_back({rest.substr(i, end - i), at});
        }
        i = end + 1;
    }
    return list;
}

// The address typed: a folder goes there, a file opens in its folder, chosen
void open_entry(Window &w, const Entry &e);
void go_to_address(Window &w, const std::string &typed) {
    std::string t = typed;
    while (!t.empty() && t.back() == ' ')
        t.pop_back();
    if (t.empty() || lower(t) == "this ps4") {
        navigate(w, "");
        return;
    }
    std::string path = existing_path(console_path(t));
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    if (is_dir(path)) {
        navigate(w, path);
    } else if (is_file(path)) {
        navigate(w, folder_of(path));
        refresh(w, path);
    } else {
        show_message("Files", "WinePS4 can't find '" + typed + "'. Check the spelling and try again.");
    }
}

void go_up(Window &w) {
    if (w.path.empty())
        return;
    for (const Entry &p : places())
        if (w.path == p.path) {
            navigate(w, "");
            return;
        }
    navigate(w, w.path == "/" ? std::string() : folder_of(w.path));
}

void go_back(Window &w) {
    if (w.back.empty())
        return;
    const std::string to = w.back.back();
    w.back.pop_back();
    w.forward.push_back(w.path);
    navigate(w, to, false);
}

void go_forward(Window &w) {
    if (w.forward.empty())
        return;
    const std::string to = w.forward.back();
    w.forward.pop_back();
    w.back.push_back(w.path);
    navigate(w, to, false);
}

// --- dragging files: onto a folder, another Files window, a place, the desktop or the Recycle Bin ---

struct DraggedFiles {
    bool pending = false;  // pressed on an entry: a drag once the pointer has moved far enough
    bool active = false;
    float x0 = 0, y0 = 0;  // where it was pressed
    std::vector<std::string> paths;
    std::string label;
    Icon icon = Icon::File;
    int click_row = -1;  // let go without a drag: this row chosen alone (it was one of several)
    int window = 0;      // the Files window they come from
};
DraggedFiles dragged;
bool key_ctrl = false, key_shift = false, key_alt = false;  // a USB keyboard's, held

int number_after(const std::string &key, const char *prefix);  // (below, with the menus)
std::string desktop_icon_folder(int i);                          // (below, with the desktop's icons)

// The folder a drop at key goes into ("" for none): a folder's row, a place, a part of the address,
// a Files window's own folder, the desktop (the user's Desktop folder), the Recycle Bin's icon
std::string drop_folder(const std::string &key) {
    if (key == "desktop")
        return user_folder("Desktop");
    if (key.rfind("icon:", 0) == 0)
        return desktop_icon_folder(number_after(key, "icon:"));
    if (key.rfind("win:", 0) != 0)
        return {};
    Window *w = find_window(number_after(key, "win:"));
    if (!w || w->kind != Kind::Files)
        return {};
    const std::string part = key.substr(key.find(':', 4) + 1);
    if (part.rfind("row", 0) == 0 || part.rfind("check", 0) == 0) {
        const int row = number_after(part, part[0] == 'r' ? "row" : "check");
        if (row >= 0 && row < static_cast<int>(w->entries.size()) && w->entries[row].dir)
            return w->entries[row].path;
    }
    if (part.rfind("nav", 0) == 0) {
        const auto list = places();
        const int i = number_after(part, "nav");
        return i >= 0 && i < static_cast<int>(list.size()) ? list[i].path : std::string();
    }
    if (part.rfind("crumb", 0) == 0) {
        const auto list = crumbs(w->path);
        const int i = number_after(part, "crumb");
        return i >= 0 && i < static_cast<int>(list.size()) ? list[i].path : std::string();
    }
    return w->path;  // ("" for This PS4: nothing)
}

// What a drop into folder does: 0 nothing, 1 move, 2 copy, 3 into the Recycle Bin (Windows' rule:
// a move on the same drive, a copy onto another; Ctrl copies, Shift moves)
int drop_effect(const std::string &folder) {
    if (folder.empty() || dragged.paths.empty())
        return 0;
    for (const std::string &p : dragged.paths)
        if (folder == p || folder.rfind(p + "/", 0) == 0)
            return 0;  // (onto itself, into itself)
    if (folder == RECYCLE_DIR)
        return std::all_of(dragged.paths.begin(), dragged.paths.end(), in_bin) ? 0 : 3;
    if (std::all_of(dragged.paths.begin(), dragged.paths.end(), [&](const std::string &p) { return folder_of(p) == folder; }))
        return 0;  // (where they are already)
    if (key_ctrl)
        return 2;
    if (key_shift)
        return 1;
    return same_drive(dragged.paths[0], folder) ? 1 : 2;
}

// Let go over key
void drop_dragged(const std::string &key) {
    const std::string folder = drop_folder(key);
    const int effect = drop_effect(folder);
    if (effect == 3) {
        delete_paths(dragged.paths, false);
    } else if (effect && !job_paste(dragged.paths, effect == 1, folder)) {
        show_message(effect == 1 ? "Move" : "Copy", "Wait for the copy that is running to finish.");
    }
}

void draw_dragged(Canvas &c) {
    const Input &in = last_input;
    const std::string folder = drop_folder(hover);
    const int effect = drop_effect(folder);
    const int x = static_cast<int>(in.x) + 18, y = static_cast<int>(in.y) + 14;
    const std::string where = place_title(folder);
    const std::string what = effect == 1 ? "Move to " + where : effect == 2 ? "+ Copy to " + where
                           : effect == 3 ? "Move to the Recycle Bin" : dragged.label;
    const int w = std::min(560, c.text_width(what, 14) + 64);
    panel(c, x, y, w, 40, 6, POPUP_BG, POPUP_BORDER);
    draw_icon(c, dragged.icon, x + 22, y + 20, 22);
    if (dragged.paths.size() > 1) {  // the count, as Windows' badge
        const std::string n = std::to_string(dragged.paths.size());
        const int bw = std::max(20, c.text_width(n, 12, Weight::Bold) + 10);
        c.fill_round_rect(x + 26, y + 2, bw, 18, 9, ACCENT);
        c.text_centered(x + 26 + bw / 2, y + 16, n, 12, ACCENT_TEXT, Weight::Bold);
    }
    c.text(x + 44, y + 26, what, 14, TEXT, Weight::Regular, w - 52);
}

// --- Properties ------------------------------------------------------------------------------------

std::string mode_text(mode_t m) {
    std::string s = S_ISDIR(m) ? "d" : "-";
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        s += (m & (0400 >> i)) ? rwx[i] : '-';
    return s;
}

void show_properties(const std::vector<std::string> &paths, const Entry *place) {
    if (place && place->path.empty())
        return;
    if (paths.size() > 1) {
        long long items = 0, bytes = 0;
        for (const std::string &p : paths)
            bytes += folder_size(p, items);
        show_message(std::to_string(paths.size()) + " items",
                     "Location: " + shown_path(folder_of(paths[0])) + "\nSize: " + size_text(bytes) + " (" +
                         std::to_string(bytes) + " bytes)\nContains: " + std::to_string(items) + " items in all");
        return;
    }
    const std::string path = paths.empty() ? (place ? place->path : std::string()) : paths[0];
    struct stat st{};
    if (path.empty() || stat(path.c_str(), &st) != 0) {
        show_message("Properties", "It can't be read: " + std::string(std::strerror(errno)));
        return;
    }
    Entry e;
    e.name = place ? place->name : base_name(path);
    e.dir = S_ISDIR(st.st_mode);
    e.icon = place ? place->icon : icon_for(e.name, e.dir);
    std::string text = "Type: " + type_text(e) + "\nLocation: " + shown_path(folder_of(path));
    if (e.dir) {
        long long items = 0;
        const long long bytes = folder_size(path, items);
        text += "\nSize: " + size_text(bytes) + " (" + std::to_string(bytes) + " bytes)\nContains: " +
                std::to_string(std::max(0ll, items - 1)) + " items";
        const std::string room = space_note(path);
        if (!room.empty())
            text += "\nDrive: " + room;
    } else {
        text += "\nSize: " + size_text(st.st_size) + " (" + std::to_string(static_cast<long long>(st.st_size)) + " bytes)";
    }
    text += "\nModified: " + date_text(st.st_mtime) + "\nAccessed: " + date_text(st.st_atime);
    char owner[96];
    std::snprintf(owner, sizeof owner, "\nPermissions: %s (%o), owner %u, group %u", mode_text(st.st_mode).c_str(),
                  static_cast<unsigned>(st.st_mode & 07777), static_cast<unsigned>(st.st_uid), static_cast<unsigned>(st.st_gid));
    text += owner;
    show_message(e.name + " properties", text);
}

// --- opening a file ------------------------------------------------------------------------------

void open_editor(Kind kind, const std::string &path);  // (below, with Notepad and the hex editor)

// a file with no NUL in its first 4 KiB (text, as Notepad would show it)
bool looks_like_text(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    unsigned char head[4096];
    const size_t n = std::fread(head, 1, sizeof head, f);
    std::fclose(f);
    return std::find(head, head + n, 0) == head + n;
}

void open_file(const Entry &e);

void open_entry(Window &w, const Entry &e) {
    if (e.dir) {
        navigate(w, e.path);
        return;
    }
    open_file(e);
}

// A file opened (a double click): a program runs, a picture can be the background, the others open in
// Notepad (text) or the hex editor
void open_file(const Entry &e) {
    if (in_bin(e.path)) {
        toast = Toast{"Restore it first (right-click it: Restore)", now_seconds() + 3};
        return;
    }
    if (is_program(e.name)) {
        run_program(e.path);
    } else if (is_picture_name(e.name)) {
        show_message("Use " + e.name + " as the background?", "It fills the desktop. Settings > Personalization has the others.");
        dialog.kind = DialogKind::Wallpaper;
        dialog.program = e.path;
    } else {
        struct stat st{};
        const bool small = stat(e.path.c_str(), &st) == 0 && st.st_size < (16 << 20);
        open_editor(small && (is_text_name(e.name) || looks_like_text(e.path)) ? Kind::Notepad : Kind::HexEditor, e.path);
    }
}

// --- Files' commands: the command bar, the right-click menu and the keyboard -------------------

void files_command(Window &w, const std::string &cmd, int row = -1) {
    const bool place = w.path.empty();
    const Entry *e = row >= 0 && row < static_cast<int>(w.entries.size()) ? &w.entries[row] : nullptr;
    if (cmd == "open" || cmd == "open-new" || cmd == "notepad" || cmd == "hex" || cmd == "wallpaper") {
        if (!e && w.cursor >= 0 && w.cursor < static_cast<int>(w.entries.size()))
            e = &w.entries[w.cursor];
        if (!e)
            return;
        const Entry it = *e;
        if (cmd == "open")
            open_entry(w, it);
        else if (cmd == "open-new" && it.dir)
            open_window(Kind::Files, it.path);
        else if (cmd == "notepad" && !it.dir)
            open_editor(Kind::Notepad, it.path);
        else if (cmd == "hex" && !it.dir)
            open_editor(Kind::HexEditor, it.path);
        else if (cmd == "wallpaper") {
            Background b;
            b.kind = BgKind::Photo;
            b.photo = it.path;
            use_background(b);
        }
    } else if ((cmd == "cut" || cmd == "copy") && !place) {
        cut_copy(subject(w, row), cmd == "cut");
    } else if (cmd == "copy-path") {
        std::string text;
        for (const std::string &p : subject(w, row))
            text += (text.empty() ? "" : "\n") + shown_path(p);
        if (text.empty())
            text = shown_path(w.path);
        text_clipboard = text;
        toast = Toast{"Copied its path: paste it into a program (Ctrl+V)", now_seconds() + 3};
    } else if (cmd == "paste") {
        paste_into(w.path);
    } else if (cmd == "paste-into" && e && e->dir) {
        paste_into(e->path);
    } else if (cmd == "rename" && !place) {
        const auto list = subject(w, row);
        if (!list.empty() && !in_bin(list[0]))
            start_rename(w, list[0]);
    } else if ((cmd == "delete" || cmd == "delete-now") && !place) {
        delete_paths(subject(w, row), cmd == "delete-now");
    } else if (cmd == "restore") {
        const std::string error = restore(subject(w, row));
        refresh_all({});
        if (!error.empty())
            show_message("Restore", error);
    } else if (cmd == "empty-bin") {
        if (!list_dir(RECYCLE_DIR).empty()) {
            show_message("Empty the Recycle Bin?", "Everything in it is deleted for good.");
            dialog.kind = DialogKind::EmptyBin;
        }
    } else if (cmd == "properties") {
        if (place && e)
            show_properties({}, e);
        else if (e || !w.chosen.empty())
            show_properties(subject(w, row), nullptr);
        else if (!place)
            show_properties({w.path}, nullptr);
    } else if (cmd == "new-folder") {
        make_new(w, true);
    } else if (cmd == "new-file") {
        make_new(w, false);
    } else if (cmd.rfind("sort", 0) == 0) {
        const int by = cmd[4] - '0';
        w.sort_down = w.sort_by == by ? !w.sort_down : by == SORT_DATE || by == SORT_SIZE;
        w.sort_by = by;
        refresh(w);
    } else if (cmd == "refresh") {
        refresh(w);
    } else if (cmd == "select-all") {
        choose_all(w);
    } else if (cmd == "select-none") {
        w.chosen.clear();
    } else if (cmd == "up") {
        go_up(w);
    } else if (cmd == "back") {
        go_back(w);
    } else if (cmd == "forward") {
        go_forward(w);
    }
}

// --- the right-click menu ---------------------------------------------------------------------

int number_after(const std::string &key, const char *prefix) {
    return std::atoi(key.c_str() + std::strlen(prefix));
}

std::string selected_icon;                // a desktop icon clicked once
const std::vector<Entry> &desktop_files();  // (below, with the desktop's icons)
void open_desktop_icon(int i);

void open_menu(const std::string &target, float px, float py) {
    std::vector<MenuItem> items;
    const auto sep = [&] { items.push_back({"", "", false}); };
    if (target.rfind("dfile:", 0) == 0) {
        const int i = number_after(target, "dfile:");
        if (i >= static_cast<int>(desktop_files().size()))
            return;
        const Entry &e = desktop_files()[i];
        selected_icon = target;
        items.push_back({is_program(e.name) ? "Run" : "Open", "dfile-open"});
        if (!e.dir)
            items.push_back({"Edit in Notepad", "dfile-notepad"});
        items.push_back({"Open file location", "dfile-location"});
        sep();
        items.push_back({"Cut", "dfile-cut"});
        items.push_back({"Copy", "dfile-copy"});
        items.push_back({"Rename", "dfile-rename"});
        items.push_back({"Delete", "dfile-delete"});
        sep();
        items.push_back({"Properties", "dfile-properties"});
    } else if (target == "desktop" || target.rfind("icon:", 0) == 0) {
        if (target != "desktop")
            items.push_back({"Open", "icon-open"}), sep();
        items.push_back({"Open Files", "files"});
        items.push_back({"Open Terminal", "terminal"});
        items.push_back({"Settings", "settings"});
        sep();
        items.push_back({"Paste", "paste-desktop", !clipboard.paths.empty()});
        items.push_back({"Refresh", "refresh-all"});
    } else if (target.rfind("taskbar:", 0) == 0) {
        const int i = number_after(target, "taskbar:");
        if (i <= 1) {
            items.push_back({"Files", "files"});
            items.push_back({"Terminal", "terminal"});
            items.push_back({"Settings", "settings"});
            items.push_back({"Task Manager", "taskmgr"});
            sep();
            items.push_back({"Restart WinePS4", "restart"});
            items.push_back({"Exit to the PS4 menu", "exit"});
        } else {
            const char *kinds[] = {"files", "terminal", "settings"};
            const char *names[] = {"New Files window", "Terminal", "Settings"};
            items.push_back({names[i - 2], kinds[i - 2]});
            sep();
            items.push_back({"Close window", "close-kind"});
        }
    } else if (target.rfind("edtask:", 0) == 0) {  // Notepad's or the hex editor's window
        items.push_back({"Close window", "edtask-close"});
    } else if (target.rfind("wtask:", 0) == 0) {  // a Windows program's window
        items.push_back({"Close window", "wtask-close"});
        items.push_back({"Paste text", "wtask-paste", !text_clipboard.empty()});
        sep();
        items.push_back({"End task", "wtask-end"});
        items.push_back({"Task Manager", "taskmgr"});
    } else if (target == "taskbar") {
        items.push_back({"Task Manager", "taskmgr"});
        items.push_back({"Settings", "settings"});
    } else if (target.rfind("win:", 0) == 0) {
        const int id = number_after(target, "win:");
        const std::string part = target.substr(target.find(':', 4) + 1);
        Window *w = find_window(id);
        if (!w)
            return;
        const bool row_part = part.rfind("row", 0) == 0 || part.rfind("check", 0) == 0;
        if (part == "title" || part.rfind("cap", 0) == 0 || part.rfind("edge", 0) == 0) {
            items.push_back({"Restore", "restore", w->maximized});
            items.push_back({"Minimize", "minimize"});
            items.push_back({"Maximize", "maximize", !w->maximized});
            sep();
            items.push_back({"Close", "close"});
        } else if (w->kind == Kind::Files && part == "menunew") {
            items.push_back({"Folder", "new-folder", !w->path.empty()});
            items.push_back({"Text document", "new-file", !w->path.empty()});
        } else if (w->kind == Kind::Files && part == "menusort") {
            static const char *const sorts[] = {"Name", "Date modified", "Type", "Size"};
            for (int i = 0; i < 4; i++)
                items.push_back({std::string(w->sort_by == i ? (w->sort_down ? "\xe2\x86\x93 " : "\xe2\x86\x91 ") : "    ") + sorts[i],
                                 "sort" + std::to_string(i), !w->path.empty()});
        } else if (w->kind == Kind::Files && row_part) {
            const int row = number_after(part, part[0] == 'r' ? "row" : "check");
            if (row >= static_cast<int>(w->entries.size()))
                return;
            const std::vector<std::string> list = subject(*w, row);
            const Entry &e = w->entries[row];
            const bool place = w->path.empty(), many = list.size() > 1, bin = in_bin(e.path);
            if (bin) {
                items.push_back({"Restore", "restore"});
                sep();
                items.push_back({"Cut", "cut"});
                sep();
                items.push_back({"Delete for good", "delete-now"});
            } else {
                if (!many) {
                    items.push_back({is_program(e.name) ? "Run" : "Open", "open"});
                    if (e.dir)
                        items.push_back({"Open in new window", "open-new"});
                    if (!e.dir) {
                        items.push_back({"Edit in Notepad", "notepad"});
                        items.push_back({"Open in Hex editor", "hex"});
                    }
                    if (!e.dir && is_picture_name(e.name))
                        items.push_back({"Set as desktop background", "wallpaper"});
                }
                if (!place) {
                    sep();
                    items.push_back({"Cut", "cut"});
                    items.push_back({"Copy", "copy"});
                    if (e.dir && !many)
                        items.push_back({"Paste into folder", "paste-into", !clipboard.paths.empty()});
                    items.push_back({"Copy as path", "copy-path"});
                    sep();
                    items.push_back({"Rename", "rename", !many});
                    items.push_back({"Delete", "delete"});
                    items.push_back({"Delete for good", "delete-now"});
                }
            }
            sep();
            items.push_back({"Properties", "properties"});
        } else if (w->kind == Kind::Files) {
            const bool place = w->path.empty(), bin = w->path == RECYCLE_DIR;
            static const char *const sorts[] = {"Name", "Date modified", "Type", "Size"};
            for (int i = 0; i < 4; i++)
                items.push_back({std::string(w->sort_by == i ? (w->sort_down ? "\xe2\x86\x93 " : "\xe2\x86\x91 ") : "    ") + "Sort by " + sorts[i],
                                 "sort" + std::to_string(i), !place});
            items.push_back({"Refresh", "refresh"});
            if (bin) {
                sep();
                items.push_back({"Empty Recycle Bin", "empty-bin", !w->entries.empty()});
            } else if (!place) {
                sep();
                items.push_back({"Paste", "paste", !clipboard.paths.empty()});
                sep();
                items.push_back({"New folder", "new-folder"});
                items.push_back({"New text document", "new-file"});
                sep();
                items.push_back({"Select all", "select-all"});
                items.push_back({"Copy as path", "copy-path"});
                items.push_back({"Properties", "properties"});
            }
        } else if (w->kind == Kind::Terminal) {
            items.push_back({"Scroll to the end", "terminal-end"});
            sep();
            items.push_back({"Close", "close"});
        } else if (w->kind == Kind::Notepad) {
            items.push_back({"Undo", "note-undo", w->text && !w->text->undo.empty()});
            sep();
            items.push_back({"Cut", "note-cut"});
            items.push_back({"Copy", "note-copy"});
            items.push_back({"Paste", "note-paste", !text_clipboard.empty()});
            items.push_back({"Type text here", "note-type"});
            sep();
            items.push_back({"Select all", "note-all"});
            items.push_back({"Save", "note-save"});
        } else if (w->kind == Kind::HexEditor) {
            items.push_back({"Go to offset", "hex-goto"});
            items.push_back({"Write bytes here", "hex-bytes"});
            sep();
            items.push_back({"Save", "hex-save", w->hex && !w->hex->edits.empty()});
            items.push_back({"Undo all changes", "hex-revert", w->hex && !w->hex->edits.empty()});
        } else {
            items.push_back({"Close", "close"});
        }
    } else {
        return;
    }
    int h = 8;
    for (const MenuItem &it : items)
        h += it.action.empty() ? MENU_SEPARATOR_H : MENU_ITEM_H;
    menu.open = true;
    menu.items = items;
    menu.target = target;
    // at the pointer, inside the screen (above the taskbar)
    menu.x = std::min(static_cast<int>(px), W - MENU_W - 8);
    menu.y = static_cast<int>(py) + h > H - TASKBAR_H - 4 ? std::max(8, static_cast<int>(py) - h) : static_cast<int>(py);
}

void request_close(int id);              // (below, with the editors: asks first about unsaved changes)
void editor_command(Window &w, const std::string &cmd);

void menu_action(const std::string &action) {
    const std::string target = menu.target;
    menu.open = false;
    Window *w = nullptr;
    std::string part;
    if (target.rfind("win:", 0) == 0) {
        w = find_window(number_after(target, "win:"));
        part = target.substr(target.find(':', 4) + 1);
    }
    if (action == "files") open_window(Kind::Files, "");
    else if (action == "terminal") open_window(Kind::Terminal);
    else if (action == "settings") open_window(Kind::Settings);
    else if (action == "refresh-all") refresh_all({});
    else if (action == "paste-desktop") paste_into(user_folder("Desktop"));
    else if (action.rfind("dfile-", 0) == 0) {
        const int i = number_after(target, "dfile:");
        if (i >= static_cast<int>(desktop_files().size()))
            return;
        const Entry e = desktop_files()[i];
        if (action == "dfile-open") {
            if (e.dir)
                open_window(Kind::Files, e.path);
            else
                open_file(e);
        } else if (action == "dfile-notepad") {
            open_editor(Kind::Notepad, e.path);
        } else if (action == "dfile-location") {
            Window *f = open_window(Kind::Files, folder_of(e.path));
            refresh(*f, e.path);
        } else if (action == "dfile-cut" || action == "dfile-copy") {
            cut_copy({e.path}, action == "dfile-cut");
        } else if (action == "dfile-rename") {
            text_request = TextRequest{TextFor::Rename, e.path, 0};
            if (!keyboard_open("Rename " + e.name, e.name))
                text_request = TextRequest{};
        } else if (action == "dfile-delete") {
            delete_paths({e.path}, false);
        } else if (action == "dfile-properties") {
            show_properties({e.path}, nullptr);
        }
    }
    else if (action == "restart") { ps4_end_programs(); sceSystemServiceLoadExec("/app0/eboot.bin", nullptr); }
    else if (action == "exit") { ps4_boot_trace("shell: exit to the PS4 menu"); ps4_end_programs(); exit(0); }
    else if (action == "icon-open") open_desktop_icon(number_after(target, "icon:"));
    else if (action == "taskmgr") open_window(Kind::TaskManager);
    else if (action == "close-kind") {
        const int i = number_after(target, "taskbar:");
        const Kind kind = i == 2 ? Kind::Files : i == 3 ? Kind::Terminal : Kind::Settings;
        windows.erase(std::remove_if(windows.begin(), windows.end(), [&](const Window &x) { return x.kind == kind; }), windows.end());
    } else if (action == "edtask-close") {
        request_close(number_after(target, "edtask:"));
    } else if (action == "wtask-close" || action == "wtask-end" || action == "wtask-paste") {
        const uint64_t hwnd = std::strtoull(target.c_str() + 6, nullptr, 10);
        for (const WinTask &t : program_tasks)
            if (t.hwnd == hwnd) {
                if (action == "wtask-close") {
                    winwin_command(hwnd, 0xf060);  // SC_CLOSE
                } else if (action == "wtask-paste") {  // (typed into it, as the PS4's keyboard's text is)
                    winwin_command(hwnd, PS4_SC_ACTIVATE);
                    desktop_on_top = false;
                    winwin_text(text_clipboard);
                } else {
                    ps4_local_kill(t.pid);
                }
            }
    }
    if (!w)
        return;
    if (action == "restore" && part.rfind("row", 0) != 0 && part.rfind("check", 0) != 0) {
        if (w->maximized)
            toggle_maximize(*w);
    } else if (action == "maximize" && !w->maximized) toggle_maximize(*w);
    else if (action == "minimize") w->minimized = true;
    else if (action == "close") request_close(w->id);
    else if (action == "terminal-end") w->scroll = 0;
    else if (action.rfind("note-", 0) == 0 || action.rfind("hex-", 0) == 0) editor_command(*w, action);
    else if (w->kind == Kind::Files) {
        const bool row_part = part.rfind("row", 0) == 0 || part.rfind("check", 0) == 0;
        files_command(*w, action, row_part ? number_after(part, part[0] == 'r' ? "row" : "check") : -1);
    }
}

// --- layout -----------------------------------------------------------------------------------

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

// the taskbar's buttons, centred: Start, then the apps
const Kind TASKBAR_APPS[] = {Kind::Files, Kind::Terminal, Kind::Settings};
constexpr int TB_BUTTON = 52;
constexpr int TB_FIRST_PROGRAM = 1 + 1 + 3;  // Start, Search, the apps, then Notepad's and the hex editor's windows, the programs'
// the editors' windows on the taskbar, oldest first
std::vector<int> editor_windows() {
    std::vector<int> ids;
    for (const Window &w : windows)
        if (w.kind == Kind::Notepad || w.kind == Kind::HexEditor)
            ids.push_back(w.id);
    std::sort(ids.begin(), ids.end());
    return ids;
}
int taskbar_count() { return TB_FIRST_PROGRAM + static_cast<int>(editor_windows().size() + program_tasks.size()); }
Rect taskbar_button(int i) {
    const int total = taskbar_count() * TB_BUTTON;
    return Rect{W / 2 - total / 2 + i * TB_BUTTON, H - TASKBAR_H + 4, TB_BUTTON, TASKBAR_H - 8};
}

Rect start_menu_rect() { return Rect{W / 2 - 340, H - TASKBAR_H - 12 - 700, 680, 700}; }

// the Start menu's pinned apps
struct Pin {
    const char *label;
    Icon icon;
    int action;  // see start_action
};
const Pin PINS[] = {
    {"Files", Icon::Folder, 0}, {"Terminal", Icon::Terminal, 1}, {"Settings", Icon::Settings, 2},
    {"Task Manager", Icon::Gauge, 9}, {"C: drive", Icon::Drive, 3}, {"Steam", Icon::Package, 8},
    {"Wine test", Icon::Program, 4}, {"32-bit test", Icon::Program, 6}, {"Window test", Icon::Program, 7},
};
// the pins in rows of 6 (as Windows 11's), "Recent programs" below them
constexpr int PINS_PER_ROW = 6, RECENT_TOP = 370;
Rect pin_rect(int i) {
    const Rect m = start_menu_rect();
    const int step = (m.w - 64) / PINS_PER_ROW;
    return Rect{m.x + 32 + (i % PINS_PER_ROW) * step, m.y + 130 + (i / PINS_PER_ROW) * 100, step - 3, 96};
}
Rect recent_rect(int i) {
    const Rect m = start_menu_rect();
    return Rect{m.x + 32 + (i % 2) * 310, m.y + RECENT_TOP + (i / 2) * 74, 300, 64};
}
Rect power_button() {
    const Rect m = start_menu_rect();
    return Rect{m.x + m.w - 32 - 48, m.y + m.h - 64, 48, 48};
}
Rect power_item(int i) {
    const Rect p = power_button();
    return Rect{p.x - 260 + 48, p.y - 112 + i * 52, 260, 48};
}

// desktop icons, down the left side
struct DesktopIcon {
    const char *label;
    Icon icon;
    int action;
};
const DesktopIcon DESKTOP_ICONS[] = {{"This PS4", Icon::Computer, 5}, {"Recycle Bin", Icon::Trash, 10}, {"C: drive", Icon::Drive, 3},
                                     {"Terminal", Icon::Terminal, 1}};
// the desktop's icons down columns: its own first, then the Desktop folder's files (as Windows')
Rect desktop_slot(int i) {
    const int per_column = std::max(1, (H - TASKBAR_H - 16) / 116);
    return Rect{16 + (i / per_column) * 112, 16 + (i % per_column) * 116, 104, 108};
}
Rect desktop_icon_rect(int i) { return desktop_slot(i); }
Rect desktop_file_rect(int i) { return desktop_slot(static_cast<int>(std::size(DESKTOP_ICONS)) + i); }

// The user's Desktop folder (programs' shortcuts, files dropped on the desktop), looked at again
// every 2 s
const std::vector<Entry> &desktop_files() {
    static std::vector<Entry> list;
    static double when = -10;
    const double t = now_seconds();
    if (t - when > 2) {
        list = list_dir(user_folder("Desktop"));
        sort_entries(list, SORT_NAME, false);
        when = t;
    }
    return list;
}

// where files dropped on a desktop icon go ("" for none): the Recycle Bin's, C:'s
std::string desktop_icon_folder(int i) {
    if (i < 0 || i >= static_cast<int>(std::size(DESKTOP_ICONS)))
        return {};
    switch (DESKTOP_ICONS[i].action) {
    case 10: return RECYCLE_DIR;
    case 3: return C_DRIVE;
    default: return user_folder("Desktop");
    }
}

// Steam: the client once it is installed, else its installer, put in WinePS4's folder
void start_steam() {
    const std::string folder = std::string(C_DRIVE) + "/Program Files (x86)/Steam/";
    for (const char *name : {"steam.exe", "Steam.exe"})
        if (is_file(folder + name)) {
            run_program(folder + name);
            return;
        }
    if (is_file(std::string(DATA_DIR) + "/SteamSetup.exe"))
        run_program(std::string(DATA_DIR) + "/SteamSetup.exe");
    else
        show_message("Steam", "Put SteamSetup.exe (from store.steampowered.com) in /data/WinePS4 to install Steam.");
}

void start_action(int action) {
    popup = Popup::None;
    switch (action) {
    case 0: open_window(Kind::Files); break;
    case 1: open_window(Kind::Terminal); break;
    case 2: open_window(Kind::Settings); break;
    case 3: open_window(Kind::Files, C_DRIVE); break;
    case 4: run_program("/app0/wine/hello.exe"); break;
    case 5: open_window(Kind::Files, ""); break;
    case 6: run_program("/app0/wine/hello32.exe"); break;
    case 7: run_program("/app0/wine/window.exe"); break;
    case 8: start_steam(); break;
    case 9: open_window(Kind::TaskManager); break;
    case 10: open_window(Kind::Files, RECYCLE_DIR); break;
    }
}

void open_desktop_icon(int i) {
    if (i >= 0 && i < static_cast<int>(std::size(DESKTOP_ICONS)))
        start_action(DESKTOP_ICONS[i].action);
}

Rect caption_button(const Window &w, int i) {  // 0 close, 1 maximize, 2 minimize
    return Rect{w.x + w.w - 46 * (i + 1), w.y, 46, TITLE_H};
}

// Files' parts, as Windows 11's File Explorer: the command bar, the address row (Back, Forward, Up,
// Refresh, the address, the search box), the navigation pane, the list with its header, the status bar
constexpr int FILES_NAV_W = 240, FILES_CMD_H = 50, FILES_ADDR_H = 48, FILES_HEADER_H = 32, ROW_H = 34, FILES_STATUS_H = 28;
constexpr int NAV_ROW_H = 32, NAV_GAP = 10, SEARCH_W = 250, CHECK_X = 12;

// the command bar's buttons: key (files_command's, or a drop-down menu's), label (none: an icon only)
struct CmdButton {
    const char *key;
    const char *label;
    Icon icon;
    int w;  // a separator when key is ""
};
const CmdButton FILES_CMDS[] = {{"new", "New", Icon::Plus, 96}, {"", nullptr, Icon::File, 14}, {"cut", nullptr, Icon::Cut, 46},
                                {"copy", nullptr, Icon::Copy, 46}, {"paste", nullptr, Icon::Paste, 46}, {"rename", nullptr, Icon::Rename, 46},
                                {"delete", nullptr, Icon::Trash, 46}, {"", nullptr, Icon::File, 14}, {"sort", "Sort", Icon::Sort, 96},
                                {"", nullptr, Icon::File, 14}, {"properties", "Properties", Icon::Info, 132}};
const CmdButton BIN_CMDS[] = {{"empty-bin", "Empty Recycle Bin", Icon::Trash, 196}, {"", nullptr, Icon::File, 14},
                              {"restore", "Restore", Icon::Back, 120}, {"delete-now", "Delete", Icon::Trash, 112},
                              {"", nullptr, Icon::File, 14}, {"properties", "Properties", Icon::Info, 132}};

const CmdButton *files_cmds(const Window &w, int &count) {
    const bool bin = w.path == RECYCLE_DIR;
    count = bin ? static_cast<int>(std::size(BIN_CMDS)) : static_cast<int>(std::size(FILES_CMDS));
    return bin ? BIN_CMDS : FILES_CMDS;
}
Rect files_cmd_rect(const Window &w, int i) {
    int count = 0, x = w.x + 12;
    const CmdButton *cmds = files_cmds(w, count);
    for (int k = 0; k < i && k < count; k++)
        x += cmds[k].w;
    return Rect{x, w.y + TITLE_H + 7, i < count ? cmds[i].w - 4 : 0, FILES_CMD_H - 14};
}
// Back, Forward, Up, Refresh
Rect files_nav_button(const Window &w, int i) { return Rect{w.x + 10 + i * 42, w.y + TITLE_H + FILES_CMD_H + 6, 38, 36}; }
Rect files_address(const Window &w) {
    const int x = w.x + 10 + 4 * 42 + 6;
    return Rect{x, w.y + TITLE_H + FILES_CMD_H + 6, w.x + w.w - 16 - SEARCH_W - 10 - x, 36};
}
Rect files_search(const Window &w) { return Rect{w.x + w.w - 16 - SEARCH_W, w.y + TITLE_H + FILES_CMD_H + 6, SEARCH_W, 36}; }
int files_top(const Window &w) { return w.y + TITLE_H + FILES_CMD_H + FILES_ADDR_H; }
Rect files_nav_item(const Window &w, int i) {
    const int quick_n = static_cast<int>(quick().size());
    const int y = files_top(w) + 8 + i * NAV_ROW_H + (i > quick_n ? NAV_GAP : 0) + (i > quick_n + 1 ? NAV_GAP : 0);
    return Rect{w.x + 8, y, FILES_NAV_W - 16, NAV_ROW_H - 2};
}
Rect files_header(const Window &w) { return Rect{w.x + FILES_NAV_W, files_top(w), w.w - FILES_NAV_W - 1, FILES_HEADER_H}; }
Rect files_list(const Window &w) {
    const int top = files_top(w) + FILES_HEADER_H;
    return Rect{w.x + FILES_NAV_W, top, w.w - FILES_NAV_W - 1, w.y + w.h - FILES_STATUS_H - top};
}
int files_rows(const Window &w) { return std::max(1, files_list(w).h / ROW_H); }
// the columns: Name, Date modified, Type, Size (the Recycle Bin: where it was instead of Type;
// This PS4: Type, then the free space)
int files_column_x(const Window &w, int i) {
    const Rect l = files_list(w);
    static const int at[] = {0, 47, 67, 85};
    return l.x + (i == 0 ? 44 : l.w * at[i] / 100);
}
void clamp_scroll(Window &w, int count, int visible) {
    w.scroll = std::clamp(w.scroll, 0.f, static_cast<float>(std::max(0, count - visible)));
}

// Settings: a page at a time, chosen on the left (as Windows 11's)
const char *const SETTINGS_PAGES[] = {"System", "Bluetooth & devices", "Personalization", "Apps", "Diagnostics"};
enum SettingsPage { PAGE_SYSTEM, PAGE_DEVICES, PAGE_PERSONALIZE, PAGE_APPS, PAGE_DIAGNOSTICS };
constexpr int SET_NAV_W = 260, TILE_W = 176, TILE_H = 99, TILE_GAP = 14, TILE_LABEL = 26, SWATCH = 58;

Rect settings_area(const Window &w) { return Rect{w.x + SET_NAV_W + 16, w.y + TITLE_H + 20, w.w - SET_NAV_W - 44, w.h - TITLE_H - 40}; }
Rect settings_page_rect(const Window &w, int i) { return Rect{w.x + 12, w.y + TITLE_H + 76 + i * 46, SET_NAV_W - 24, 42}; }
Rect theme_card(const Window &w) { const Rect a = settings_area(w); return Rect{a.x, a.y + 56, a.w, 76}; }
Rect bg_card(const Window &w) { const Rect a = settings_area(w); return Rect{a.x, a.y + 144, a.w, 76}; }
Rect segment(const Rect &card, int i, int n, int width) {
    return Rect{card.x + card.w - 20 - n * width + i * width, card.y + 20, width - 4, 36};
}
Rect tile_rect(const Window &w, int i) {
    const Rect a = settings_area(w);
    const int per_row = std::max(1, (a.w + TILE_GAP) / (TILE_W + TILE_GAP));
    return Rect{a.x + (i % per_row) * (TILE_W + TILE_GAP), a.y + 244 + (i / per_row) * (TILE_H + TILE_GAP + TILE_LABEL), TILE_W, TILE_H};
}
Rect swatch_rect(const Window &w, int i) {
    const Rect a = settings_area(w);
    return Rect{a.x + (i % 8) * (SWATCH + 16), a.y + 244 + (i / 8) * (SWATCH + 16), SWATCH, SWATCH};
}
Rect custom_color_rect(const Window &w) { const Rect a = settings_area(w); return Rect{a.x, a.y + 244 + 2 * (SWATCH + 16) + 10, 240, 40}; }
Rect toggle_rect(const Window &w, int i) { const Rect a = settings_area(w); return Rect{a.x, a.y + 60 + i * 80, a.w, 72}; }
// a page's cards, one under the other (System, Bluetooth & devices)
Rect card_at(const Window &w, int top, int h) { const Rect a = settings_area(w); return Rect{a.x, a.y + top, a.w, h}; }
Rect mouse_card(const Window &w) { return card_at(w, 56, 92); }
Rect keyboard_card(const Window &w) { return card_at(w, 160, 120); }
Rect autokb_card(const Window &w) { return card_at(w, 292, 80); }
Rect controller_card(const Window &w) { return card_at(w, 384, 120); }
Rect speed_button(const Window &w, int i) {  // 0 slower, 1 faster: the bar between them
    const Rect c = mouse_card(w);
    return Rect{i ? c.x + c.w - 60 : c.x + c.w - 380, c.y + 28, 40, 36};
}
// Apps: a row for each program installed, with its Uninstall button
// (under the cards for Chromium programs in one process, for helpers left out and for processes
// outside the PS4's 7: the PS4 runs 7 processes for the app)
Rect chromium_card(const Window &w) { return card_at(w, 56, 84); }
Rect rules_card(const Window &w) { return card_at(w, 150, 84); }
Rect outside_card(const Window &w) { return card_at(w, 244, 84); }
Rect dxvk_card(const Window &w) { return card_at(w, 338, 84); }
Rect app_row(const Window &w, int i) { return card_at(w, 478 + i * 70, 62); }
Rect app_uninstall(const Window &w, int i) { const Rect r = app_row(w, i); return Rect{r.x + r.w - 140, r.y + 13, 120, 36}; }

// --- typing on a USB keyboard: characters, edit boxes ---------------------------------------------

// Virtual keys (Windows' VK_*) the desktop's own windows use
enum : uint16_t {
    VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0d, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12, VK_CAPITAL = 0x14,
    VK_ESCAPE = 0x1b, VK_SPACE = 0x20, VK_PRIOR = 0x21, VK_NEXT = 0x22, VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25,
    VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_INSERT = 0x2d, VK_DELETE = 0x2e, VK_F2 = 0x71, VK_F3 = 0x72,
    VK_F5 = 0x74, VK_LSHIFT = 0xa0, VK_RSHIFT = 0xa1, VK_LCONTROL = 0xa2, VK_RCONTROL = 0xa3, VK_LMENU = 0xa4, VK_RMENU = 0xa5,
    VK_APPS = 0x5d,
};
bool caps_lock = false;

// The character a key types on a US keyboard layout (0: none)
char key_char(uint16_t vk, bool shift) {
    const bool upper = shift != caps_lock;
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<char>(upper ? vk : vk + 32);
    if (vk >= '0' && vk <= '9')
        return shift ? ")!@#$%^&*("[vk - '0'] : static_cast<char>(vk);
    if (vk >= 0x60 && vk <= 0x69)  // the number pad's
        return static_cast<char>('0' + vk - 0x60);
    switch (vk) {
    case VK_SPACE: return ' ';
    case 0x6a: return '*';
    case 0x6b: return '+';
    case 0x6d: return '-';
    case 0x6e: return '.';
    case 0x6f: return '/';
    case 0xba: return shift ? ':' : ';';
    case 0xbb: return shift ? '+' : '=';
    case 0xbc: return shift ? '<' : ',';
    case 0xbd: return shift ? '_' : '-';
    case 0xbe: return shift ? '>' : '.';
    case 0xbf: return shift ? '?' : '/';
    case 0xc0: return shift ? '~' : '`';
    case 0xdb: return shift ? '{' : '[';
    case 0xdc: return shift ? '|' : '\\';
    case 0xdd: return shift ? '}' : ']';
    case 0xde: return shift ? '"' : '\'';
    }
    return 0;
}

// UTF-8: the start of the character after / before byte at
size_t next_char(const std::string &s, size_t at) {
    if (at >= s.size())
        return s.size();
    at++;
    while (at < s.size() && (static_cast<unsigned char>(s[at]) & 0xc0) == 0x80)
        at++;
    return at;
}
size_t prev_char(const std::string &s, size_t at) {
    if (at == 0)
        return 0;
    at--;
    while (at > 0 && (static_cast<unsigned char>(s[at]) & 0xc0) == 0x80)
        at--;
    return at;
}

// A key for an edit box; false when it isn't one (Enter, Escape, Tab: the caller's)
bool edit_key(LineEdit &e, const KeyEvent &k) {
    const size_t from = std::min(e.cursor, e.anchor), to = std::max(e.cursor, e.anchor);
    const auto erase_selection = [&] {
        e.text.erase(from, to - from);
        e.cursor = e.anchor = from;
    };
    const auto move = [&](size_t at) {
        e.cursor = at;
        if (!key_shift)
            e.anchor = at;
    };
    if (key_ctrl) {
        if (k.vkey == 'A') {
            e.anchor = 0, e.cursor = e.text.size();
        } else if (k.vkey == 'C' || k.vkey == 'X') {
            if (to > from)
                text_clipboard = e.text.substr(from, to - from);
            if (k.vkey == 'X')
                erase_selection();
        } else if (k.vkey == 'V') {
            std::string paste = text_clipboard.substr(0, text_clipboard.find('\n'));
            erase_selection();
            e.text.insert(e.cursor, paste);
            e.cursor = e.anchor = e.cursor + paste.size();
        } else {
            return false;
        }
        return true;
    }
    switch (k.vkey) {
    case VK_LEFT: move(from != to && !key_shift ? from : prev_char(e.text, e.cursor)); return true;
    case VK_RIGHT: move(from != to && !key_shift ? to : next_char(e.text, e.cursor)); return true;
    case VK_HOME: move(0); return true;
    case VK_END: move(e.text.size()); return true;
    case VK_BACK:
        if (to > from)
            erase_selection();
        else if (e.cursor > 0) {
            const size_t p = prev_char(e.text, e.cursor);
            e.text.erase(p, e.cursor - p);
            e.cursor = e.anchor = p;
        }
        return true;
    case VK_DELETE:
        if (to > from)
            erase_selection();
        else if (e.cursor < e.text.size())
            e.text.erase(e.cursor, next_char(e.text, e.cursor) - e.cursor);
        return true;
    }
    if (const char ch = key_char(k.vkey, key_shift)) {
        erase_selection();
        e.text.insert(e.cursor, 1, ch);
        e.cursor = e.anchor = e.cursor + 1;
        return true;
    }
    return false;
}

// An edit box drawn in r: its text, the selection, the cursor
void draw_edit(Canvas &c, const Rect &r, const LineEdit &e, int px) {
    const int tx = r.x + 10, base = r.y + r.h / 2 + px / 2 - 1;
    const size_t from = std::min(e.cursor, e.anchor), to = std::max(e.cursor, e.anchor);
    // (a long text: its end, where the cursor is, stays in the box)
    int shift = 0;
    const int cursor_x = c.text_width(e.text.substr(0, e.cursor), px);
    if (cursor_x > r.w - 24)
        shift = cursor_x - (r.w - 24);
    size_t first = 0;
    while (first < e.text.size() && c.text_width(e.text.substr(0, first), px) < shift)
        first = next_char(e.text, first);
    const int skipped = c.text_width(e.text.substr(0, first), px);
    if (to > from) {
        const int sx = std::max(0, c.text_width(e.text.substr(0, from), px) - skipped);
        const int ex = std::min(r.w - 16, c.text_width(e.text.substr(0, to), px) - skipped);
        if (ex > sx)
            c.fill_rect(tx + sx, r.y + 6, ex - sx, r.h - 12, rgb(0x0078d4, 0xa0));
    }
    c.text(tx, base, e.text.substr(first), px, TEXT, Weight::Regular, r.w - 20);
    c.fill_rect(tx + cursor_x - skipped, r.y + 7, 2, r.h - 14, TEXT);
}

// --- Notepad: text files ---------------------------------------------------------------------------

constexpr int NOTE_BAR_H = 44, NOTE_STATUS_H = 28, NOTE_LINE_H = 24, NOTE_PX = 16, NOTE_LEFT = 16;
Canvas *metrics = nullptr;  // the scene: text widths for the pointer's clicks

std::string expand_tabs(const std::string &s) {
    std::string out;
    for (char ch : s)
        out += ch == '\t' ? std::string("    ") : std::string(1, ch);
    return out;
}
// pixels from a line's start to col (past 20000 bytes, guessed: a minified file's one long line)
int text_x(const std::string &line, size_t col) {
    if (!metrics)
        return 0;
    col = std::min(col, line.size());
    const size_t measured = std::min<size_t>(col, 20000);
    return metrics->text_width(expand_tabs(line.substr(0, measured)), NOTE_PX) + static_cast<int>(col - measured) * 8;
}

// the column nearest to x (pixels from the text's left): the last character starting left of it,
// found by halves, or the one after it when x is past its middle
size_t col_at(const std::string &line, int x) {
    if (x <= 0 || line.empty())
        return 0;
    size_t lo = 0, hi = line.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        while (mid > lo && mid < line.size() && (static_cast<unsigned char>(line[mid]) & 0xc0) == 0x80)
            mid--;
        if (mid == lo)
            break;
        if (text_x(line, mid) <= x)
            lo = mid;
        else
            hi = mid - 1;
    }
    const size_t next = next_char(line, lo);
    return next > lo && x > (text_x(line, lo) + text_x(line, next)) / 2 ? next : lo;
}

bool load_text(const std::string &path, TextDoc &doc, std::string &error) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = std::strerror(errno);
        return false;
    }
    std::string data;
    char buffer[65536];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0 && data.size() < (32u << 20))
        data.append(buffer, n);
    std::fclose(f);
    doc = TextDoc{};
    doc.lines.clear();
    if (data.size() >= 3 && !data.compare(0, 3, "\xef\xbb\xbf"))
        data.erase(0, 3);  // (a byte order mark: not shown)
    doc.crlf = data.find("\r\n") != std::string::npos;
    doc.final_newline = !data.empty() && data.back() == '\n';
    size_t start = 0;
    for (;;) {
        const size_t end = data.find('\n', start);
        std::string line = data.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (end == std::string::npos) {
            if (!line.empty() || doc.lines.empty())
                doc.lines.push_back(line);
            break;
        }
        doc.lines.push_back(line);
        start = end + 1;
    }
    if (doc.lines.empty())
        doc.lines.emplace_back();
    return true;
}

bool save_text(const std::string &path, TextDoc &doc, std::string &error) {
    const std::string tmp = path + ".wineps4-save";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        error = std::strerror(errno);
        return false;
    }
    const char *eol = doc.crlf ? "\r\n" : "\n";
    bool ok = true;
    for (size_t i = 0; i < doc.lines.size() && ok; i++) {
        ok = std::fwrite(doc.lines[i].data(), 1, doc.lines[i].size(), f) == doc.lines[i].size();
        if (ok && (i + 1 < doc.lines.size() || doc.final_newline))
            ok = std::fputs(eol, f) >= 0;
    }
    ok = std::fclose(f) == 0 && ok;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        error = std::strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    doc.modified = false;
    return true;
}

void note_title(Window &w) { w.title = (w.text && w.text->modified ? "*" : "") + base_name(w.file) + " - Notepad"; }

bool note_has_selection(const TextDoc &d) { return d.line != d.anchor_line || d.col != d.anchor_col; }

// the selection's ends, in order
void note_range(const TextDoc &d, int &l0, size_t &c0, int &l1, size_t &c1) {
    const bool cursor_first = d.line < d.anchor_line || (d.line == d.anchor_line && d.col < d.anchor_col);
    l0 = cursor_first ? d.line : d.anchor_line, c0 = cursor_first ? d.col : d.anchor_col;
    l1 = cursor_first ? d.anchor_line : d.line, c1 = cursor_first ? d.anchor_col : d.col;
}

std::string note_selected(const TextDoc &d) {
    int l0, l1;
    size_t c0, c1;
    note_range(d, l0, c0, l1, c1);
    if (l0 == l1)
        return d.lines[l0].substr(c0, c1 - c0);
    std::string out = d.lines[l0].substr(c0);
    for (int l = l0 + 1; l < l1; l++)
        out += "\n" + d.lines[l];
    return out + "\n" + d.lines[l1].substr(0, c1);
}

// before a change: a step to undo (typing in a burst is one), for files that aren't huge
void note_remember(TextDoc &d, bool typing) {
    const double t = now_seconds();
    // (each step is a copy of the whole text: 50 for a small file, fewer for bigger ones, none past 4 MB)
    size_t bytes = 0;
    for (const std::string &l : d.lines)
        bytes += l.size() + 1;
    const size_t keep = bytes < (256u << 10) ? 50 : bytes < (1u << 20) ? 10 : bytes < (4u << 20) ? 3 : 0;
    if (!(typing && t - d.last_typed < 1.0) && keep) {
        d.undo.push_back({d.lines, d.line, d.col});
        while (d.undo.size() > keep)
            d.undo.erase(d.undo.begin());
    }
    d.last_typed = typing ? t : 0;
    d.modified = true;
}

void note_erase_selection(TextDoc &d) {
    if (!note_has_selection(d))
        return;
    int l0, l1;
    size_t c0, c1;
    note_range(d, l0, c0, l1, c1);
    d.lines[l0] = d.lines[l0].substr(0, c0) + d.lines[l1].substr(c1);
    d.lines.erase(d.lines.begin() + l0 + 1, d.lines.begin() + l1 + 1);
    d.line = d.anchor_line = l0;
    d.col = d.anchor_col = c0;
}

void note_insert(TextDoc &d, const std::string &text) {
    note_erase_selection(d);
    std::string &line = d.lines[d.line];
    const std::string tail = line.substr(d.col);
    line.erase(d.col);
    size_t start = 0;
    for (;;) {
        const size_t end = text.find('\n', start);
        std::string piece = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!piece.empty() && piece.back() == '\r')
            piece.pop_back();
        d.lines[d.line] += piece;
        if (end == std::string::npos)
            break;
        d.lines.insert(d.lines.begin() + d.line + 1, std::string());
        d.line++;
        start = end + 1;
    }
    d.col = d.lines[d.line].size();
    d.lines[d.line] += tail;
    d.anchor_line = d.line, d.anchor_col = d.col;
}

int note_rows(const Window &w) { return std::max(1, (w.h - TITLE_H - NOTE_BAR_H - NOTE_STATUS_H - 8) / NOTE_LINE_H); }
Rect note_area(const Window &w) {
    return Rect{w.x + 1, w.y + TITLE_H + NOTE_BAR_H, w.w - 2, w.h - TITLE_H - NOTE_BAR_H - NOTE_STATUS_H};
}

// the cursor kept on the screen
void note_follow(Window &w) {
    TextDoc &d = *w.text;
    const int rows = note_rows(w);
    if (d.line < d.top)
        d.top = static_cast<float>(d.line);
    else if (d.line >= d.top + rows)
        d.top = static_cast<float>(d.line - rows + 1);
    const int x = text_x(d.lines[d.line], d.col), visible = w.w - 2 * NOTE_LEFT - 20;
    if (x < d.left)
        d.left = std::max(0, x - visible / 3);
    else if (x > d.left + visible)
        d.left = x - visible + visible / 3;
}

void note_save(Window &w) {
    std::string error;
    if (!w.text || !save_text(w.file, *w.text, error))
        show_message("Notepad", "Could not save " + base_name(w.file) + ": " + error);
    else
        toast = Toast{"Saved " + base_name(w.file), now_seconds() + 2};
    note_title(w);
    refresh_all({folder_of(w.file)});
}

void notepad_key(Window &w, const KeyEvent &k) {
    TextDoc &d = *w.text;
    const auto move = [&](int line, size_t col) {
        d.line = std::clamp(line, 0, static_cast<int>(d.lines.size()) - 1);
        d.col = std::min(col, d.lines[d.line].size());
        if (!key_shift)
            d.anchor_line = d.line, d.anchor_col = d.col;
    };
    const int rows = note_rows(w);
    if (key_ctrl) {
        switch (k.vkey) {
        case 'S': note_save(w); return;
        case 'A': d.anchor_line = 0, d.anchor_col = 0, d.line = static_cast<int>(d.lines.size()) - 1, d.col = d.lines.back().size(); return;
        case 'C':
        case 'X':
            if (note_has_selection(d)) {
                text_clipboard = note_selected(d);
                if (k.vkey == 'X')
                    note_remember(d, false), note_erase_selection(d);
            }
            break;
        case 'V':
            if (!text_clipboard.empty())
                note_remember(d, false), note_insert(d, text_clipboard);
            break;
        case 'Z':
            if (!d.undo.empty()) {
                d.lines = d.undo.back().lines;
                d.line = d.anchor_line = std::min(d.undo.back().line, static_cast<int>(d.lines.size()) - 1);
                d.col = d.anchor_col = std::min(d.undo.back().col, d.lines[d.line].size());
                d.undo.pop_back();
                d.modified = true;
            }
            break;
        case VK_HOME: move(0, 0); break;
        case VK_END: move(static_cast<int>(d.lines.size()) - 1, std::string::npos); break;
        default: return;
        }
        note_follow(w);
        note_title(w);
        return;
    }
    switch (k.vkey) {
    case VK_LEFT:
        if (note_has_selection(d) && !key_shift) {
            int l0, l1;
            size_t c0, c1;
            note_range(d, l0, c0, l1, c1);
            move(l0, c0);
        } else if (d.col > 0) move(d.line, prev_char(d.lines[d.line], d.col));
        else if (d.line > 0) move(d.line - 1, std::string::npos);
        break;
    case VK_RIGHT:
        if (note_has_selection(d) && !key_shift) {
            int l0, l1;
            size_t c0, c1;
            note_range(d, l0, c0, l1, c1);
            move(l1, c1);
        } else if (d.col < d.lines[d.line].size()) move(d.line, next_char(d.lines[d.line], d.col));
        else if (d.line + 1 < static_cast<int>(d.lines.size())) move(d.line + 1, 0);
        break;
    case VK_UP:
    case VK_DOWN:
    case VK_PRIOR:
    case VK_NEXT: {  // (to the same place across, as near as the line has one)
        const int step = k.vkey == VK_UP ? -1 : k.vkey == VK_DOWN ? 1 : k.vkey == VK_PRIOR ? -rows : rows;
        const int x = text_x(d.lines[d.line], d.col);
        const int to = std::clamp(d.line + step, 0, static_cast<int>(d.lines.size()) - 1);
        move(to, col_at(d.lines[to], x));
        break;
    }
    case VK_HOME: {  // the first letter of the line, then its start (as Notepad does)
        size_t first = 0;
        while (first < d.lines[d.line].size() && (d.lines[d.line][first] == ' ' || d.lines[d.line][first] == '\t'))
            first++;
        move(d.line, d.col == first ? 0 : first);
        break;
    }
    case VK_END: move(d.line, std::string::npos); break;
    case VK_RETURN: note_remember(d, false), note_insert(d, "\n"); break;
    case VK_TAB: note_remember(d, true), note_insert(d, "\t"); break;
    case VK_BACK:
        note_remember(d, true);
        if (note_has_selection(d)) note_erase_selection(d);
        else if (d.col > 0) {
            const size_t p = prev_char(d.lines[d.line], d.col);
            d.lines[d.line].erase(p, d.col - p);
            d.col = d.anchor_col = p;
        } else if (d.line > 0) {
            d.col = d.anchor_col = d.lines[d.line - 1].size();
            d.lines[d.line - 1] += d.lines[d.line];
            d.lines.erase(d.lines.begin() + d.line);
            d.line = --d.anchor_line;
        }
        break;
    case VK_DELETE:
        note_remember(d, true);
        if (note_has_selection(d)) note_erase_selection(d);
        else if (d.col < d.lines[d.line].size()) d.lines[d.line].erase(d.col, next_char(d.lines[d.line], d.col) - d.col);
        else if (d.line + 1 < static_cast<int>(d.lines.size())) {
            d.lines[d.line] += d.lines[d.line + 1];
            d.lines.erase(d.lines.begin() + d.line + 1);
        }
        break;
    default:
        if (const char ch = key_char(k.vkey, key_shift)) {
            note_remember(d, true);
            note_insert(d, std::string(1, ch));
        } else {
            return;
        }
    }
    note_follow(w);
    note_title(w);
}

// --- the hex editor: any file, byte by byte ---------------------------------------------------------

constexpr int HEX_BAR_H = 44, HEX_STATUS_H = 28, HEX_ROW_H = 24, HEX_PX = 15, HEX_CELL = 30, HEX_CHAR = 12;
int hex_rows(const Window &w) { return std::max(1, (w.h - TITLE_H - HEX_BAR_H - HEX_STATUS_H - 40) / HEX_ROW_H); }
int hex_offset_x(const Window &w) { return w.x + 18; }
int hex_bytes_x(const Window &w) { return w.x + 130; }
int hex_cell_x(const Window &w, int i) { return hex_bytes_x(w) + i * HEX_CELL + (i >= 8 ? 10 : 0); }
int hex_text_x(const Window &w) { return hex_cell_x(w, 16) + 24; }
int hex_top(const Window &w) { return w.y + TITLE_H + HEX_BAR_H + 34; }

void hex_title(Window &w) { w.title = (w.hex && !w.hex->edits.empty() ? "*" : "") + base_name(w.file) + " - Hex editor"; }

void hex_follow(Window &w) {
    HexDoc &h = *w.hex;
    const long long row = h.cursor / 16, rows = hex_rows(w);
    if (row < h.top)
        h.top = row;
    else if (row >= h.top + rows)
        h.top = row - rows + 1;
}

void hex_save(Window &w) {
    HexDoc &h = *w.hex;
    const int fd = open(w.file.c_str(), O_WRONLY);
    std::string error;
    if (fd < 0)
        error = std::strerror(errno);
    for (const auto &[at, value] : h.edits)
        if (error.empty() && pwrite(fd, &value, 1, at) != 1)
            error = std::strerror(errno);
    if (fd >= 0)
        close(fd);
    if (!error.empty()) {
        show_message("Hex editor", "Could not save " + base_name(w.file) + ": " + error);
        return;
    }
    h.edits.clear();
    h.pages.clear();
    hex_title(w);
    toast = Toast{"Saved " + base_name(w.file), now_seconds() + 2};
}

void hex_set(HexDoc &h, long long at, uint8_t value) {
    if (at < 0 || at >= h.size)
        return;
    h.edits[at] = value;
}

// hex digits typed (spaces between them or not) written from the cursor on
void hex_write(Window &w, const std::string &text) {
    HexDoc &h = *w.hex;
    std::string digits;
    for (char ch : text)
        if (std::isxdigit(static_cast<unsigned char>(ch)))
            digits += ch;
    for (size_t i = 0; i + 2 <= digits.size(); i += 2, h.cursor++)
        hex_set(h, h.cursor, static_cast<uint8_t>(std::strtoul(digits.substr(i, 2).c_str(), nullptr, 16)));
    h.cursor = std::min(h.cursor, std::max(0ll, h.size - 1));
    hex_follow(w);
    hex_title(w);
}

void hex_goto(Window &w, const std::string &text) {
    HexDoc &h = *w.hex;
    std::string t = text;
    t.erase(std::remove(t.begin(), t.end(), ' '), t.end());
    if (t.empty())
        return;
    // 0x1f40 or 1f40h: hex; plain digits: decimal (as most hex editors read them)
    const bool hex = t.rfind("0x", 0) == 0 || t.rfind("0X", 0) == 0 || t.back() == 'h' || t.find_first_of("abcdefABCDEF") != std::string::npos;
    if (t.back() == 'h')
        t.pop_back();
    const long long at = std::strtoll(t.c_str(), nullptr, hex ? 16 : 10);
    h.cursor = std::clamp(at, 0ll, std::max(0ll, h.size - 1));
    h.low = false;
    hex_follow(w);
}

void hex_key(Window &w, const KeyEvent &k) {
    HexDoc &h = *w.hex;
    const long long last = std::max(0ll, h.size - 1), rows = hex_rows(w);
    const auto go = [&](long long at) {
        h.cursor = std::clamp(at, 0ll, last);
        h.low = false;
        hex_follow(w);
    };
    if (key_ctrl) {
        if (k.vkey == 'S')
            hex_save(w);
        else if (k.vkey == 'G')
            editor_command(w, "hex-goto");
        else if (k.vkey == VK_HOME)
            go(0);
        else if (k.vkey == VK_END)
            go(last);
        else if (k.vkey == 'Z')
            h.edits.erase(h.cursor), hex_title(w);
        return;
    }
    switch (k.vkey) {
    case VK_LEFT: go(h.cursor - 1); return;
    case VK_RIGHT: go(h.cursor + 1); return;
    case VK_UP: go(h.cursor - 16); return;
    case VK_DOWN: go(h.cursor + 16); return;
    case VK_PRIOR: go(h.cursor - 16 * rows); return;
    case VK_NEXT: go(h.cursor + 16 * rows); return;
    case VK_HOME: go(h.cursor & ~15ll); return;
    case VK_END: go(h.cursor | 15); return;
    case VK_TAB: h.ascii = !h.ascii, h.low = false; return;
    case VK_BACK: go(h.cursor - 1); return;
    }
    const char ch = key_char(k.vkey, key_shift);
    if (!ch || h.size == 0)
        return;
    if (h.ascii) {
        hex_set(h, h.cursor, static_cast<uint8_t>(ch));
        go(h.cursor + 1);
    } else if (std::isxdigit(static_cast<unsigned char>(ch))) {
        const int digit = std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0' : std::tolower(ch) - 'a' + 10;
        const int old = std::max(0, h.byte(h.cursor));
        if (!h.low) {
            hex_set(h, h.cursor, static_cast<uint8_t>((digit << 4) | (old & 0x0f)));
            h.low = true;
        } else {
            hex_set(h, h.cursor, static_cast<uint8_t>((old & 0xf0) | digit));
            go(h.cursor + 1);
        }
    }
    hex_title(w);
}

// --- the editors' windows: opening, their bars, closing ----------------------------------------------

struct EditorButton {
    const char *key;
    const char *label;
    Icon icon;
    int w;
};
const EditorButton NOTE_BUTTONS[] = {{"note-save", "Save", Icon::Save, 96},     {"note-undo", "Undo", Icon::Back, 96},
                                     {"note-cut", nullptr, Icon::Cut, 46},       {"note-copy", nullptr, Icon::Copy, 46},
                                     {"note-paste", nullptr, Icon::Paste, 46},   {"note-type", "Type text", Icon::Keyboard, 132},
                                     {"note-find", "Find", Icon::Search, 92},    {"note-hex", "Hex view", Icon::Hex, 122}};
const EditorButton HEX_BUTTONS[] = {{"hex-save", "Save", Icon::Save, 96},        {"hex-revert", "Undo all", Icon::Back, 120},
                                    {"hex-goto", "Go to offset", Icon::Forward, 150}, {"hex-bytes", "Write bytes", Icon::Keyboard, 150},
                                    {"hex-text", "Text column", Icon::Rename, 150},   {"hex-note", "Notepad view", Icon::Notepad, 160}};

const EditorButton *editor_buttons(const Window &w, int &count) {
    count = w.kind == Kind::Notepad ? static_cast<int>(std::size(NOTE_BUTTONS)) : static_cast<int>(std::size(HEX_BUTTONS));
    return w.kind == Kind::Notepad ? NOTE_BUTTONS : HEX_BUTTONS;
}
Rect editor_button_rect(const Window &w, int i) {
    int count = 0, x = w.x + 10;
    const EditorButton *buttons = editor_buttons(w, count);
    for (int k = 0; k < i; k++)
        x += buttons[k].w;
    return Rect{x, w.y + TITLE_H + 5, buttons[i].w - 4, (w.kind == Kind::Notepad ? NOTE_BAR_H : HEX_BAR_H) - 10};
}

void open_editor(Kind kind, const std::string &path) {
    for (Window &w : windows)  // (open already: to the front)
        if (w.kind == kind && w.file == path) {
            bring_to_front(w.id);
            return;
        }
    if (kind == Kind::Notepad) {
        auto doc = std::make_shared<TextDoc>();
        std::string error;
        if (!load_text(path, *doc, error)) {
            show_message("Notepad", "Could not open " + base_name(path) + ": " + error);
            return;
        }
        Window *w = open_window(Kind::Notepad, path);
        w->text = doc;
        note_title(*w);
    } else {
        auto doc = std::make_shared<HexDoc>();
        doc->fd = open(path.c_str(), O_RDONLY);
        struct stat st{};
        if (doc->fd < 0 || fstat(doc->fd, &st) != 0) {
            show_message("Hex editor", "Could not open " + base_name(path) + ": " + std::strerror(errno));
            return;
        }
        doc->size = st.st_size;
        Window *w = open_window(Kind::HexEditor, path);
        w->hex = doc;
        hex_title(*w);
    }
}

void open_terminal() {
    if (Window *w = open_window(Kind::Terminal))
        w->scroll = 0;
}

// the close button: an editor with changes not saved asks first
void request_close(int id) {
    Window *w = find_window(id);
    if (w && ((w->text && w->text->modified) || (w->hex && !w->hex->edits.empty()))) {
        show_message("Save changes to " + base_name(w->file) + "?", "Your changes are lost if you don't save them.");
        dialog.kind = DialogKind::Unsaved;
        dialog.window = id;
        return;
    }
    close_window(id);
}

void editor_command(Window &w, const std::string &cmd) {
    if (w.text) {
        TextDoc &d = *w.text;
        if (cmd == "note-save") {
            note_save(w);
        } else if (cmd == "note-undo") {
            KeyEvent z{'Z', 0, false, true};
            const bool ctrl = key_ctrl;
            key_ctrl = true;
            notepad_key(w, z);
            key_ctrl = ctrl;
        } else if ((cmd == "note-cut" || cmd == "note-copy") && note_has_selection(d)) {
            text_clipboard = note_selected(d);
            if (cmd == "note-cut")
                note_remember(d, false), note_erase_selection(d);
        } else if (cmd == "note-paste" && !text_clipboard.empty()) {
            note_remember(d, false), note_insert(d, text_clipboard);
        } else if (cmd == "note-all") {
            d.anchor_line = 0, d.anchor_col = 0, d.line = static_cast<int>(d.lines.size()) - 1, d.col = d.lines.back().size();
        } else if (cmd == "note-type" || cmd == "note-find") {
            const bool find = cmd == "note-find";
            text_request = TextRequest{find ? TextFor::NoteFind : TextFor::NoteText, w.file, w.id};
            if (!keyboard_open(find ? "Find" : "Type into " + base_name(w.file), find ? note_selected(d) : ""))
                text_request = TextRequest{};
        } else if (cmd == "note-hex") {
            open_editor(Kind::HexEditor, w.file);
        }
        note_follow(w);
        note_title(w);
    } else if (w.hex) {
        HexDoc &h = *w.hex;
        if (cmd == "hex-save" && !h.edits.empty()) {
            hex_save(w);
        } else if (cmd == "hex-revert") {
            h.edits.clear();
        } else if (cmd == "hex-goto" || cmd == "hex-bytes") {
            char at[32];
            std::snprintf(at, sizeof at, "0x%llX", h.cursor);
            const bool go = cmd == "hex-goto";
            text_request = TextRequest{go ? TextFor::HexGoto : TextFor::HexBytes, w.file, w.id};
            if (!keyboard_open(go ? "Go to offset (0x1F40 hex, 8000 decimal)" : std::string("Bytes to write at ") + at + " (hex: 4D 5A 90)",
                               go ? at : ""))
                text_request = TextRequest{};
        } else if (cmd == "hex-text") {
            h.ascii = !h.ascii, h.low = false;
        } else if (cmd == "hex-note") {
            if (h.size < (16 << 20))
                open_editor(Kind::Notepad, w.file);
            else
                show_message("Notepad", "It opens files up to 16 MB.");
        }
        hex_title(w);
    }
}

// Find in Notepad: the next match after the cursor (from the top again at the end), chosen
void note_find(Window &w, const std::string &what) {
    if (!w.text || what.empty())
        return;
    TextDoc &d = *w.text;
    const std::string needle = lower(what);
    const int n = static_cast<int>(d.lines.size());
    for (int k = 0; k <= n; k++) {
        const int l = (d.line + k) % n;
        const size_t from = k == 0 ? std::max(d.col, d.anchor_col) : 0;
        const size_t at = lower(d.lines[l]).find(needle, from);
        if (at != std::string::npos) {
            d.anchor_line = d.line = l;
            d.anchor_col = at;
            d.col = at + needle.size();
            note_follow(w);
            return;
        }
    }
    toast = Toast{"Can't find \"" + what + "\"", now_seconds() + 3};
}

std::string editor_hit(const Window &w, float px, float py) {
    int count = 0;
    const EditorButton *buttons = editor_buttons(w, count);
    for (int i = 0; i < count; i++)
        if (editor_button_rect(w, i).contains(px, py))
            return std::string("ed:") + buttons[i].key;
    if (w.kind == Kind::Notepad && note_area(w).contains(px, py))
        return "text";
    if (w.kind == Kind::HexEditor && py >= hex_top(w) && py < w.y + w.h - HEX_STATUS_H)
        return "bytes";
    return {};
}

int selecting = 0;  // a Notepad window whose text the pointer is choosing (pressed in it, not let go)
int note_type_window = 0;  // a Notepad window clicked: the PS4's keyboard opens for it (the main loop)
double note_type_when = 0;

// the pointer at (px, py) in Notepad's text: the line and column there
void note_point(Window &w, float px, float py, bool extend) {
    TextDoc &d = *w.text;
    const Rect a = note_area(w);
    const int line = std::clamp(static_cast<int>(d.top) + static_cast<int>((py - a.y - 4) / NOTE_LINE_H), 0,
                                static_cast<int>(d.lines.size()) - 1);
    d.line = line;
    d.col = col_at(d.lines[line], static_cast<int>(px) - a.x - NOTE_LEFT + d.left);
    if (!extend)
        d.anchor_line = d.line, d.anchor_col = d.col;
}

void editor_click(Window &w, const std::string &part, bool twice, float px, float py) {
    if (part.rfind("ed:", 0) == 0) {
        editor_command(w, part.substr(3));
    } else if (part == "text" && w.text) {
        note_point(w, px, py, key_shift);
        if (twice) {  // a word
            TextDoc &d = *w.text;
            const std::string &line = d.lines[d.line];
            const auto word = [&](size_t i) { return i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_'); };
            size_t a = d.col, b = d.col;
            while (a > 0 && word(a - 1))
                a--;
            while (word(b))
                b++;
            d.anchor_col = a, d.col = b;
        } else {
            selecting = w.id;
        }
    } else if (part == "bytes" && w.hex) {
        HexDoc &h = *w.hex;
        const long long row = h.top + static_cast<long long>((py - hex_top(w)) / HEX_ROW_H);
        int col = -1;
        h.ascii = px >= hex_text_x(w) - 6;
        for (int i = 0; i < 16; i++)
            if (h.ascii ? px < hex_text_x(w) + (i + 1) * HEX_CHAR : px < hex_cell_x(w, i) + HEX_CELL - 4) {
                col = i;
                break;
            }
        if (col < 0)
            col = 15;
        h.cursor = std::clamp(row * 16 + col, 0ll, std::max(0ll, h.size - 1));
        h.low = false;
        if (twice && !h.ascii)
            editor_command(w, "hex-bytes");
    }
}

void draw_editor_bar(Canvas &c, const Window &w) {
    const int bar_h = w.kind == Kind::Notepad ? NOTE_BAR_H : HEX_BAR_H;
    c.fill_rect(w.x + 1, w.y + TITLE_H, w.w - 2, bar_h, WINDOW_BG);
    c.fill_rect(w.x + 1, w.y + TITLE_H + bar_h - 1, w.w - 2, 1, BORDER);
    int count = 0;
    const EditorButton *buttons = editor_buttons(w, count);
    const std::string id = "win:" + std::to_string(w.id) + ":ed:";
    for (int i = 0; i < count; i++) {
        const Rect r = editor_button_rect(w, i);
        const bool on = w.hex && std::strcmp(buttons[i].key, "hex-text") == 0 && w.hex->ascii;
        if (on)
            c.fill_round_rect(r.x, r.y, r.w, r.h, 5, SELECTED);
        else if (hover == id + buttons[i].key)
            c.fill_round_rect(r.x, r.y, r.w, r.h, 5, HOVER);
        draw_icon(c, buttons[i].icon, r.x + 22, r.y + r.h / 2.f, 18);
        if (buttons[i].label)
            c.text(r.x + 40, r.y + r.h / 2 + 5, buttons[i].label, 14, TEXT);
    }
}

void draw_notepad(Canvas &c, Window &w) {
    if (!w.text)
        return;
    TextDoc &d = *w.text;
    draw_editor_bar(c, w);
    const Rect a = note_area(w);
    c.fill_rect(a.x, a.y, a.w, a.h, CONTENT_BG);
    const int rows = note_rows(w);
    d.top = std::clamp(d.top, 0.f, static_cast<float>(std::max(0, static_cast<int>(d.lines.size()) - rows)));
    int l0, l1;
    size_t c0, c1;
    note_range(d, l0, c0, l1, c1);
    const bool chosen = note_has_selection(d);
    const int tx = a.x + NOTE_LEFT, right = a.x + a.w - 14;
    for (int r = 0; r < rows; r++) {
        const int l = static_cast<int>(d.top) + r;
        if (l >= static_cast<int>(d.lines.size()))
            break;
        const std::string &line = d.lines[l];
        const int y = a.y + 4 + r * NOTE_LINE_H;
        // the selection on this line
        if (chosen && l >= l0 && l <= l1) {
            const int sx = l == l0 ? text_x(line, c0) : 0;
            const int ex = l == l1 ? text_x(line, c1) : std::min(text_x(line, line.size()) + 8, d.left + right - tx);
            const int x0 = std::max(tx, tx + sx - d.left), x1 = std::min(right, tx + ex - d.left);
            if (x1 > x0)
                c.fill_rect(x0, y + 2, x1 - x0, NOTE_LINE_H - 3, rgb(0x0078d4, 0x80));
        }
        // (scrolled sideways: the line from its first letter at the left edge on)
        size_t first = d.left > 0 ? col_at(line, d.left) : 0;
        if (first > 0 && text_x(line, first) > d.left)
            first = prev_char(line, first);
        const int x = tx + text_x(line, first) - d.left;
        if (first < line.size())
            c.text(x, y + 17, expand_tabs(line.substr(first, 1500)), NOTE_PX, TEXT, Weight::Regular, std::max(1, right - x));
        if (l == d.line && (w.id == windows.back().id)) {
            const int cx = tx + text_x(line, d.col) - d.left;
            if (cx >= tx - 1 && cx <= right)
                c.fill_rect(cx, y + 3, 2, NOTE_LINE_H - 5, TEXT);
        }
    }
    // the scroll bar
    const int total = static_cast<int>(d.lines.size());
    if (total > rows) {
        const int bh = std::max(30, a.h * rows / total);
        const int by = a.y + static_cast<int>((a.h - bh) * (d.top / std::max(1, total - rows)));
        c.fill_round_rect(a.x + a.w - 8, by, 4, bh, 2, SCROLLBAR);
    }
    // the status bar: where the cursor is, the file's line ends
    const int sy = w.y + w.h - NOTE_STATUS_H;
    c.fill_rect(w.x + 1, sy, w.w - 2, NOTE_STATUS_H - 1, WINDOW_BG);
    c.fill_rect(w.x + 1, sy, w.w - 2, 1, BORDER);
    size_t chars = 0;
    for (const std::string &l : d.lines)
        chars += l.size();
    const std::string where = "Ln " + std::to_string(d.line + 1) + ", Col " + std::to_string(d.col + 1);
    c.text(w.x + 16, sy + 19, where, 13, TEXT2);
    c.text(w.x + 200, sy + 19, std::to_string(chars) + " characters", 13, TEXT2);
    const std::string kind = std::string(d.crlf ? "Windows (CRLF)" : "Unix (LF)") + "    UTF-8";
    c.text(w.x + w.w - 16 - c.text_width(kind, 13), sy + 19, kind, 13, TEXT2);
}

void draw_hex(Canvas &c, Window &w) {
    if (!w.hex)
        return;
    HexDoc &h = *w.hex;
    draw_editor_bar(c, w);
    const int top = w.y + TITLE_H + HEX_BAR_H, bottom = w.y + w.h - HEX_STATUS_H;
    c.fill_rect(w.x + 1, top, w.w - 2, bottom - top, CONTENT_BG);
    // the header: the offsets across
    const int hy = top + 24;
    c.text(hex_offset_x(w), hy, "Offset", 13, TEXT3);
    char digit[8];
    for (int i = 0; i < 16; i++) {
        std::snprintf(digit, sizeof digit, "%02X", i);
        c.text(hex_cell_x(w, i) + 2, hy, digit, 13, TEXT3);
    }
    c.text(hex_text_x(w), hy, "Text", 13, TEXT3);
    c.fill_rect(w.x + 12, top + 32, w.w - 24, 1, BORDER);
    const int rows = hex_rows(w);
    const long long total_rows = (h.size + 15) / 16;
    h.top = std::clamp(h.top, 0ll, std::max(0ll, total_rows - rows));
    const bool wide = h.size > 0xffffffffll;
    for (int r = 0; r < rows; r++) {
        const long long row = h.top + r;
        if (row >= total_rows)
            break;
        const int y = hex_top(w) + r * HEX_ROW_H;
        char off[24];
        std::snprintf(off, sizeof off, wide ? "%010llX" : "%08llX", row * 16);
        c.text(hex_offset_x(w), y + 17, off, HEX_PX, TEXT3);
        for (int i = 0; i < 16; i++) {
            const long long at = row * 16 + i;
            const int b = h.byte(at);
            if (b < 0)
                break;
            const bool here = at == h.cursor, changed = h.edits.count(at) > 0;
            const int cx = hex_cell_x(w, i), ax = hex_text_x(w) + i * HEX_CHAR;
            if (here) {
                c.fill_round_rect(cx - 2, y + 1, HEX_CELL - 4, HEX_ROW_H - 2, 4, h.ascii ? SELECTED : ACCENT);
                c.fill_round_rect(ax - 1, y + 1, HEX_CHAR + 1, HEX_ROW_H - 2, 3, h.ascii ? ACCENT : SELECTED);
            }
            std::snprintf(digit, sizeof digit, "%02X", b);
            const Color col = here && !h.ascii ? ACCENT_TEXT : changed ? rgb(0xff8c42) : b == 0 ? TEXT3 : TEXT;
            c.text(cx + 1, y + 17, digit, HEX_PX, col);
            const char shown[2] = {b >= 0x20 && b < 0x7f ? static_cast<char>(b) : '.', 0};
            c.text(ax, y + 17, shown, HEX_PX, here && h.ascii ? ACCENT_TEXT : changed ? rgb(0xff8c42) : b >= 0x20 && b < 0x7f ? TEXT2 : TEXT3);
        }
    }
    if (total_rows > rows) {
        const int area = bottom - hex_top(w);
        const int bh = std::max(30, static_cast<int>(area * rows / static_cast<double>(total_rows)));
        const int by = hex_top(w) + static_cast<int>((area - bh) * (static_cast<double>(h.top) / std::max(1ll, total_rows - rows)));
        c.fill_round_rect(w.x + w.w - 10, by, 4, bh, 2, SCROLLBAR);
    }
    // the status bar: the byte under the cursor
    c.fill_rect(w.x + 1, bottom, w.w - 2, HEX_STATUS_H - 1, WINDOW_BG);
    c.fill_rect(w.x + 1, bottom, w.w - 2, 1, BORDER);
    const int b = h.byte(h.cursor);
    char status[160];
    std::snprintf(status, sizeof status, "Offset 0x%llX (%lld)    Byte 0x%02X = %d%s", h.cursor, h.cursor, std::max(0, b), std::max(0, b),
                  h.ascii ? "    Typing: text" : "    Typing: hex digits");
    c.text(w.x + 16, bottom + 19, status, 13, TEXT2);
    const std::string right = size_text(h.size) + (h.edits.empty() ? std::string() : "    " + std::to_string(h.edits.size()) + " bytes changed");
    c.text(w.x + w.w - 16 - c.text_width(right, 13), bottom + 19, right, 13, TEXT2);
}

// --- the PS4 keyboard's text, for what asked for it -------------------------------------------------

void use_background(const Background &b);

void finish_text(const std::string &text) {
    const TextRequest request = text_request;
    text_request = TextRequest{};
    Window *w = find_window(request.window);
    switch (request.what) {
    case TextFor::Color: {
        std::string hex;
        for (char ch : text)
            if (std::isxdigit(static_cast<unsigned char>(ch)))
                hex += ch;
        if (hex.size() != 6) {
            show_message("Background color", "A color is six hex digits, like 1E64C8 (red, green, blue).");
            return;
        }
        Background b;
        b.kind = BgKind::Color;
        b.color = static_cast<uint32_t>(std::strtoul(hex.c_str(), nullptr, 16));
        use_background(b);
        return;
    }
    case TextFor::Wine: winwin_text(text); return;
    case TextFor::Rename: finish_rename(w, request.path, text); return;
    case TextFor::Address:
        if (w)
            go_to_address(*w, text);
        return;
    case TextFor::Search:
        if (w) {
            w->filter = w->search.text = text;
            load_entries(*w);
            w->scroll = 0;
            w->chosen.clear();
            w->cursor = w->anchor = -1;
        }
        return;
    case TextFor::NoteText:
        if (w && w->text) {
            note_remember(*w->text, false);
            note_insert(*w->text, text);
            note_follow(*w);
            note_title(*w);
        }
        return;
    case TextFor::NoteFind:
        if (w)
            note_find(*w, text);
        return;
    case TextFor::HexGoto:
        if (w && w->hex)
            hex_goto(*w, text);
        return;
    case TextFor::HexBytes:
        if (w && w->hex)
            hex_write(*w, text);
        return;
    case TextFor::None: return;
    }
}

// --- Files: the pointer's parts and clicks -----------------------------------------------------------

std::map<int, std::vector<Rect>> drawn_crumbs;  // each Files window's address parts, as drawn last

std::string files_hit(const Window &w, float px, float py) {
    int count = 0;
    const CmdButton *cmds = files_cmds(w, count);
    for (int i = 0; i < count; i++)
        if (*cmds[i].key && files_cmd_rect(w, i).contains(px, py))
            return std::string("cmd:") + cmds[i].key;
    static const char *const nav_buttons[] = {"back", "forward", "up", "refresh"};
    for (int i = 0; i < 4; i++)
        if (files_nav_button(w, i).contains(px, py))
            return nav_buttons[i];
    if (files_address(w).contains(px, py)) {
        auto it = drawn_crumbs.find(w.id);
        if (!w.address.active && it != drawn_crumbs.end())
            for (int i = 0; i < static_cast<int>(it->second.size()); i++)
                if (it->second[i].contains(px, py))
                    return "crumb" + std::to_string(i);
        return "addr";
    }
    if (files_search(w).contains(px, py))
        return "search";
    const auto nav = places();
    for (int i = 0; i < static_cast<int>(nav.size()); i++)
        if (files_nav_item(w, i).contains(px, py))
            return "nav" + std::to_string(i);
    if (files_header(w).contains(px, py)) {
        for (int i = 3; i > 0; i--)
            if (px >= files_column_x(w, i) - 10)
                return "col" + std::to_string(i);
        return "col0";
    }
    const Rect list = files_list(w);
    if (list.contains(px, py)) {
        const int row = static_cast<int>((py - list.y) / ROW_H) + static_cast<int>(w.scroll);
        if (row < static_cast<int>(w.entries.size()))
            return (px < list.x + 36 ? "check" : "row") + std::to_string(row);
        return "list";
    }
    return {};
}

void commit_rename(Window &w) {
    if (!w.rename.active)
        return;
    w.rename.active = false;
    finish_rename(&w, w.rename_path, w.rename.text);
}

// The address bar to type into: on a USB keyboard there, else on the PS4's keyboard
void start_address(Window &w) {
    commit_rename(w);
    if (hwkeyboard_connected()) {
        const std::string text = shown_path(w.path);
        w.address = LineEdit{true, text, text.size(), 0};
        w.search.active = false;
        return;
    }
    text_request = TextRequest{TextFor::Address, std::string(), w.id};
    if (!keyboard_open("Go to (C:\\users\\ps4, /data, ...)", w.path.empty() ? std::string() : shown_path(w.path)))
        text_request = TextRequest{};
}

void start_search(Window &w) {
    commit_rename(w);
    if (hwkeyboard_connected()) {
        w.search.active = true;
        w.search.cursor = w.search.text.size(), w.search.anchor = 0;
        w.address.active = false;
        return;
    }
    text_request = TextRequest{TextFor::Search, std::string(), w.id};
    if (!keyboard_open("Search " + place_title(w.path), w.filter))
        text_request = TextRequest{};
}

void files_click(Window &w, const std::string &part, bool twice, float px, float py) {
    // a click elsewhere ends typing in a box (the name typed is kept, as Windows does)
    if (w.rename.active && part.rfind("row", 0) != 0)
        commit_rename(w);
    if (part != "addr")
        w.address.active = false;
    if (part != "search")
        w.search.active = false;
    if (part.rfind("cmd:", 0) == 0) {
        const std::string cmd = part.substr(4);
        if (cmd == "new" || cmd == "sort") {  // their drop-down menus
            int count = 0;
            const CmdButton *cmds = files_cmds(w, count);
            for (int i = 0; i < count; i++)
                if (cmd == cmds[i].key) {
                    const Rect r = files_cmd_rect(w, i);
                    open_menu("win:" + std::to_string(w.id) + ":menu" + cmd, static_cast<float>(r.x), static_cast<float>(r.y + r.h + 4));
                }
            return;
        }
        files_command(w, cmd);
    } else if (part == "back" || part == "forward" || part == "up" || part == "refresh") {
        files_command(w, part);
    } else if (part.rfind("crumb", 0) == 0) {
        const auto list = crumbs(w.path);
        const int i = number_after(part, "crumb");
        if (i >= 0 && i < static_cast<int>(list.size()))
            navigate(w, list[i].path);
    } else if (part == "addr") {
        if (!w.address.active)
            start_address(w);
    } else if (part == "search") {
        if (!w.search.active)
            start_search(w);
    } else if (part.rfind("nav", 0) == 0) {
        const auto list = places();
        const int i = number_after(part, "nav");
        if (i >= 0 && i < static_cast<int>(list.size()))
            navigate(w, list[i].path);
    } else if (part.rfind("col", 0) == 0) {
        if (!w.path.empty())
            files_command(w, "sort" + std::to_string(number_after(part, "col")));
    } else if (part.rfind("check", 0) == 0) {
        choose_toggle(w, number_after(part, "check"));
    } else if (part.rfind("row", 0) == 0) {
        const int row = number_after(part, "row");
        if (row >= static_cast<int>(w.entries.size()))
            return;
        const Entry e = w.entries[row];
        if (w.rename.active && w.rename_path == e.path)
            return;  // (a click in the name being typed)
        commit_rename(w);
        if (twice && w.chosen.count(e.path) && !key_ctrl && !key_shift) {
            open_entry(w, e);
            return;
        }
        int click_row = -1;
        if (key_ctrl)
            choose_toggle(w, row);
        else if (key_shift)
            choose_range(w, row);
        else if (!w.chosen.count(e.path))
            choose_only(w, row);
        else  // (one of several chosen: they drag together; let go without a drag, it's chosen alone)
            w.cursor = w.anchor = row, click_row = row;
        if (!w.path.empty() && w.chosen.count(e.path)) {
            const auto list = chosen_paths(w);
            dragged = DraggedFiles{true, false, px, py, list, list.size() == 1 ? e.name : std::to_string(list.size()) + " items", e.icon};
            dragged.click_row = click_row;
            dragged.window = w.id;
        }
    } else if (part == "list") {
        // empty space: the selection goes (not with Ctrl), and a selection rectangle starts
        if (!key_ctrl)
            w.chosen.clear();
        w.cursor = -1;
        const Rect list = files_list(w);
        w.banding = true;
        w.band_x = px;
        w.band_y = (py - list.y) / ROW_H + w.scroll;
    }
}

// --- the USB keyboard in the desktop's own windows -------------------------------------------------

void click(const std::string &key, bool twice, float px, float py);  // (below)

void files_key(Window &w, const KeyEvent &k) {
    // typing in one of its boxes
    if (w.rename.active) {
        if (k.vkey == VK_RETURN)
            commit_rename(w);
        else if (k.vkey == VK_ESCAPE)
            w.rename.active = false;
        else
            edit_key(w.rename, k);
        return;
    }
    if (w.address.active) {
        if (k.vkey == VK_RETURN) {
            w.address.active = false;
            go_to_address(w, w.address.text);
        } else if (k.vkey == VK_ESCAPE) {
            w.address.active = false;
        } else {
            edit_key(w.address, k);
        }
        return;
    }
    if (w.search.active) {
        if (k.vkey == VK_ESCAPE) {
            w.search = LineEdit{};
            w.filter.clear();
            refresh(w);
        } else if (k.vkey == VK_RETURN || k.vkey == VK_DOWN) {
            w.search.active = false;
            if (!w.entries.empty())
                choose_only(w, 0);
        } else if (edit_key(w.search, k)) {
            w.filter = w.search.text;
            load_entries(w);
            w.scroll = 0;
            w.chosen.clear();
            w.cursor = w.anchor = -1;
        }
        return;
    }
    const int n = static_cast<int>(w.entries.size());
    const auto go = [&](int i) {
        if (n == 0)
            return;
        i = std::clamp(i, 0, n - 1);
        if (key_shift)
            choose_range(w, i);
        else if (key_ctrl)
            w.cursor = i;  // (Ctrl moves the focus only; Ctrl+Space chooses)
        else
            choose_only(w, i);
        keep_in_view(w, i);
    };
    if (key_alt) {
        if (k.vkey == VK_LEFT) go_back(w);
        else if (k.vkey == VK_RIGHT) go_forward(w);
        else if (k.vkey == VK_UP) go_up(w);
        else if (k.vkey == VK_RETURN) files_command(w, "properties", w.cursor);
        else if (k.vkey == 'D') start_address(w);
        return;
    }
    if (key_ctrl) {
        switch (k.vkey) {
        case 'A': choose_all(w); return;
        case 'C': files_command(w, "copy"); return;
        case 'X': files_command(w, "cut"); return;
        case 'V': files_command(w, "paste"); return;
        case 'N':
            if (key_shift) make_new(w, true);
            else open_window(Kind::Files, w.path);
            return;
        case 'W': request_close(w.id); return;
        case 'F':
        case 'E': start_search(w); return;
        case 'L': start_address(w); return;
        case VK_SPACE: choose_toggle(w, w.cursor); return;
        }
    }
    switch (k.vkey) {
    case VK_UP: go(w.cursor < 0 ? 0 : w.cursor - 1); return;
    case VK_DOWN: go(w.cursor < 0 ? 0 : w.cursor + 1); return;
    case VK_PRIOR: go(w.cursor - files_rows(w)); return;
    case VK_NEXT: go(w.cursor + files_rows(w)); return;
    case VK_HOME: go(0); return;
    case VK_END: go(n - 1); return;
    case VK_RETURN:
        if (w.cursor >= 0 && w.cursor < n)
            open_entry(w, Entry(w.entries[w.cursor]));
        return;
    case VK_BACK: go_back(w); return;
    case VK_DELETE: files_command(w, key_shift ? "delete-now" : "delete"); return;
    case VK_F2: files_command(w, "rename", w.cursor); return;
    case VK_F3: start_search(w); return;
    case VK_F5: refresh(w); return;
    case VK_ESCAPE: w.chosen.clear(); return;
    case VK_APPS: {  // the context menu key: the entry's menu, by it
        const Rect list = files_list(w);
        const int row = w.cursor >= 0 ? w.cursor : -1;
        const float y = row >= 0 ? list.y + (row - w.scroll) * ROW_H + ROW_H : list.y + 20.f;
        open_menu("win:" + std::to_string(w.id) + (row >= 0 ? ":row" + std::to_string(row) : std::string(":list")),
                  static_cast<float>(list.x + 80), y);
        return;
    }
    }
    // letters: the next entry whose name starts with what was typed (as Explorer's)
    if (const char ch = key_char(k.vkey, false); ch && ch != ' ' && n > 0) {
        static std::string typed;
        static double typed_at = 0;
        const double t = now_seconds();
        if (t - typed_at > 1.0)
            typed.clear();
        typed_at = t;
        typed += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const int from = w.cursor < 0 ? 0 : w.cursor + (typed.size() == 1 ? 1 : 0);
        for (int k2 = 0; k2 < n; k2++) {
            const int i = (from + k2) % n;
            if (lower(w.entries[i].name).rfind(typed, 0) == 0) {
                choose_only(w, i);
                keep_in_view(w, i);
                break;
            }
        }
    }
}

// A USB keyboard's key for the desktop's own windows (the one in front), its menus and dialogs
// the desktop's window (or menu, dialog) a USB keyboard's press went to, for the Keyboard log
std::string key_target() {
    if (dialog.open)
        return "dialog";
    if (menu.open)
        return "menu";
    if (popup != Popup::None)
        return "Start or flyout";
    const Window *w = focused();
    if (!w)
        return "desktop (no window open)";
    switch (w->kind) {
    case Kind::Files: return "Files";
    case Kind::Terminal: return "Terminal";
    case Kind::Settings: return "Settings";
    case Kind::TaskManager: return "Task Manager";
    case Kind::Notepad: return w->text ? "Notepad" : "Notepad (no document)";
    case Kind::HexEditor: return "hex editor";
    }
    return "window";
}

// a line a second while presses come (Settings > Diagnostics > Keyboard log)
void log_key_routes(double now) {
    static const bool on = env_get("WINEPS4_KEY_LOG") != "0";
    KeyRoutes &r = key_routes;
    if (!on || now - r.since < 1)
        return;
    if (r.desktop || r.program || r.held_back || r.not_sent) {
        std::string top = "none";
        for (const WinTask &t : program_tasks)
            if (t.active)
                top = t.title.empty() ? "untitled" : t.title;
        std::fprintf(stderr,
                     "shell: keyboard: %d presses to the desktop's %s, %d to programs (the top one: %s), %d held back "
                     "(the PS4's keyboard up), %d not sent (PS4 + Keyboard mode)\n",
                     r.desktop, r.to.empty() ? "-" : r.to.c_str(), r.program, top.c_str(), r.held_back, r.not_sent);
    }
    r = KeyRoutes{};
    r.since = now;
}

void desktop_key(const KeyEvent &k) {
    if (!k.down)
        return;
    if (dialog.open) {
        if (k.vkey == VK_RETURN)
            click("dialog:0", false, 0, 0);
        else if (k.vkey == VK_ESCAPE)
            dialog.open = false;
        return;
    }
    if (menu.open) {
        if (k.vkey == VK_ESCAPE)
            menu.open = false;
        else if (k.vkey == VK_RETURN && hover.rfind("menu:", 0) == 0)
            click(hover, false, 0, 0);
        return;
    }
    if (popup != Popup::None) {
        if (k.vkey == VK_ESCAPE)
            popup = Popup::None;
        return;
    }
    Window *w = focused();
    if (!w)
        return;
    if (key_alt && k.vkey == 0x73) {  // Alt+F4
        request_close(w->id);
        return;
    }
    switch (w->kind) {
    case Kind::Files: files_key(*w, k); break;
    case Kind::Notepad:
        if (w->text)
            notepad_key(*w, k);
        break;
    case Kind::HexEditor:
        if (w->hex)
            hex_key(*w, k);
        break;
    case Kind::Terminal:
        if (k.vkey == VK_PRIOR) w->scroll += 10;
        else if (k.vkey == VK_NEXT) w->scroll = std::max(0.f, w->scroll - 10);
        else if (k.vkey == VK_END) w->scroll = 0;
        break;
    default: break;
    }
}

// --- installed programs: the uninstall entries in the registry's files ------------------------

struct InstalledApp {
    std::string name, publisher, version, uninstall, icon;
};

// a value on a registry file's line ("name"="text" or "name"=str(2):"text"), unescaped
std::string reg_string(const std::string &line, const char *name) {
    const std::string key = std::string("\"") + name + "\"=";
    if (line.rfind(key, 0) != 0)
        return {};
    size_t at = key.size();
    if (line.compare(at, 7, "str(2):") == 0)
        at += 7;
    if (at >= line.size() || line[at] != '"')
        return {};
    std::string out;
    for (size_t i = at + 1; i < line.size() && line[i] != '"'; i++) {
        if (line[i] == '\\' && i + 1 < line.size())
            i++;
        out += line[i];
    }
    return out;
}

// what the programs registered (HKLM, its 32-bit view and HKCU; wineserver writes the files out
// every half minute, so a program just installed may come a moment later)
std::vector<InstalledApp> read_installed_apps() {
    std::vector<InstalledApp> apps;
    for (const char *file : {"/prefix/system.reg", "/prefix/user.reg"}) {
        FILE *f = std::fopen((std::string(DATA_DIR) + file).c_str(), "r");
        if (!f)
            continue;
        char buf[4096];
        bool entry = false, hidden = false;
        InstalledApp app;
        const auto finish = [&] {
            if (entry && !hidden && !app.name.empty() && !app.uninstall.empty())
                apps.push_back(app);
            entry = hidden = false;
            app = InstalledApp{};
        };
        while (std::fgets(buf, sizeof buf, f)) {
            std::string l = buf;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
                l.pop_back();
            if (!l.empty() && l[0] == '[') {
                finish();
                entry = l.find("Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\") != std::string::npos;
                continue;
            }
            if (!entry)
                continue;
            if (l.rfind("\"DisplayName\"=", 0) == 0) app.name = reg_string(l, "DisplayName");
            else if (l.rfind("\"Publisher\"=", 0) == 0) app.publisher = reg_string(l, "Publisher");
            else if (l.rfind("\"DisplayVersion\"=", 0) == 0) app.version = reg_string(l, "DisplayVersion");
            else if (l.rfind("\"UninstallString\"=", 0) == 0) app.uninstall = reg_string(l, "UninstallString");
            else if (l.rfind("\"DisplayIcon\"=", 0) == 0) app.icon = reg_string(l, "DisplayIcon");
            else if (l == "\"SystemComponent\"=dword:00000001") hidden = true;
        }
        finish();
        std::fclose(f);
    }
    std::sort(apps.begin(), apps.end(), [](const InstalledApp &a, const InstalledApp &b) { return lower(a.name) < lower(b.name); });
    apps.erase(std::unique(apps.begin(), apps.end(), [](const InstalledApp &a, const InstalledApp &b) { return a.name == b.name; }), apps.end());
    return apps;
}

const std::vector<InstalledApp> &installed_apps() {
    static std::vector<InstalledApp> apps;
    static double when = -10;
    if (now_seconds() - when > 5) {
        apps = read_installed_apps();
        when = now_seconds();
    }
    return apps;
}

// A Windows command line's words (the quotes taken out): what an uninstall entry runs
std::vector<std::string> split_command_line(const std::string &cmd) {
    std::vector<std::string> words;
    std::string word;
    bool quoted = false, any = false;
    for (char ch : cmd) {
        if (ch == '"') {
            quoted = !quoted;
            any = true;
        } else if ((ch == ' ' || ch == '\t') && !quoted) {
            if (any || !word.empty())
                words.push_back(word);
            word.clear();
            any = false;
        } else {
            word += ch;
        }
    }
    if (any || !word.empty())
        words.push_back(word);
    return words;
}

// An app's icon file: its DisplayIcon ("path,index"), else the program its uninstaller is
std::string app_icon_path(const InstalledApp &app) {
    std::string path = app.icon;
    if (!path.empty() && path.front() == '"') {
        const auto words = split_command_line(path);
        path = words.empty() ? std::string() : words.front();
    }
    const size_t comma = path.rfind(',');
    if (comma != std::string::npos && path.find_first_not_of("-0123456789", comma + 1) == std::string::npos)
        path.resize(comma);
    if (path.empty()) {
        const auto words = split_command_line(app.uninstall);
        if (!words.empty())
            path = words.front();
    }
    return console_path(path);
}

// The background tiles: the photos, the styles, the user's own photo (when it's one), Browse
struct Tile {
    std::string key;    // "photo:<path>", "style:<n>", "browse"
    std::string label;
};
std::vector<Tile> settings_tiles() {
    std::vector<Tile> tiles;
    for (const Photo &p : PHOTOS)
        tiles.push_back({std::string("photo:") + p.path, p.name});
    for (int i = 0; i < static_cast<int>(std::size(STYLES)); i++)
        tiles.push_back({"style:" + std::to_string(i), STYLES[i].name});
    if (background.kind == BgKind::Photo &&
        std::none_of(std::begin(PHOTOS), std::end(PHOTOS), [](const Photo &p) { return background.photo == p.path; }))
        tiles.push_back({"photo:" + background.photo, "Your photo"});
    tiles.push_back({"browse", "Browse photos"});
    return tiles;
}

// a tile's picture, made once (photos read and shrunk; styles drawn small)
const Image &tile_image(const std::string &key) {
    static std::map<std::string, Image> cache;
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;
    Image img;
    img.width = TILE_W;
    img.height = TILE_H;
    img.pixels.assign(static_cast<size_t>(TILE_W) * TILE_H, 0xff606060u);
    if (key.rfind("style:", 0) == 0) {
        const Style &st = STYLES[std::clamp(std::atoi(key.c_str() + 6), 0, static_cast<int>(std::size(STYLES)) - 1)];
        for (int y = 0; y < TILE_H; y++)
            for (int x = 0; x < TILE_W; x++)
                img.pixels[static_cast<size_t>(y) * TILE_W + x] = style_pixel(st, (x + 0.5f) / TILE_W, (y + 0.5f) / TILE_H);
    } else if (key.rfind("photo:", 0) == 0) {
        Picture pic;
        std::string why;
        if (load_picture(key.substr(6), pic, why)) {
            Image big;
            big.width = TILE_W * 2;
            big.height = TILE_H * 2;
            big.pixels.resize(static_cast<size_t>(big.width) * big.height);
            cover(pic, big.width, big.height, big.pixels.data());
            img = shrink_image(big, TILE_W, TILE_H);
        }
    }
    return cache.emplace(key, img).first->second;
}

bool tile_selected(const std::string &key) {
    return (background.kind == BgKind::Photo && key == "photo:" + background.photo) ||
           (background.kind == BgKind::Style && key == "style:" + std::to_string(background.style));
}

// What in Settings is under the pointer ("" for nothing)
std::string settings_hit(const Window &w, float px, float py) {
    for (int i = 0; i < static_cast<int>(std::size(SETTINGS_PAGES)); i++)
        if (settings_page_rect(w, i).contains(px, py))
            return "page" + std::to_string(i);
    if (w.page == PAGE_DEVICES) {
        for (int i = 0; i < 2; i++) {
            if (speed_button(w, i).contains(px, py))
                return "speed" + std::to_string(i);
            if (segment(keyboard_card(w), i, 2, 170).contains(px, py))
                return "kbmode" + std::to_string(i);
        }
        if (autokb_card(w).contains(px, py))
            return "autokb";
    } else if (w.page == PAGE_APPS) {
        if (chromium_card(w).contains(px, py))
            return "chromium";
        if (rules_card(w).contains(px, py))
            return "rules";
        if (outside_card(w).contains(px, py))
            return "outside";
        if (dxvk_card(w).contains(px, py))
            return "dxvk";
        for (int i = 0; i < static_cast<int>(installed_apps().size()); i++)
            if (app_uninstall(w, i).contains(px, py))
                return "uninstall" + std::to_string(i);
    } else if (w.page == PAGE_PERSONALIZE) {
        for (int i = 0; i < 2; i++) {
            if (segment(theme_card(w), i, 2, 116).contains(px, py))
                return "theme" + std::to_string(i);
            if (segment(bg_card(w), i, 2, 140).contains(px, py))
                return "bgtab" + std::to_string(i);
        }
        if (w.bg_tab == 0) {
            const auto tiles = settings_tiles();
            for (int i = 0; i < static_cast<int>(tiles.size()); i++)
                if (tile_rect(w, i).contains(px, py))
                    return "tile" + std::to_string(i);
        } else {
            for (int i = 0; i < static_cast<int>(std::size(SWATCHES)); i++)
                if (swatch_rect(w, i).contains(px, py))
                    return "swatch" + std::to_string(i);
            if (custom_color_rect(w).contains(px, py))
                return "customcolor";
        }
    } else if (w.page == PAGE_DIAGNOSTICS) {
        for (int i = 0; i < static_cast<int>(std::size(TOGGLES)); i++)
            if (toggle_rect(w, i).contains(px, py))
                return "toggle" + std::to_string(i);
    }
    return {};
}

// Dialog buttons
int dialog_lines() { return 1 + static_cast<int>(std::count(dialog.text.begin(), dialog.text.end(), '\n')); }
Rect dialog_rect() {
    const int h = 200 + 26 * std::max(0, dialog_lines() - 1) + (dialog.text.size() > 60 && dialog_lines() == 1 ? 22 : 0);
    return Rect{W / 2 - 300, H / 2 - h / 2, 600, h};
}
Rect dialog_button(int i) {  // 0 primary (OK, Delete, Save), then Don't save, Cancel
    const Rect d = dialog_rect();
    const int n = dialog.buttons(), bw = (540 - (n - 1) * 16) / n;
    return Rect{d.x + 30 + i * (bw + 16), d.y + d.h - 64, bw, 44};
}

// The right-click menu's items (separators are thinner)
Rect menu_item_rect(int i) {
    int y = menu.y + 4;
    for (int k = 0; k < i; k++)
        y += menu.items[k].action.empty() ? MENU_SEPARATOR_H : MENU_ITEM_H;
    return Rect{menu.x + 4, y, MENU_W - 8, menu.items[i].action.empty() ? MENU_SEPARATOR_H : MENU_ITEM_H};
}
Rect menu_rect() {
    int h = 8;
    for (const MenuItem &it : menu.items)
        h += it.action.empty() ? MENU_SEPARATOR_H : MENU_ITEM_H;
    return Rect{menu.x, menu.y, MENU_W, h};
}

// A window's edge under the pointer: 1 left, 2 right, 4 top, 8 bottom (0: none). The grip reaches
// a little outside the window, as on Windows.
constexpr int GRIP_OUT = 8, GRIP_IN = 5;
int edge_at(const Window &w, float px, float py) {
    if (w.maximized || w.minimized)
        return 0;
    if (px < w.x - GRIP_OUT || py < w.y - GRIP_OUT || px >= w.x + w.w + GRIP_OUT || py >= w.y + w.h + GRIP_OUT)
        return 0;
    int edges = 0;
    if (px < w.x + GRIP_IN) edges |= 1;
    if (px >= w.x + w.w - GRIP_IN) edges |= 2;
    if (py < w.y + GRIP_IN) edges |= 4;
    if (py >= w.y + w.h - GRIP_IN) edges |= 8;
    return edges;
}

// --- hit testing and clicks -------------------------------------------------------------------

std::string taskmgr_hit(const Window &w, float px, float py);  // (below, with Task Manager)

std::string hit(float px, float py) {
    if (dialog.open) {
        for (int i = 0; i < dialog.buttons(); i++)
            if (dialog_button(i).contains(px, py))
                return "dialog:" + std::to_string(i);
        return "dialog";
    }
    if (menu.open) {
        if (menu_rect().contains(px, py)) {
            for (int i = 0; i < static_cast<int>(menu.items.size()); i++)
                if (!menu.items[i].action.empty() && menu_item_rect(i).contains(px, py))
                    return "menu:" + std::to_string(i);
            return "menu";
        }
        return "outside-menu";
    }
    if (popup == Popup::Power)
        for (int i = 0; i < 2; i++)
            if (power_item(i).contains(px, py))
                return "power:" + std::to_string(i);
    if (popup != Popup::None) {
        const Rect m = start_menu_rect();
        if (m.contains(px, py)) {
            for (int i = 0; i < static_cast<int>(std::size(PINS)); i++)
                if (pin_rect(i).contains(px, py))
                    return "pin:" + std::to_string(i);
            const auto recent = recent_programs();
            for (int i = 0; i < static_cast<int>(recent.size()); i++)
                if (recent_rect(i).contains(px, py))
                    return "recent:" + std::to_string(i);
            if (power_button().contains(px, py))
                return "powerbutton";
            return "start";
        }
    }
    if (py >= H - TASKBAR_H) {
        const auto editors = editor_windows();
        const int first_program = TB_FIRST_PROGRAM + static_cast<int>(editors.size());
        for (int i = 0; i < taskbar_count(); i++)
            if (taskbar_button(i).contains(px, py))
                return i < TB_FIRST_PROGRAM ? "taskbar:" + std::to_string(i)
                     : i < first_program    ? "edtask:" + std::to_string(editors[i - TB_FIRST_PROGRAM])
                                            : "wtask:" + std::to_string(program_tasks[i - first_program].hwnd);
        return "taskbar";
    }
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
        const Window &w = *it;
        const std::string id = "win:" + std::to_string(w.id) + ":";
        if (const int edges = edge_at(w, px, py))
            return id + "edge" + std::to_string(edges);
        if (w.minimized || !Rect{w.x, w.y, w.w, w.h}.contains(px, py))
            continue;
        for (int i = 0; i < 3; i++)
            if (caption_button(w, i).contains(px, py))
                return id + "cap" + std::to_string(i);
        if (py < w.y + TITLE_H)
            return id + "title";
        if (w.kind == Kind::Files) {
            const std::string part = files_hit(w, px, py);
            if (!part.empty())
                return id + part;
        }
        if (w.kind == Kind::Notepad || w.kind == Kind::HexEditor) {
            const std::string part = editor_hit(w, px, py);
            if (!part.empty())
                return id + part;
        }
        if (w.kind == Kind::Settings) {
            const std::string part = settings_hit(w, px, py);
            if (!part.empty())
                return id + part;
        }
        if (w.kind == Kind::TaskManager) {
            const std::string part = taskmgr_hit(w, px, py);
            if (!part.empty())
                return id + part;
        }
        return id + "body";
    }
    for (int i = 0; i < static_cast<int>(std::size(DESKTOP_ICONS)); i++)
        if (desktop_icon_rect(i).contains(px, py))
            return "icon:" + std::to_string(i);
    for (int i = 0; i < static_cast<int>(desktop_files().size()); i++)
        if (desktop_file_rect(i).contains(px, py))
            return "dfile:" + std::to_string(i);
    return "desktop";
}


// A background chosen: shown (a photo may take a moment to read) and kept for the next start
void use_background(const Background &b) {
    std::string why;
    if (!set_background(b, &why)) {
        show_message("Background", "That picture could not be used: " + why + ".");
        return;
    }
    save_desktop();
    toast = Toast{"Background changed", now_seconds() + 2};
}

void settings_click(Window &w, const std::string &part) {
    if (part.rfind("page", 0) == 0) {
        w.page = number_after(part, "page");
    } else if (part.rfind("theme", 0) == 0) {
        apply_theme(number_after(part, "theme") == 1);
        save_desktop();
    } else if (part.rfind("bgtab", 0) == 0) {
        w.bg_tab = number_after(part, "bgtab");
    } else if (part.rfind("tile", 0) == 0) {
        const auto tiles = settings_tiles();
        const int i = number_after(part, "tile");
        if (i >= static_cast<int>(tiles.size()))
            return;
        const std::string &key = tiles[i].key;
        Background b;
        if (key == "browse") {
            open_window(Kind::Files, "");
            toast = Toast{"Double-click a picture, or right-click it: Set as desktop background", now_seconds() + 6};
            return;
        }
        if (key.rfind("style:", 0) == 0)
            b.kind = BgKind::Style, b.style = std::atoi(key.c_str() + 6);
        else
            b.kind = BgKind::Photo, b.photo = key.substr(6);
        use_background(b);
    } else if (part.rfind("swatch", 0) == 0) {
        Background b;
        b.kind = BgKind::Color;
        b.color = SWATCHES[number_after(part, "swatch")];
        use_background(b);
    } else if (part == "customcolor") {
        char hex[16];
        std::snprintf(hex, sizeof hex, "%06X", background.color & 0xffffff);
        text_request = TextRequest{TextFor::Color, std::string(), w.id};
        if (!keyboard_open("Background color (RRGGBB)", background.kind == BgKind::Color ? hex : ""))
            text_request = TextRequest{};
    } else if (part.rfind("toggle", 0) == 0) {
        toggle_flip(TOGGLES[number_after(part, "toggle")]);
        toast = Toast{"Applies the next time a program starts", now_seconds() + 3};
    } else if (part.rfind("speed", 0) == 0) {
        pointer_speed = std::clamp(pointer_speed + (number_after(part, "speed") ? 1 : -1), 1, 20);
        input_set_pointer_speed(pointer_speed);
        save_desktop();
    } else if (part.rfind("kbmode", 0) == 0) {
        direct_keyboard = number_after(part, "kbmode") == 1;
        if (direct_keyboard)
            hwkeyboard_open();
        save_desktop();
    } else if (part == "autokb") {
        auto_keyboard = !auto_keyboard;
        save_desktop();
    } else if (part == "chromium" || part == "rules" || part == "outside") {
        const char *key = part == "chromium" ? "WINEPS4_CHROMIUM_ONE_PROCESS"
                          : part == "rules"  ? "WINEPS4_PROCESS_RULES"
                                             : "WINEPS4_OUTSIDE";
        env_set(key, env_get(key) != "0" ? "0" : "");
        toast = Toast{"Applies the next time WinePS4 starts", now_seconds() + 3};
    } else if (part == "dxvk") {
        env_set("WINEPS4_DXVK", env_get("WINEPS4_DXVK") != "0" ? "0" : "");
        toast = Toast{"Applies the next time a program starts", now_seconds() + 3};
    } else if (part.rfind("uninstall", 0) == 0) {
        const auto &apps = installed_apps();
        const int i = number_after(part, "uninstall");
        if (i < 0 || i >= static_cast<int>(apps.size()))
            return;
        std::vector<std::string> args = split_command_line(apps[i].uninstall);
        const std::string error = args.empty() ? "It has no uninstaller." : launch(args);
        if (!error.empty())
            show_message("Uninstall " + apps[i].name, error);
        else
            toast = Toast{"Starting " + apps[i].name + "'s uninstaller", now_seconds() + 4};
    }
}

// --- Task Manager: the processes (Windows programs as the PS4 runs them), and how busy it is ----

constexpr int TM_NAV_W = 210, TM_ROW_H = 38;
const char *const TM_TABS[] = {"Processes", "Performance"};
double desktop_started = 0;  // (Performance's up time)

Rect tm_tab_rect(const Window &w, int i) { return Rect{w.x + 10, w.y + TITLE_H + 20 + i * 46, TM_NAV_W - 20, 42}; }
Rect tm_end_button(const Window &w) { return Rect{w.x + w.w - 24 - 130, w.y + TITLE_H + 18, 130, 38}; }
int tm_rows_top(const Window &w) { return w.y + TITLE_H + 112; }

// Processes' rows, top to bottom: section titles (pid 0) and processes; the desktop's own last
struct TmRow {
    std::string section;      // a section's title row ("" for a process)
    const ProcessView *view = nullptr;
    bool desktop = false;     // WinePS4's own process (the desktop and wineserver)
};
std::vector<TmRow> tm_rows() {
    std::vector<TmRow> rows;
    for (const bool own : {false, true}) {
        int n = 0;
        for (const ProcessView &v : process_views)
            n += v.windows_own == own;
        if (!n)
            continue;
        rows.push_back({std::string(own ? "Windows processes" : "Apps") + " (" + std::to_string(n) + ")"});
        for (const ProcessView &v : process_views)
            if (v.windows_own == own)
                rows.push_back({std::string(), &v});
    }
    rows.push_back({"WinePS4 (1)"});
    rows.push_back({std::string(), nullptr, true});
    return rows;
}

std::string taskmgr_hit(const Window &w, float px, float py) {
    for (int i = 0; i < 2; i++)
        if (tm_tab_rect(w, i).contains(px, py))
            return "tmtab" + std::to_string(i);
    if (w.tab != 0)
        return {};
    if (tm_end_button(w).contains(px, py))
        return "tmend";
    const auto rows = tm_rows();
    const int row = static_cast<int>((py - tm_rows_top(w)) / TM_ROW_H);
    if (px > w.x + TM_NAV_W && py >= tm_rows_top(w) && row >= 0 && row < static_cast<int>(rows.size()) && rows[row].view)
        return "tmrow" + std::to_string(rows[row].view->pid);
    return {};
}

void taskmgr_click(Window &w, const std::string &part) {
    if (part.rfind("tmtab", 0) == 0) {
        w.tab = number_after(part, "tmtab");
    } else if (part.rfind("tmrow", 0) == 0) {
        w.chosen_pid = number_after(part, "tmrow");
    } else if (part == "tmend") {
        for (const ProcessView &v : process_views)
            if (v.pid == w.chosen_pid) {
                ps4_local_kill(v.pid);
                toast = Toast{"Ended " + v.name, now_seconds() + 3};
                w.chosen_pid = 0;
                refresh_processes(true);
                break;
            }
    }
}

void click(const std::string &key, bool twice, float px, float py) {
    if (key.rfind("dialog:", 0) == 0) {
        const int i = number_after(key, "dialog:");
        const Dialog d = dialog;
        dialog.open = false;
        if (i == 0 && d.kind == DialogKind::Delete && !job_delete(d.paths))
            show_message("Delete", "Wait for the copy that is running to finish.");
        else if (i == 0 && d.kind == DialogKind::EmptyBin)
            empty_bin();
        else if (d.kind == DialogKind::Unsaved && i < 2) {  // Save, Don't save (Cancel keeps it open)
            if (Window *ed = find_window(d.window)) {
                if (i == 0 && ed->text) {
                    std::string error;
                    if (!save_text(ed->file, *ed->text, error)) {
                        show_message("Notepad", "Could not save " + base_name(ed->file) + ": " + error);
                        return;
                    }
                } else if (i == 0 && ed->hex) {
                    hex_save(*ed);
                    if (!ed->hex->edits.empty())
                        return;
                }
                close_window(d.window);
            }
        }
        else if (i == 0 && d.kind == DialogKind::Wallpaper) {
            Background b;
            b.kind = BgKind::Photo;
            b.photo = d.program;
            use_background(b);
        }
        return;
    }
    if (key == "dialog")
        return;
    if (key.rfind("menu:", 0) == 0) {
        const int i = number_after(key, "menu:");
        if (i < static_cast<int>(menu.items.size()) && menu.items[i].enabled)
            menu_action(menu.items[i].action);
        return;
    }
    if (key == "menu")
        return;
    if (key == "outside-menu") {
        menu.open = false;  // (a click away from a menu only closes it, as on Windows)
        return;
    }
    if (key.rfind("power:", 0) == 0) {
        const int i = number_after(key, "power:");
        popup = Popup::None;
        if (i == 0) {
            ps4_end_programs();
            const int r = sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
            char text[120];
            std::snprintf(text, sizeof text, "WinePS4 could not restart itself (%08x).", static_cast<unsigned>(r));
            show_message("Restart WinePS4", text);
        } else {
            ps4_boot_trace("shell: exit to the PS4 menu");
            ps4_end_programs();
            exit(0);
        }
        return;
    }
    if (key == "powerbutton") {
        popup = popup == Popup::Power ? Popup::Start : Popup::Power;
        return;
    }
    if (key.rfind("pin:", 0) == 0) {
        start_action(PINS[number_after(key, "pin:")].action);
        return;
    }
    if (key.rfind("recent:", 0) == 0) {
        const auto recent = recent_programs();
        const int i = number_after(key, "recent:");
        popup = Popup::None;
        if (i < static_cast<int>(recent.size()))
            run_program(recent[i]);
        return;
    }
    if (key == "start") {
        if (popup == Popup::Power)
            popup = Popup::Start;
        return;
    }
    // anything else closes the Start menu
    const bool was_open = popup != Popup::None;
    popup = Popup::None;
    if (key.rfind("taskbar:", 0) == 0) {
        const int i = number_after(key, "taskbar:");
        if (i == 0) {
            popup = was_open ? Popup::None : Popup::Start;
            return;
        }
        if (i == 1) {  // search: the Start menu for now
            popup = was_open ? Popup::None : Popup::Start;
            return;
        }
        const Kind kind = TASKBAR_APPS[i - 2];
        Window *top = focused();
        Window *mine = nullptr;
        for (auto it = windows.rbegin(); it != windows.rend(); ++it)
            if (it->kind == kind) {
                mine = &*it;
                break;
            }
        if (!mine)
            open_window(kind, kind == Kind::Files ? "" : std::string());
        else if (top == mine)
            mine->minimized = true;
        else
            bring_to_front(mine->id);
        return;
    }
    if (key.rfind("edtask:", 0) == 0) {  // Notepad's or the hex editor's window
        const int id = number_after(key, "edtask:");
        Window *w = find_window(id);
        if (w && w == focused() && desktop_on_top && !w->minimized)
            w->minimized = true;
        else if (w)
            bring_to_front(id);
        return;
    }
    if (key.rfind("wtask:", 0) == 0) {  // a Windows program's window: to the front, or minimized
        const uint64_t hwnd = std::strtoull(key.c_str() + 6, nullptr, 10);
        for (const WinTask &t : program_tasks)
            if (t.hwnd == hwnd) {
                const bool minimize = t.active && !t.minimized && !desktop_on_top;
                winwin_command(hwnd, minimize ? 0xf020 /* SC_MINIMIZE */ : PS4_SC_ACTIVATE);
                desktop_on_top = false;
            }
        return;
    }
    if (key.rfind("icon:", 0) == 0) {
        if (twice)
            start_action(DESKTOP_ICONS[number_after(key, "icon:")].action), selected_icon.clear();
        else
            selected_icon = key;
        return;
    }
    if (key.rfind("dfile:", 0) == 0) {
        const int i = number_after(key, "dfile:");
        if (i >= static_cast<int>(desktop_files().size()))
            return;
        const Entry e = desktop_files()[i];
        if (twice) {
            selected_icon.clear();
            if (e.dir)
                open_window(Kind::Files, e.path);
            else
                open_file(e);
        } else {
            selected_icon = key;
            dragged = DraggedFiles{true, false, px, py, {e.path}, e.name, e.icon};
        }
        return;
    }
    selected_icon.clear();
    if (key.rfind("win:", 0) != 0)
        return;
    const int id = number_after(key, "win:");
    const std::string part = key.substr(key.find(':', 4) + 1);
    bring_to_front(id);
    Window *w = find_window(id);
    if (!w)
        return;
    if (part.rfind("edge", 0) == 0) {
        resizing = id;
        resize_edges = number_after(part, "edge");
        resize_px = px, resize_py = py;
        resize_x = w->x, resize_y = w->y, resize_w = w->w, resize_h = w->h;
    } else if (part == "cap0") {
        request_close(id);
    } else if (part == "cap1") {
        toggle_maximize(*w);
    } else if (part == "cap2") {
        w->minimized = true;
    } else if (part == "title") {
        if (twice) {
            toggle_maximize(*w);
        } else if (!w->maximized) {
            dragging = id;
            drag_dx = px - w->x;
            drag_dy = py - w->y;
        }
    } else if (w->kind == Kind::Files) {
        files_click(*w, part, twice, px, py);
    } else if (w->kind == Kind::Notepad || w->kind == Kind::HexEditor) {
        editor_click(*w, part, twice, px, py);
    } else if (w->kind == Kind::TaskManager) {
        taskmgr_click(*w, part);
    } else if (w->kind == Kind::Settings) {
        settings_click(*w, part);
    }
}

// --- drawing ---------------------------------------------------------------------------------

void draw_desktop_icons(Canvas &c) {
    for (int i = 0; i < static_cast<int>(std::size(DESKTOP_ICONS)); i++) {
        const Rect r = desktop_icon_rect(i);
        const std::string key = "icon:" + std::to_string(i);
        if (selected_icon == key)
            c.fill_round_rect(r.x, r.y, r.w, r.h, 6, rgb(0x6fb8ff, 0x40));
        else if (hover == key)
            c.fill_round_rect(r.x, r.y, r.w, r.h, 6, rgb(0xffffff, 0x20));
        draw_icon(c, DESKTOP_ICONS[i].icon, r.x + r.w / 2.f, r.y + 38, 54);
        // white on any wallpaper, with a soft shadow
        c.text_centered(r.x + r.w / 2 + 1, r.y + 95, DESKTOP_ICONS[i].label, 16, rgb(0x000000, 0x90));
        c.text_centered(r.x + r.w / 2, r.y + 94, DESKTOP_ICONS[i].label, 16, rgb(0xffffff));
    }
    const auto &files = desktop_files();
    for (int i = 0; i < static_cast<int>(files.size()); i++) {
        const Rect r = desktop_file_rect(i);
        if (r.x > W - 120)
            break;
        const Entry &e = files[i];
        const std::string key = "dfile:" + std::to_string(i);
        if (selected_icon == key)
            c.fill_round_rect(r.x, r.y, r.w, r.h, 6, rgb(0x6fb8ff, 0x40));
        else if (hover == key)
            c.fill_round_rect(r.x, r.y, r.w, r.h, 6, rgb(0xffffff, 0x20));
        // a shortcut: its program's icon, with Windows' little arrow
        const bool shortcut = ends_with(lower(e.name), ".lnk");
        const std::string program = shortcut ? lnk_target(e.path) : !e.dir && has_program_icon(lower(e.name)) ? e.path : std::string();
        if (const Image *icon = program.empty() ? nullptr : program_icon(program, 48))
            c.blit(*icon, r.x + r.w / 2 - 24, r.y + 14);
        else
            draw_icon(c, e.icon, r.x + r.w / 2.f, r.y + 38, 54);
        if (shortcut) {
            const float ax = r.x + r.w / 2 - 24.f, ay = r.y + 46.f;
            c.fill_round_rect(static_cast<int>(ax), static_cast<int>(ay), 16, 16, 3, rgb(0xffffff));
            c.line(ax + 4, ay + 12, ax + 12, ay + 4, 2.f, rgb(0x2060c0));
            c.line(ax + 7, ay + 4, ax + 12, ay + 4, 2.f, rgb(0x2060c0));
            c.line(ax + 12, ay + 4, ax + 12, ay + 9, 2.f, rgb(0x2060c0));
        }
        std::string label = shortcut ? e.name.substr(0, e.name.size() - 4) : e.name;
        while (label.size() > 4 && c.text_width(label, 14) > r.w - 4)
            label = label.substr(0, prev_char(label, label.size() - 3)) + "...";
        c.text_centered(r.x + r.w / 2 + 1, r.y + 95, label, 14, rgb(0x000000, 0x90));
        c.text_centered(r.x + r.w / 2, r.y + 94, label, 14, rgb(0xffffff));
    }
}

// --- Windows programs' windows: the desktop's title bar and border over Windows' own ----------
// (Windows 11's look: one colour from the title bar down the edges, rounded corners, its buttons)

int hovered_wbutton = -1, pressed_wbutton = -1;  // a title bar button under the pointer, pressed
uint64_t hovered_wbutton_hwnd = 0, pressed_wbutton_hwnd = 0;

void outline_square(Canvas &c, float x, float y, float s, Color col) {
    c.line(x, y, x + s, y, 1.2f, col);
    c.line(x + s, y, x + s, y + s, 1.2f, col);
    c.line(x + s, y + s, x, y + s, 1.2f, col);
    c.line(x, y + s, x, y, 1.2f, col);
}

void draw_program_under(Canvas &c, const WinFrame &f) {
    if (!f.caption_bottom || f.maximized)
        return;
    c.blit(shadow_for(f.w, f.h, f.active), f.x - SHADOW, f.y - SHADOW);
    c.fill_round_rect(f.x - 1, f.y - 1, f.w + 2, f.h + 2, RADIUS + 1, f.active ? BORDER_ACTIVE : BORDER);
}

void draw_program_over(Canvas &c, const WinFrame &f) {
    const int top = std::max(f.y, 0), cap_h = f.caption_bottom - top;
    if (!f.caption_bottom || cap_h <= 0)
        return;
    // the title bar, then Windows' frame around the client area in its colour
    if (f.maximized || cap_h <= RADIUS) {
        c.fill_rect(f.x, top, f.w, cap_h, WINDOW_BG);
    } else {
        c.fill_round_rect(f.x, top, f.w, cap_h, RADIUS, WINDOW_BG);
        c.fill_rect(f.x, top + RADIUS, f.w, cap_h - RADIUS, WINDOW_BG);
    }
    if (f.border > 0) {
        const int bottom = f.y + f.h, cut = f.maximized ? 0 : RADIUS;
        c.fill_rect(f.x, f.caption_bottom, f.border, bottom - f.caption_bottom - cut, WINDOW_BG);
        c.fill_rect(f.x + f.w - f.border, f.caption_bottom, f.border, bottom - f.caption_bottom - cut, WINDOW_BG);
        c.fill_rect(f.x + cut, bottom - f.border, f.w - 2 * cut, f.border, WINDOW_BG);
    }
    // its icon and title
    const float cy = top + cap_h / 2.f;
    const int buttons = (f.can_minimize || f.can_maximize) ? 3 : 1;
    if (const Image *icon = process_icon(f.pid, 16))
        c.blit(*icon, f.x + 14, static_cast<int>(cy) - 8);
    else
        draw_icon(c, Icon::Program, f.x + 22, cy, 16);
    c.text(f.x + 40, static_cast<int>(cy) + 5, f.title, 13, f.active ? TEXT : TEXT3, Weight::Regular,
           std::max(1, f.w - 48 - buttons * WIN_BUTTON_W));
    // minimize, maximize or restore, close
    for (int i = 0; i < buttons; i++) {
        const WinRect b = winwin_button_rect(f, i);
        const bool enabled = i == 0 || (i == 1 ? f.can_maximize : f.can_minimize);
        const bool hot = enabled && hovered_wbutton == i && hovered_wbutton_hwnd == f.hwnd;
        const bool down = hot && pressed_wbutton == i && pressed_wbutton_hwnd == f.hwnd;
        if (hot) {
            const Color bg = i == 0 ? (down ? rgb(0x94261b) : CLOSE_RED) : (down ? PRESSED : HOVER);
            if (i == 0 && !f.maximized && b.h > RADIUS) {  // (in the window's rounded corner)
                c.fill_round_rect(b.x, b.y, b.w, b.h, RADIUS, bg);
                c.fill_rect(b.x, b.y, b.w - RADIUS, b.h, bg);
                c.fill_rect(b.x, b.y + RADIUS, b.w, b.h - RADIUS, bg);
            } else {
                c.fill_rect(b.x, b.y, b.w, b.h, bg);
            }
        }
        const float bx = b.x + b.w / 2.f, by = b.y + b.h / 2.f;
        const Color col = !enabled ? DISABLED : (hot && i == 0) ? rgb(0xffffff) : f.active ? TEXT : TEXT3;
        if (i == 0) {
            c.line(bx - 5, by - 5, bx + 5, by + 5, 1.3f, col);
            c.line(bx - 5, by + 5, bx + 5, by - 5, 1.3f, col);
        } else if (i == 1 && f.maximized) {  // restore: a square with another behind it
            outline_square(c, bx - 5, by - 3, 8, col);
            c.line(bx - 3, by - 5, bx + 5, by - 5, 1.2f, col);
            c.line(bx + 5, by - 5, bx + 5, by + 3, 1.2f, col);
        } else if (i == 1) {
            outline_square(c, bx - 5, by - 5, 10, col);
        } else {
            c.line(bx - 5, by, bx + 5, by, 1.3f, col);
        }
    }
}

// a title bar button let go over: what Windows' own would do (the program may say no to Close)
void program_button(uint64_t hwnd, int button, bool maximized) {
    constexpr uint32_t SC_MINIMIZE = 0xf020, SC_MAXIMIZE = 0xf030, SC_CLOSE = 0xf060, SC_RESTORE = 0xf120;
    winwin_command(hwnd, button == 0 ? SC_CLOSE : button == 2 ? SC_MINIMIZE : maximized ? SC_RESTORE : SC_MAXIMIZE);
}

void draw_caption(Canvas &c, const Window &w, bool active) {
    draw_icon(c, window_icon(w), w.x + 26, w.y + TITLE_H / 2.f, 20);
    c.text(w.x + 46, w.y + 26, w.title, 15, active ? TEXT : TEXT3, Weight::Regular, w.w - 46 - 46 * 3 - 10);
    const std::string id = "win:" + std::to_string(w.id) + ":cap";
    for (int i = 0; i < 3; i++) {
        const Rect b = caption_button(w, i);
        if (hover == id + std::to_string(i)) {
            if (i == 0)
                c.fill_round_rect(b.x, b.y + 1, b.w - 1, b.h - 1, 6, CLOSE_RED);
            else
                c.fill_rect(b.x, b.y + 1, b.w, b.h - 1, HOVER);
        }
        const float cx = b.x + b.w / 2.f, cy = b.y + b.h / 2.f;
        const Color col = active ? TEXT : TEXT3;
        if (i == 0) {
            c.line(cx - 6, cy - 6, cx + 6, cy + 6, 1.4f, col);
            c.line(cx - 6, cy + 6, cx + 6, cy - 6, 1.4f, col);
        } else if (i == 1) {
            c.fill_round_rect(static_cast<int>(cx) - 6, static_cast<int>(cy) - 6, 12, 12, 2, col);
            c.fill_round_rect(static_cast<int>(cx) - 5, static_cast<int>(cy) - 5, 10, 10, 1, WINDOW_BG);
        } else {
            c.line(cx - 6, cy, cx + 6, cy, 1.4f, col);
        }
    }
}

// a disabled button: its colours faded into the bar's
void fade(Canvas &c, const Rect &r, Color bg) { c.fill_rect(r.x, r.y, r.w, r.h, Color{bg.r, bg.g, bg.b, 0xa8}); }

void draw_files(Canvas &c, Window &w) {
    const std::string id = "win:" + std::to_string(w.id) + ":";
    const bool place = w.path.empty(), bin = w.path == RECYCLE_DIR;
    const std::string drop = dragged.active ? drop_folder(hover) : std::string();
    const bool drop_ok = dragged.active && drop_effect(drop) != 0;
    const int n = static_cast<int>(w.entries.size());

    // the command bar
    c.fill_rect(w.x + 1, w.y + TITLE_H, w.w - 2, FILES_CMD_H, WINDOW_BG);
    int count = 0;
    const CmdButton *cmds = files_cmds(w, count);
    const bool any = !w.chosen.empty();
    for (int i = 0; i < count; i++) {
        const Rect r = files_cmd_rect(w, i);
        if (!*cmds[i].key) {
            c.fill_rect(r.x + r.w / 2 + 2, r.y + 6, 1, r.h - 12, BORDER);
            continue;
        }
        const std::string key = cmds[i].key;
        const bool enabled = key == "new" ? !place && !bin
                           : key == "cut" || key == "copy" || key == "delete" || key == "delete-now" || key == "restore" ? any && !place
                           : key == "rename" ? w.chosen.size() == 1 && !place
                           : key == "paste" ? !clipboard.paths.empty() && !place && !bin
                           : key == "sort" ? !place
                           : key == "empty-bin" ? n > 0
                           : true;
        if (enabled && hover == id + "cmd:" + key)
            c.fill_round_rect(r.x, r.y, r.w, r.h, 5, HOVER);
        draw_icon(c, cmds[i].icon, r.x + (cmds[i].label ? 20.f : r.w / 2.f), r.y + r.h / 2.f, 18);
        if (cmds[i].label)
            c.text(r.x + 38, r.y + r.h / 2 + 5, cmds[i].label, 14, TEXT);
        if (!enabled)
            fade(c, r, WINDOW_BG);
    }

    // the address row: Back, Forward, Up, Refresh, the address, the search box
    const int ay = w.y + TITLE_H + FILES_CMD_H;
    c.fill_rect(w.x + 1, ay, w.w - 2, FILES_ADDR_H, WINDOW_BG);
    c.fill_rect(w.x + 1, ay + FILES_ADDR_H - 1, w.w - 2, 1, BORDER);
    const Icon nav_icons[] = {Icon::Back, Icon::Forward, Icon::Up, Icon::Refresh};
    const bool nav_on[] = {!w.back.empty(), !w.forward.empty(), !place, true};
    static const char *const nav_keys[] = {"back", "forward", "up", "refresh"};
    for (int i = 0; i < 4; i++) {
        const Rect r = files_nav_button(w, i);
        if (nav_on[i] && hover == id + nav_keys[i])
            c.fill_round_rect(r.x, r.y, r.w, r.h, 5, HOVER);
        draw_icon(c, nav_icons[i], r.x + r.w / 2.f, r.y + r.h / 2.f, 18);
        if (!nav_on[i])
            fade(c, r, WINDOW_BG);
    }
    const Rect a = files_address(w);
    panel(c, a.x, a.y, a.w, a.h, 5, CONTROL, w.address.active ? ACCENT : BORDER);
    std::vector<Rect> &crumb_rects = drawn_crumbs[w.id];
    crumb_rects.clear();
    if (w.address.active) {
        draw_edit(c, a, w.address, 15);
    } else {
        // the folder's icon, then its path's parts (the first ones left out when they don't fit)
        draw_icon(c, place ? Icon::Computer : bin ? Icon::Trash : Icon::Folder, a.x + 20, a.y + a.h / 2.f, 18);
        const auto list = crumbs(w.path);
        std::vector<int> widths;
        int total = 0;
        for (const Crumb &cr : list) {
            widths.push_back(c.text_width(cr.label, 14) + 16);
            total += widths.back() + 18;
        }
        int first = 0;
        while (first < static_cast<int>(list.size()) - 1 && total > a.w - 50)
            total -= widths[first] + 18, first++;
        crumb_rects.assign(list.size(), Rect{});
        int x = a.x + 36;
        for (int i = first; i < static_cast<int>(list.size()); i++) {
            const Rect r{x, a.y + 4, std::min(widths[i], a.x + a.w - 4 - x), a.h - 8};
            crumb_rects[i] = r;
            const std::string key = id + "crumb" + std::to_string(i);
            if (hover == key)
                c.fill_round_rect(r.x, r.y, r.w, r.h, 4, drop_ok ? SELECTED : HOVER);
            c.text(r.x + 8, a.y + a.h / 2 + 5, list[i].label, 14, TEXT, Weight::Regular, std::max(1, r.w - 8));
            x += r.w;
            if (i + 1 < static_cast<int>(list.size())) {
                draw_icon(c, Icon::Chevron, x + 9, a.y + a.h / 2.f, 12);
                x += 18;
            }
        }
    }
    const Rect sr = files_search(w);
    panel(c, sr.x, sr.y, sr.w, sr.h, 5, CONTROL, w.search.active ? ACCENT : BORDER);
    draw_icon(c, Icon::Search, sr.x + 18, sr.y + sr.h / 2.f, 14);
    if (w.search.active)
        draw_edit(c, Rect{sr.x + 26, sr.y, sr.w - 26, sr.h}, w.search, 14);
    else
        c.text(sr.x + 36, sr.y + sr.h / 2 + 5, w.filter.empty() ? "Search " + place_title(w.path) : w.filter, 14,
               w.filter.empty() ? TEXT3 : TEXT, Weight::Regular, sr.w - 46);

    // the navigation pane: the place holding the folder marked (the longest path that matches)
    const int top = files_top(w), bottom = w.y + w.h - FILES_STATUS_H;
    c.fill_rect(w.x + 1, top, FILES_NAV_W - 1, bottom - top, WINDOW_BG);
    c.fill_rect(w.x + FILES_NAV_W - 1, top, 1, bottom - top, BORDER);
    const auto nav = places();
    int current = -1;
    size_t best = 0;
    for (int i = 0; i < static_cast<int>(nav.size()); i++) {
        const std::string &p = nav[i].path;
        if (p.empty()) {
            if (place)
                current = i;
            continue;
        }
        if (w.path.rfind(p, 0) == 0 && (w.path.size() == p.size() || w.path[p.size()] == '/' || p == "/") && p.size() > best)
            current = i, best = p.size();
    }
    const int quick_n = static_cast<int>(quick().size());
    for (int i = 0; i < static_cast<int>(nav.size()); i++) {
        const Rect r = files_nav_item(w, i);
        if (r.y + r.h > bottom - 4)
            break;
        const std::string key = id + "nav" + std::to_string(i);
        if (i == current || (hover == key && drop_ok))
            c.fill_round_rect(r.x, r.y, r.w, r.h, 5, SELECTED);
        else if (hover == key)
            c.fill_round_rect(r.x, r.y, r.w, r.h, 5, HOVER);
        if (i == current)
            c.fill_round_rect(r.x, r.y + 8, 3, r.h - 16, 2, ACCENT);
        draw_icon(c, nav[i].icon, r.x + 24, r.y + r.h / 2.f, 18);
        c.text(r.x + 44, r.y + r.h / 2 + 5, nav[i].name, 14, TEXT, Weight::Regular, r.w - 52);
        if (i == quick_n || i == quick_n + 1)  // (between Quick access, the Recycle Bin and the drives)
            c.fill_rect(r.x + 10, r.y + r.h + NAV_GAP / 2, r.w - 20, 1, BORDER);
    }

    // the list's header: its columns, the one it is sorted by with an arrow
    const Rect hd = files_header(w);
    c.fill_rect(hd.x, hd.y, hd.w, hd.h, CONTENT_BG);
    const char *const titles[] = {"Name", bin ? "Original location" : place ? "Type" : "Date modified", place ? "Free space" : bin ? "Date modified" : "Type",
                                  place ? "" : "Size"};
    for (int i = 0; i < 4; i++) {
        const int x = files_column_x(w, i), next = i < 3 ? files_column_x(w, i + 1) : hd.x + hd.w;
        if (!place && hover == id + "col" + std::to_string(i))
            c.fill_rect(x - 10, hd.y + 2, next - x, hd.h - 4, HOVER);
        if (i > 0)
            c.fill_rect(x - 10, hd.y + 8, 1, hd.h - 16, BORDER);
        c.text(x, hd.y + 21, titles[i], 13, TEXT2);
        if (!place && !bin && w.sort_by == i) {  // a small arrow, up for A to Z
            const float tx = x + c.text_width(titles[i], 13) + 12, ty = hd.y + hd.h / 2.f, d = w.sort_down ? 3.f : -3.f;
            c.line(tx - 4, ty - d, tx, ty + d, 1.4f, TEXT3);
            c.line(tx, ty + d, tx + 4, ty - d, 1.4f, TEXT3);
        }
    }
    c.fill_rect(hd.x + 8, hd.y + hd.h - 1, hd.w - 16, 1, BORDER);

    // the rows
    const Rect list = files_list(w);
    c.fill_rect(list.x, list.y, list.w, list.h, CONTENT_BG);
    const int visible = files_rows(w);
    clamp_scroll(w, n, visible);
    const int first = static_cast<int>(w.scroll);
    if (!n) {
        const char *why = !w.filter.empty() ? "No items match your search."
                        : bin ? "The Recycle Bin is empty."
                        : place ? ""
                        : access(w.path.c_str(), R_OK | X_OK) != 0 ? "This folder can't be opened: the PS4 doesn't let the app read it."
                        : "This folder is empty.";
        c.text_centered(list.x + list.w / 2, list.y + 60, why, 15, TEXT3);
    }
    const bool checks = any;
    for (int i = first; i < n && i < first + visible; i++) {
        const Entry &e = w.entries[i];
        const int ry = list.y + (i - first) * ROW_H;
        const std::string row_key = id + "row" + std::to_string(i), check_key = id + "check" + std::to_string(i);
        const bool chosen = w.chosen.count(e.path) > 0, hot = hover == row_key || hover == check_key;
        const bool target = drop_ok && e.dir && drop == e.path && hot;
        if (chosen || target)
            c.fill_round_rect(list.x + 6, ry + 1, list.w - 14, ROW_H - 2, 5, SELECTED);
        else if (hot)
            c.fill_round_rect(list.x + 6, ry + 1, list.w - 14, ROW_H - 2, 5, HOVER);
        if (i == w.cursor && !chosen && hwkeyboard_connected()) {  // the keyboard's place, outlined
            c.fill_rect(list.x + 8, ry + 2, list.w - 18, 1, TEXT3);
            c.fill_rect(list.x + 8, ry + ROW_H - 3, list.w - 18, 1, TEXT3);
        }
        // the check box (Windows' item check boxes): on the row under the pointer, and on all once one is chosen
        if (chosen || hot || checks) {
            const int bx = list.x + CHECK_X, by = ry + ROW_H / 2 - 9;
            if (chosen) {
                c.fill_round_rect(bx, by, 18, 18, 4, ACCENT);
                draw_icon(c, Icon::Check, bx + 9.f, by + 9.f, 14);
            } else {
                c.fill_round_rect(bx, by, 18, 18, 4, TEXT3);
                c.fill_round_rect(bx + 1, by + 1, 16, 16, 3, CONTENT_BG);
            }
        }
        const Image *icon = !e.dir && has_program_icon(lower(e.name)) ? program_icon(e.path, 24) : nullptr;
        if (icon)
            c.blit(*icon, list.x + 46, ry + ROW_H / 2 - 12);
        else
            draw_icon(c, e.icon, list.x + 58, ry + ROW_H / 2.f, 22);
        // (a cut entry is shown faded until it is pasted)
        const bool cut = clipboard.cut && std::find(clipboard.paths.begin(), clipboard.paths.end(), e.path) != clipboard.paths.end();
        if (cut)
            c.fill_rect(list.x + 44, ry + 5, 28, ROW_H - 10, Color{CONTENT_BG.r, CONTENT_BG.g, CONTENT_BG.b, 0x90});
        const int nx = list.x + 78, col1 = files_column_x(w, 1), col2 = files_column_x(w, 2), col3 = files_column_x(w, 3);
        if (w.rename.active && w.rename_path == e.path) {
            const Rect box{nx - 6, ry + 3, col1 - nx - 4, ROW_H - 6};
            panel(c, box.x, box.y, box.w, box.h, 4, CONTROL, ACCENT);
            draw_edit(c, box, w.rename, 15);
        } else {
            c.text(nx, ry + 22, e.name, 15, cut ? TEXT3 : TEXT, Weight::Regular, col1 - nx - 14);
        }
        if (place) {
            c.text(col1, ry + 22, type_text(e), 14, TEXT2, Weight::Regular, col2 - col1 - 14);
            c.text(col2, ry + 22, e.note, 14, TEXT2, Weight::Regular, list.x + list.w - col2 - 16);
            continue;
        }
        c.text(col1, ry + 22, bin ? shown_path(folder_of(e.note)) : date_text(e.modified), 14, TEXT2, Weight::Regular, col2 - col1 - 14);
        c.text(col2, ry + 22, bin ? date_text(e.modified) : type_text(e), 14, TEXT2, Weight::Regular, col3 - col2 - 14);
        if (!e.dir) {  // (sizes to the right, as Windows')
            const std::string size = size_text(e.size);
            c.text(list.x + list.w - 22 - c.text_width(size, 14), ry + 22, size, 14, TEXT2);
        }
    }
    // a selection rectangle being drawn
    if (w.banding) {
        const Input &in = last_input;
        const float y0 = list.y + (w.band_y - w.scroll) * ROW_H;
        const int x0 = static_cast<int>(std::clamp(std::min(w.band_x, in.x), static_cast<float>(list.x), static_cast<float>(list.x + list.w)));
        const int x1 = static_cast<int>(std::clamp(std::max(w.band_x, in.x), static_cast<float>(list.x), static_cast<float>(list.x + list.w)));
        const int y_0 = static_cast<int>(std::clamp(std::min(y0, in.y), static_cast<float>(list.y), static_cast<float>(list.y + list.h)));
        const int y_1 = static_cast<int>(std::clamp(std::max(y0, in.y), static_cast<float>(list.y), static_cast<float>(list.y + list.h)));
        if (x1 - x0 > 3 && y_1 - y_0 > 3) {
            c.fill_rect(x0, y_0, x1 - x0, y_1 - y_0, Color{ACCENT.r, ACCENT.g, ACCENT.b, 0x30});
            c.fill_rect(x0, y_0, x1 - x0, 1, ACCENT);
            c.fill_rect(x0, y_1 - 1, x1 - x0, 1, ACCENT);
            c.fill_rect(x0, y_0, 1, y_1 - y_0, ACCENT);
            c.fill_rect(x1 - 1, y_0, 1, y_1 - y_0, ACCENT);
        }
    }
    // a drop into the folder itself: the list outlined
    if (drop_ok && drop == w.path && hover.rfind(id, 0) == 0) {
        c.fill_rect(list.x + 2, list.y, list.w - 4, 2, ACCENT);
        c.fill_rect(list.x + 2, list.y + list.h - 2, list.w - 4, 2, ACCENT);
        c.fill_rect(list.x + 2, list.y, 2, list.h, ACCENT);
        c.fill_rect(list.x + list.w - 4, list.y, 2, list.h, ACCENT);
    }
    // the scroll bar
    if (n > visible) {
        const float total = static_cast<float>(n);
        const int bh = std::max(30, static_cast<int>(list.h * visible / total));
        const int by = list.y + static_cast<int>((list.h - bh) * (w.scroll / (total - visible)));
        c.fill_round_rect(list.x + list.w - 8, by, 4, bh, 2, SCROLLBAR);
    }

    // the status bar: how many, what's chosen, the drive's room
    const int sy = w.y + w.h - FILES_STATUS_H;
    c.fill_rect(w.x + 1, sy, w.w - 2, FILES_STATUS_H - 1, WINDOW_BG);
    c.fill_rect(w.x + 1, sy, w.w - 2, 1, BORDER);
    std::string status = std::to_string(n) + (n == 1 ? " item" : " items");
    if (any) {
        long long bytes = 0;
        bool files_only = true;
        for (const Entry &e : w.entries)
            if (w.chosen.count(e.path)) {
                if (e.dir)
                    files_only = false;
                else
                    bytes += std::max(0ll, e.size);
            }
        status += "      " + std::to_string(w.chosen.size()) + (w.chosen.size() == 1 ? " item selected" : " items selected") +
                  (files_only && bytes ? "  " + size_text(bytes) : std::string());
    }
    c.text(w.x + 16, sy + 19, status, 13, TEXT2);
    if (!place) {
        const std::string room = space_note(w.path);
        c.text(w.x + w.w - 16 - c.text_width(room, 13), sy + 19, room, 13, TEXT2);
    }
}

void draw_terminal(Canvas &c, Window &w) {
    c.fill_rect(w.x + 1, w.y + TITLE_H, w.w - 2, w.h - TITLE_H - 1, rgb(0x0c0c0c));
    std::lock_guard lock(program_lock);
    // what runs, and how it went
    std::string head;
    Color head_col = rgb(0xc8c8c8);  // (the terminal is dark in both themes)
    if (!program_status.empty()) {
        head = "Windows: " + program_status;
        head_col = rgb(0x4cc2ff);
    } else if (!programs_running.empty()) {
        head = "Running:";
        for (const auto &[pid, name] : programs_running)
            head += " " + name + " (" + std::to_string(pid) + ")";
        head_col = rgb(0x4cc2ff);
    } else if (!last_ended.empty()) {
        head = last_ended;
        head_col = last_ended_code == 0 ? rgb(0x6ccb5f) : rgb(0xff99a4);
    } else {
        head = "No program is running. Open one from Files or the Start menu.";
    }
    c.text(w.x + 18, w.y + TITLE_H + 28, head, 15, head_col, Weight::Bold, w.w - 36);
    c.fill_rect(w.x + 12, w.y + TITLE_H + 42, w.w - 24, 1, rgb(0x333333));
    const int line_h = 22, top = w.y + TITLE_H + 52;
    const int visible = std::max(1, (w.h - TITLE_H - 60) / line_h);
    std::vector<std::string> lines(program_lines.begin(), program_lines.end());
    if (!program_partial.empty())
        lines.push_back(program_partial);
    // scroll: lines up from the bottom (0 follows the output)
    const int count = static_cast<int>(lines.size());
    w.scroll = std::clamp(w.scroll, 0.f, static_cast<float>(std::max(0, count - visible)));
    const int last = count - static_cast<int>(w.scroll);
    const int first = std::max(0, last - visible);
    for (int i = first; i < last; i++)
        c.text(w.x + 18, top + (i - first + 1) * line_h - 6, lines[i], 15, rgb(0xcccccc), Weight::Regular, w.w - 36);
}

// a two-or-three-way choice (Dark | Light), the chosen one in the accent colour
void draw_segments(Canvas &c, const Rect &card, const char *const *labels, int n, int width, int chosen, const std::string &key) {
    for (int i = 0; i < n; i++) {
        const Rect r = segment(card, i, n, width);
        const bool on = i == chosen, hot = hover == key + std::to_string(i);
        panel(c, r.x, r.y, r.w, r.h, 5, on ? (hot ? ACCENT_HOT : ACCENT) : (hot ? BUTTON_HOT : BUTTON), on ? ACCENT_BORDER : BUTTON_BORDER);
        c.text_centered(r.x + r.w / 2, r.y + 24, labels[i], 15, on ? ACCENT_TEXT : TEXT);
    }
}

void draw_switch(Canvas &c, const Rect &r, bool on) {
    const int sx = r.x + r.w - 84, sy = r.y + r.h / 2 - 12;
    if (on) {
        c.fill_round_rect(sx, sy, 48, 24, 12, ACCENT);
        c.fill_circle(sx + 36, sy + 12, 7, ACCENT_TEXT);
    } else {
        c.fill_round_rect(sx, sy, 48, 24, 12, TEXT2);
        c.fill_round_rect(sx + 1, sy + 1, 46, 22, 11, CARD);
        c.fill_circle(sx + 12, sy + 12, 6, TEXT2);
    }
    c.text(sx - 40, r.y + r.h / 2 + 6, on ? "On" : "Off", 15, TEXT2);
}

void draw_system_page(Canvas &c, const Window &w);   // (below)
void draw_devices_page(Canvas &c, const Window &w);
void draw_apps_page(Canvas &c, const Window &w);

void draw_settings(Canvas &c, Window &w) {
    const std::string id = "win:" + std::to_string(w.id) + ":";
    // the pages, on the left
    c.fill_rect(w.x + 1, w.y + TITLE_H, SET_NAV_W, w.h - TITLE_H - 1, WINDOW_BG);
    draw_icon(c, Icon::User, w.x + 40, w.y + TITLE_H + 34, 36);
    c.text(w.x + 70, w.y + TITLE_H + 32, "PS4 user", 16, TEXT, Weight::Bold);
    c.text(w.x + 70, w.y + TITLE_H + 52, "WinePS4 " WINEPS4_VERSION, 13, TEXT3);
    const Icon page_icons[] = {Icon::Computer, Icon::Keyboard, Icon::Brush, Icon::Apps, Icon::Settings};
    for (int i = 0; i < static_cast<int>(std::size(SETTINGS_PAGES)); i++) {
        const Rect r = settings_page_rect(w, i);
        if (w.page == i)
            c.fill_round_rect(r.x, r.y, r.w, r.h, 5, SELECTED), c.fill_round_rect(r.x, r.y + 11, 3, r.h - 22, 2, ACCENT);
        else if (hover == id + "page" + std::to_string(i))
            c.fill_round_rect(r.x, r.y, r.w, r.h, 5, HOVER);
        draw_icon(c, page_icons[i], r.x + 24, r.y + r.h / 2.f, 22);
        c.text(r.x + 46, r.y + 27, SETTINGS_PAGES[i], 15, TEXT);
    }
    // the page
    c.fill_rect(w.x + SET_NAV_W + 1, w.y + TITLE_H, w.w - SET_NAV_W - 2, w.h - TITLE_H - 1, CONTENT_BG);
    const Rect a = settings_area(w);
    c.text(a.x, a.y + 34, SETTINGS_PAGES[w.page], 26, TEXT, Weight::Bold);
    if (w.page == PAGE_SYSTEM) {
        draw_system_page(c, w);
    } else if (w.page == PAGE_DEVICES) {
        draw_devices_page(c, w);
    } else if (w.page == PAGE_APPS) {
        draw_apps_page(c, w);
    } else if (w.page == PAGE_PERSONALIZE) {
        const Rect tc = theme_card(w), bc = bg_card(w);
        panel(c, tc.x, tc.y, tc.w, tc.h, 6, CARD, CARD_BORDER);
        c.text(tc.x + 22, tc.y + 32, "Theme", 16, TEXT);
        c.text(tc.x + 22, tc.y + 56, "Dark or light windows, menus and taskbar", 14, TEXT3);
        const char *const themes[] = {"Dark", "Light"};
        draw_segments(c, tc, themes, 2, 116, light_theme ? 1 : 0, id + "theme");
        panel(c, bc.x, bc.y, bc.w, bc.h, 6, CARD, CARD_BORDER);
        c.text(bc.x + 22, bc.y + 32, "Background", 16, TEXT);
        c.text(bc.x + 22, bc.y + 56, "A picture (any of yours too), or a solid color", 14, TEXT3);
        const char *const tabs[] = {"Picture", "Solid color"};
        draw_segments(c, bc, tabs, 2, 140, w.bg_tab, id + "bgtab");
        if (w.bg_tab == 0) {
            const auto tiles = settings_tiles();
            for (int i = 0; i < static_cast<int>(tiles.size()); i++) {
                const Rect r = tile_rect(w, i);
                const bool chosen = tile_selected(tiles[i].key), hot = hover == id + "tile" + std::to_string(i);
                if (chosen || hot)
                    c.fill_round_rect(r.x - 4, r.y - 4, r.w + 8, r.h + 8, 8, chosen ? ACCENT : HOVER);
                if (tiles[i].key == "browse") {
                    panel(c, r.x, r.y, r.w, r.h, 5, CARD, CARD_BORDER);
                    draw_icon(c, Icon::Folder, r.x + r.w / 2.f, r.y + r.h / 2.f, 44);
                } else {
                    c.draw_image(tile_image(tiles[i].key), r.x, r.y);
                }
                c.text(r.x, r.y + r.h + 20, tiles[i].label, 14, chosen ? TEXT : TEXT2, Weight::Regular, r.w);
            }
        } else {
            for (int i = 0; i < static_cast<int>(std::size(SWATCHES)); i++) {
                const Rect r = swatch_rect(w, i);
                const bool chosen = background.kind == BgKind::Color && background.color == SWATCHES[i];
                if (chosen || hover == id + "swatch" + std::to_string(i))
                    c.fill_round_rect(r.x - 4, r.y - 4, r.w + 8, r.h + 8, 10, chosen ? ACCENT : HOVER);
                c.fill_round_rect(r.x, r.y, r.w, r.h, 7, rgb(SWATCHES[i]));
            }
            const Rect cr = custom_color_rect(w);
            const bool hot = hover == id + "customcolor";
            panel(c, cr.x, cr.y, cr.w, cr.h, 5, hot ? BUTTON_HOT : BUTTON, BUTTON_BORDER);
            c.text_centered(cr.x + cr.w / 2, cr.y + 26, "Custom color...", 15, TEXT);
            if (background.kind == BgKind::Color) {
                char hex[24];
                std::snprintf(hex, sizeof hex, "Now: #%06X", background.color & 0xffffff);
                c.fill_round_rect(cr.x + cr.w + 24, cr.y + 4, 32, 32, 6, rgb(background.color & 0xffffff));
                c.text(cr.x + cr.w + 68, cr.y + 26, hex, 15, TEXT2);
            }
        }
    } else if (w.page == PAGE_DIAGNOSTICS) {
        for (int i = 0; i < static_cast<int>(std::size(TOGGLES)); i++) {
            const Rect r = toggle_rect(w, i);
            panel(c, r.x, r.y, r.w, r.h, 6, hover == id + "toggle" + std::to_string(i) ? CARD_HOT : CARD, CARD_BORDER);
            c.text(r.x + 22, r.y + 33, TOGGLES[i].title, 16, TEXT);
            c.text(r.x + 22, r.y + 58, TOGGLES[i].detail, 14, TEXT3, Weight::Regular, r.w - 140);
            draw_switch(c, r, toggle_on(TOGGLES[i]));
        }
        c.text_wrapped(a.x, a.y + 60 + static_cast<int>(std::size(TOGGLES)) * 80 + 10,
                       "The switches go to /data/WinePS4/env.txt and apply the next time a program starts. "
                       "Logs: /data/WinePS4/wine.log, crash.log, hang.log.",
                       14, TEXT3, a.w, 22);
    }
}

// a bar of how full something is (memory, storage): used of total
void usage_bar(Canvas &c, int x, int y, int w, double used, double total) {
    c.fill_round_rect(x, y, w, 8, 4, TRACK);
    if (total > 0)
        c.fill_round_rect(x, y, std::max(8, static_cast<int>(w * std::clamp(used / total, 0.0, 1.0))), 8, 4, ACCENT);
}

std::string gib_text(double bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    return text;
}

// System: this console as Windows sees it, its memory and storage, and WinePS4
void draw_system_page(Canvas &c, const Window &w) {
    const Rect a = settings_area(w);
    const char *name = std::getenv("WINEPS4_HOSTNAME");
    const bool pro = sceKernelIsNeoMode() == 1;
    Rect r = card_at(w, 56, 96);
    panel(c, r.x, r.y, r.w, r.h, 6, CARD, CARD_BORDER);
    draw_icon(c, Icon::Computer, r.x + 48, r.y + r.h / 2.f, 48);
    c.text(r.x + 92, r.y + 38, name ? name : "PS4", 22, TEXT, Weight::Bold);
    c.text(r.x + 92, r.y + 66, pro ? "PlayStation 4 Pro, in its own (NEO) mode: CPU at 2.1 GHz" : "PlayStation 4 (CPU at 1.6 GHz)", 14, TEXT3);
    // memory (the direct memory programs get) and the PS4's storage (/data: the C: drive is in it)
    struct statvfs fs{};
    const double disk_total = statvfs("/data", &fs) == 0 ? static_cast<double>(fs.f_blocks) * fs.f_frsize : 0;
    const double disk_free = disk_total > 0 ? static_cast<double>(fs.f_bavail) * fs.f_frsize : 0;
    const double mem_total = static_cast<double>(memory_total), mem_used = mem_total - std::min<double>(memory_free, mem_total);
    const struct {
        const char *title;
        double used, total;
    } meters[] = {{"Memory for Windows programs", mem_used, mem_total}, {"Storage (/data)", disk_total - disk_free, disk_total}};
    for (int i = 0; i < 2; i++) {
        r = card_at(w, 166 + i * 100, 88);
        panel(c, r.x, r.y, r.w, r.h, 6, CARD, CARD_BORDER);
        c.text(r.x + 22, r.y + 32, meters[i].title, 16, TEXT);
        const std::string figures = gib_text(meters[i].used) + " used of " + gib_text(meters[i].total);
        c.text(r.x + r.w - 22 - c.text_width(figures, 14), r.y + 32, figures, 14, TEXT2);
        usage_bar(c, r.x + 22, r.y + 54, r.w - 44, meters[i].used, meters[i].total);
    }
    r = card_at(w, 376, 70);
    panel(c, r.x, r.y, r.w, r.h, 6, CARD, CARD_BORDER);
    c.text(r.x + 22, r.y + 30, "Display", 16, TEXT);
    c.text(r.x + 22, r.y + 52, "1920 x 1080, 60 Hz (programs' full screen is the same)", 14, TEXT3);
    int y = a.y + 486;
    const char *const lines[] = {
        "WinePS4 " WINEPS4_VERSION ": Windows programs on the PS4. Wine 11.0 (64-bit and 32-bit), wineserver in the app,",
        "each program a process of its own, Windows' memory in the PS4's direct memory.",
        "Wine (LGPL 2.1), FFmpeg (LGPL 2.1), stb (public domain), Roboto (SIL OFL), Mozilla's root certificates (MPL 2.0).",
        "Data: /data/WinePS4 (the Windows C: drive is /data/WinePS4/prefix/drive_c).",
    };
    for (const char *line : lines) {
        c.text(a.x, y, line, 14, TEXT3, Weight::Regular, a.w);
        y += 24;
    }
}

// Bluetooth & devices: the mouse's speed, the keyboard's mode, the controller
void draw_devices_page(Canvas &c, const Window &w) {
    const std::string id = "win:" + std::to_string(w.id) + ":";
    const Input &in = last_input;
    Rect r = mouse_card(w);
    panel(c, r.x, r.y, r.w, r.h, 6, CARD, CARD_BORDER);
    draw_icon(c, Icon::Mouse, r.x + 34, r.y + r.h / 2.f, 30);
    c.text(r.x + 64, r.y + 38, "Mouse pointer speed", 16, TEXT);
    c.text(r.x + 64, r.y + 62, in.mouse ? "A mouse is connected" : "No mouse connected (the touch pad and left stick move the pointer)", 14,
           TEXT3, Weight::Regular, r.w - 470);
    for (int i = 0; i < 2; i++) {
        const Rect b = speed_button(w, i);
        panel(c, b.x, b.y, b.w, b.h, 5, hover == id + "speed" + std::to_string(i) ? BUTTON_HOT : BUTTON, BUTTON_BORDER);
        c.line(b.x + 13, b.y + b.h / 2.f, b.x + 27, b.y + b.h / 2.f, 2.f, TEXT);
        if (i)
            c.line(b.x + 20, b.y + 11, b.x + 20, b.y + 25, 2.f, TEXT);
    }
    const Rect lo = speed_button(w, 0), hi = speed_button(w, 1);
    const int bx = lo.x + lo.w + 16, bw = hi.x - 16 - bx;
    c.fill_round_rect(bx, lo.y + 15, bw, 6, 3, TRACK);
    c.fill_round_rect(bx, lo.y + 15, bw * pointer_speed / 20, 6, 3, ACCENT);
    c.fill_circle(bx + bw * pointer_speed / 20.f, lo.y + 18, 9, ACCENT);
    c.text_centered(bx + bw / 2, lo.y - 6, std::to_string(pointer_speed), 13, TEXT2);

    r = keyboard_card(w);
    panel(c, r.x, r.y, r.w, r.h, 6, CARD, CARD_BORDER);
    draw_icon(c, Icon::Keyboard, r.x + 34, r.y + 40, 30);
    c.text(r.x + 64, r.y + 38, "Keyboard", 16, TEXT);
    const char *const modes[] = {"PS4 + Keyboard", "Keyboard"};
    draw_segments(c, r, modes, 2, 170, direct_keyboard ? 1 : 0, id + "kbmode");
    c.text_wrapped(r.x + 64, r.y + 70,
                   direct_keyboard ? "A USB keyboard plugged into the PS4 types straight into programs; ENG shows on the taskbar while it does."
                                   : "Triangle over a program opens the PS4's keyboard, which a USB keyboard plugged in types into too.",
                   14, TEXT3, r.w - 88, 20);

    r = autokb_card(w);
    panel(c, r.x, r.y, r.w, r.h, 6, hover == id + "autokb" ? CARD_HOT : CARD, CARD_BORDER);
    c.text(r.x + 22, r.y + 33, "Open the PS4's keyboard for a text field clicked", 16, TEXT);
    c.text(r.x + 22, r.y + 58,
           "In programs (Steam's sign-in, Windows' text boxes) and Notepad; not while a USB keyboard types straight into programs.",
           14, TEXT3, Weight::Regular, r.w - 140);
    draw_switch(c, r, auto_keyboard);

    r = controller_card(w);
    panel(c, r.x, r.y, r.w, r.h, 6, CARD, CARD_BORDER);
    draw_icon(c, Icon::Pad, r.x + 34, r.y + 40, 32);
    c.text(r.x + 64, r.y + 38, in.pad ? "Controller connected" : "No controller connected", 16, TEXT);
    c.text_wrapped(r.x + 64, r.y + 64,
                   "Left stick or touch pad: pointer. Cross: click, Square: right-click, Triangle: keyboard, "
                   "OPTIONS: Start, L1/R1: windows, Circle: back, right stick: scroll.",
                   14, TEXT3, r.w - 88, 20);
}

// Apps: the Windows programs installed, each with its Uninstall
void draw_apps_page(Canvas &c, const Window &w) {
    const std::string id = "win:" + std::to_string(w.id) + ":";
    const Rect a = settings_area(w);
    const auto &apps = installed_apps();
    // Chromium programs (Steam, Chrome, Edge, Discord, ...) in one process each
    const Rect cr = chromium_card(w);
    panel(c, cr.x, cr.y, cr.w, cr.h, 6, hover == id + "chromium" ? CARD_HOT : CARD, CARD_BORDER);
    draw_icon(c, Icon::Apps, cr.x + 34, cr.y + cr.h / 2.f, 26);
    c.text(cr.x + 66, cr.y + 34, "Chromium programs in one process", 16, TEXT);
    c.text(cr.x + 66, cr.y + 58, "Steam, Chrome, Edge, Discord: the PS4 runs 7 processes for the app, they would take more", 13, TEXT3,
           Weight::Regular, cr.w - 180);
    draw_switch(c, cr, env_get("WINEPS4_CHROMIUM_ONE_PROCESS") != "0");
    // helper programs that would only take processes (Steam's start-up probes, its overlay)
    const Rect rr = rules_card(w);
    panel(c, rr.x, rr.y, rr.w, rr.h, 6, hover == id + "rules" ? CARD_HOT : CARD, CARD_BORDER);
    draw_icon(c, Icon::Gauge, rr.x + 34, rr.y + rr.h / 2.f, 26);
    c.text(rr.x + 66, rr.y + 34, "Leave out helpers that only take processes", 16, TEXT);
    c.text(rr.x + 66, rr.y + 58, "Steam's start-up probes, its install scripts and its overlay window", 13, TEXT3,
           Weight::Regular, rr.w - 180);
    draw_switch(c, rr, env_get("WINEPS4_PROCESS_RULES") != "0");
    // Windows' background programs and helpers outside the PS4's 7, and any program once 7 run
    const Rect orr = outside_card(w);
    panel(c, orr.x, orr.y, orr.w, orr.h, 6, hover == id + "outside" ? CARD_HOT : CARD, CARD_BORDER);
    draw_icon(c, Icon::Program, orr.x + 34, orr.y + orr.h / 2.f, 26);
    c.text(orr.x + 66, orr.y + 34, "More than 7 processes (test)", 16, TEXT);
    c.text(orr.x + 66, orr.y + 58, "Windows' services and helpers run outside the PS4's 7, which stay for programs and games. "
                                   "Close WinePS4 with Power > Exit", 13, TEXT3, Weight::Regular, orr.w - 180);
    draw_switch(c, orr, env_get("WINEPS4_OUTSIDE") != "0");
    // Direct3D 8-11 through DXVK on the PS4's GPU (Vulkan, RADV), else Wine's own (which has no GPU here)
    const Rect dr = dxvk_card(w);
    panel(c, dr.x, dr.y, dr.w, dr.h, 6, hover == id + "dxvk" ? CARD_HOT : CARD, CARD_BORDER);
    draw_icon(c, Icon::Gauge, dr.x + 34, dr.y + dr.h / 2.f, 26);
    c.text(dr.x + 66, dr.y + 34, "3D games on the PS4's GPU (DXVK)", 16, TEXT);
    c.text(dr.x + 66, dr.y + 58, "Direct3D 8 to 11 through DXVK and Vulkan; off: Wine's own Direct3D, which has no GPU here", 13,
           TEXT3, Weight::Regular, dr.w - 180);
    draw_switch(c, dr, env_get("WINEPS4_DXVK") != "0");
    c.text(a.x, a.y + 454, apps.empty() ? "No Windows programs are installed yet: run an installer (.exe or .msi) from Files."
                                        : "Installed apps (" + std::to_string(apps.size()) + ")",
           15, TEXT2);
    for (int i = 0; i < static_cast<int>(apps.size()); i++) {
        const Rect r = app_row(w, i);
        if (r.y + r.h > w.y + w.h - 10)
            break;
        panel(c, r.x, r.y, r.w, r.h, 6, CARD, CARD_BORDER);
        if (const Image *icon = program_icon(app_icon_path(apps[i]), 32))
            c.blit(*icon, r.x + 18, r.y + r.h / 2 - 16);
        else
            draw_icon(c, Icon::Program, r.x + 34, r.y + r.h / 2.f, 32);
        c.text(r.x + 66, r.y + 27, apps[i].name, 16, TEXT, Weight::Regular, r.w - 230);
        std::string detail = apps[i].publisher;
        if (!apps[i].version.empty())
            detail += (detail.empty() ? "" : "  |  ") + apps[i].version;
        c.text(r.x + 66, r.y + 49, detail, 13, TEXT3, Weight::Regular, r.w - 230);
        const Rect b = app_uninstall(w, i);
        panel(c, b.x, b.y, b.w, b.h, 5, hover == id + "uninstall" + std::to_string(i) ? BUTTON_HOT : BUTTON, BUTTON_BORDER);
        c.text_centered(b.x + b.w / 2, b.y + 24, "Uninstall", 14, TEXT);
    }
}

std::string mib_text(unsigned long long bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f MB", bytes / (1024.0 * 1024));
    return text;
}

// a graph of the last minute (0-100 %), as Task Manager's Performance draws it
void draw_graph(Canvas &c, const Rect &r, const float *values, int n, Color line) {
    panel(c, r.x, r.y, r.w, r.h, 4, CONTROL, BORDER);
    for (int g = 1; g < 4; g++)
        c.fill_rect(r.x + 1, r.y + r.h * g / 4, r.w - 2, 1, LINE);
    const Color fill = Color{line.r, line.g, line.b, 0x30};
    for (int i = 0; i < n - 1; i++) {
        const float x0 = r.x + 2 + (r.w - 4) * i / float(n - 1), x1 = r.x + 2 + (r.w - 4) * (i + 1) / float(n - 1);
        const float y0 = r.y + r.h - 2 - (r.h - 4) * values[i] / 100.f, y1 = r.y + r.h - 2 - (r.h - 4) * values[i + 1] / 100.f;
        const int fx = static_cast<int>(x0), fy = static_cast<int>(std::min(y0, y1));
        c.fill_rect(fx, fy, std::max(1, static_cast<int>(x1) - fx), r.y + r.h - 2 - fy, fill);
        c.line(x0, y0, x1, y1, 2.f, line);
    }
}

void draw_taskmgr(Canvas &c, Window &w) {
    const std::string id = "win:" + std::to_string(w.id) + ":";
    // the tabs, on the left
    c.fill_rect(w.x + 1, w.y + TITLE_H, TM_NAV_W, w.h - TITLE_H - 1, WINDOW_BG);
    const Icon tab_icons[] = {Icon::Apps, Icon::Gauge};
    for (int i = 0; i < 2; i++) {
        const Rect r = tm_tab_rect(w, i);
        if (w.tab == i)
            c.fill_round_rect(r.x, r.y, r.w, r.h, 5, SELECTED), c.fill_round_rect(r.x, r.y + 11, 3, r.h - 22, 2, ACCENT);
        else if (hover == id + "tmtab" + std::to_string(i))
            c.fill_round_rect(r.x, r.y, r.w, r.h, 5, HOVER);
        draw_icon(c, tab_icons[i], r.x + 24, r.y + r.h / 2.f, 22);
        c.text(r.x + 46, r.y + 27, TM_TABS[i], 15, TEXT);
    }
    const int x0 = w.x + TM_NAV_W + 1, cw = w.w - TM_NAV_W - 2;
    c.fill_rect(x0, w.y + TITLE_H, cw, w.h - TITLE_H - 1, CONTENT_BG);
    c.text(x0 + 24, w.y + TITLE_H + 46, TM_TABS[w.tab], 22, TEXT, Weight::Bold);
    if (w.tab == 0) {
        // End task, for the process chosen
        const bool can_end = std::any_of(process_views.begin(), process_views.end(), [&](const ProcessView &v) { return v.pid == w.chosen_pid; });
        const Rect e = tm_end_button(w);
        panel(c, e.x, e.y, e.w, e.h, 5, can_end && hover == id + "tmend" ? BUTTON_HOT : BUTTON, BUTTON_BORDER);
        c.text_centered(e.x + e.w / 2, e.y + 25, "End task", 15, can_end ? TEXT : DISABLED);
        const int name_x = x0 + 24, pid_x = x0 + cw - 330, cpu_x = x0 + cw - 220, mem_x = x0 + cw - 120;
        const int hy = tm_rows_top(w) - 14;
        c.text(name_x, hy, "Name", 14, TEXT2);
        c.text(pid_x, hy, "PID", 14, TEXT2);
        c.text(cpu_x, hy, "CPU", 14, TEXT2);
        c.text(mem_x, hy, "Memory", 14, TEXT2);
        c.fill_rect(x0 + 12, hy + 8, cw - 24, 1, BORDER);
        const auto rows = tm_rows();
        for (int i = 0; i < static_cast<int>(rows.size()); i++) {
            const int ry = tm_rows_top(w) + i * TM_ROW_H;
            if (ry + TM_ROW_H > w.y + w.h - 8)
                break;
            const TmRow &row = rows[i];
            if (!row.section.empty()) {
                c.text(name_x, ry + 26, row.section, 14, TEXT2, Weight::Bold);
                continue;
            }
            const int pid = row.view ? row.view->pid : static_cast<int>(getpid());
            if (row.view && w.chosen_pid == pid)
                c.fill_round_rect(x0 + 8, ry + 1, cw - 16, TM_ROW_H - 2, 5, SELECTED);
            else if (row.view && hover == id + "tmrow" + std::to_string(pid))
                c.fill_round_rect(x0 + 8, ry + 1, cw - 16, TM_ROW_H - 2, 5, HOVER);
            const Image *icon = row.view ? program_icon(console_path(row.view->image), 16) : nullptr;
            if (icon)
                c.blit(*icon, name_x + 4, ry + TM_ROW_H / 2 - 8);
            else
                draw_icon(c, row.desktop ? Icon::Logo : row.view->windows_own ? Icon::Settings : Icon::Program, name_x + 12, ry + TM_ROW_H / 2.f, 18);
            const std::string name = row.desktop ? "Desktop and Windows server"
                                     : row.view->outside ? row.view->name + "  (outside the 7)" : row.view->name;
            const float cpu = row.desktop ? desktop_cpu_percent : row.view->cpu_percent;
            const unsigned long long memory = row.desktop ? ps4_vm_backed_bytes() : row.view->memory;
            char cpu_text[16];
            std::snprintf(cpu_text, sizeof cpu_text, "%.1f %%", cpu);
            c.text(name_x + 32, ry + 25, name, 15, TEXT, Weight::Regular, pid_x - name_x - 40);
            c.text(pid_x, ry + 25, std::to_string(pid), 15, TEXT2);
            c.text(cpu_x, ry + 25, cpu_text, 15, cpu >= 50 ? ACCENT : TEXT2);
            c.text(mem_x, ry + 25, mib_text(memory), 15, TEXT2);
        }
    } else {
        // the CPU and memory over the last minute, then the figures
        const int gw = (cw - 72) / 2, gy = w.y + TITLE_H + 110, gh = 220;
        char now[64];
        std::snprintf(now, sizeof now, "CPU  %.0f %%", cpu_history[59]);
        c.text(x0 + 24, gy - 14, now, 16, TEXT, Weight::Bold);
        draw_graph(c, Rect{x0 + 24, gy, gw, gh}, cpu_history, 60, ACCENT);
        const double used = memory_total - std::min(memory_free, memory_total);
        c.text(x0 + 48 + gw, gy - 14, "Memory  " + gib_text(used) + " of " + gib_text(memory_total), 16, TEXT, Weight::Bold);
        draw_graph(c, Rect{x0 + 48 + gw, gy, gw, gh}, memory_history, 60, rgb(0xb36ae2));
        c.text(x0 + 24, gy + gh + 22, "60 seconds", 13, TEXT3);
        const long up = static_cast<long>(now_seconds() - desktop_started);
        char up_text[32];
        std::snprintf(up_text, sizeof up_text, "%ld:%02ld:%02ld", up / 3600, up / 60 % 60, up % 60);
        const bool pro = sceKernelIsNeoMode() == 1;
        int outside = 0;
        for (const ProcessView &v : process_views)
            outside += v.outside;
        const int local = static_cast<int>(process_views.size()) - outside;
        const std::string lines[][2] = {
            {"Windows processes", std::to_string(local) + " of the PS4's 7" +
                                      (outside ? ", " + std::to_string(outside) + " outside them" : std::string())},
            {"Cores", std::to_string(APP_CORES) + " for apps (AMD Jaguar)"},
            {"CPU speed", pro ? "2.13 GHz (PS4 Pro, NEO mode)" : "1.6 GHz"},
            {"Up time", up_text},
        };
        int y = gy + gh + 64;
        for (const auto &line : lines) {
            c.text(x0 + 24, y, line[0], 14, TEXT3);
            c.text(x0 + 220, y, line[1], 15, TEXT);
            y += 32;
        }
    }
}

void draw_window(Canvas &c, Window &w, bool active) {
    const Image &shadow = shadow_for(w.w, w.h, active);
    c.blit(shadow, w.x - SHADOW, w.y - SHADOW);
    panel(c, w.x, w.y, w.w, w.h, w.maximized ? 0 : RADIUS, WINDOW_BG, active ? BORDER_ACTIVE : BORDER);
    switch (w.kind) {
    case Kind::Files: draw_files(c, w); break;
    case Kind::Terminal: draw_terminal(c, w); break;
    case Kind::Settings: draw_settings(c, w); break;
    case Kind::TaskManager: draw_taskmgr(c, w); break;
    case Kind::Notepad: draw_notepad(c, w); break;
    case Kind::HexEditor: draw_hex(c, w); break;
    }
    draw_caption(c, w, active);
}

void draw_taskbar(Canvas &c) {
    acrylic(c, 0, H - TASKBAR_H, W, TASKBAR_H, 0, TASKBAR_TINT);
    c.fill_rect(0, H - TASKBAR_H, W, 1, LINE);
    Window *top = focused();
    const auto editors = editor_windows();
    const int first_program = TB_FIRST_PROGRAM + static_cast<int>(editors.size());
    for (int i = 0; i < taskbar_count(); i++) {
        const Rect r = taskbar_button(i);
        if (i >= TB_FIRST_PROGRAM && i < first_program) {  // Notepad's or the hex editor's window
            const Window *w = find_window(editors[i - TB_FIRST_PROGRAM]);
            if (!w)
                continue;
            const bool active = w == top && desktop_on_top;
            if (active)
                c.fill_round_rect(r.x + 2, r.y, r.w - 4, r.h, 6, TASK_ACTIVE);
            else if (hover == "edtask:" + std::to_string(w->id))
                c.fill_round_rect(r.x + 2, r.y, r.w - 4, r.h, 6, TASK_HOVER);
            const float cx = r.x + r.w / 2.f, cy = r.y + r.h / 2.f;
            draw_icon(c, window_icon(*w), cx, cy, 26);
            const int lw = active ? 16 : 6;
            c.fill_round_rect(static_cast<int>(cx) - lw / 2, r.y + r.h - 4, lw, 3, 2, active ? ACCENT : TEXT3);
            continue;
        }
        if (i >= first_program) {  // a Windows program's window: its program's icon
            const WinTask &t = program_tasks[i - first_program];
            const bool active = t.active && !t.minimized;
            if (active)
                c.fill_round_rect(r.x + 2, r.y, r.w - 4, r.h, 6, TASK_ACTIVE);
            else if (hover == "wtask:" + std::to_string(t.hwnd))
                c.fill_round_rect(r.x + 2, r.y, r.w - 4, r.h, 6, TASK_HOVER);
            const float cx = r.x + r.w / 2.f, cy = r.y + r.h / 2.f;
            if (const Image *icon = process_icon(t.pid, 24))
                c.blit(*icon, static_cast<int>(cx) - 12, static_cast<int>(cy) - 12);
            else
                draw_icon(c, Icon::Program, cx, cy, 26);
            const int lw = active ? 16 : 6;
            c.fill_round_rect(static_cast<int>(cx) - lw / 2, r.y + r.h - 4, lw, 3, 2, active ? ACCENT : TEXT3);
            continue;
        }
        const std::string key = "taskbar:" + std::to_string(i);
        const bool active = (i == 0 && popup != Popup::None) ||
                            (i >= 2 && top && top->kind == TASKBAR_APPS[i - 2]);
        if (active)
            c.fill_round_rect(r.x + 2, r.y, r.w - 4, r.h, 6, TASK_ACTIVE);
        else if (hover == key)
            c.fill_round_rect(r.x + 2, r.y, r.w - 4, r.h, 6, TASK_HOVER);
        const float cx = r.x + r.w / 2.f, cy = r.y + r.h / 2.f;
        if (i == 0)
            draw_icon(c, Icon::Logo, cx, cy, 30);
        else if (i == 1)
            draw_icon(c, Icon::Search, cx, cy, 26);
        else {
            const Kind kind = TASKBAR_APPS[i - 2];
            draw_icon(c, kind == Kind::Files ? Icon::Folder : kind == Kind::Terminal ? Icon::Terminal : Icon::Settings, cx, cy, 28);
            const bool open = std::any_of(windows.begin(), windows.end(), [&](const Window &w) { return w.kind == kind; });
            if (open) {
                const int lw = active ? 16 : 6;
                c.fill_round_rect(static_cast<int>(cx) - lw / 2, r.y + r.h - 4, lw, 3, 2, active ? ACCENT : TEXT3);
            }
        }
    }
    // the tray: what controls the desktop, then the time
    const Input &in = last_input;
    int tx = W - 170;
    if (direct_keyboard && hwkeyboard_connected()) {  // a USB keyboard types straight into programs
        draw_icon(c, Icon::Keyboard, tx, H - TASKBAR_H / 2.f, 24);
        c.text(tx + 16, H - TASKBAR_H / 2 + 5, "ENG", 13, TEXT2, Weight::Bold);
        tx -= 40;
    }
    if (in.mouse) {
        draw_icon(c, Icon::Mouse, tx, H - TASKBAR_H / 2.f, 22);
        tx -= 34;
    }
    if (in.pad)
        draw_icon(c, Icon::Pad, tx, H - TASKBAR_H / 2.f, 24);
    std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_r(&t, &local);
    char clock[16], date[16];
    std::strftime(clock, sizeof clock, "%H:%M", &local);
    std::strftime(date, sizeof date, "%d/%m/%Y", &local);
    const int rx = W - 24;
    c.text(rx - c.text_width(clock, 15), H - TASKBAR_H + 27, clock, 15, TEXT);
    c.text(rx - c.text_width(date, 15), H - TASKBAR_H + 49, date, 15, TEXT);
}

void draw_start_menu(Canvas &c) {
    const Rect m = start_menu_rect();
    const Image &shadow = shadow_for(m.w, m.h, true);
    c.blit(shadow, m.x - SHADOW, m.y - SHADOW);
    acrylic(c, m.x, m.y, m.w, m.h, RADIUS, START_TINT);
    // search box
    panel(c, m.x + 32, m.y + 28, m.w - 64, 40, 20, SEARCH_BG, SEARCH_BORDER);
    draw_icon(c, Icon::Search, m.x + 58, m.y + 48, 18);
    c.text(m.x + 80, m.y + 54, "Search for programs and files", 15, TEXT3);
    c.text(m.x + 52, m.y + 112, "Pinned", 16, TEXT, Weight::Bold);
    for (int i = 0; i < static_cast<int>(std::size(PINS)); i++) {
        const Rect r = pin_rect(i);
        if (hover == "pin:" + std::to_string(i))
            c.fill_round_rect(r.x, r.y, r.w, r.h, 6, HOVER);
        draw_icon(c, PINS[i].icon, r.x + r.w / 2.f, r.y + 36, 40);
        c.text_centered(r.x + r.w / 2, r.y + 82, PINS[i].label, 14, TEXT);
    }
    c.text(m.x + 52, m.y + RECENT_TOP - 18, "Recent programs", 16, TEXT, Weight::Bold);
    const auto recent = recent_programs();
    if (recent.empty())
        c.text(m.x + 52, m.y + RECENT_TOP + 20, "Programs you run show here.", 14, TEXT3);
    for (int i = 0; i < static_cast<int>(recent.size()); i++) {
        const Rect r = recent_rect(i);
        if (hover == "recent:" + std::to_string(i))
            c.fill_round_rect(r.x, r.y, r.w, r.h, 6, HOVER);
        if (const Image *icon = program_icon(recent[i], 32))
            c.blit(*icon, r.x + 16, r.y + r.h / 2 - 16);
        else
            draw_icon(c, Icon::Program, r.x + 32, r.y + r.h / 2.f, 32);
        c.text(r.x + 60, r.y + 28, base_name(recent[i]), 15, TEXT, Weight::Regular, r.w - 70);
        c.text(r.x + 60, r.y + 50, recent[i], 13, TEXT3, Weight::Regular, r.w - 70);
    }
    // bottom: the user, and power
    c.fill_rect(m.x, m.y + m.h - 80, m.w, 1, LINE);
    acrylic(c, m.x, m.y + m.h - 79, m.w, 79, 0, START_FOOTER);
    draw_icon(c, Icon::User, m.x + 64, m.y + m.h - 40, 34);
    c.text(m.x + 92, m.y + m.h - 34, "PS4 user", 15, TEXT);
    const Rect p = power_button();
    if (hover == "powerbutton" || popup == Popup::Power)
        c.fill_round_rect(p.x, p.y, p.w, p.h, 6, HOVER);
    draw_icon(c, Icon::Power, p.x + p.w / 2.f, p.y + p.h / 2.f, 22);
    if (popup == Popup::Power) {
        const Rect a = power_item(0);
        panel(c, a.x - 4, a.y - 4, a.w + 8, 2 * 52 + 4, 8, POPUP_BG, POPUP_BORDER);
        const char *labels[] = {"Restart WinePS4", "Exit to the PS4 menu"};
        for (int i = 0; i < 2; i++) {
            const Rect r = power_item(i);
            if (hover == "power:" + std::to_string(i))
                c.fill_round_rect(r.x, r.y, r.w, r.h, 5, HOVER);
            c.text(r.x + 18, r.y + 30, labels[i], 15, TEXT);
        }
    }
}

void draw_dialog(Canvas &c) {
    c.fill_rect(0, 0, W, H, rgb(0x000000, 0x60));
    const Rect d = dialog_rect();
    const Image &shadow = shadow_for(d.w, d.h, true);
    c.blit(shadow, d.x - SHADOW, d.y - SHADOW);
    panel(c, d.x, d.y, d.w, d.h, RADIUS, DIALOG_BG, POPUP_BORDER);
    c.fill_round_rect(d.x + 1, d.y + d.h - 88, d.w - 2, 87, RADIUS - 1, DIALOG_FOOT);
    c.fill_rect(d.x + 1, d.y + d.h - 88, d.w - 2, 10, DIALOG_FOOT);
    c.text(d.x + 30, d.y + 48, dialog.title, 20, TEXT, Weight::Bold, d.w - 60);
    if (dialog_lines() == 1) {
        c.text_wrapped(d.x + 30, d.y + 84, dialog.text, 15, TEXT2, d.w - 60, 22);
    } else {
        size_t start = 0;
        for (int line = 0; start <= dialog.text.size(); line++) {
            const size_t end = dialog.text.find('\n', start);
            const std::string piece = dialog.text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            c.text(d.x + 30, d.y + 84 + line * 26, piece, 15, TEXT2, Weight::Regular, d.w - 60);
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }
    for (int i = 0; i < dialog.buttons(); i++) {
        const Rect b = dialog_button(i);
        const bool primary = i == 0;
        const bool hot = hover == "dialog:" + std::to_string(i);
        const bool danger = primary && (dialog.kind == DialogKind::Delete || dialog.kind == DialogKind::EmptyBin);
        const Color fill = danger ? (hot ? rgb(0xe0453a) : CLOSE_RED) : primary ? (hot ? ACCENT_HOT : ACCENT)
                                                                     : (hot ? BUTTON_HOT : BUTTON);
        panel(c, b.x, b.y, b.w, b.h, 5, fill, danger ? rgb(0xa32317) : primary ? ACCENT_BORDER : BUTTON_BORDER);
        const char *label = i == dialog.buttons() - 1 && i > 0     ? "Cancel"
                            : dialog.kind == DialogKind::Unsaved   ? (primary ? "Save" : "Don't save")
                            : dialog.kind == DialogKind::Delete    ? "Delete"
                            : dialog.kind == DialogKind::EmptyBin  ? "Empty"
                            : dialog.kind == DialogKind::Wallpaper ? "Set as background"
                                                                   : "OK";
        c.text_centered(b.x + b.w / 2, b.y + 28, label, 15, danger ? rgb(0xffffff) : primary ? ACCENT_TEXT : TEXT);
    }
}

void draw_menu(Canvas &c) {
    const Rect m = menu_rect();
    c.blit(shadow_for(m.w, m.h, true), m.x - SHADOW, m.y - SHADOW);
    acrylic(c, m.x, m.y, m.w, m.h, RADIUS, MENU_TINT);
    c.fill_round_rect(m.x, m.y, m.w, 1, 0, LINE);
    for (int i = 0; i < static_cast<int>(menu.items.size()); i++) {
        const MenuItem &it = menu.items[i];
        const Rect r = menu_item_rect(i);
        if (it.action.empty()) {
            c.fill_rect(r.x + 8, r.y + r.h / 2, r.w - 16, 1, LINE);
            continue;
        }
        if (it.enabled && hover == "menu:" + std::to_string(i))
            c.fill_round_rect(r.x, r.y + 1, r.w, r.h - 2, 5, HOVER);
        c.text(r.x + 16, r.y + 25, it.label, 15, it.enabled ? TEXT : DISABLED);
    }
}

// a copy, move or delete running: a card above the taskbar with its progress
void draw_job(Canvas &c) {
    const JobStatus s = job_status();
    if (!s.running)
        return;
    const int w = 440, h = 104, x = W - w - 16, y = H - TASKBAR_H - 16 - h;
    c.blit(shadow_for(w, h, true), x - SHADOW, y - SHADOW);
    acrylic(c, x, y, w, h, RADIUS, CARD_TINT);
    c.text(x + 22, y + 32, s.title, 16, TEXT, Weight::Bold, w - 44);
    c.text(x + 22, y + 56, s.current, 14, TEXT2, Weight::Regular, w - 44);
    const float f = s.total > 0 ? std::clamp(static_cast<float>(s.done) / s.total, 0.f, 1.f) : 0.f;
    c.fill_round_rect(x + 22, y + 74, w - 44, 6, 3, TRACK);
    c.fill_round_rect(x + 22, y + 74, std::max(6, static_cast<int>((w - 44) * f)), 6, 3, ACCENT);
    const std::string amount = s.title.rfind("Deleting", 0) == 0
                                   ? std::to_string(s.done) + " of " + std::to_string(s.total)
                                   : size_text(s.done) + " of " + size_text(s.total);
    c.text(x + 22, y + 98, amount, 13, TEXT3);
}

void draw_toast(Canvas &c) {
    if (toast.text.empty() || now_seconds() > toast.until)
        return;
    const int tw = c.text_width(toast.text, 15) + 48;
    const int x = W - tw - 16, y = H - TASKBAR_H - 16 - 56;
    c.blit(shadow_for(tw, 56, true), x - SHADOW, y - SHADOW);
    acrylic(c, x, y, tw, 56, RADIUS, CARD_TINT);
    c.text(x + 24, y + 34, toast.text, 15, TEXT);
}

void draw_scene(Canvas &c) {
    std::memcpy(c.data(), wallpaper.data(), wallpaper.size() * 4);
    draw_desktop_icons(c);
    Window *top = focused();
    const auto desktop_windows = [&] {
        for (Window &w : windows)
            if (!w.minimized)
                draw_window(c, w, &w == top && popup == Popup::None);
    };
    // Windows programs' windows with the desktop's title bars: over the desktop's own, or under them
    // when one of those was clicked last; a full-screen one (a game's) over the taskbar too
    if (!desktop_on_top)
        desktop_windows();
    winwin_draw(c, WinLayer::Normal, draw_program_under, draw_program_over);
    if (desktop_on_top)
        desktop_windows();
    draw_taskbar(c);
    winwin_draw(c, WinLayer::FullScreen, [](Canvas &, const WinFrame &) {}, [](Canvas &, const WinFrame &) {});
    if (popup != Popup::None)
        draw_start_menu(c);
    if (dragged.active)
        draw_dragged(c);
    draw_job(c);
    if (!job_status().running)
        draw_toast(c);
    if (menu.open)
        draw_menu(c);
    if (dialog.open)
        draw_dialog(c);
}

// The resize pointers: a double arrow along (dx, dy), white with a dark outline, centred
Image make_resize_pointer(float dx, float dy) {
    const float len = std::sqrt(dx * dx + dy * dy);
    dx /= len, dy /= len;
    const float nx = -dy, ny = dx;  // across it
    constexpr int S = 34;
    const float c = S / 2.f;
    struct Seg { float ax, ay, bx, by; };
    const Seg segs[] = {
        {c - dx * 12, c - dy * 12, c + dx * 12, c + dy * 12},
        {c + dx * 12, c + dy * 12, c + dx * 6 + nx * 6, c + dy * 6 + ny * 6},
        {c + dx * 12, c + dy * 12, c + dx * 6 - nx * 6, c + dy * 6 - ny * 6},
        {c - dx * 12, c - dy * 12, c - dx * 6 + nx * 6, c - dy * 6 + ny * 6},
        {c - dx * 12, c - dy * 12, c - dx * 6 - nx * 6, c - dy * 6 - ny * 6},
    };
    Image img;
    img.width = img.height = S;
    img.pixels.assign(S * S, 0);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float best = 1e9f;
            for (const Seg &s : segs) {
                const float vx = s.bx - s.ax, vy = s.by - s.ay;
                const float t = std::clamp(((x + 0.5f - s.ax) * vx + (y + 0.5f - s.ay) * vy) / (vx * vx + vy * vy), 0.f, 1.f);
                const float ex = x + 0.5f - (s.ax + vx * t), ey = y + 0.5f - (s.ay + vy * t);
                best = std::min(best, std::sqrt(ex * ex + ey * ey));
            }
            const float white = std::clamp(1.6f - best, 0.f, 1.f);
            const float all = std::clamp(3.2f - best, 0.f, 1.f);
            const uint32_t v = static_cast<uint32_t>(255.f * white);
            img.pixels[y * S + x] = v | (v << 8) | (v << 16) | (static_cast<uint32_t>(all * 255.f) << 24);
        }
    return img;
}

// The frame on the screen: the scene, then the pointer on top (the pointer moves without the
// scene being drawn again); (hx, hy) is the pointer picture's hot spot
void present(const Canvas &scene, const Image &pointer, float px, float py, int hx = 1, int hy = 1) {
    uint32_t *const dst = screen_back_buffer();
    const uint32_t *const src = scene.data();
    parallel_rows(0, H, static_cast<size_t>(W) * H, [&](int a, int b) {
        std::memcpy(dst + static_cast<size_t>(a) * W, src + static_cast<size_t>(a) * W, static_cast<size_t>(b - a) * W * 4);
        __builtin_ia32_sfence();
    });
    const int ox = static_cast<int>(px) - hx, oy = static_cast<int>(py) - hy;
    for (int y = 0; y < pointer.height; y++) {
        const int sy = oy + y;
        if (sy < 0 || sy >= H)
            continue;
        for (int x = 0; x < pointer.width; x++) {
            const int sx = ox + x;
            if (sx < 0 || sx >= W)
                continue;
            const uint32_t p = pointer.pixels[static_cast<size_t>(y) * pointer.width + x];
            if (p >> 24)
                dst[static_cast<size_t>(sy) * W + sx] = over(src[static_cast<size_t>(sy) * W + sx], p);
        }
    }
    screen_flip();
}

void block_wine_signals() {
    // Wine's signals go to Wine's threads (by thread id): a process-wide one landing here would run
    // Wine's handler on a thread without a TEB
    sigset_t set;
    sigemptyset(&set);
    for (int sig : {1, 2, 3, 13, 14, 15, 16, 20, 23, 30, 31})  // FreeBSD: HUP INT QUIT PIPE ALRM TERM URG CHLD IO USR1 USR2
        sigaddset(&set, sig);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

} // namespace

} // namespace shell

using namespace shell;

extern "C" void ps4_shell_output(const char *text, size_t len) {
    std::lock_guard lock(program_lock);
    for (size_t i = 0; i < len; i++) {
        const char ch = text[i];
        if (ch == '\n') {
            // (the desktop's own lines are for wine.log only)
            if (program_partial.rfind("shell: ", 0) != 0)
                program_lines.push_back(program_partial);
            program_partial.clear();
            if (program_lines.size() > 4000)
                program_lines.pop_front();
        } else if (ch != '\r') {
            program_partial += ch;
        }
    }
    program_changed = true;
}

extern "C" void ps4_shell_status(const char *text) {
    std::lock_guard lock(program_lock);
    program_status = text ? text : "";
    program_changed = true;
}

extern "C" void ps4_shell_program_exited(int pid, int code) {
    {
        std::lock_guard lock(program_lock);
        auto it = programs_running.find(pid);
        if (it == programs_running.end())
            return;  // (not one the desktop started)
        last_ended = it->second + " ended (exit code " + std::to_string(code) + ")";
        last_ended_code = code;
        ended_toasts.push_back(last_ended);
        programs_running.erase(it);
    }
    program_changed = true;
}

extern "C" void ps4_shell_notice(const char *text) {
    {
        std::lock_guard lock(program_lock);
        ended_toasts.push_back(text);
    }
    program_changed = true;
}

// launch.txt, run.txt (from the port's setup thread): started on the desktop's thread (launch_queue)

extern "C" void ps4_shell_launch(char **args) {
    std::vector<std::string> a;
    for (int i = 0; args[i]; i++)
        a.push_back(args[i]);
    std::lock_guard lock(launch_lock);
    launch_queue.push_back(a);
}

extern "C" void ps4_shell_run(const char *) {
    block_wine_signals();
    if (!screen_open()) {
        // no screen: nothing to show; programs from run.txt still run
        for (;;) {
            sleep(1);
            std::vector<std::vector<std::string>> queue;
            {
                std::lock_guard lock(launch_lock);
                queue.swap(launch_queue);
            }
            for (const auto &a : queue)
                launch(a);
        }
    }
    input_open();
    winwin_start();
    load_desktop();
    desktop_started = now_seconds();
    hwkeyboard_open();  // (the desktop's own windows take its keys in both of Settings' modes)
    const Image pointer = make_pointer();
    const Image resize_ew = make_resize_pointer(1, 0), resize_ns = make_resize_pointer(0, 1);
    const Image resize_nwse = make_resize_pointer(1, 1), resize_nesw = make_resize_pointer(1, -1);
    Canvas scene(W, H);
    metrics = &scene;
    bool dirty = true;
    int last_minute = -1;
    double last = now_seconds(), last_job_frame = 0;
    float last_px = -1, last_py = -1;
    const Image *last_cursor = nullptr;
    for (;;) {
        const double t = now_seconds();
        const float dt = static_cast<float>(std::min(0.1, t - last));
        last = t;
        Input in = input_poll(dt);
        last_input = in;

        // the system keyboard: the desktop waits while it's open, then takes its text
        if (keyboard_active()) {
            std::string text;
            const int done = keyboard_poll(text);
            if (done == 0) {
                const timespec pause = {0, 16000000};
                nanosleep(&pause, nullptr);
                continue;
            }
            if (done > 0)
                finish_text(text);
            else
                text_request = TextRequest{};
            hwkeyboard_dialog_closed();
            input_held_back = true;  // until the buttons that closed it are let go
            dirty = true;
        }
        if (input_held_back && !in.held)
            input_held_back = false;
        const bool live = !input_held_back;

        if (program_changed.exchange(false)) {
            dirty = true;
            std::lock_guard lock(program_lock);
            if (!ended_toasts.empty()) {
                toast = Toast{ended_toasts.back(), t + 5};
                ended_toasts.clear();
            }
        }
        {
            std::vector<std::vector<std::string>> queue;
            {
                std::lock_guard lock(launch_lock);
                if (ps4_wine_ready())
                    queue.swap(launch_queue);
            }
            for (const auto &a : queue) {
                const std::string error = launch(a);
                if (!error.empty())
                    show_message("Run " + base_name(a.back()), error);
                dirty = true;
            }
        }
        // a copy, move or delete: its progress a few times a second, its end
        const JobStatus job = job_status();
        if (job.running && t - last_job_frame > 0.25) {
            last_job_frame = t;
            dirty = true;
        }
        if (job.finished) {
            job_acknowledge();
            refresh_all(job.touched);
            if (!job.error.empty())
                show_message(job.title + " stopped", job.error);
            else
                toast = Toast{job.title + ": done", t + 3};
            dirty = true;
        }

        // over a Windows program's window (and nothing of the desktop's open over it): the pointer,
        // its buttons and the wheel go to the program; a button pressed there goes on to it until
        // it's let go. Triangle opens the system keyboard for it.
        if (winwin_changed())
            dirty = true;
        program_text_events(t);
        program_tasks = winwin_tasks();
        if (icons_changed.exchange(false))
            dirty = true;
        // the programs' figures once a second (their taskbar icons; Task Manager shows them changing)
        const double figures_before = processes_when;
        refresh_processes();
        if (processes_when != figures_before &&
            std::any_of(windows.begin(), windows.end(), [](const Window &w) { return w.kind == Kind::TaskManager && !w.minimized; }))
            dirty = true;

        // a USB keyboard: the desktop's own window in front (Files, Notepad, ...), its menus and dialogs
        // take its keys; else, in Settings' Keyboard mode, the program with the focus does (in PS4 +
        // Keyboard mode programs get text from the PS4's keyboard, Triangle). Its Windows key opens
        // Start (Windows+E Files, Windows+D the desktop), Ctrl+Shift+Esc Task Manager.
        {
            static bool win_held = false, win_used = false;
            for (const KeyEvent &k : hwkeyboard_poll(dt)) {
                if (k.vkey == VK_LCONTROL || k.vkey == VK_RCONTROL || k.vkey == VK_CONTROL) key_ctrl = k.down;
                if (k.vkey == VK_LSHIFT || k.vkey == VK_RSHIFT || k.vkey == VK_SHIFT) key_shift = k.down;
                if (k.vkey == VK_LMENU || k.vkey == VK_RMENU || k.vkey == VK_MENU) key_alt = k.down;
                if (k.vkey == VK_CAPITAL && k.down) caps_lock = !caps_lock;
                const bool to_desktop = (desktop_on_top && focused()) || menu.open || dialog.open || popup != Popup::None ||
                                        program_tasks.empty();
                if (k.vkey == 0x5b || k.vkey == 0x5c) {  // VK_LWIN, VK_RWIN: Start when let go alone
                    if (k.down)
                        win_held = true, win_used = false;
                    else if (win_held && !win_used && live)
                        popup = popup == Popup::None ? Popup::Start : Popup::None, dirty = true;
                    if (!k.down)
                        win_held = false;
                    continue;
                }
                if (win_held && k.down) {
                    win_used = true;
                    if (k.vkey == 'E' && live)
                        open_window(Kind::Files, ""), dirty = true;
                    else if (k.vkey == 'D' && live) {
                        for (Window &w : windows)
                            w.minimized = true;
                        dirty = true;
                    }
                    continue;
                }
                if (k.vkey == VK_ESCAPE && key_ctrl && key_shift) {
                    if (k.down && live)
                        open_window(Kind::TaskManager), dirty = true;
                    continue;
                }
                if (keyboard_active() || !live) {
                    if (k.down)
                        key_routes.held_back++;
                    continue;
                }
                if (to_desktop) {
                    if (k.down)
                        key_routes.desktop++, key_routes.to = key_target();
                    desktop_key(k);
                    dirty = true;
                } else if (direct_keyboard) {
                    if (k.down)
                        key_routes.program++;
                    winwin_key(k.vkey, k.down, k.scan, k.extended);
                } else if (k.down) {
                    key_routes.not_sent++;
                }
            }
            log_key_routes(t);
        }

        // a program window's title bar buttons (the desktop's, over Windows' own): pressed here, done
        // when let go over the same one
        // (a desktop window over the programs' takes the pointer where it is)
        const bool over_desktop_window = desktop_on_top && hit(in.x, in.y).rfind("win:", 0) == 0;
        const bool desktop_busy = menu.open || dialog.open || popup != Popup::None || dragging || resizing || dragged.active ||
                                  over_desktop_window;
        uint64_t button_hwnd = 0;
        bool button_maximized = false;
        const int button = desktop_busy || wine_buttons ? -1 : winwin_button_at(in.x, in.y, &button_hwnd, &button_maximized);
        if (button != hovered_wbutton || button_hwnd != hovered_wbutton_hwnd) {
            hovered_wbutton = button, hovered_wbutton_hwnd = button_hwnd;
            dirty = true;
        }
        if (live && button >= 0 && (in.pressed & BTN_CLICK)) {
            pressed_wbutton = button, pressed_wbutton_hwnd = button_hwnd;
            in.pressed &= ~BTN_CLICK;
            dirty = true;
        }
        if (pressed_wbutton >= 0 && (in.released & BTN_CLICK)) {
            if (button == pressed_wbutton && button_hwnd == pressed_wbutton_hwnd)
                program_button(button_hwnd, button, button_maximized);
            pressed_wbutton = -1;
            in.released &= ~BTN_CLICK;
            dirty = true;
        }
        bool full_screen = false;
        uint64_t over_wine = desktop_busy ? 0 : winwin_at(in.x, in.y, &full_screen);
        if (over_wine && !full_screen && in.y >= H - TASKBAR_H)
            over_wine = 0;  // (the taskbar is over it)
        if (button >= 0 || pressed_wbutton >= 0)
            over_wine = 0;
        if (live && (over_wine || button >= 0) && (in.pressed & (BTN_CLICK | BTN_MENU)))
            desktop_on_top = false;  // (a program's window clicked: its windows over the desktop's again)
        if (live && (over_wine || wine_buttons)) {
            constexpr uint32_t MOVE = 0x0001, LEFTDOWN = 0x0002, LEFTUP = 0x0004, RIGHTDOWN = 0x0008,
                               RIGHTUP = 0x0010, WHEEL = 0x0800;
            uint32_t flags = in.moved ? MOVE : 0;
            if (over_wine && (in.pressed & BTN_CLICK)) {
                flags |= LEFTDOWN, wine_buttons |= BTN_CLICK;
                program_click = ProgramClick{over_wine, winwin_pid(over_wine), in.x, in.y, t, false};
            }
            if ((in.released & BTN_CLICK) && (wine_buttons & BTN_CLICK))
                flags |= LEFTUP, wine_buttons &= ~BTN_CLICK;
            if (over_wine && (in.pressed & BTN_MENU))
                flags |= RIGHTDOWN, wine_buttons |= BTN_MENU;
            if ((in.released & BTN_MENU) && (wine_buttons & BTN_MENU))
                flags |= RIGHTUP, wine_buttons &= ~BTN_MENU;
            if (flags)
                winwin_mouse(in.x, in.y, flags);
            if (over_wine && in.scroll != 0)
                winwin_mouse(in.x, in.y, WHEEL, static_cast<int>(-in.scroll * 120));
            if (over_wine && (in.pressed & BTN_SWITCH) && !keyboard_active()) {
                if (keyboard_open("Type into the program", "")) {
                    text_request = TextRequest{};
                    text_request.what = TextFor::Wine;
                }
            }
            in.pressed &= ~(BTN_CLICK | BTN_MENU | BTN_SWITCH);
            in.released &= ~(BTN_CLICK | BTN_MENU);
            in.scroll = 0;
        }
        const std::string now_hover = over_wine || button >= 0 || pressed_wbutton >= 0 ? std::string() : hit(in.x, in.y);
        if (now_hover != hover) {
            hover = now_hover;
            dirty = true;
        }
        if (live && (in.pressed & BTN_CLICK)) {
            note_type_window = 0;  // (a second click, or one elsewhere: Notepad's keyboard waits for none)
            const bool twice = hover == last_click_key && t - last_click_time < 0.5;
            last_click_key = twice ? std::string() : hover;
            last_click_time = t;
            click(hover, twice, in.x, in.y);
            dirty = true;
        }
        if (live && (in.pressed & BTN_MENU)) {
            // right click: the menu for what's under the pointer
            if (menu.open) {
                menu.open = false;
            } else if (!dialog.open) {
                if (popup != Popup::None && hover.rfind("taskbar", 0) != 0)
                    popup = Popup::None;
                std::string target = hover;
                if (target.rfind("win:", 0) == 0)
                    bring_to_front(number_after(target, "win:"));
                open_menu(target, in.x, in.y);
            }
            dirty = true;
        }
        // files dragged in Files: once the pointer has gone far enough, then dropped where it's let go
        if (dragged.pending && (in.held & BTN_CLICK) && !dragged.active &&
            std::hypot(in.x - dragged.x0, in.y - dragged.y0) > 14) {
            dragged.active = true;
            dirty = true;
        }
        if (dragged.active && in.moved)
            dirty = true;
        // (held at a list's top or bottom: it scrolls)
        if (dragged.active && hover.rfind("win:", 0) == 0)
            if (Window *w = find_window(number_after(hover, "win:")); w && w->kind == Kind::Files) {
                const Rect list = files_list(*w);
                if (in.x >= list.x && in.x < list.x + list.w && in.y < list.y + 24 && in.y >= list.y - 8)
                    w->scroll = std::max(0.f, w->scroll - dt * 12), dirty = true;
                else if (in.x >= list.x && in.x < list.x + list.w && in.y > list.y + list.h - 24 && in.y < list.y + list.h + 8)
                    w->scroll += dt * 12, dirty = true;
            }
        // a selection rectangle in Files: the rows it covers, chosen (the list scrolls past its ends)
        for (Window &w : windows) {
            if (!w.banding)
                continue;
            if (!(in.held & BTN_CLICK)) {
                w.banding = false;
                dirty = true;
                continue;
            }
            const Rect list = files_list(w);
            if (in.y < list.y)
                w.scroll -= dt * 12;
            else if (in.y > list.y + list.h)
                w.scroll += dt * 12;
            clamp_scroll(w, static_cast<int>(w.entries.size()), files_rows(w));
            const float row = (std::clamp(in.y, static_cast<float>(list.y), static_cast<float>(list.y + list.h - 1)) - list.y) / ROW_H + w.scroll;
            if (std::fabs(row - w.band_y) * ROW_H > 4 || std::fabs(in.x - w.band_x) > 4) {
                const int from = static_cast<int>(std::floor(std::min(w.band_y, row))), to = static_cast<int>(std::floor(std::max(w.band_y, row)));
                w.chosen.clear();
                for (int i = std::max(0, from); i <= to && i < static_cast<int>(w.entries.size()); i++)
                    w.chosen.insert(w.entries[i].path);
            }
            if (in.moved)
                dirty = true;
        }
        // text chosen in Notepad with the pointer held; a click that chose none: the PS4's keyboard types
        // there, once no second click came (a double click chooses a word)
        if (selecting) {
            Window *w = find_window(selecting);
            if (!w || !w->text || !(in.held & BTN_CLICK)) {
                if (w && w->text && !note_has_selection(*w->text))
                    note_type_window = selecting, note_type_when = t;
                selecting = 0;
            } else if (in.moved) {
                const Rect a = note_area(*w);
                if (in.y < a.y)
                    w->text->top = std::max(0.f, w->text->top - 1);
                else if (in.y > a.y + a.h)
                    w->text->top += 1;
                note_point(*w, in.x, in.y, true);
                dirty = true;
            }
        }
        if (note_type_window && t - note_type_when > 0.45) {
            Window *w = find_window(note_type_window);
            note_type_window = 0;
            if (w && w->text && auto_keyboard && !keyboard_active() && !(direct_keyboard && hwkeyboard_connected()))
                editor_command(*w, "note-type");
            dirty = true;
        }
        if (in.released & BTN_CLICK) {
            if (dragged.active && live) {
                drop_dragged(hit(in.x, in.y));
            } else if (dragged.pending && dragged.click_row >= 0) {
                if (Window *w = find_window(dragged.window))
                    choose_only(*w, dragged.click_row);
            }
            if (dragged.pending || dragged.active)
                dirty = true;
            dragged = DraggedFiles{};
        }
        if (in.released & BTN_CLICK)
            dragging = 0, resizing = 0;
        if (dragging && (in.held & BTN_CLICK) && in.moved) {
            if (Window *w = find_window(dragging)) {
                w->x = std::clamp(static_cast<int>(in.x - drag_dx), -w->w + 120, W - 120);
                w->y = std::clamp(static_cast<int>(in.y - drag_dy), 0, H - TASKBAR_H - TITLE_H);
                dirty = true;
            }
        }
        if (resizing && (in.held & BTN_CLICK) && in.moved) {
            if (Window *w = find_window(resizing)) {
                const int dx = static_cast<int>(in.x - resize_px), dy = static_cast<int>(in.y - resize_py);
                int x = resize_x, y = resize_y, ww = resize_w, hh = resize_h;
                if (resize_edges & 1) {
                    ww = std::max(MIN_W, resize_w - dx);
                    x = resize_x + resize_w - ww;
                }
                if (resize_edges & 2)
                    ww = std::max(MIN_W, resize_w + dx);
                if (resize_edges & 4) {
                    hh = std::max(MIN_H, resize_h - dy);
                    y = resize_y + resize_h - hh;
                    if (y < 0)
                        hh += y, y = 0;
                }
                if (resize_edges & 8)
                    hh = std::max(MIN_H, std::min(resize_h + dy, H - TASKBAR_H - y));
                w->x = x, w->y = y, w->w = ww, w->h = hh;
                dirty = true;
            }
        }
        if (live && (in.pressed & BTN_START)) {
            menu.open = false;
            popup = popup == Popup::None ? Popup::Start : Popup::None;
            dirty = true;
        }
        if (live && (in.pressed & BTN_BACK)) {
            if (dialog.open)
                dialog.open = false;
            else if (menu.open)
                menu.open = false;
            else if (popup == Popup::Power)
                popup = Popup::Start;
            else if (popup != Popup::None)
                popup = Popup::None;
            else if (Window *w = focused(); w && w->kind == Kind::Files)
                go_up(*w);
            dirty = true;
        }
        if (live && (in.pressed & (BTN_SWITCH | BTN_NEXT | BTN_PREV)) && !menu.open && !dialog.open) {
            // the next window to the front (minimized ones too)
            if (!windows.empty()) {
                if (in.pressed & BTN_PREV) {
                    Window back = windows.back();
                    windows.pop_back();
                    windows.insert(windows.begin(), back);
                    windows.back().minimized = false;
                } else {
                    bring_to_front(windows.front().id);
                }
            }
            dirty = true;
        }
        // the D-pad: steps through a menu's items or a dialog's buttons (the pointer goes there),
        // else scrolls like the right stick
        float scroll = in.scroll;
        if (live && menu.open && (in.repeat & (BTN_UP | BTN_DOWN))) {
            const int n = static_cast<int>(menu.items.size());
            int at = hover.rfind("menu:", 0) == 0 ? number_after(hover, "menu:") : -1;
            const int step = (in.repeat & BTN_DOWN) ? 1 : -1;
            for (int k = 0; k < n; k++) {
                at = at < 0 ? (step > 0 ? 0 : n - 1) : (at + step + n) % n;
                if (!menu.items[at].action.empty() && menu.items[at].enabled)
                    break;
            }
            if (at >= 0) {
                const Rect r = menu_item_rect(at);
                input_warp(r.x + 40.f, r.y + r.h / 2.f);
            }
        } else if (live && dialog.open && (in.repeat & (BTN_LEFT | BTN_RIGHT | BTN_UP | BTN_DOWN))) {
            int at = hover.rfind("dialog:", 0) == 0 ? number_after(hover, "dialog:") : -1;
            at = (at + 1) % dialog.buttons();
            const Rect r = dialog_button(at);
            input_warp(r.x + r.w / 2.f, r.y + r.h / 2.f);
        } else if (live && desktop_on_top && popup == Popup::None && focused() &&
                   (focused()->kind == Kind::Files || focused()->kind == Kind::Notepad || focused()->kind == Kind::HexEditor)) {
            // in Files and the editors the D-pad is the arrow keys (it moves through the entries)
            const std::pair<uint32_t, uint16_t> keys[] = {{BTN_UP, VK_UP}, {BTN_DOWN, VK_DOWN}, {BTN_LEFT, VK_LEFT}, {BTN_RIGHT, VK_RIGHT}};
            for (const auto &[button, vk] : keys)
                if (in.repeat & button) {
                    if (focused()->kind == Kind::Files && (vk == VK_LEFT || vk == VK_RIGHT))
                        continue;
                    desktop_key(KeyEvent{vk, 0, true, true});
                    dirty = true;
                }
        } else if (live) {
            if (in.repeat & BTN_DOWN) scroll += 1;
            if (in.repeat & BTN_UP) scroll -= 1;
        }
        if (scroll != 0 && !menu.open && !dialog.open) {
            Window *target = nullptr;
            if (hover.rfind("win:", 0) == 0)
                target = find_window(number_after(hover, "win:"));
            if (!target)
                target = focused();
            if (target) {
                if (target->kind == Kind::Notepad && target->text) {
                    target->text->top = std::max(0.f, target->text->top + scroll * 3);
                } else if (target->kind == Kind::HexEditor && target->hex) {
                    target->scroll += scroll * 3;  // (whole rows; the rest kept for the next turn)
                    const long long rows = static_cast<long long>(target->scroll);
                    target->hex->top = std::max(0ll, target->hex->top + rows);
                    target->scroll -= static_cast<float>(rows);
                } else {
                    target->scroll += target->kind == Kind::Terminal ? -scroll : scroll;
                }
                dirty = true;
            }
        }
        std::time_t tt = std::time(nullptr);
        std::tm local{};
        localtime_r(&tt, &local);
        if (local.tm_min != last_minute) {
            last_minute = local.tm_min;
            dirty = true;
        }
        if (!toast.text.empty() && t > toast.until) {
            toast.text.clear();
            dirty = true;
        }

        // the pointer: an arrow, or a double arrow over a window's edge
        int edges = resizing ? resize_edges : 0;
        if (!edges) {
            const size_t at = hover.find(":edge");
            if (at != std::string::npos)
                edges = std::atoi(hover.c_str() + at + 5);
        }
        const Image *cursor = &pointer;
        int hx = 1, hy = 1;
        if (edges) {
            const bool across = edges & 3, up_down = edges & 12;
            cursor = across && !up_down ? &resize_ew
                   : up_down && !across ? &resize_ns
                   : ((edges & 1) && (edges & 4)) || ((edges & 2) && (edges & 8)) ? &resize_nwse : &resize_nesw;
            hx = hy = cursor->width / 2;
        }

        if (dirty) {
            draw_scene(scene);
            dirty = false;
            present(scene, *cursor, in.x, in.y, hx, hy);
            last_px = in.x, last_py = in.y, last_cursor = cursor;
        } else if (in.x != last_px || in.y != last_py || cursor != last_cursor) {
            present(scene, *cursor, in.x, in.y, hx, hy);
            last_px = in.x, last_py = in.y, last_cursor = cursor;
        } else {
            const timespec pause = {0, 8000000};
            nanosleep(&pause, nullptr);
        }
    }
}
