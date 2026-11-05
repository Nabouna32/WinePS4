// WinePS4's screen: the desktop's frames on the TV. Two linear 1920x1080 buffers in VideoOut's
// app slots 12-13 (ps4-kit keeps 0-11 for Vulkan swapchains: DXVK's, later), flipped at vsync.
// From HENkaku's launcher (ps4_launcher.cpp: setup_buffers, wait_flipped, present).
#pragma once

#include <cstdint>

namespace shell {

constexpr int SCREEN_W = 1920;
constexpr int SCREEN_H = 1080;

// Opens the display; false (logged) if it can't
bool screen_open();
// The buffer to draw the next frame into (RGBA, SCREEN_W per row): wait until it isn't shown
uint32_t *screen_back_buffer();
// Which of the two that is (0 or 1), for what the caller keeps of each
int screen_back_index();
// Shows it at the next vsync
void screen_flip();

} // namespace shell
