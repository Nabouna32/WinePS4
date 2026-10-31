// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// PS4: a small software canvas for the launcher (ps4_canvas.cpp): shapes, pictures and text in the
// console's own fonts, drawn by the CPU into a 1920x1080 RGBA picture.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ps4ui {

// R, G, B, A in memory: VideoOut's A8B8G8R8
struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

constexpr Color rgb(uint32_t hex, uint8_t a = 255) {
    return Color{ static_cast<uint8_t>(hex >> 16), static_cast<uint8_t>(hex >> 8), static_cast<uint8_t>(hex), a };
}

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels; // RGBA in memory
    bool empty() const { return pixels.empty(); }
};

// Loads a PNG/JPEG, scaled to fit width x height (0 = as it is)
Image load_image(const std::string &path, int width = 0, int height = 0);
// A picture's size in pixels, without loading it; false if it can't be read
bool image_size(const std::string &path, int &width, int &height);
// Its colours multiplied by its alpha: what Canvas::blit and blit_scaled take (a sprite)
void premultiply(Image &image);
// Scaled to width x height, each pixel the average of the pixels it covers (for shrinking)
Image shrink_image(const Image &image, int width, int height);

enum class Weight {
    Regular,
    Bold,
};

// Text as a sprite (premultiplied), with a soft shadow under it when shadow > 0: its baseline
// at `baseline` from its top, which the result's height leaves room under
struct TextSprite {
    Image image;
    int baseline = 0;
};
TextSprite render_text(std::string_view utf8, int px, Color c, Weight weight, int max_width, uint8_t shadow);

class Canvas {
public:
    Canvas(int width, int height);

    int width() const { return w; }
    int height() const { return h; }
    const uint32_t *data() const { return pixels.data(); }
    uint32_t *data() { return pixels.data(); }
    // What it shows, as a picture (to draw again later)
    Image snapshot() const;

    // The same size: all of `src`, moved by dx, dy (what it doesn't cover stays as it was)
    void copy_from(const Canvas &src, int dx = 0, int dy = 0);
    // Premultiplied sprites: at (x, y), or centered on (cx, cy) at `scale`, faded by alpha
    void blit(const Image &sprite, int x, int y, uint8_t alpha = 255);
    void blit_scaled(const Image &sprite, float cx, float cy, float scale, uint8_t alpha = 255);

    void clear_gradient(Color top, Color bottom);
    void fill_rect(int x, int y, int width, int height, Color c);
    void fill_round_rect(int x, int y, int width, int height, int radius, Color c);
    void fill_circle(float cx, float cy, float radius, Color c);
    void ring(float cx, float cy, float radius, float thickness, Color c);
    void line(float x0, float y0, float x1, float y1, float thickness, Color c);
    void draw_image(const Image &image, int x, int y, uint8_t alpha = 255);
    // Centered on (cx, cy), inside a circle of that radius (a bubble), its edge smoothed
    void draw_image_circle(const Image &image, float cx, float cy, float radius, uint8_t alpha = 255);

    // Text with its baseline at y; returns the width drawn. Characters the console's Latin font
    // lacks come from its Japanese, Chinese and Korean fonts.
    int text(int x, int y, std::string_view utf8, int px, Color c, Weight weight = Weight::Regular, int max_width = 0);
    int text_width(std::string_view utf8, int px, Weight weight = Weight::Regular);
    // Centered on cx
    void text_centered(int cx, int y, std::string_view utf8, int px, Color c, Weight weight = Weight::Regular);
    // Wrapped into lines of at most max_width; returns the y after the last line
    int text_wrapped(int x, int y, std::string_view utf8, int px, Color c, int max_width, int line_height, Weight weight = Weight::Regular);

    // The PlayStation button symbols, in their colours
    void cross_button(float cx, float cy, float r);
    void circle_button(float cx, float cy, float r);
    void triangle_button(float cx, float cy, float r);
    void square_button(float cx, float cy, float r);
    void pill_button(int x, int y, std::string_view label, int px);

private:
    void blend(int x, int y, Color c, uint8_t coverage);

    int w;
    int h;
    std::vector<uint32_t> pixels;
};

// The canvas's big jobs run in parts on a few threads of its own and the caller (the launcher has
// the console to itself): fn over the rows [y0, y1), here at once when the job is small (pixels).
// A thread drawing in the background (a card being made) does its own work alone: set_serial(true).
void parallel_rows(int y0, int y1, size_t pixels, const std::function<void(int, int)> &fn);
void set_serial(bool serial);

// Whether there are fonts (the text is missing otherwise)
bool fonts_loaded();

// Where the PS Vita's fonts are (Vita3K's sa0/data/font/pvf/, from the font package): the first
// choice for text. The fonts load again, from there if it has them, for the next text.
void set_vita_font_dir(const std::string &dir);

} // namespace ps4ui
