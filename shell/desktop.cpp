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
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fileops.h"
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

// desktop.ini: theme=dark|light, background=style:<n>|photo:<path>|color:<rrggbb>
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
        }
        std::fclose(f);
    }
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

enum class Icon { Logo, Search, Folder, Terminal, Settings, Computer, Drive, Usb, Program, File, Power, Pad, Mouse, Up, User, Package, Picture, Brush, Info };

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
    }
}

// --- files ----------------------------------------------------------------------------------

struct Entry {
    std::string name;
    std::string path;  // the location's root ("" in the list of places)
    bool dir = false;
    long long size = -1;
    Icon icon = Icon::File;
};

// The places: "This PS4" (path "") lists them
std::vector<Entry> find_places() {
    std::vector<Entry> list;
    list.push_back({"Wine C: drive", C_DRIVE, true, -1, Icon::Drive});
    for (int i = 0; i < 8; i++) {
        const std::string usb = "/mnt/usb" + std::to_string(i);
        if (is_dir(usb))
            list.push_back({"USB drive " + std::to_string(i + 1), usb, true, -1, Icon::Usb});
    }
    list.push_back({"PS4 data (/data)", "/data", true, -1, Icon::Drive});
    list.push_back({"WinePS4 package", "/app0/wine", true, -1, Icon::Package});
    return list;
}

// (the USB drives looked at again every 2 s, not every frame)
const std::vector<Entry> &places() {
    static std::vector<Entry> list;
    static double when = -10;
    const double t = now_seconds();
    if (t - when > 2) {
        list = find_places();
        when = t;
    }
    return list;
}

std::vector<Entry> list_dir(const std::string &path) {
    std::vector<Entry> list;
    if (path.empty())
        return places();
    DIR *dir = opendir(path.c_str());
    if (!dir)
        return list;
    while (dirent *e = readdir(dir)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..")
            continue;
        Entry entry;
        entry.name = name;
        entry.path = path == "/" ? "/" + name : path + "/" + name;
        struct stat st{};
        if (stat(entry.path.c_str(), &st) == 0) {
            entry.dir = S_ISDIR(st.st_mode);
            entry.size = entry.dir ? -1 : static_cast<long long>(st.st_size);
        }
        entry.icon = entry.dir ? Icon::Folder : (ends_with(name, ".exe") || ends_with(name, ".msi")) ? Icon::Program
                   : is_picture_name(name) ? Icon::Picture : Icon::File;
        list.push_back(entry);
    }
    closedir(dir);
    std::sort(list.begin(), list.end(), [](const Entry &a, const Entry &b) {
        if (a.dir != b.dir)
            return a.dir;
        return lower(a.name) < lower(b.name);
    });
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
    if (e.path.empty() || (e.dir && e.icon != Icon::Folder))
        return "Drive";
    if (e.dir)
        return "File folder";
    if (ends_with(e.name, ".exe"))
        return "Application";
    if (ends_with(e.name, ".msi"))
        return "Windows Installer";
    const size_t dot = e.name.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
        return "File";
    std::string ext = e.name.substr(dot + 1);
    for (char &ch : ext)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return ext + " File";
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
    {"Hang report", "Writes where Wine is stuck to hang.log when a program runs for 20 s and more", "WINEPS4_HANG_DUMP", "", "0"},
    {"Wine debug messages", "Exceptions and DLL loading in wine.log (WINEDEBUG=+seh,+loaddll)", "WINEDEBUG", "+seh,+loaddll", ""},
    {"Memory log", "Every mapping of WinePS4's memory layer in wine.log (slow)", "WINEPS4_VM_LOG", "1", ""},
};

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

enum class Kind { Files, Terminal, Settings };

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
    std::vector<Entry> entries;
    int selected = -1;
    // Settings
    int page = 0;      // Personalization, Diagnostics, About
    int bg_tab = 0;    // pictures, colours
};

std::vector<Window> windows;  // back to front
int next_id = 1;

enum class Popup { None, Start, Power };
Popup popup = Popup::None;

enum class DialogKind { Message, Run, Delete, Wallpaper };

struct Dialog {
    bool open = false;
    DialogKind kind = DialogKind::Message;  // [OK], [Run] [Cancel], [Delete] [Cancel]
    std::string title, text;                // text: lines split by '\n'
    std::string program;                    // what Run runs
    std::vector<std::string> paths;         // what Delete deletes
    bool two() const { return kind != DialogKind::Message; }
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

// Cut or Copy, for Paste
struct Clipboard {
    std::vector<std::string> paths;
    bool cut = false;
};
Clipboard clipboard;

// What the keyboard's text is for
enum class TextFor { None, Rename, Color, Wine };  // Wine: typed into the program window with the focus
struct TextRequest {
    TextFor what = TextFor::None;
    std::string path;  // the entry to rename
    int window = 0;    // the Files window showing it
};
TextRequest text_request;
bool input_held_back = false;  // after the keyboard closes: until the buttons that closed it are let go

// A window being resized by an edge: which ones (1 left, 2 right, 4 top, 8 bottom)
int resizing = 0, resize_edges = 0;
float resize_px = 0, resize_py = 0;
int resize_x = 0, resize_y = 0, resize_w = 0, resize_h = 0;
constexpr int MIN_W = 480, MIN_H = 300;

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
}

void navigate(Window &w, const std::string &path) {
    w.path = path;
    w.entries = list_dir(path);
    w.selected = -1;
    w.scroll = 0;
    w.title = path.empty() ? "This PS4" : path == C_DRIVE ? "Wine C: drive" : base_name(path);
}

Window *open_window(Kind kind, const std::string &path = std::string()) {
    // Terminal and Settings once; Files as many as wanted
    if (kind != Kind::Files)
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
        w.w = 1120, w.h = 700, w.x = 260 + 30 * (n % 5), w.y = 110 + 30 * (n % 5);
        navigate(w, path);
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
    }
    windows.push_back(w);
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
    }
    return Icon::File;
}

// --- running a program: launch.txt, then the app again ----------------------------------------

void show_message(const std::string &title, const std::string &text) {
    dialog = Dialog{};
    dialog.open = true;
    dialog.title = title;
    dialog.text = text;
}

void ask_run(const std::string &path) {
    show_message("Run " + base_name(path) + "?", "It runs next to the desktop and any other program. What it prints is in Terminal.");
    dialog.kind = DialogKind::Run;
    dialog.program = path;
}

void ask_delete(const std::string &path) {
    show_message("Delete " + base_name(path) + "?",
                 is_dir(path) ? "The folder and everything in it are deleted for good: WinePS4 has no Recycle Bin."
                              : "It is deleted for good: WinePS4 has no Recycle Bin.");
    dialog.kind = DialogKind::Delete;
    dialog.paths = {path};
}

// Starts a program as a process of its own (port/ps4_main.c); "" when it started, else why not
std::string launch(const std::vector<std::string> &args) {
    std::vector<char *> argv;
    for (const std::string &a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    const int pid = ps4_launch_program(argv.data());
    if (pid < 0)
        return errno == EAGAIN ? "Windows is still being set up: try again in a moment." : std::string("It could not start: ") + std::strerror(errno);
    {
        std::lock_guard lock(program_lock);
        // its name: the program's, or the installer's for msiexec /i <file>
        const bool msi = args.size() > 2 && args[0].find("msiexec") != std::string::npos;
        programs_running[pid] = base_name(msi ? args[2] : args[0]);
    }
    program_changed = true;
    return {};
}

// programs to start once Windows is ready: launch.txt, run.txt, and what's run while it's set up
std::mutex launch_lock;
std::vector<std::vector<std::string>> launch_queue;

void run_program(const std::string &path) {
    std::vector<std::string> args;
    if (ends_with(path, ".msi")) {
        args.push_back("C:\\windows\\system32\\msiexec.exe");
        args.push_back("/i");
    }
    args.push_back(path);
    std::vector<std::string> recent = read_lines(RECENT_FILE), out{path};
    for (const std::string &r : recent)
        if (r != path && out.size() < 8)
            out.push_back(r);
    write_lines(RECENT_FILE, out);
    if (!ps4_wine_ready()) {
        // (the first start after an update copies Windows' files): it starts when that's done
        std::lock_guard lock(launch_lock);
        launch_queue.push_back(args);
        toast = Toast{base_name(path) + " starts when Windows is set up", now_seconds() + 6};
        return;
    }
    const std::string error = launch(args);
    if (!error.empty()) {
        show_message("Run " + base_name(path), error);
        return;
    }
    toast = Toast{"Starting " + base_name(path), now_seconds() + 3};
    if (Window *w = open_window(Kind::Terminal))
        w->scroll = 0;
}

void open_entry(Window &w, const Entry &e) {
    if (e.dir) {
        navigate(w, e.path);
        return;
    }
    if (ends_with(e.name, ".exe") || ends_with(e.name, ".msi"))
        ask_run(e.path);
    else if (is_picture_name(e.name)) {
        show_message("Use " + e.name + " as the background?", "It fills the desktop. Settings > Personalization has the others.");
        dialog.kind = DialogKind::Wallpaper;
        dialog.program = e.path;
    } else
        toast = Toast{"WinePS4 runs .exe and .msi files", now_seconds() + 3};
}

void go_up(Window &w) {
    if (w.path.empty())
        return;
    for (const Entry &p : places())
        if (w.path == p.path) {
            navigate(w, "");
            return;
        }
    const size_t slash = w.path.find_last_of('/');
    navigate(w, slash == 0 || slash == std::string::npos ? "" : w.path.substr(0, slash));
}

// --- file commands ----------------------------------------------------------------------------

// The list again (keeping the selection, by name): after a change in the folder
void refresh(Window &w, const std::string &select = std::string()) {
    if (w.kind != Kind::Files)
        return;
    std::string keep = select;
    if (keep.empty() && w.selected >= 0 && w.selected < static_cast<int>(w.entries.size()))
        keep = w.entries[w.selected].path;
    const float scroll = w.scroll;
    w.entries = list_dir(w.path);
    w.scroll = scroll;
    w.selected = -1;
    for (int i = 0; i < static_cast<int>(w.entries.size()); i++)
        if (w.entries[i].path == keep) {
            w.selected = i;
            // into view
            const int rows = 12;
            if (i < w.scroll || i >= w.scroll + rows)
                w.scroll = static_cast<float>(std::max(0, i - 2));
        }
}

void refresh_all(const std::vector<std::string> &folders) {
    for (Window &w : windows)
        if (w.kind == Kind::Files &&
            (folders.empty() || std::find(folders.begin(), folders.end(), w.path) != folders.end()))
            refresh(w);
}

std::string folder_of(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == 0 || slash == std::string::npos ? "/" : path.substr(0, slash);
}

void start_rename(Window &w, const std::string &path) {
    text_request = TextRequest{TextFor::Rename, path, w.id};
    if (!keyboard_open("Rename " + base_name(path), base_name(path))) {
        text_request = TextRequest{};
        show_message("Rename", "The system keyboard did not open.");
    }
}

void use_background(const Background &b);

void finish_text(const std::string &text) {
    const TextRequest request = text_request;
    text_request = TextRequest{};
    if (request.what == TextFor::Color) {
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
    if (request.what == TextFor::Wine) {
        winwin_text(text);
        return;
    }
    if (request.what != TextFor::Rename)
        return;
    std::string name = text;
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    if (name.empty() || name == base_name(request.path))
        return;
    if (name.find('/') != std::string::npos || name == "." || name == "..") {
        show_message("Rename", "A name can't contain / or be . or ..");
        return;
    }
    const std::string to = folder_of(request.path) + "/" + name;
    const std::string error = rename_entry(request.path, to);
    if (!error.empty()) {
        show_message("Rename", error);
        return;
    }
    if (Window *w = find_window(request.window))
        refresh(*w, to);
}

// New folder / New text document: made at once, then named on the keyboard (as Windows does)
void make_new(Window &w, bool folder) {
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

void paste_into(Window &w) {
    if (clipboard.paths.empty() || w.path.empty())
        return;
    if (!job_paste(clipboard.paths, clipboard.cut, w.path)) {
        show_message("Paste", "Wait for the copy that is running to finish.");
        return;
    }
    if (clipboard.cut)
        clipboard = Clipboard{};  // moved: nothing left to paste
}

void show_properties(const Entry &e) {
    struct stat st{};
    const bool ok = stat(e.path.c_str(), &st) == 0;
    std::string text = "Type: " + type_text(e) + "\nLocation: " + (e.path.empty() ? "This PS4" : folder_of(e.path));
    if (ok && !S_ISDIR(st.st_mode)) {
        text += "\nSize: " + size_text(st.st_size);
    } else if (ok) {
        int count = 0;
        if (DIR *dir = opendir(e.path.c_str())) {
            while (dirent *d = readdir(dir))
                if (std::strcmp(d->d_name, ".") && std::strcmp(d->d_name, ".."))
                    count++;
            closedir(dir);
        }
        text += "\nContains: " + std::to_string(count) + (count == 1 ? " item" : " items");
    }
    if (ok) {
        char when[64];
        std::tm local{};
        localtime_r(&st.st_mtime, &local);
        std::strftime(when, sizeof when, "%d/%m/%Y %H:%M", &local);
        text += std::string("\nModified: ") + when;
    }
    show_message(e.name + " properties", text);
}

// --- the right-click menu ---------------------------------------------------------------------

int number_after(const std::string &key, const char *prefix) {
    return std::atoi(key.c_str() + std::strlen(prefix));
}

bool is_program(const std::string &name) { return ends_with(name, ".exe") || ends_with(name, ".msi"); }

void open_menu(const std::string &target, float px, float py) {
    std::vector<MenuItem> items;
    const auto sep = [&] { items.push_back({"", "", false}); };
    if (target == "desktop" || target.rfind("icon:", 0) == 0) {
        if (target != "desktop")
            items.push_back({"Open", "icon-open"}), sep();
        items.push_back({"Open Files", "files"});
        items.push_back({"Open Terminal", "terminal"});
        items.push_back({"Settings", "settings"});
        sep();
        items.push_back({"Refresh", "refresh-all"});
    } else if (target.rfind("taskbar:", 0) == 0) {
        const int i = number_after(target, "taskbar:");
        if (i <= 1) {
            items.push_back({"Files", "files"});
            items.push_back({"Terminal", "terminal"});
            items.push_back({"Settings", "settings"});
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
    } else if (target.rfind("win:", 0) == 0) {
        const int id = number_after(target, "win:");
        const std::string part = target.substr(target.find(':', 4) + 1);
        Window *w = find_window(id);
        if (!w)
            return;
        if (part == "title" || part.rfind("cap", 0) == 0 || part.rfind("edge", 0) == 0) {
            items.push_back({"Restore", "restore", w->maximized});
            items.push_back({"Minimize", "minimize"});
            items.push_back({"Maximize", "maximize", !w->maximized});
            sep();
            items.push_back({"Close", "close"});
        } else if (w->kind == Kind::Files && part.rfind("row", 0) == 0) {
            const int row = number_after(part, "row");
            if (row >= static_cast<int>(w->entries.size()))
                return;
            w->selected = row;
            const Entry &e = w->entries[row];
            const bool place = w->path.empty();
            items.push_back({is_program(e.name) ? "Run" : "Open", "open"});
            if (!e.dir && is_picture_name(e.name))
                items.push_back({"Set as desktop background", "wallpaper"});
            if (!place) {
                sep();
                items.push_back({"Cut", "cut"});
                items.push_back({"Copy", "copy"});
                if (e.dir)
                    items.push_back({"Paste into folder", "paste-into", !clipboard.paths.empty()});
                sep();
                items.push_back({"Rename", "rename"});
                items.push_back({"Delete", "delete"});
            }
            sep();
            items.push_back({"Properties", "properties"});
        } else if (w->kind == Kind::Files) {
            const bool place = w->path.empty();
            items.push_back({"Refresh", "refresh"});
            if (!place) {
                sep();
                items.push_back({"New folder", "new-folder"});
                items.push_back({"New text document", "new-file"});
                sep();
                items.push_back({"Paste", "paste", !clipboard.paths.empty()});
                sep();
                items.push_back({"Up one folder", "up"});
            }
        } else if (w->kind == Kind::Terminal) {
            items.push_back({"Scroll to the end", "terminal-end"});
            sep();
            items.push_back({"Close", "close"});
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

void open_desktop_icon(int i);  // (below, with the icons)

void menu_action(const std::string &action) {
    const std::string target = menu.target;
    menu.open = false;
    Window *w = nullptr;
    std::string part;
    if (target.rfind("win:", 0) == 0) {
        w = find_window(number_after(target, "win:"));
        part = target.substr(target.find(':', 4) + 1);
    }
    const Entry *entry = w && w->kind == Kind::Files && w->selected >= 0 && w->selected < static_cast<int>(w->entries.size())
                             ? &w->entries[w->selected] : nullptr;
    if (action == "files") open_window(Kind::Files, "");
    else if (action == "terminal") open_window(Kind::Terminal);
    else if (action == "settings") open_window(Kind::Settings);
    else if (action == "refresh-all") refresh_all({});
    else if (action == "restart") { ps4_end_programs(); sceSystemServiceLoadExec("/app0/eboot.bin", nullptr); }
    else if (action == "exit") { ps4_boot_trace("shell: exit to the PS4 menu"); ps4_end_programs(); exit(0); }
    else if (action == "icon-open") open_desktop_icon(number_after(target, "icon:"));
    else if (action == "close-kind") {
        const int i = number_after(target, "taskbar:");
        const Kind kind = i == 2 ? Kind::Files : i == 3 ? Kind::Terminal : Kind::Settings;
        windows.erase(std::remove_if(windows.begin(), windows.end(), [&](const Window &x) { return x.kind == kind; }), windows.end());
    }
    if (!w)
        return;
    if (action == "restore" && w->maximized) toggle_maximize(*w);
    else if (action == "maximize" && !w->maximized) toggle_maximize(*w);
    else if (action == "minimize") w->minimized = true;
    else if (action == "close") close_window(w->id);
    else if (action == "terminal-end") w->scroll = 0;
    else if (action == "refresh") refresh(*w);
    else if (action == "up") go_up(*w);
    else if (action == "new-folder") make_new(*w, true);
    else if (action == "new-file") make_new(*w, false);
    else if (action == "paste") paste_into(*w);
    else if (entry) {
        const Entry e = *entry;
        if (action == "open") open_entry(*w, e);
        else if (action == "cut" || action == "copy") {
            clipboard = Clipboard{{e.path}, action == "cut"};
            toast = Toast{std::string(action == "cut" ? "Cut " : "Copied ") + e.name + ": Paste it in a folder", now_seconds() + 3};
        } else if (action == "paste-into") {
            if (!clipboard.paths.empty() && job_paste(clipboard.paths, clipboard.cut, e.path) && clipboard.cut)
                clipboard = Clipboard{};
        } else if (action == "rename") start_rename(*w, e.path);
        else if (action == "delete") ask_delete(e.path);
        else if (action == "properties") show_properties(e);
        else if (action == "wallpaper") {
            Background b;
            b.kind = BgKind::Photo;
            b.photo = e.path;
            use_background(b);
        }
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
int taskbar_count() { return 1 + 1 + 3; }  // Start, Search, the apps
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
    {"C: drive", Icon::Drive, 3}, {"Wine test", Icon::Program, 4}, {"32-bit test", Icon::Program, 6},
    {"Window test", Icon::Program, 7},
};
// the pins share one row (narrower as there are more), so "Recent programs" below stays clear
Rect pin_rect(int i) {
    const Rect m = start_menu_rect();
    const int n = static_cast<int>(std::size(PINS)), step = (m.w - 64) / std::max(6, n);
    return Rect{m.x + 32 + i * step, m.y + 130, step - 3, 96};
}
Rect recent_rect(int i) {
    const Rect m = start_menu_rect();
    return Rect{m.x + 32 + (i % 2) * 310, m.y + 330 + (i / 2) * 74, 300, 64};
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
const DesktopIcon DESKTOP_ICONS[] = {{"This PS4", Icon::Computer, 5}, {"C: drive", Icon::Drive, 3}, {"Terminal", Icon::Terminal, 1}};
Rect desktop_icon_rect(int i) { return Rect{16, 16 + i * 116, 104, 108}; }

void start_action(int action) {
    popup = Popup::None;
    switch (action) {
    case 0: open_window(Kind::Files); break;
    case 1: open_window(Kind::Terminal); break;
    case 2: open_window(Kind::Settings); break;
    case 3: open_window(Kind::Files, C_DRIVE); break;
    case 4: ask_run("/app0/wine/hello.exe"); break;
    case 5: open_window(Kind::Files, ""); break;
    case 6: ask_run("/app0/wine/hello32.exe"); break;
    case 7: ask_run("/app0/wine/window.exe"); break;
    }
}

void open_desktop_icon(int i) {
    if (i >= 0 && i < static_cast<int>(std::size(DESKTOP_ICONS)))
        start_action(DESKTOP_ICONS[i].action);
}

Rect caption_button(const Window &w, int i) {  // 0 close, 1 maximize, 2 minimize
    return Rect{w.x + w.w - 46 * (i + 1), w.y, 46, TITLE_H};
}

// Files' parts
constexpr int FILES_NAV_W = 230, FILES_TOOLBAR_H = 52, FILES_HEADER_H = 34, ROW_H = 36;
Rect files_list(const Window &w) {
    return Rect{w.x + FILES_NAV_W, w.y + TITLE_H + FILES_TOOLBAR_H + FILES_HEADER_H, w.w - FILES_NAV_W - 1,
                w.h - TITLE_H - FILES_TOOLBAR_H - FILES_HEADER_H - 1};
}
Rect files_up_button(const Window &w) { return Rect{w.x + 12, w.y + TITLE_H + 8, 40, 36}; }
Rect files_nav_item(const Window &w, int i) { return Rect{w.x + 8, w.y + TITLE_H + FILES_TOOLBAR_H + 8 + i * 40, FILES_NAV_W - 16, 38}; }
int files_rows(const Window &w) { return std::max(1, files_list(w).h / ROW_H); }
void clamp_scroll(Window &w, int count, int visible) {
    w.scroll = std::clamp(w.scroll, 0.f, static_cast<float>(std::max(0, count - visible)));
}

// Settings: a page at a time, chosen on the left (as Windows 11's)
const char *const SETTINGS_PAGES[] = {"Personalization", "Diagnostics", "About"};
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
Rect toggle_rect(const Window &w, int i) { const Rect a = settings_area(w); return Rect{a.x, a.y + 60 + i * 92, a.w, 80}; }

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
    if (w.page == 0) {
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
    } else if (w.page == 1) {
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
Rect dialog_button(int i) {  // 0 primary (OK, Run, Delete), 1 Cancel
    const Rect d = dialog_rect();
    const int bw = dialog.two() ? 262 : 540;
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

std::string hit(float px, float py) {
    if (dialog.open) {
        for (int i = 0; i < (dialog.two() ? 2 : 1); i++)
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
        for (int i = 0; i < taskbar_count(); i++)
            if (taskbar_button(i).contains(px, py))
                return "taskbar:" + std::to_string(i);
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
            if (files_up_button(w).contains(px, py))
                return id + "up";
            const auto &nav = places();
            for (int i = 0; i < static_cast<int>(nav.size()) + 1; i++)
                if (files_nav_item(w, i).contains(px, py))
                    return id + "nav" + std::to_string(i);
            const Rect list = files_list(w);
            if (list.contains(px, py)) {
                const int row = static_cast<int>((py - list.y) / ROW_H) + static_cast<int>(w.scroll);
                if (row < static_cast<int>(w.entries.size()))
                    return id + "row" + std::to_string(row);
                return id + "list";
            }
        }
        if (w.kind == Kind::Settings) {
            const std::string part = settings_hit(w, px, py);
            if (!part.empty())
                return id + part;
        }
        return id + "body";
    }
    for (int i = 0; i < static_cast<int>(std::size(DESKTOP_ICONS)); i++)
        if (desktop_icon_rect(i).contains(px, py))
            return "icon:" + std::to_string(i);
    return "desktop";
}

std::string selected_icon;  // a desktop icon clicked once

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
    }
}

void click(const std::string &key, bool twice, float px, float py) {
    if (key.rfind("dialog:", 0) == 0) {
        const int i = number_after(key, "dialog:");
        const Dialog d = dialog;
        dialog.open = false;
        if (i == 0 && d.kind == DialogKind::Run)
            run_program(d.program);
        else if (i == 0 && d.kind == DialogKind::Delete && !job_delete(d.paths))
            show_message("Delete", "Wait for the copy that is running to finish.");
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
            ask_run(recent[i]);
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
    if (key.rfind("icon:", 0) == 0) {
        if (twice)
            start_action(DESKTOP_ICONS[number_after(key, "icon:")].action), selected_icon.clear();
        else
            selected_icon = key;
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
        close_window(id);
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
    } else if (part == "up") {
        go_up(*w);
    } else if (part.rfind("nav", 0) == 0) {
        const int i = number_after(part, "nav");
        navigate(*w, i == 0 ? std::string() : places()[i - 1].path);
    } else if (part.rfind("row", 0) == 0) {
        const int row = number_after(part, "row");
        if (twice && w->selected == row) {
            const Entry e = w->entries[row];
            open_entry(*w, e);
        } else {
            w->selected = row;
        }
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

void draw_files(Canvas &c, Window &w) {
    const std::string id = "win:" + std::to_string(w.id) + ":";
    // navigation pane
    c.fill_rect(w.x + 1, w.y + TITLE_H, FILES_NAV_W - 1, w.h - TITLE_H - 1, WINDOW_BG);
    const auto &nav = places();
    int current_place = 0;  // the place holding the folder (the longest path that matches)
    size_t best = 0;
    for (int i = 0; i < static_cast<int>(nav.size()); i++) {
        const std::string &p = nav[i].path;
        if (w.path.rfind(p, 0) == 0 && (w.path.size() == p.size() || w.path[p.size()] == '/') && p.size() > best)
            current_place = i + 1, best = p.size();
    }
    for (int i = 0; i < static_cast<int>(nav.size()) + 1; i++) {
        const Rect r = files_nav_item(w, i);
        const bool current = i == current_place;
        if (current)
            c.fill_round_rect(r.x, r.y, r.w, r.h, 5, SELECTED);
        else if (hover == id + "nav" + std::to_string(i))
            c.fill_round_rect(r.x, r.y, r.w, r.h, 5, HOVER);
        if (current)
            c.fill_round_rect(r.x, r.y + 10, 3, r.h - 20, 2, ACCENT);
        draw_icon(c, i == 0 ? Icon::Computer : nav[i - 1].icon, r.x + 22, r.y + r.h / 2.f, 20);
        c.text(r.x + 42, r.y + 25, i == 0 ? "This PS4" : nav[i - 1].name, 15, TEXT, Weight::Regular, r.w - 50);
    }
    // toolbar: up, then the address
    const int cx = w.x + FILES_NAV_W;
    c.fill_rect(cx, w.y + TITLE_H, w.w - FILES_NAV_W - 1, w.h - TITLE_H - 1, CONTENT_BG);
    const Rect up = files_up_button(w);
    if (hover == id + "up" && !w.path.empty())
        c.fill_round_rect(up.x, up.y, up.w, up.h, 5, HOVER);
    draw_icon(c, Icon::Up, up.x + up.w / 2.f, up.y + up.h / 2.f, 22);
    const int ax = up.x + up.w + 12;
    panel(c, ax, w.y + TITLE_H + 8, w.x + w.w - 16 - ax, 36, 5, CONTROL, BORDER);
    std::string address = w.path.empty() ? "This PS4" : w.path;
    c.text(ax + 14, w.y + TITLE_H + 32, address, 15, TEXT, Weight::Regular, w.x + w.w - 30 - ax - 14);
    // the list's header
    const int hy = w.y + TITLE_H + FILES_TOOLBAR_H;
    const int name_w = (w.w - FILES_NAV_W) * 50 / 100, type_w = (w.w - FILES_NAV_W) * 25 / 100;
    c.text(cx + 52, hy + 23, "Name", 14, TEXT2);
    c.text(cx + name_w, hy + 23, "Type", 14, TEXT2);
    c.text(cx + name_w + type_w, hy + 23, "Size", 14, TEXT2);
    c.fill_rect(cx + 12, hy + FILES_HEADER_H - 1, w.w - FILES_NAV_W - 24, 1, BORDER);
    // the rows
    const Rect list = files_list(w);
    const int visible = files_rows(w);
    clamp_scroll(w, static_cast<int>(w.entries.size()), visible);
    const int first = static_cast<int>(w.scroll);
    if (w.entries.empty())
        c.text_centered(list.x + list.w / 2, list.y + 60, w.path.empty() ? "" : "This folder is empty", 15, TEXT3);
    for (int i = first; i < static_cast<int>(w.entries.size()) && i < first + visible; i++) {
        const Entry &e = w.entries[i];
        const int ry = list.y + (i - first) * ROW_H;
        if (w.selected == i)
            c.fill_round_rect(list.x + 8, ry + 1, list.w - 16, ROW_H - 2, 5, SELECTED);
        else if (hover == id + "row" + std::to_string(i))
            c.fill_round_rect(list.x + 8, ry + 1, list.w - 16, ROW_H - 2, 5, HOVER);
        draw_icon(c, e.icon, list.x + 32, ry + ROW_H / 2.f, 22);
        // (a cut entry is shown faded until it is pasted)
        const bool cut = clipboard.cut && std::find(clipboard.paths.begin(), clipboard.paths.end(), e.path) != clipboard.paths.end();
        if (cut)
            c.fill_rect(list.x + 18, ry + 6, 30, ROW_H - 12, Color{CONTENT_BG.r, CONTENT_BG.g, CONTENT_BG.b, 0x90});
        c.text(list.x + 52, ry + 24, e.name, 15, cut ? TEXT3 : TEXT, Weight::Regular, name_w - 60);
        c.text(list.x + name_w, ry + 24, type_text(e), 15, TEXT2, Weight::Regular, type_w - 10);
        c.text(list.x + name_w + type_w, ry + 24, size_text(e.size), 15, TEXT2, Weight::Regular, list.w - name_w - type_w - 20);
    }
    // the scroll bar
    if (static_cast<int>(w.entries.size()) > visible) {
        const float total = static_cast<float>(w.entries.size());
        const int bh = std::max(30, static_cast<int>(list.h * visible / total));
        const int by = list.y + static_cast<int>((list.h - bh) * (w.scroll / (total - visible)));
        c.fill_round_rect(list.x + list.w - 8, by, 4, bh, 2, SCROLLBAR);
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

void draw_settings(Canvas &c, Window &w) {
    const std::string id = "win:" + std::to_string(w.id) + ":";
    // the pages, on the left
    c.fill_rect(w.x + 1, w.y + TITLE_H, SET_NAV_W, w.h - TITLE_H - 1, WINDOW_BG);
    draw_icon(c, Icon::User, w.x + 40, w.y + TITLE_H + 34, 36);
    c.text(w.x + 70, w.y + TITLE_H + 32, "PS4 user", 16, TEXT, Weight::Bold);
    c.text(w.x + 70, w.y + TITLE_H + 52, "WinePS4 " WINEPS4_VERSION, 13, TEXT3);
    const Icon page_icons[] = {Icon::Brush, Icon::Settings, Icon::Info};
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
    if (w.page == 0) {
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
    } else if (w.page == 1) {
        for (int i = 0; i < static_cast<int>(std::size(TOGGLES)); i++) {
            const Rect r = toggle_rect(w, i);
            panel(c, r.x, r.y, r.w, r.h, 6, hover == id + "toggle" + std::to_string(i) ? CARD_HOT : CARD, CARD_BORDER);
            c.text(r.x + 22, r.y + 33, TOGGLES[i].title, 16, TEXT);
            c.text(r.x + 22, r.y + 58, TOGGLES[i].detail, 14, TEXT3, Weight::Regular, r.w - 140);
            draw_switch(c, r, toggle_on(TOGGLES[i]));
        }
        c.text_wrapped(a.x, a.y + 60 + 3 * 92 + 30,
                       "The switches go to /data/WinePS4/env.txt and apply the next time a program starts. "
                       "Logs: /data/WinePS4/wine.log, crash.log, hang.log.",
                       14, TEXT3, a.w, 22);
    } else {
        int y = a.y + 80;
        const char *const lines[] = {
            "WinePS4 " WINEPS4_VERSION " runs Windows programs on the PS4: Wine 11.0 with its 64-bit and 32-bit",
            "(WoW64) sides, wineserver as a thread of the app, Windows' memory in the PS4's direct memory.",
            "",
            "Wine (LGPL 2.1), FFmpeg (LGPL 2.1), stb_image and stb_truetype (public domain),",
            "Roboto (SIL Open Font License). The default photos are WinePS4's own.",
            "",
            "Data: /data/WinePS4 (the Windows C: drive is /data/WinePS4/prefix/drive_c).",
        };
        for (const char *line : lines) {
            c.text(a.x, y, line, 15, TEXT2, Weight::Regular, a.w);
            y += 28;
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
    }
    draw_caption(c, w, active);
}

void draw_taskbar(Canvas &c) {
    acrylic(c, 0, H - TASKBAR_H, W, TASKBAR_H, 0, TASKBAR_TINT);
    c.fill_rect(0, H - TASKBAR_H, W, 1, LINE);
    Window *top = focused();
    for (int i = 0; i < taskbar_count(); i++) {
        const Rect r = taskbar_button(i);
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
    c.text(m.x + 52, m.y + 312, "Recent programs", 16, TEXT, Weight::Bold);
    const auto recent = recent_programs();
    if (recent.empty())
        c.text(m.x + 52, m.y + 350, "Programs you run show here.", 14, TEXT3);
    for (int i = 0; i < static_cast<int>(recent.size()); i++) {
        const Rect r = recent_rect(i);
        if (hover == "recent:" + std::to_string(i))
            c.fill_round_rect(r.x, r.y, r.w, r.h, 6, HOVER);
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
    for (int i = 0; i < (dialog.two() ? 2 : 1); i++) {
        const Rect b = dialog_button(i);
        const bool primary = i == 0;
        const bool hot = hover == "dialog:" + std::to_string(i);
        const bool danger = primary && dialog.kind == DialogKind::Delete;
        const Color fill = danger ? (hot ? rgb(0xe0453a) : CLOSE_RED) : primary ? (hot ? ACCENT_HOT : ACCENT)
                                                                     : (hot ? BUTTON_HOT : BUTTON);
        panel(c, b.x, b.y, b.w, b.h, 5, fill, danger ? rgb(0xa32317) : primary ? ACCENT_BORDER : BUTTON_BORDER);
        const char *label = !primary                               ? "Cancel"
                            : dialog.kind == DialogKind::Run       ? "Run"
                            : dialog.kind == DialogKind::Delete    ? "Delete"
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
    for (Window &w : windows)
        if (!w.minimized)
            draw_window(c, w, &w == top && popup == Popup::None);
    winwin_draw(c);  // Windows programs' windows: over the desktop's own
    draw_taskbar(c);
    if (popup != Popup::None)
        draw_start_menu(c);
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
    const Image pointer = make_pointer();
    const Image resize_ew = make_resize_pointer(1, 0), resize_ns = make_resize_pointer(0, 1);
    const Image resize_nwse = make_resize_pointer(1, 1), resize_nesw = make_resize_pointer(1, -1);
    Canvas scene(W, H);
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
        const bool desktop_busy = menu.open || dialog.open || popup != Popup::None || dragging || resizing;
        const uint64_t over_wine = desktop_busy ? 0 : winwin_at(in.x, in.y);
        if (live && (over_wine || wine_buttons)) {
            constexpr uint32_t MOVE = 0x0001, LEFTDOWN = 0x0002, LEFTUP = 0x0004, RIGHTDOWN = 0x0008,
                               RIGHTUP = 0x0010, WHEEL = 0x0800;
            uint32_t flags = in.moved ? MOVE : 0;
            if (over_wine && (in.pressed & BTN_CLICK))
                flags |= LEFTDOWN, wine_buttons |= BTN_CLICK;
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
        const std::string now_hover = over_wine ? std::string() : hit(in.x, in.y);
        if (now_hover != hover) {
            hover = now_hover;
            dirty = true;
        }
        if (live && (in.pressed & BTN_CLICK)) {
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
            at = dialog.two() ? (at == 0 ? 1 : 0) : 0;
            const Rect r = dialog_button(at);
            input_warp(r.x + r.w / 2.f, r.y + r.h / 2.f);
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
                target->scroll += target->kind == Kind::Terminal ? -scroll : scroll;
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
