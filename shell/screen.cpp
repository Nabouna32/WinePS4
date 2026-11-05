// WinePS4's screen (screen.h). The buffers are write-combined direct memory, which the display
// reads: the desktop draws its frame in ordinary memory and copies it in (screen_back_buffer is
// written once per frame, front to back, by the frame's last step).
#include "screen.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include <orbis/SystemService.h>
#include <orbis/VideoOut.h>

extern "C" {
int32_t ps4s_allocate_direct(off_t start, off_t end, size_t len, size_t align, int32_t type, off_t *phys) __asm__("sceKernelAllocateDirectMemory");
int32_t ps4s_map_direct(void **addr, size_t len, int32_t prot, int32_t flags, off_t phys, size_t align) __asm__("sceKernelMapDirectMemory");
size_t ps4s_direct_memory_size() __asm__("sceKernelGetDirectMemorySize");
void ps4_boot_trace(const char *line);
}

namespace shell {
namespace {

constexpr size_t BUFFER_SIZE = 8ull << 20;  // 1920x1080x4 rounded up to 2 MiB pages
constexpr int32_t WC_GARLIC = 3;            // write-combined, what the display reads
constexpr int32_t PROT_CPU_GPU_RW = 0x33;
constexpr int FIRST_SLOT = 12;              // ps4-kit's app slots (ps4_vk_wsi.h kAppFirstBuffer)
constexpr int64_t FLIP_ARG_BASE = 0x57490000;  // our flips' numbers ("WI"), to tell when one is shown

int video = -1;
uint32_t *buffers[2] = {};
int current = 0;
int64_t flips = 0;
bool splash_hidden = false;

void trace(const char *fmt, unsigned a = 0, unsigned b = 0) {
    char line[160];
    std::snprintf(line, sizeof line, fmt, a, b);
    std::fprintf(stderr, "shell: %s\n", line);
    ps4_boot_trace(line);
}

} // namespace

bool screen_open() {
    if (video >= 0)
        return true;
    video = sceVideoOutOpen(0xff, 0, 0, nullptr);
    if (video < 0) {
        trace("screen: sceVideoOutOpen failed (%08x)", static_cast<unsigned>(video));
        return false;
    }
    off_t phys = 0;
    void *base = nullptr;
    const int32_t allocated = ps4s_allocate_direct(0, static_cast<off_t>(ps4s_direct_memory_size()), 2 * BUFFER_SIZE,
                                                   2 << 20, WC_GARLIC, &phys);
    const int32_t mapped = allocated != 0 ? -1 : ps4s_map_direct(&base, 2 * BUFFER_SIZE, PROT_CPU_GPU_RW, 0, phys, 2 << 20);
    if (allocated != 0 || mapped != 0) {
        trace("screen: no direct memory for the screen (%08x %08x)", static_cast<unsigned>(allocated), static_cast<unsigned>(mapped));
        video = -1;
        return false;
    }
    buffers[0] = static_cast<uint32_t *>(base);
    buffers[1] = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(base) + BUFFER_SIZE);
    std::memset(base, 0, 2 * BUFFER_SIZE);
    void *addresses[2] = {buffers[0], buffers[1]};
    OrbisVideoOutBufferAttribute attribute;
    sceVideoOutSetBufferAttribute(&attribute, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR,
                                  0, SCREEN_W, SCREEN_H, SCREEN_W);
    const int set = sceVideoOutRegisterBuffers(video, FIRST_SLOT, addresses, 2, &attribute);
    if (set < 0) {
        trace("screen: sceVideoOutRegisterBuffers failed (%08x)", static_cast<unsigned>(set));
        video = -1;
        return false;
    }
    trace("screen: ready (VideoOut %u, buffers %u-...)", static_cast<unsigned>(video), FIRST_SLOT);
    return true;
}

uint32_t *screen_back_buffer() {
    // until the last flip is on screen, so the buffer about to be written isn't: this paces the
    // desktop to the TV's refresh
    if (flips) {
        for (int i = 0; i < 80; i++) {
            OrbisVideoOutFlipStatus status{};
            if (sceVideoOutGetFlipStatus(video, &status) < 0 || status.flipArg >= FLIP_ARG_BASE + flips)
                break;
            const timespec pause = {0, 500000};
            nanosleep(&pause, nullptr);
        }
    }
    return buffers[current];
}

int screen_back_index() { return current; }

void screen_flip() {
    __builtin_ia32_sfence();  // the write-combined stores out before the flip
    const int flip = sceVideoOutSubmitFlip(video, FIRST_SLOT + current, ORBIS_VIDEO_OUT_FLIP_VSYNC, FLIP_ARG_BASE + ++flips);
    if (!splash_hidden) {
        trace("screen: first frame (flip %08x)", static_cast<unsigned>(flip));
        // the system shows the app's splash until the app has a picture of its own
        sceSystemServiceHideSplashScreen();
        splash_hidden = true;
    }
    current ^= 1;
}

} // namespace shell
