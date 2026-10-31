// WinePS4's pictures (picture.h). FFmpeg reads what stb can't: one frame of the picture's video
// stream, or for HEIC (iPhone photos) the tiles of its grid, put together (FFmpeg 7.1's tile grid
// stream groups).
#include "picture.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "third_party/stb_image.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace shell {
namespace {

constexpr long long MAX_PIXELS = 50ll * 1000 * 1000;  // ~200 MiB as RGBA: the heap's limit, about

std::string lower_ext(const std::string &name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return {};
    std::string e = name.substr(dot + 1);
    for (char &c : e)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool from_stb(const std::string &path, Picture &out) {
    int w = 0, h = 0, n = 0;
    if (!stbi_info(path.c_str(), &w, &h, &n) || static_cast<long long>(w) * h > MAX_PIXELS)
        return false;
    unsigned char *data = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!data)
        return false;
    out.width = w;
    out.height = h;
    out.pixels.resize(static_cast<size_t>(w) * h);
    std::memcpy(out.pixels.data(), data, out.pixels.size() * 4);
    for (uint32_t &p : out.pixels)
        p |= 0xff000000u;  // (a transparent picture as it would be on black)
    stbi_image_free(data);
    return true;
}

// a frame into RGBA at (x, y) of out (width out_w), clipped
void put_frame(const AVFrame *f, uint32_t *out, int out_w, int out_h, int x, int y) {
    SwsContext *sws = sws_getContext(f->width, f->height, static_cast<AVPixelFormat>(f->format), f->width, f->height,
                                     AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws)
        return;
    std::vector<uint32_t> rgba(static_cast<size_t>(f->width) * f->height);
    uint8_t *dst[4] = {reinterpret_cast<uint8_t *>(rgba.data()), nullptr, nullptr, nullptr};
    int stride[4] = {f->width * 4, 0, 0, 0};
    sws_scale(sws, f->data, f->linesize, 0, f->height, dst, stride);
    sws_freeContext(sws);
    for (int r = 0; r < f->height; r++) {
        const int oy = y + r;
        if (oy < 0 || oy >= out_h)
            continue;
        for (int c = 0; c < f->width; c++) {
            const int ox = x + c;
            if (ox >= 0 && ox < out_w)
                out[static_cast<size_t>(oy) * out_w + ox] = rgba[static_cast<size_t>(r) * f->width + c] | 0xff000000u;
        }
    }
}

struct Decoder {
    AVCodecContext *ctx = nullptr;
    AVFrame *frame = nullptr;
    bool done = false;
};

bool from_ffmpeg(const std::string &path, Picture &out, std::string &error) {
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        error = "not a picture WinePS4 can read";
        return false;
    }
    avformat_find_stream_info(fmt, nullptr);

    // the streams to decode: a HEIC grid's tiles, else the best video stream
    std::vector<int> wanted;
    const AVStreamGroupTileGrid *grid = nullptr;
    const AVStreamGroup *grid_group = nullptr;
    for (unsigned g = 0; g < fmt->nb_stream_groups; g++) {
        const AVStreamGroup *group = fmt->stream_groups[g];
        if (group->type == AV_STREAM_GROUP_PARAMS_TILE_GRID && group->params.tile_grid) {
            grid = group->params.tile_grid;
            grid_group = group;
            for (unsigned t = 0; t < grid->nb_tiles; t++)
                wanted.push_back(group->streams[grid->offsets[t].idx]->index);
            break;
        }
    }
    if (!grid) {
        const int best = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (best < 0) {
            avformat_close_input(&fmt);
            error = "no picture in it";
            return false;
        }
        wanted.push_back(best);
    }

    std::vector<Decoder> dec(fmt->nb_streams);
    for (int s : wanted) {
        const AVCodecParameters *par = fmt->streams[s]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(par->codec_id);
        if (!codec)
            continue;
        dec[s].ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(dec[s].ctx, par);
        dec[s].ctx->thread_count = 1;
        if (avcodec_open2(dec[s].ctx, codec, nullptr) < 0) {
            avcodec_free_context(&dec[s].ctx);
            continue;
        }
        dec[s].frame = av_frame_alloc();
    }
    size_t remaining = 0;
    for (int s : wanted)
        if (dec[s].ctx)
            remaining++;

    // a frame from each, from the packets in the file's order; then what the decoders still hold
    AVPacket *pkt = av_packet_alloc();
    while (remaining && av_read_frame(fmt, pkt) >= 0) {
        Decoder &d = dec[pkt->stream_index];
        if (d.ctx && !d.done && avcodec_send_packet(d.ctx, pkt) >= 0 && avcodec_receive_frame(d.ctx, d.frame) >= 0) {
            d.done = true;
            remaining--;
        }
        av_packet_unref(pkt);
    }
    for (int s : wanted) {
        Decoder &d = dec[s];
        if (d.ctx && !d.done && avcodec_send_packet(d.ctx, nullptr) >= 0 && avcodec_receive_frame(d.ctx, d.frame) >= 0)
            d.done = true;
    }
    av_packet_free(&pkt);

    bool ok = false;
    if (grid) {
        const long long pixels = static_cast<long long>(grid->coded_width) * grid->coded_height;
        if (pixels > 0 && pixels <= MAX_PIXELS) {
            std::vector<uint32_t> whole(static_cast<size_t>(pixels), 0xff000000u);
            for (unsigned t = 0; t < grid->nb_tiles; t++) {
                const Decoder &d = dec[grid_group->streams[grid->offsets[t].idx]->index];
                if (d.done)
                    put_frame(d.frame, whole.data(), grid->coded_width, grid->coded_height, grid->offsets[t].horizontal,
                              grid->offsets[t].vertical);
            }
            // the picture's own size, within the grid
            const int w = grid->width ? grid->width : grid->coded_width, h = grid->height ? grid->height : grid->coded_height;
            out.width = w;
            out.height = h;
            out.pixels.resize(static_cast<size_t>(w) * h);
            for (int y = 0; y < h; y++)
                std::memcpy(&out.pixels[static_cast<size_t>(y) * w],
                            &whole[static_cast<size_t>(y + grid->vertical_offset) * grid->coded_width + grid->horizontal_offset],
                            static_cast<size_t>(w) * 4);
            ok = true;
        }
    } else if (dec[wanted[0]].done) {
        const AVFrame *f = dec[wanted[0]].frame;
        if (static_cast<long long>(f->width) * f->height <= MAX_PIXELS) {
            out.width = f->width;
            out.height = f->height;
            out.pixels.assign(static_cast<size_t>(f->width) * f->height, 0xff000000u);
            put_frame(f, out.pixels.data(), f->width, f->height, 0, 0);
            ok = true;
        }
    }
    if (!ok)
        error = "the picture could not be decoded (or is over 50 megapixels)";
    for (Decoder &d : dec) {
        av_frame_free(&d.frame);
        avcodec_free_context(&d.ctx);
    }
    avformat_close_input(&fmt);
    return ok;
}

} // namespace

bool is_picture_name(const std::string &name) {
    static const char *const exts[] = {"jpg", "jpeg", "jpe", "jfif", "png", "bmp", "gif", "tga", "psd", "hdr", "ppm",
                                       "pgm", "pnm", "pbm", "webp", "heic", "heif", "tif", "tiff", "jp2", "j2k",
                                       "qoi", "dds", "sgi", "pcx", "pic"};
    const std::string e = lower_ext(name);
    return std::any_of(std::begin(exts), std::end(exts), [&](const char *x) { return e == x; });
}

bool load_picture(const std::string &path, Picture &out, std::string &error) {
    out = Picture{};
    if (from_stb(path, out))
        return true;
    return from_ffmpeg(path, out, error);
}

void cover(const Picture &src, int width, int height, uint32_t *out) {
    if (src.width <= 0 || src.height <= 0)
        return;
    const float scale = std::max(static_cast<float>(width) / src.width, static_cast<float>(height) / src.height);
    const float ox = (src.width * scale - width) / 2, oy = (src.height * scale - height) / 2;
    // each pixel the average of the source's under it (2x2 samples): smooth when shrinking a photo
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            uint32_t acc[3] = {};
            for (int s = 0; s < 4; s++) {
                const float fx = (x + 0.25f + 0.5f * (s & 1) + ox) / scale, fy = (y + 0.25f + 0.5f * (s >> 1) + oy) / scale;
                const int sx = std::clamp(static_cast<int>(fx), 0, src.width - 1);
                const int sy = std::clamp(static_cast<int>(fy), 0, src.height - 1);
                const uint32_t p = src.pixels[static_cast<size_t>(sy) * src.width + sx];
                acc[0] += p & 0xff;
                acc[1] += (p >> 8) & 0xff;
                acc[2] += (p >> 16) & 0xff;
            }
            out[static_cast<size_t>(y) * width + x] = (acc[0] / 4) | ((acc[1] / 4) << 8) | ((acc[2] / 4) << 16) | 0xff000000u;
        }
}

} // namespace shell
