#ifndef PS4_KIT_H
#define PS4_KIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Clean-room source contract reconstructed from WinePS4's checked-in
 * ps4_kit_configure_heap() callback. This is not claimed to be the private
 * kit's original binary struct layout; the matching runtime implementation
 * must be built together with this header.
 */
enum ps4_kit_heap_source_order {
    PS4_KIT_HEAP_SYSTEM_FIRST = 0,
    PS4_KIT_HEAP_DIRECT_FIRST = 1
};

typedef struct ps4_kit_heap_options {
    const char *log_tag;
    int source_order;
    size_t direct_max;
    size_t system_pool_max;
    int regular_only;
    size_t regular_max;
    unsigned int max_overflow_heaps;
    size_t overflow_direct_max;
    size_t overflow_direct_reserve;
    size_t auto_arena_size;
    unsigned int max_auto_arenas;
    size_t regular_reserve;
} ps4_kit_heap_options;

/* Application-provided policy callback, called by the kit heap backend. */
void ps4_kit_configure_heap(ps4_kit_heap_options *options);

/* Runtime helpers used by WinePS4. */
void ps4_kit_set_min_thread_stack(size_t bytes);
void ps4_kit_set_thread_priority(int priority);
void ps4_kit_set_new_thread_priority(int priority);
int ps4_kit_thread_priority(void);
int ps4_kit_load_module(uint32_t module_id, const char *module_name);

#ifdef __cplusplus
}
#endif

#endif
