// WinePS4: Vita3K's logging, as the canvas (ps4_canvas.cpp, from HENkaku) uses it: lines to the
// app's log (wine.log, stderr).
#pragma once

#include <cstdio>

#include <fmt/format.h>

#define LOG_INFO(...) std::fprintf(stderr, "shell: %s\n", fmt::format(__VA_ARGS__).c_str())
#define LOG_WARN(...) std::fprintf(stderr, "shell: warning: %s\n", fmt::format(__VA_ARGS__).c_str())
#define LOG_ERROR(...) std::fprintf(stderr, "shell: error: %s\n", fmt::format(__VA_ARGS__).c_str())
