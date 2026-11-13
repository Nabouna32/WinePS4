#ifndef PS4_KIT_HEAP_H
#define PS4_KIT_HEAP_H

#include "ps4_kit.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PS4_KIT_HEAP_MAX_MAIN_ATTEMPTS 3u
#define PS4_KIT_HEAP_DEFAULT_SYSTEM_MAX (512u * 1024u * 1024u)
#define PS4_KIT_HEAP_DEFAULT_OVERFLOW_SIZE (512u * 1024u * 1024u)

enum ps4_kit_heap_pool {
    PS4_KIT_POOL_DIRECT = 1,
    PS4_KIT_POOL_SYSTEM_FLEXIBLE = 2,
    PS4_KIT_POOL_REGULAR_FLEXIBLE = 3
};

typedef struct ps4_kit_heap_attempt {
    enum ps4_kit_heap_pool pool;
    size_t size;
} ps4_kit_heap_attempt;

typedef struct ps4_kit_heap_plan {
    ps4_kit_heap_attempt main_attempts[PS4_KIT_HEAP_MAX_MAIN_ATTEMPTS];
    unsigned int main_attempt_count;
    enum ps4_kit_heap_pool overflow_pool;
    size_t overflow_heap_size;
    unsigned int max_overflow_heaps;
    size_t overflow_reserve;
    size_t auto_arena_size;
    unsigned int max_auto_arenas;
    size_t regular_reserve;
} ps4_kit_heap_plan;

/*
 * Initialize the reconstructed shared-port defaults before calling the
 * application's ps4_kit_configure_heap() callback.
 *
 * The 512 MiB system-flexible default follows the public Resonance/Dolphin
 * runtime lineage. A 512 MiB direct overflow size is used when a consumer
 * enables overflow heaps without specifying a size (Godot's public callback
 * does exactly that). These are source-reconstruction defaults, not a claim
 * that the private kit's original struct ABI or every default is recovered.
 */
void ps4_kit_heap_options_init(ps4_kit_heap_options *options);

/*
 * Translate the app-provided heap policy into an explicit, deterministic
 * allocation plan. This function performs no allocation and calls no PS4 API.
 * The platform backend is responsible for executing attempts in order.
 *
 * Returns 0 on success, -1 for an inconsistent/unsupported policy.
 */
int ps4_kit_heap_make_plan(const ps4_kit_heap_options *options,
                           ps4_kit_heap_plan *plan);

#ifdef __cplusplus
}
#endif

#endif
