// WinePS4's pictures for the desktop: a photo in about any format, as RGBA pixels. stb_image first
// (JPEG, PNG, BMP, GIF, TGA, PSD, HDR, PNM), then FFmpeg (WebP, HEIC, TIFF, JPEG 2000, QOI, DDS, SGI,
// PCX: the image decoders of WinePS4's own FFmpeg build).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace shell {

struct Picture {
    int width = 0, height = 0;
    std::vector<uint32_t> pixels;  // RGBA in memory, opaque
};

// false with why in error (a file that isn't a picture, a format nothing here reads, too big)
bool load_picture(const std::string &path, Picture &out, std::string &error);

// Whether the name looks like a picture (by its extension), for Files
bool is_picture_name(const std::string &name);

// The picture scaled to cover width x height (cut at the sides or the top and bottom)
void cover(const Picture &src, int width, int height, uint32_t *out);

} // namespace shell
