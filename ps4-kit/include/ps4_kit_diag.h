#ifndef PS4_KIT_DIAG_H
#define PS4_KIT_DIAG_H

/*
 * Register the application data directory for the reconstructed diagnostics
 * runtime. The symbol is intentionally simple and allocation-free so it can
 * be emitted from an application translation unit before main().
 */
#define PS4_DIAG_DATA_DIR(path) \
    const char ps4_kit_diag_data_dir[] __attribute__((used, visibility("default"))) = path

#endif
