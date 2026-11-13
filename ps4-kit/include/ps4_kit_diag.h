#ifndef PS4_KIT_DIAG_H
#define PS4_KIT_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Per-application diagnostics directory used by crash logs and profilers. */
__attribute__((visibility("default"))) const char *ps4_data_dir(void);

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
