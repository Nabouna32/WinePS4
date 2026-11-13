#include "ps4_kit_heap.h"

#include <string.h>

static int append_attempt(ps4_kit_heap_plan *plan,
                          enum ps4_kit_heap_pool pool, size_t size)
{
    if (!size)
        return 0;
    if (plan->main_attempt_count >= PS4_KIT_HEAP_MAX_MAIN_ATTEMPTS)
        return -1;
    plan->main_attempts[plan->main_attempt_count].pool = pool;
    plan->main_attempts[plan->main_attempt_count].size = size;
    plan->main_attempt_count++;
    return 0;
}

void ps4_kit_heap_options_init(ps4_kit_heap_options *options)
{
    if (!options)
        return;

    memset(options, 0, sizeof(*options));
    options->log_tag = "ps4-kit";
    options->source_order = PS4_KIT_HEAP_SYSTEM_FIRST;
    options->system_pool_max = PS4_KIT_HEAP_DEFAULT_SYSTEM_MAX;
}

int ps4_kit_heap_make_plan(const ps4_kit_heap_options *options,
                           ps4_kit_heap_plan *plan)
{
    if (!options || !plan)
        return -1;

    memset(plan, 0, sizeof(*plan));
    plan->regular_reserve = options->regular_reserve;
    plan->auto_arena_size = options->auto_arena_size;
    plan->max_auto_arenas = options->max_auto_arenas;

    if (!!options->auto_arena_size != !!options->max_auto_arenas)
        return -1;

    /*
     * regular_only is an overriding mode. Wine's callback sets this flag and
     * regular_max without clearing the shared defaults (system_pool_max is
     * initialized to 512 MiB); the backend must ignore all other pools.
     */
    if (options->regular_only) {
        if (!options->regular_max)
            return -1;
        if (append_attempt(plan, PS4_KIT_POOL_REGULAR_FLEXIBLE,
                           options->regular_max) != 0)
            return -1;
        if (options->max_overflow_heaps) {
            plan->overflow_pool = PS4_KIT_POOL_REGULAR_FLEXIBLE;
            plan->overflow_heap_size = options->regular_max;
            plan->max_overflow_heaps = options->max_overflow_heaps;
        }
        return 0;
    }

    if (options->source_order != PS4_KIT_HEAP_DIRECT_FIRST &&
        options->source_order != PS4_KIT_HEAP_SYSTEM_FIRST)
        return -1;

    if (options->source_order == PS4_KIT_HEAP_DIRECT_FIRST) {
        if (append_attempt(plan, PS4_KIT_POOL_DIRECT,
                           options->direct_max) != 0 ||
            append_attempt(plan, PS4_KIT_POOL_SYSTEM_FLEXIBLE,
                           options->system_pool_max) != 0)
            return -1;
    } else {
        if (append_attempt(plan, PS4_KIT_POOL_SYSTEM_FLEXIBLE,
                           options->system_pool_max) != 0 ||
            append_attempt(plan, PS4_KIT_POOL_DIRECT,
                           options->direct_max) != 0)
            return -1;
    }

    /*
     * A regular flexible-memory main heap is only eligible when explicitly
     * bounded by regular_max. Wine's normal process sets regular_reserve to
     * SIZE_MAX - 32 MiB and leaves regular_max zero: that intentionally means
     * "do not use regular flexible memory", not "try a gigantic allocation".
     */
    if (options->regular_max &&
        append_attempt(plan, PS4_KIT_POOL_REGULAR_FLEXIBLE,
                       options->regular_max) != 0)
        return -1;

    if (!plan->main_attempt_count)
        return -1;

    if (options->max_overflow_heaps) {
        plan->overflow_pool = PS4_KIT_POOL_DIRECT;
        plan->overflow_heap_size = options->overflow_direct_max
            ? options->overflow_direct_max
            : PS4_KIT_HEAP_DEFAULT_OVERFLOW_SIZE;
        plan->max_overflow_heaps = options->max_overflow_heaps;
        plan->overflow_reserve = options->overflow_direct_reserve;
    } else if (options->overflow_direct_max ||
               options->overflow_direct_reserve) {
        return -1;
    }

    return 0;
}
