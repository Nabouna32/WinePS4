#ifndef PS4_KIT_DIAG_H
#define PS4_KIT_DIAG_H

/*
 * Set the application-specific directory used by ps4-kit diagnostics.
 * The runtime reads this exported symbol when creating sampler/profile logs.
 * Call this macro once at global scope in one application translation unit.
 */
#ifdef __cplusplus
#define PS4_DIAG_DATA_DIR(path) \
    extern "C" { \
        extern __attribute__((used, visibility("default"))) \
        const char ps4_kit_diag_data_dir[] = path; \
    }
#else
#define PS4_DIAG_DATA_DIR(path) \
    __attribute__((used, visibility("default"))) \
    const char ps4_kit_diag_data_dir[] = path
#endif

#endif
