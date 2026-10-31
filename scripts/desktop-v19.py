"""WinePS4 v00.19: themes (dark, light) and backgrounds (built-in styles, default photos, the user's
photos in about any format, solid colours) for the desktop (shell/desktop.cpp). One-time edit.
  python scripts/desktop-v19.py shell/desktop.cpp"""
import sys

path = sys.argv[1]
s = open(path, encoding='utf-8').read()


def rep(old, new, count=1):
    global s
    n = s.count(old)
    assert n == count, 'found %d of %r' % (n, old[:80])
    s = s.replace(old, new)


def between(start, end, new):
    """replaces from start up to (not including) end"""
    global s
    a = s.index(start)
    b = s.index(end, a)
    s = s[:a] + new + s[b:]


rep('#include "keyboard.h"\n', '#include "keyboard.h"\n#include "picture.h"\n')

# --- the theme --------------------------------------------------------------------------------
between('// the dark theme\n', 'const char *const DATA_DIR', r'''// the theme: dark or light (Settings > Personalization), set by apply_theme
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

''')

# --- backgrounds: the generated styles, photos, colours -------------------------------------
between('void make_wallpaper() {\n', '// the blurred wallpaper at a screen pixel (bilinear)', r'''// The generated backgrounds: a deep base, two soft glows and a ribbon of light sweeping across
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

''')

# --- icons: pictures ---------------------------------------------------------------------------
rep('enum class Icon { Logo, Search, Folder, Terminal, Settings, Computer, Drive, Usb, Program, File, Power, Pad, Mouse, Up, User, Package };',
    'enum class Icon { Logo, Search, Folder, Terminal, Settings, Computer, Drive, Usb, Program, File, Power, Pad, Mouse, Up, User, Package, Picture, Brush, Info };')
rep('    case Icon::File:\n', r'''    case Icon::Picture:  // a framed landscape
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
''')
rep('        c.fill_circle(cx, cy, s * 0.13f, rgb(0x2a2a2a));', '        c.fill_circle(cx, cy, s * 0.13f, ICON_HOLE);')
rep('        c.fill_circle(cx - s * 0.2f, cy, s * 0.07f, rgb(0x202020));', '        c.fill_circle(cx - s * 0.2f, cy, s * 0.07f, ICON_HOLE);')
rep('        c.fill_circle(cx + s * 0.2f, cy, s * 0.07f, rgb(0x202020));', '        c.fill_circle(cx + s * 0.2f, cy, s * 0.07f, ICON_HOLE);')
rep('        c.fill_rect(x + is * 49 / 100, y + is * 14 / 100, is * 3 / 100, is * 22 / 100, rgb(0x202020));',
    '        c.fill_rect(x + is * 49 / 100, y + is * 14 / 100, is * 3 / 100, is * 22 / 100, ICON_HOLE);')
rep('        entry.icon = entry.dir ? Icon::Folder : (ends_with(name, ".exe") || ends_with(name, ".msi")) ? Icon::Program : Icon::File;',
    '        entry.icon = entry.dir ? Icon::Folder : (ends_with(name, ".exe") || ends_with(name, ".msi")) ? Icon::Program\n'
    '                   : is_picture_name(name) ? Icon::Picture : Icon::File;')

# --- the window, the dialog, the keyboard ----------------------------------------------------
rep('    int selected = -1;\n};', '    int selected = -1;\n    // Settings\n    int page = 0;      // Personalization, Diagnostics, About\n    int bg_tab = 0;    // pictures, colours\n};')
rep('        w.w = 900, w.h = 600, w.x = 500, w.y = 170;', '        w.w = 1100, w.h = 760, w.x = 410, w.y = 110;\n        w.bg_tab = background.kind == BgKind::Color;')
rep('enum class DialogKind { Message, Run, Delete };', 'enum class DialogKind { Message, Run, Delete, Wallpaper };')
rep('enum class TextFor { None, Rename };', 'enum class TextFor { None, Rename, Color };')

rep('''void finish_text(const std::string &text) {
    const TextRequest request = text_request;
    text_request = TextRequest{};
    if (request.what != TextFor::Rename)
        return;''', '''void use_background(const Background &b);

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
    if (request.what != TextFor::Rename)
        return;''')

# a picture: Set as desktop background (double click, right click)
rep('''    if (ends_with(e.name, ".exe") || ends_with(e.name, ".msi"))
        ask_run(e.path);
    else''', '''    if (ends_with(e.name, ".exe") || ends_with(e.name, ".msi"))
        ask_run(e.path);
    else if (is_picture_name(e.name)) {
        show_message("Use " + e.name + " as the background?", "It fills the desktop. Settings > Personalization has the others.");
        dialog.kind = DialogKind::Wallpaper;
        dialog.program = e.path;
    } else''')
rep('''            items.push_back({is_program(e.name) ? "Run" : "Open", "open"});''',
    '''            items.push_back({is_program(e.name) ? "Run" : "Open", "open"});
            if (!e.dir && is_picture_name(e.name))
                items.push_back({"Set as desktop background", "wallpaper"});''')
rep('''        else if (action == "properties") show_properties(e);''', '''        else if (action == "properties") show_properties(e);
        else if (action == "wallpaper") {
            Background b;
            b.kind = BgKind::Photo;
            b.photo = e.path;
            use_background(b);
        }''')
rep('''        else if (i == 0 && d.kind == DialogKind::Delete && !job_delete(d.paths))
            show_message("Delete", "Wait for the copy that is running to finish.");''', '''        else if (i == 0 && d.kind == DialogKind::Delete && !job_delete(d.paths))
            show_message("Delete", "Wait for the copy that is running to finish.");
        else if (i == 0 && d.kind == DialogKind::Wallpaper) {
            Background b;
            b.kind = BgKind::Photo;
            b.photo = d.program;
            use_background(b);
        }''')

# --- Settings ---------------------------------------------------------------------------------
between('// Settings\' toggles\n', '// Dialog buttons\n', r'''// Settings: a page at a time, chosen on the left (as Windows 11's)
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

''')

rep('''        if (w.kind == Kind::Settings)
            for (int i = 0; i < static_cast<int>(std::size(TOGGLES)); i++)
                if (toggle_rect(w, i).contains(px, py))
                    return id + "toggle" + std::to_string(i);''', '''        if (w.kind == Kind::Settings) {
            const std::string part = settings_hit(w, px, py);
            if (!part.empty())
                return id + part;
        }''')

rep('''    } else if (part.rfind("toggle", 0) == 0) {
        toggle_flip(TOGGLES[number_after(part, "toggle")]);
        toast = Toast{"Applies the next time a program starts", now_seconds() + 3};
    }
}''', '''    } else if (w->kind == Kind::Settings) {
        settings_click(*w, part);
    }
}''')
rep('''std::string selected_icon;  // a desktop icon clicked once
''', '''std::string selected_icon;  // a desktop icon clicked once

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
''')

between('void draw_settings(Canvas &c, Window &w) {\n', 'void draw_window(Canvas &c, Window &w, bool active) {', r'''// a two-or-three-way choice (Dark | Light), the chosen one in the accent colour
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

''')

# --- the theme's colours where the dark ones were written out -------------------------------
rep('        c.text_centered(r.x + r.w / 2, r.y + 94, DESKTOP_ICONS[i].label, 16, TEXT);',
    '        // white on any wallpaper, with a soft shadow\n'
    '        c.text_centered(r.x + r.w / 2 + 1, r.y + 95, DESKTOP_ICONS[i].label, 16, rgb(0x000000, 0x90));\n'
    '        c.text_centered(r.x + r.w / 2, r.y + 94, DESKTOP_ICONS[i].label, 16, rgb(0xffffff));')
rep('            c.fill_rect(list.x + 18, ry + 6, 30, ROW_H - 12, rgb(0x1b1b1b, 0x90));',
    '            c.fill_rect(list.x + 18, ry + 6, 30, ROW_H - 12, Color{CONTENT_BG.r, CONTENT_BG.g, CONTENT_BG.b, 0x90});')
rep('        c.fill_round_rect(list.x + list.w - 8, by, 4, bh, 2, rgb(0xffffff, 0x50));', '        c.fill_round_rect(list.x + list.w - 8, by, 4, bh, 2, SCROLLBAR);')
rep('    Color head_col = TEXT2;', '    Color head_col = rgb(0xc8c8c8);  // (the terminal is dark in both themes)')
rep('        head_col = ACCENT;', '        head_col = rgb(0x4cc2ff);')
rep('panel(c, w.x, w.y, w.w, w.h, w.maximized ? 0 : RADIUS, WINDOW_BG, active ? rgb(0x4a4a4a) : BORDER);',
    'panel(c, w.x, w.y, w.w, w.h, w.maximized ? 0 : RADIUS, WINDOW_BG, active ? BORDER_ACTIVE : BORDER);')
rep('acrylic(c, 0, H - TASKBAR_H, W, TASKBAR_H, 0, rgb(0x1c1c1c, 0xc8));', 'acrylic(c, 0, H - TASKBAR_H, W, TASKBAR_H, 0, TASKBAR_TINT);')
rep('c.fill_rect(0, H - TASKBAR_H, W, 1, rgb(0xffffff, 0x18));', 'c.fill_rect(0, H - TASKBAR_H, W, 1, LINE);')
rep('c.fill_round_rect(r.x + 2, r.y, r.w - 4, r.h, 6, rgb(0xffffff, 0x1a));', 'c.fill_round_rect(r.x + 2, r.y, r.w - 4, r.h, 6, TASK_ACTIVE);')
rep('c.fill_round_rect(r.x + 2, r.y, r.w - 4, r.h, 6, rgb(0xffffff, 0x10));', 'c.fill_round_rect(r.x + 2, r.y, r.w - 4, r.h, 6, TASK_HOVER);')
rep('acrylic(c, m.x, m.y, m.w, m.h, RADIUS, rgb(0x242424, 0xd8));', 'acrylic(c, m.x, m.y, m.w, m.h, RADIUS, START_TINT);')
rep('panel(c, m.x + 32, m.y + 28, m.w - 64, 40, 20, rgb(0x1d1d1d), rgb(0x444444));', 'panel(c, m.x + 32, m.y + 28, m.w - 64, 40, 20, SEARCH_BG, SEARCH_BORDER);')
rep('c.fill_rect(m.x, m.y + m.h - 80, m.w, 1, rgb(0xffffff, 0x14));', 'c.fill_rect(m.x, m.y + m.h - 80, m.w, 1, LINE);')
rep('acrylic(c, m.x, m.y + m.h - 79, m.w, 79, 0, rgb(0x1c1c1c, 0xb0));', 'acrylic(c, m.x, m.y + m.h - 79, m.w, 79, 0, START_FOOTER);')
rep('panel(c, a.x - 4, a.y - 4, a.w + 8, 2 * 52 + 4, 8, rgb(0x2c2c2c), rgb(0x454545));', 'panel(c, a.x - 4, a.y - 4, a.w + 8, 2 * 52 + 4, 8, POPUP_BG, POPUP_BORDER);')
rep('panel(c, d.x, d.y, d.w, d.h, RADIUS, rgb(0x2b2b2b), rgb(0x454545));', 'panel(c, d.x, d.y, d.w, d.h, RADIUS, DIALOG_BG, POPUP_BORDER);')
rep('c.fill_round_rect(d.x + 1, d.y + d.h - 88, d.w - 2, 87, RADIUS - 1, rgb(0x202020));', 'c.fill_round_rect(d.x + 1, d.y + d.h - 88, d.w - 2, 87, RADIUS - 1, DIALOG_FOOT);')
rep('c.fill_rect(d.x + 1, d.y + d.h - 88, d.w - 2, 10, rgb(0x202020));', 'c.fill_rect(d.x + 1, d.y + d.h - 88, d.w - 2, 10, DIALOG_FOOT);')
rep('''        const Color fill = danger ? (hot ? rgb(0xe0453a) : CLOSE_RED) : primary ? (hot ? rgb(0x6dd0ff) : ACCENT)
                                                                     : (hot ? rgb(0x3d3d3d) : rgb(0x333333));
        panel(c, b.x, b.y, b.w, b.h, 5, fill, danger ? rgb(0xa32317) : primary ? rgb(0x3aa6e0) : rgb(0x454545));
        const char *label = !primary ? "Cancel" : dialog.kind == DialogKind::Run ? "Run" : dialog.kind == DialogKind::Delete ? "Delete" : "OK";
        c.text_centered(b.x + b.w / 2, b.y + 28, label, 15, primary && !danger ? rgb(0x000000) : TEXT);''',
    '''        const Color fill = danger ? (hot ? rgb(0xe0453a) : CLOSE_RED) : primary ? (hot ? ACCENT_HOT : ACCENT)
                                                                     : (hot ? BUTTON_HOT : BUTTON);
        panel(c, b.x, b.y, b.w, b.h, 5, fill, danger ? rgb(0xa32317) : primary ? ACCENT_BORDER : BUTTON_BORDER);
        const char *label = !primary                               ? "Cancel"
                            : dialog.kind == DialogKind::Run       ? "Run"
                            : dialog.kind == DialogKind::Delete    ? "Delete"
                            : dialog.kind == DialogKind::Wallpaper ? "Set as background"
                                                                   : "OK";
        c.text_centered(b.x + b.w / 2, b.y + 28, label, 15, danger ? rgb(0xffffff) : primary ? ACCENT_TEXT : TEXT);''')
rep('acrylic(c, m.x, m.y, m.w, m.h, RADIUS, rgb(0x2c2c2c, 0xec));', 'acrylic(c, m.x, m.y, m.w, m.h, RADIUS, MENU_TINT);')
rep('c.fill_round_rect(m.x, m.y, m.w, 1, 0, rgb(0xffffff, 0x10));', 'c.fill_round_rect(m.x, m.y, m.w, 1, 0, LINE);')
rep('c.fill_rect(r.x + 8, r.y + r.h / 2, r.w - 16, 1, rgb(0xffffff, 0x18));', 'c.fill_rect(r.x + 8, r.y + r.h / 2, r.w - 16, 1, LINE);')
rep('c.fill_round_rect(r.x, r.y + 1, r.w, r.h - 2, 5, rgb(0xffffff, 0x14));', 'c.fill_round_rect(r.x, r.y + 1, r.w, r.h - 2, 5, HOVER);')
rep('c.text(r.x + 16, r.y + 25, it.label, 15, it.enabled ? TEXT : rgb(0x6e6e6e));', 'c.text(r.x + 16, r.y + 25, it.label, 15, it.enabled ? TEXT : DISABLED);')
rep('acrylic(c, x, y, w, h, RADIUS, rgb(0x262626, 0xe8));', 'acrylic(c, x, y, w, h, RADIUS, CARD_TINT);')
rep('c.fill_round_rect(x + 22, y + 74, w - 44, 6, 3, rgb(0xffffff, 0x20));', 'c.fill_round_rect(x + 22, y + 74, w - 44, 6, 3, TRACK);')
rep('acrylic(c, x, y, tw, 56, RADIUS, rgb(0x262626, 0xe0));', 'acrylic(c, x, y, tw, 56, RADIUS, CARD_TINT);')

# the start: the saved theme and background
rep('    make_wallpaper();\n', '    load_desktop();\n')
open(path, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
