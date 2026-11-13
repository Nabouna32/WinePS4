#ifndef PS4_KIT_DIAG_H
#define PS4_KIT_DIAG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-application diagnostics directory used by crash logs and profilers. */
__attribute__((visibility("default"))) const char *ps4_data_dir(void);

void ps4_watch_thread(const char *name);
void ps4_dump_thread_stacks(const char *path, const char *reason);
void ps4_thread_busy_report(char *out, size_t size, int min_percent);
void ps4_sampler_register_thread(const char *name);
void ps4_sampler_set_code_dumps(int enabled);
void ps4_sampler_set_enabled(int enabled);
void ps4_sampler_start(void);
void ps4_hot_profiler_set_enabled(int enabled);
int ps4_hot_profiler_enabled(void);

#ifdef __cplusplus
}
#define PS4_DIAG_DATA_DIR(path) \
    extern "C" __attribute__((used, visibility("default"))) \
    const char *ps4_data_dir(void) { return path; }
#else
#define PS4_DIAG_DATA_DIR(path) \
    __attribute__((used, visibility("default"))) \
    const char *ps4_data_dir(void) { return path; }
#endif

#endif
