// WinePS4's program icons (exeicon.h). A program keeps its icon in its resources: a group
// (RT_GROUP_ICON) listing the icon's images, their sizes and colour depths, by the RT_ICON resource
// holding each; an .ico file is the same list with the images' places in it. Only the headers, the
// resource directories and the image chosen are read: an installer can be 100+ MB.
#include "exeicon.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "third_party/stb_image.h"

namespace shell {
namespace {

constexpr int MAX_SIDE = 1024;           // the largest image read (Windows' own stop at 256)
constexpr uint32_t MAX_DATA = 4u << 20;  // the most bytes read for one resource or image
constexpr size_t MAX_TRIES = 8;          // images tried in turn when the best ones don't decode
constexpr uint32_t RT_ICON = 3, RT_GROUP_ICON = 14;
constexpr uint32_t SUBDIRECTORY = 0x80000000u;  // in a resource entry's offset

uint16_t u16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

uint32_t u32(const uint8_t *p) {
    return p[0] | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

// The file, read in pieces where they are; nothing past its end
class File {
public:
    explicit File(const std::string &path) : f(std::fopen(path.c_str(), "rb")) {
        if (f && std::fseek(f, 0, SEEK_END) == 0)
            size = std::ftell(f);
    }
    ~File() {
        if (f)
            std::fclose(f);
    }
    File(const File &) = delete;
    File &operator=(const File &) = delete;

    bool read(uint64_t offset, void *out, size_t n) {
        if (!holds(offset, n))
            return false;
        return n == 0 || (std::fseek(f, static_cast<long>(offset), SEEK_SET) == 0 && std::fread(out, 1, n, f) == n);
    }
    bool read(uint64_t offset, size_t n, std::vector<uint8_t> &out) {
        if (!holds(offset, n))
            return false;
        out.resize(n);
        return read(offset, out.data(), n);
    }

private:
    bool holds(uint64_t offset, size_t n) const {
        return f && size >= 0 && offset <= static_cast<uint64_t>(size) && n <= static_cast<uint64_t>(size) - offset;
    }

    FILE *f;
    long size = -1;
};

struct Section {
    uint32_t address, size, offset;  // its RVA; its bytes in the file and where they are
};

// A PE file's resources: a tree of directories (type, name, language) whose entries lead to the
// next directory or, at the end, to the data. Offsets in it count from where it starts.
class Resources {
public:
    explicit Resources(File &f) : file(f) {}

    // false: not a PE file, or no icons in it
    bool open();
    // The first group (Windows' choice: names sort before IDs, then IDs from the lowest), in its
    // first language
    bool group(std::vector<uint8_t> &data, uint32_t &language);
    // An RT_ICON image, in the group's language if there is one in it
    bool icon(uint32_t id, uint32_t language, std::vector<uint8_t> &data);

private:
    struct Entry {
        uint32_t name, offset;  // an ID (a name's string has the top bit set); see SUBDIRECTORY
    };

    bool read(uint64_t rva, void *out, size_t n);
    bool directory(const Entry &parent, std::vector<Entry> &entries);
    bool leaf(const Entry &entry, std::vector<uint8_t> &data);

    static const Entry *find(const std::vector<Entry> &entries, uint32_t id) {
        for (const Entry &e : entries)
            if (e.name == id)
                return &e;
        return nullptr;
    }

    File &file;
    std::vector<Section> sections;
    uint32_t root = 0;                 // the tree's RVA
    std::vector<Entry> groups, icons;  // the RT_GROUP_ICON and RT_ICON directories
};

bool Resources::open() {
    uint8_t dos[64];
    if (!file.read(0, dos, sizeof dos) || dos[0] != 'M' || dos[1] != 'Z')
        return false;
    // "PE\0\0" and the file header, then the optional header as far as the data directories
    const uint64_t nt = u32(dos + 0x3c);
    uint8_t head[24], optional[136] = {};
    if (!file.read(nt, head, sizeof head) || std::memcmp(head, "PE\0\0", 4) != 0)
        return false;
    const size_t count = u16(head + 6), optional_size = u16(head + 20);
    if (!file.read(nt + 24, optional, std::min(optional_size, sizeof optional)))
        return false;
    // the resources are data directory 2, after the fields PE32 and PE32+ (64-bit) differ in
    const uint16_t magic = u16(optional);
    const size_t dirs = magic == 0x10b ? 96 : magic == 0x20b ? 112 : 0;
    if (!dirs || optional_size < dirs + 24 || u32(optional + dirs - 4) < 3)
        return false;
    root = u32(optional + dirs + 16);
    std::vector<uint8_t> table;
    if (!root || !file.read(nt + 24 + optional_size, count * 40, table))
        return false;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *s = table.data() + i * 40;
        sections.push_back({u32(s + 12), u32(s + 16), u32(s + 20)});
    }
    std::vector<Entry> types;
    if (!directory({0, SUBDIRECTORY}, types))
        return false;
    const Entry *g = find(types, RT_GROUP_ICON), *i = find(types, RT_ICON);
    return g && i && directory(*g, groups) && directory(*i, icons) && !groups.empty();
}

// n bytes at an RVA, all of them in what one section has in the file
bool Resources::read(uint64_t rva, void *out, size_t n) {
    for (const Section &s : sections)
        if (rva >= s.address && rva - s.address + n <= s.size)
            return file.read(s.offset + (rva - s.address), out, n);
    return false;
}

bool Resources::directory(const Entry &parent, std::vector<Entry> &entries) {
    if (!(parent.offset & SUBDIRECTORY))
        return false;
    const uint64_t at = static_cast<uint64_t>(root) + (parent.offset & ~SUBDIRECTORY);
    uint8_t head[16];
    if (!read(at, head, sizeof head))
        return false;
    const size_t count = static_cast<size_t>(u16(head + 12)) + u16(head + 14);  // named, then IDs
    std::vector<uint8_t> raw(count * 8);
    if (!read(at + 16, raw.data(), raw.size()))
        return false;
    entries.resize(count);
    for (size_t i = 0; i < count; i++)
        entries[i] = {u32(&raw[i * 8]), u32(&raw[i * 8 + 4])};
    return true;
}

// A data entry (the bytes' RVA and size), then the bytes
bool Resources::leaf(const Entry &entry, std::vector<uint8_t> &data) {
    uint8_t head[8];
    if ((entry.offset & SUBDIRECTORY) || !read(static_cast<uint64_t>(root) + entry.offset, head, sizeof head))
        return false;
    const uint32_t size = u32(head + 4);
    if (!size || size > MAX_DATA)
        return false;
    data.resize(size);
    return read(u32(head), data.data(), size);
}

bool Resources::group(std::vector<uint8_t> &data, uint32_t &language) {
    std::vector<Entry> languages;
    if (!directory(groups[0], languages) || languages.empty())
        return false;
    language = languages[0].name;
    return leaf(languages[0], data);
}

bool Resources::icon(uint32_t id, uint32_t language, std::vector<uint8_t> &data) {
    std::vector<Entry> languages;
    const Entry *name = find(icons, id);
    if (!name || !directory(*name, languages) || languages.empty())
        return false;
    const Entry *same = find(languages, language);
    return leaf(same ? *same : languages[0], data);
}

// One image of an icon as its directory describes it, and where it is
struct Choice {
    int side;        // pixels: the larger of its width and height
    int depth;       // bits per pixel
    uint32_t place;  // its RT_ICON id (a program) or offset (an .ico file)
    uint32_t bytes;
};

// The 8 bytes both directories start their entries with: width, height, colours, reserved,
// planes, bits per pixel. A side of 0 is 256; old icons give only their number of colours.
Choice describe(const uint8_t *entry, uint32_t place, uint32_t bytes) {
    const int width = entry[0] ? entry[0] : 256, height = entry[1] ? entry[1] : 256;
    const int colours = entry[2], bits = u16(entry + 6);
    return {std::max(width, height), bits ? bits : colours == 2 ? 1 : colours == 16 ? 4 : 8, place, bytes};
}

// The order to try them in: the size asked for, else the smallest larger one (shrinking looks
// best), else the largest; the most bits first among the same size
void rank(std::vector<Choice> &choices, int size) {
    const auto kind = [size](int side) { return side == size ? 0 : side > size ? 1 : 2; };
    std::stable_sort(choices.begin(), choices.end(), [&](const Choice &a, const Choice &b) {
        if (kind(a.side) != kind(b.side))
            return kind(a.side) < kind(b.side);
        if (a.side != b.side)
            return a.side > size ? a.side < b.side : a.side > b.side;
        return a.depth > b.depth;
    });
    if (choices.size() > MAX_TRIES)
        choices.resize(MAX_TRIES);
}

// A PNG image (256x256 ones, mostly)
bool from_png(const std::vector<uint8_t> &data, ps4ui::Image &out) {
    int w = 0, h = 0, n = 0;
    const int length = static_cast<int>(data.size());
    // its size first: a small file can claim a huge picture
    if (!stbi_info_from_memory(data.data(), length, &w, &h, &n) || w <= 0 || h <= 0 || w > MAX_SIDE || h > MAX_SIDE)
        return false;
    unsigned char *rgba = stbi_load_from_memory(data.data(), length, &w, &h, &n, 4);
    if (!rgba)
        return false;
    out.width = w;
    out.height = h;
    out.pixels.resize(static_cast<size_t>(w) * h);
    std::memcpy(out.pixels.data(), rgba, out.pixels.size() * 4);
    stbi_image_free(rgba);
    return true;
}

// A DIB as icons keep it: a BITMAPINFOHEADER whose height counts two bitmaps, the palette, the
// colours, then a 1-bit AND mask (1: transparent), both bottom-up with rows padded to 4 bytes. The
// palette's size is worked out as Wine does (bitmap_info_size).
bool from_dib(const std::vector<uint8_t> &data, ps4ui::Image &out) {
    const uint8_t *d = data.data();
    const size_t n = data.size();
    if (n < 40)
        return false;
    const uint32_t header = u32(d), compression = u32(d + 16);
    const int32_t width = static_cast<int32_t>(u32(d + 4)), height = static_cast<int32_t>(u32(d + 8)) / 2;
    const int bpp = u16(d + 14);
    if (header < 40 || header > n || compression != 0 || width <= 0 || height <= 0 || width > MAX_SIDE ||
        height > MAX_SIDE || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32))
        return false;
    uint32_t colours = std::min<uint32_t>(u32(d + 32), 256);
    if (!colours && bpp <= 8)
        colours = 1u << bpp;
    const size_t stride = (static_cast<size_t>(width) * bpp + 31) / 32 * 4;
    const size_t mask_stride = (static_cast<size_t>(width) + 31) / 32 * 4;
    const size_t palette = header, bits = palette + colours * 4, mask = bits + stride * height;
    if (mask > n)
        return false;
    // (converters often leave the mask out of 32-bit images)
    const bool has_mask = mask_stride * height <= n - mask;
    // a 32-bit image's own alpha, unless all of it is 0 (older ones: the mask says then)
    bool alpha = false;
    if (bpp == 32)
        for (size_t i = 0; i < static_cast<size_t>(width) * height && !alpha; i++)
            alpha = d[bits + i * 4 + 3] != 0;

    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; y++) {
        const uint8_t *row = d + bits + static_cast<size_t>(height - 1 - y) * stride;
        const uint8_t *and_row = has_mask ? d + mask + static_cast<size_t>(height - 1 - y) * mask_stride : nullptr;
        for (int x = 0; x < width; x++) {
            uint32_t r = 0, g = 0, b = 0, a = 255;
            if (bpp >= 24) {
                const uint8_t *p = row + static_cast<size_t>(x) * (bpp / 8);
                b = p[0];
                g = p[1];
                r = p[2];
                if (alpha)
                    a = p[3];
            } else {
                const uint32_t i = bpp == 8   ? row[x]
                                   : bpp == 4 ? (row[x / 2] >> (x % 2 ? 0 : 4)) & 15
                                              : (row[x / 8] >> (7 - x % 8)) & 1;
                if (i < colours) {  // (past the palette: black)
                    const uint8_t *c = d + palette + i * 4;
                    b = c[0];
                    g = c[1];
                    r = c[2];
                }
            }
            if (!alpha && and_row && (and_row[x / 8] >> (7 - x % 8)) & 1)
                a = 0;
            out.pixels[static_cast<size_t>(y) * width + x] = r | g << 8 | b << 16 | a << 24;
        }
    }
    return true;
}

bool decode(const std::vector<uint8_t> &data, ps4ui::Image &out) {
    if (data.size() >= 4 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        return from_png(data, out);
    return from_dib(data, out);
}

// A program's icon: its first group, then the best of the RT_ICON images it lists
bool from_program(File &file, int size, ps4ui::Image &out) {
    Resources resources(file);
    std::vector<uint8_t> group, data;
    uint32_t language = 0;
    // GRPICONDIR (reserved, type, count), then 14-byte entries ending in their bytes and RT_ICON id
    if (!resources.open() || !resources.group(group, language) || group.size() < 6 || u16(group.data()) != 0)
        return false;
    const size_t count = std::min<size_t>(u16(group.data() + 4), (group.size() - 6) / 14);
    std::vector<Choice> choices;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *e = group.data() + 6 + i * 14;
        choices.push_back(describe(e, u16(e + 12), u32(e + 8)));
    }
    rank(choices, size);
    for (const Choice &c : choices)
        if (resources.icon(c.place, language, data) && decode(data, out))
            return true;
    return false;
}

// An .ico file: ICONDIR (reserved, type 1, count), then 16-byte entries ending in their bytes and
// offset
bool from_ico(File &file, int size, ps4ui::Image &out) {
    uint8_t head[6];
    std::vector<uint8_t> list, data;
    if (!file.read(0, head, sizeof head) || !file.read(6, static_cast<size_t>(u16(head + 4)) * 16, list))
        return false;
    std::vector<Choice> choices;
    for (size_t i = 0; i < list.size() / 16; i++) {
        const uint8_t *e = list.data() + i * 16;
        choices.push_back(describe(e, u32(e + 12), u32(e + 8)));
    }
    rank(choices, size);
    for (const Choice &c : choices)
        if (c.bytes <= MAX_DATA && file.read(c.place, c.bytes, data) && decode(data, out))
            return true;
    return false;
}

// Along one axis, each output pixel's source pixels and their weights: the share of it each covers
// when shrinking (area averaging), the two nearest when growing (bilinear)
struct Tap {
    int from;
    float weight;
};

std::vector<std::vector<Tap>> taps(int from, int to) {
    std::vector<std::vector<Tap>> out(to);
    const double ratio = static_cast<double>(from) / to;
    for (int i = 0; i < to; i++) {
        std::vector<Tap> &t = out[i];
        if (ratio >= 1) {
            const double a = i * ratio, b = std::min((i + 1) * ratio, static_cast<double>(from));
            for (int j = static_cast<int>(a); j < b; j++)
                t.push_back({j, static_cast<float>(std::min(j + 1.0, b) - std::max(static_cast<double>(j), a))});
        } else {
            const double x = std::clamp((i + 0.5) * ratio - 0.5, 0.0, from - 1.0);
            const int j = static_cast<int>(x);
            t.push_back({j, static_cast<float>(1 - (x - j))});
            if (j + 1 < from)
                t.push_back({j + 1, static_cast<float>(x - j)});
        }
        float sum = 0;
        for (const Tap &k : t)
            sum += k.weight;
        for (Tap &k : t)
            k.weight /= sum;
    }
    return out;
}

// The image fitted into size x size (centered, its proportions kept). Colours are weighted by their
// alpha: the colour of transparent pixels (black, often) doesn't seep into the edges.
ps4ui::Image scaled(const ps4ui::Image &src, int size) {
    const int longest = std::max(src.width, src.height);
    const int w = std::max(1, (src.width * size + longest / 2) / longest);
    const int h = std::max(1, (src.height * size + longest / 2) / longest);
    const std::vector<std::vector<Tap>> xs = taps(src.width, w), ys = taps(src.height, h);

    // across first, premultiplied: w x src.height
    std::vector<float> across(static_cast<size_t>(w) * src.height * 4);
    for (int y = 0; y < src.height; y++) {
        const uint32_t *row = &src.pixels[static_cast<size_t>(y) * src.width];
        for (int x = 0; x < w; x++) {
            float *o = &across[(static_cast<size_t>(y) * w + x) * 4];
            for (const Tap &t : xs[x]) {
                const uint32_t p = row[t.from];
                const float a = (p >> 24) * t.weight;
                o[0] += (p & 0xff) * a;
                o[1] += ((p >> 8) & 0xff) * a;
                o[2] += ((p >> 16) & 0xff) * a;
                o[3] += a;
            }
        }
    }
    // then down, back to straight alpha
    ps4ui::Image out;
    out.width = out.height = size;
    out.pixels.assign(static_cast<size_t>(size) * size, 0);
    const int left = (size - w) / 2, top = (size - h) / 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float sum[4] = {};
            for (const Tap &t : ys[y]) {
                const float *p = &across[(static_cast<size_t>(t.from) * w + x) * 4];
                for (int c = 0; c < 4; c++)
                    sum[c] += p[c] * t.weight;
            }
            const uint32_t a = static_cast<uint32_t>(std::min(sum[3], 255.f) + 0.5f);
            if (!a)
                continue;
            uint32_t p = a << 24;
            for (int c = 0; c < 3; c++)
                p |= static_cast<uint32_t>(std::min(sum[c] / sum[3], 255.f) + 0.5f) << (c * 8);
            out.pixels[static_cast<size_t>(top + y) * size + left + x] = p;
        }
    return out;
}

} // namespace

ps4ui::Image file_icon(const std::string &path, int size) {
    if (size <= 0 || size > MAX_SIDE)
        return {};
    File file(path);
    uint8_t magic[4];
    if (!file.read(0, magic, sizeof magic))
        return {};
    // by what the file holds, not its name: .ocx, .mui, .ax, ... are programs too
    ps4ui::Image image;
    const bool program = magic[0] == 'M' && magic[1] == 'Z', ico = u16(magic) == 0 && u16(magic + 2) == 1;
    if (!(program ? from_program(file, size, image) : ico && from_ico(file, size, image)))
        return {};
    if (image.width == size && image.height == size)
        return image;
    return scaled(image, size);
}

bool exe_is_console(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    uint8_t head[0x400];
    const size_t n = std::fread(head, 1, sizeof head, f);
    std::fclose(f);
    if (n < 0x40 || head[0] != 'M' || head[1] != 'Z')
        return false;
    uint32_t pe = 0;
    std::memcpy(&pe, head + 0x3c, 4);
    // the PE signature, the file header (20 bytes), then the optional header's Subsystem at 68
    if (pe + 24 + 70 > n || std::memcmp(head + pe, "PE  ", 4) != 0)
        return false;
    uint16_t subsystem = 0;
    std::memcpy(&subsystem, head + pe + 24 + 68, 2);
    return subsystem == 3;  // IMAGE_SUBSYSTEM_WINDOWS_CUI
}

} // namespace shell
