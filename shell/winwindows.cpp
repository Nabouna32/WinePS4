// The windows of Windows programs (see winwindows.h)
#include "winwindows.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../port/include/ps4_display.h"
#include "shell.h"

namespace shell {
namespace {

// MOUSEEVENTF_* and KEYEVENTF_* (winuser.h)
constexpr uint32_t KEYEVENTF_EXTENDEDKEY = 0x0001, KEYEVENTF_KEYUP = 0x0002, KEYEVENTF_UNICODE = 0x0004;
constexpr int CORNER = 8;  // the rounded corners of a window with a title bar of ours

struct WinWindow {
    int pid = 0;
    uint32_t flags = 0;
    int x = 0, y = 0, w = 0, h = 0;       // the shown part, on the screen
    int bw = 0, bh = 0;                   // the pixels kept (the surface: at least w x h)
    std::vector<uint32_t> pixels;         // RGBA, like the canvas
    std::string title;
    uint64_t order = 0;                   // higher: nearer the top
    int client_left = 0, client_top = 0;  // the client area's corner, on the screen
    int caption_bottom = 0;               // where Windows' title bar ends (PS4_WINDOW_CAPTION)
};

std::mutex lock;
std::map<uint64_t, WinWindow> windows;
std::set<int> connected;  // the processes whose display driver is connected
uint64_t next_order = 1;
std::atomic<bool> changed{false};
std::vector<WinTextEvent> text_events;  // (PS4_DISPLAY_TEXT) until the desktop takes them

// OpenOrbis' sockaddr_un is a Linux one: the PS4 (FreeBSD) has a length byte first
struct BsdSockaddrUn {
    uint8_t len;
    uint8_t family;
    char path[104];
};

bool read_all(int fd, void *data, size_t size) {
    char *p = static_cast<char *>(data);
    while (size) {
        const ssize_t got = read(fd, p, size);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return false;
        p += got, size -= got;
    }
    return true;
}

// (with lock held) the kept pixels made bw x bh, what was there staying at its place
void resize_pixels(WinWindow &w, int bw, int bh) {
    if (bw == w.bw && bh == w.bh)
        return;
    std::vector<uint32_t> pixels(static_cast<size_t>(bw) * bh, 0xff000000);
    for (int y = 0; y < std::min(bh, w.bh); y++)
        std::memcpy(&pixels[static_cast<size_t>(y) * bw], &w.pixels[static_cast<size_t>(y) * w.bw],
                    std::min(bw, w.bw) * 4);
    w.pixels.swap(pixels);
    w.bw = bw, w.bh = bh;
}

void handle(const ps4_display_header &m, std::vector<uint8_t> &data) {
    std::lock_guard guard(lock);
    switch (m.type) {
    case PS4_DISPLAY_WINDOW: {
        WinWindow &w = windows[m.hwnd];
        const bool was_visible = w.flags & PS4_WINDOW_VISIBLE;
        w.pid = m.pid;
        w.flags = m.flags;
        w.x = m.left, w.y = m.top, w.w = m.right - m.left, w.h = m.bottom - m.top;
        w.client_left = m.client_left, w.client_top = m.client_top;
        w.caption_bottom = (m.flags & PS4_WINDOW_CAPTION) ? m.caption_bottom : 0;
        if (m.width > 0 && m.height > 0)
            resize_pixels(w, m.width, m.height);
        if (!was_visible && (m.flags & PS4_WINDOW_VISIBLE))
            w.order = next_order++;  // shown: on top
        break;
    }
    case PS4_DISPLAY_PIXELS: {
        WinWindow &w = windows[m.hwnd];
        w.pid = m.pid;
        resize_pixels(w, m.width, m.height);
        const int cols = m.right - m.left;
        if (m.left < 0 || m.top < 0 || m.right > w.bw || m.bottom > w.bh || cols <= 0)
            break;
        const uint32_t *src = reinterpret_cast<const uint32_t *>(data.data());
        const bool rgba = m.flags & PS4_PIXELS_RGBA;  // (a Vulkan frame: the canvas's order already)
        for (int y = m.top; y < m.bottom; y++, src += cols) {
            uint32_t *dst = &w.pixels[static_cast<size_t>(y) * w.bw + m.left];
            if (rgba) {
                for (int x = 0; x < cols; x++)
                    dst[x] = 0xff000000 | src[x];
                continue;
            }
            for (int x = 0; x < cols; x++) {
                const uint32_t p = src[x];  // Windows' BGRX
                dst[x] = 0xff000000 | (p & 0x0000ff00) | ((p >> 16) & 0xff) | ((p & 0xff) << 16);
            }
        }
        break;
    }
    case PS4_DISPLAY_TITLE:
        windows[m.hwnd].title.assign(data.begin(), data.end());
        break;
    case PS4_DISPLAY_DESTROY:
        windows.erase(m.hwnd);
        break;
    case PS4_DISPLAY_RAISE: {
        auto it = windows.find(m.hwnd);
        if (it != windows.end())
            it->second.order = next_order++;
        break;
    }
    case PS4_DISPLAY_TEXT:
        if (text_events.size() >= 64)
            text_events.erase(text_events.begin());  // (a desktop that wasn't asking: the newest count)
        text_events.push_back(WinTextEvent{m.hwnd, m.pid, m.flags, m.left, m.top, m.right, m.bottom});
        return;  // (nothing to draw)
    }
    changed = true;
}

void *client_thread(void *arg) {
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    int pid = 0;
    std::vector<uint8_t> data;
    ps4_display_header m;
    while (read_all(fd, &m, sizeof(m))) {
        if (m.size < sizeof(m) || m.size > 64u << 20) {
            std::fprintf(stderr, "shell: display: bad message (type %u, size %u) from %d\n", m.type, m.size, pid);
            break;
        }
        data.resize(m.size - sizeof(m));
        if (!data.empty() && !read_all(fd, data.data(), data.size()))
            break;
        if (m.type == PS4_DISPLAY_HELLO) {
            pid = m.pid;
            std::fprintf(stderr, "shell: display: process %d connected\n", pid);
            std::lock_guard guard(lock);
            connected.insert(pid);
            continue;
        }
        handle(m, data);
    }
    close(fd);
    // the program ended: its windows go
    std::lock_guard guard(lock);
    connected.erase(pid);
    for (auto it = windows.begin(); it != windows.end();)
        it = it->second.pid == pid ? windows.erase(it) : std::next(it);
    changed = true;
    return nullptr;
}

void *listen_thread(void *arg) {
    const int server = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    for (;;) {
        const int fd = accept(server, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            std::fprintf(stderr, "shell: display: accept: %s\n", std::strerror(errno));
            return nullptr;
        }
        pthread_t thread;
        if (pthread_create(&thread, nullptr, client_thread, reinterpret_cast<void *>(static_cast<intptr_t>(fd))) == 0)
            pthread_detach(thread);
        else
            close(fd);
    }
}

// A window that covers the whole screen without a title bar: a game or a video's full screen, over
// the taskbar as Windows has it
bool is_full_screen(const WinWindow &w) {
    return !w.caption_bottom && w.x <= 0 && w.y <= 0 && w.x + w.w >= PS4_DISPLAY_WIDTH && w.y + w.h >= PS4_DISPLAY_HEIGHT;
}

// The windows bottom to top (minimized ones aren't drawn): topmost ones over the others, full-screen
// ones over all of them
std::vector<std::pair<uint64_t, const WinWindow *>> stacked() {
    std::vector<std::pair<uint64_t, const WinWindow *>> list;
    for (const auto &[hwnd, w] : windows)
        if ((w.flags & PS4_WINDOW_VISIBLE) && !(w.flags & PS4_WINDOW_MINIMIZED) && w.w > 0 && w.h > 0 && !w.pixels.empty())
            list.emplace_back(hwnd, &w);
    std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) {
        const bool fa = is_full_screen(*a.second), fb = is_full_screen(*b.second);
        if (fa != fb)
            return fb;
        const bool ta = a.second->flags & PS4_WINDOW_TOPMOST, tb = b.second->flags & PS4_WINDOW_TOPMOST;
        return ta != tb ? tb : a.second->order < b.second->order;
    });
    return list;
}

// The active window: the top one that gets a taskbar button
uint64_t active_window(const std::vector<std::pair<uint64_t, const WinWindow *>> &list) {
    for (auto it = list.rbegin(); it != list.rend(); ++it)
        if (!(it->second->flags & PS4_WINDOW_POPUP))
            return it->first;
    return 0;
}

WinFrame frame_of(uint64_t hwnd, const WinWindow &w, uint64_t active) {
    WinFrame f;
    f.hwnd = hwnd;
    f.pid = w.pid;
    f.x = w.x, f.y = w.y, f.w = w.w, f.h = w.h;
    f.caption_bottom = w.caption_bottom;
    f.border = w.caption_bottom ? std::clamp(w.client_left - w.x, 0, 16) : 0;
    f.active = hwnd == active;
    f.maximized = w.flags & PS4_WINDOW_MAXIMIZED;
    f.can_minimize = w.flags & PS4_WINDOW_CAN_MINIMIZE;
    f.can_maximize = w.flags & PS4_WINDOW_CAN_MAXIMIZE;
    f.title = w.title;
    return f;
}

// How far in from the side a row of a rounded corner starts (row 0 the corner's outer one)
int corner_inset(int row) {
    const float dy = CORNER - row - 0.5f;
    return CORNER - static_cast<int>(std::sqrt(std::max(0.f, CORNER * CORNER - dy * dy)) + 0.5f);
}

void send(const ps4_input_record &r) {
    if (!ps4_server_input(&r))
        std::fprintf(stderr, "shell: input: the wineserver did not take it\n");
}

} // namespace

void winwin_start() {
    const int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) {
        std::fprintf(stderr, "shell: display: socket: %s\n", std::strerror(errno));
        return;
    }
    BsdSockaddrUn addr{};
    addr.len = sizeof(addr);
    addr.family = AF_UNIX;
    std::strncpy(addr.path, PS4_DISPLAY_SOCKET, sizeof(addr.path) - 1);
    unlink(PS4_DISPLAY_SOCKET);
    if (bind(server, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || listen(server, 16) < 0) {
        std::fprintf(stderr, "shell: display: %s: %s\n", PS4_DISPLAY_SOCKET, std::strerror(errno));
        close(server);
        return;
    }
    pthread_t thread;
    if (pthread_create(&thread, nullptr, listen_thread, reinterpret_cast<void *>(static_cast<intptr_t>(server))) == 0)
        pthread_detach(thread);
    std::fprintf(stderr, "shell: display: programs' windows at %s\n", PS4_DISPLAY_SOCKET);
}

bool winwin_changed() { return changed.exchange(false); }

void winwin_draw(ps4ui::Canvas &c, WinLayer layer, const std::function<void(ps4ui::Canvas &, const WinFrame &)> &under,
                 const std::function<void(ps4ui::Canvas &, const WinFrame &)> &over) {
    std::lock_guard guard(lock);
    uint32_t *const dst = c.data();
    const int cw = c.width(), ch = c.height();
    const auto list = stacked();
    const uint64_t active = active_window(list);
    for (const auto &[hwnd, w] : list) {
        if (is_full_screen(*w) != (layer == WinLayer::FullScreen))
            continue;
        const WinFrame f = frame_of(hwnd, *w, active);
        under(c, f);
        // Windows' pixels, but for its title bar (ours goes there) and outside rounded corners
        const bool rounded = w->caption_bottom && !f.maximized;
        const int x0 = std::max(0, w->x), x1 = std::min(cw, w->x + std::min(w->w, w->bw));
        const int y0 = std::max({0, w->y, w->caption_bottom}), y1 = std::min(ch, w->y + std::min(w->h, w->bh));
        for (int y = y0; y < y1 && x0 < x1; y++) {
            int a = x0, b = x1;
            const int from_bottom = w->y + w->h - 1 - y;
            if (rounded && from_bottom < CORNER) {
                const int inset = corner_inset(from_bottom);
                a = std::max(a, w->x + inset);
                b = std::min(b, w->x + w->w - inset);
            }
            if (a < b)
                std::memcpy(dst + static_cast<size_t>(y) * cw + a,
                            &w->pixels[static_cast<size_t>(y - w->y) * w->bw + (a - w->x)], (b - a) * 4);
        }
        over(c, f);
    }
}

uint64_t winwin_at(float x, float y, bool *full_screen) {
    std::lock_guard guard(lock);
    const auto list = stacked();
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        const WinWindow &w = *it->second;
        if (x >= w.x && x < w.x + w.w && y >= w.y && y < w.y + w.h) {
            if (full_screen)
                *full_screen = is_full_screen(w);
            return it->first;
        }
    }
    return 0;
}

WinRect winwin_button_rect(const WinFrame &f, int button) {
    const int right = std::min(f.x + f.w, PS4_DISPLAY_WIDTH), top = std::max(f.y, 0);
    return WinRect{right - WIN_BUTTON_W * (button + 1), top, WIN_BUTTON_W, std::max(0, f.caption_bottom - top)};
}

int winwin_button_at(float x, float y, uint64_t *hwnd, bool *maximized) {
    std::lock_guard guard(lock);
    const auto list = stacked();
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        const WinWindow &w = *it->second;
        if (!(x >= w.x && x < w.x + w.w && y >= w.y && y < w.y + w.h))
            continue;
        if (!w.caption_bottom || y >= w.caption_bottom)
            return -1;  // (the top window here has no title bar of ours, or not at this height)
        const WinFrame f = frame_of(it->first, w, 0);
        const int buttons = (f.can_minimize || f.can_maximize) ? 3 : 1;
        for (int i = 0; i < buttons; i++)
            if (winwin_button_rect(f, i).contains(x, y)) {
                *hwnd = it->first;
                if (maximized)
                    *maximized = f.maximized;
                return i;
            }
        return -1;
    }
    return -1;
}

void winwin_command(uint64_t hwnd, uint32_t command) {
    ps4_input_record r{};
    r.type = PS4_INPUT_SYSCOMMAND;
    r.x = static_cast<int32_t>(static_cast<uint32_t>(hwnd));
    r.data = static_cast<int32_t>(command);
    send(r);
}

std::vector<WinTextEvent> winwin_text_events() {
    std::lock_guard guard(lock);
    std::vector<WinTextEvent> events;
    events.swap(text_events);
    return events;
}

int winwin_pid(uint64_t hwnd) {
    std::lock_guard guard(lock);
    const auto it = windows.find(hwnd);
    return it == windows.end() ? 0 : it->second.pid;
}

std::string winwin_title(int pid) {
    std::lock_guard guard(lock);
    const uint64_t active = active_window(stacked());
    std::string title;
    for (const auto &[hwnd, w] : windows)
        if (w.pid == pid && (w.flags & PS4_WINDOW_VISIBLE) && !w.title.empty() && (title.empty() || hwnd == active))
            title = w.title;
    return title;
}

std::vector<WinTask> winwin_tasks() {
    std::lock_guard guard(lock);
    const uint64_t active = active_window(stacked());
    std::vector<WinTask> tasks;
    for (const auto &[hwnd, w] : windows)
        if ((w.flags & PS4_WINDOW_VISIBLE) && !(w.flags & PS4_WINDOW_POPUP) && w.w > 0 && w.h > 0)
            tasks.push_back(WinTask{hwnd, w.pid, w.title.empty() ? std::string("Window") : w.title, hwnd == active,
                                    (w.flags & PS4_WINDOW_MINIMIZED) != 0});
    return tasks;  // (a map: oldest window first, as handles grow)
}

void winwin_mouse(float x, float y, uint32_t flags, int wheel) {
    ps4_input_record r{};
    r.type = PS4_INPUT_MOUSE;
    r.x = static_cast<int>(x), r.y = static_cast<int>(y);
    r.flags = flags;
    r.data = wheel;
    send(r);
}

void winwin_key(uint16_t vkey, bool down, uint16_t scan, bool extended) {
    ps4_input_record r{};
    r.type = PS4_INPUT_KEY;
    r.vkey = vkey;
    r.scan = scan;
    r.flags = (down ? 0 : KEYEVENTF_KEYUP) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
    send(r);
}

void winwin_text(const std::string &utf8) {
    // each character as a VK_PACKET key press (KEYEVENTF_UNICODE); a new line is Enter
    for (size_t i = 0; i < utf8.size();) {
        uint32_t cp = static_cast<unsigned char>(utf8[i]);
        int extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
        if (extra)
            cp &= 0x3f >> extra;
        i++;
        for (; extra && i < utf8.size(); extra--, i++)
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i]) & 0x3f);
        if (cp == '\n') {
            winwin_key(0x0d, true);  // VK_RETURN
            winwin_key(0x0d, false);
            continue;
        }
        auto unit = [](uint16_t u) {
            for (const bool down : {true, false}) {
                ps4_input_record r{};
                r.type = PS4_INPUT_KEY;
                r.scan = u;
                r.flags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP);
                send(r);
            }
        };
        if (cp >= 0x10000) {
            cp -= 0x10000;
            unit(static_cast<uint16_t>(0xd800 + (cp >> 10)));
            unit(static_cast<uint16_t>(0xdc00 + (cp & 0x3ff)));
        } else {
            unit(static_cast<uint16_t>(cp));
        }
    }
}

} // namespace shell

extern "C" int ps4_shell_display_connected(int pid) {
    std::lock_guard guard(shell::lock);
    return shell::connected.count(pid) ? 1 : 0;
}
