#include "ps4_kit_heap.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MiB (1024u * 1024u)

static void test_desktop_policy(void)
{
    ps4_kit_heap_options options;
    ps4_kit_heap_plan plan;
    memset(&options, 0, sizeof(options));
    options.source_order = PS4_KIT_HEAP_DIRECT_FIRST;
    options.direct_max = 384u * MiB;
    options.system_pool_max = 128u * MiB;

    assert(ps4_kit_heap_make_plan(&options, &plan) == 0);
    assert(plan.main_attempt_count == 2);
    assert(plan.main_attempts[0].pool == PS4_KIT_POOL_DIRECT);
    assert(plan.main_attempts[0].size == 384u * MiB);
    assert(plan.main_attempts[1].pool == PS4_KIT_POOL_SYSTEM_FLEXIBLE);
    assert(plan.main_attempts[1].size == 128u * MiB);
    assert(plan.max_overflow_heaps == 0);
}

static void test_wine_program_policy(void)
{
    ps4_kit_heap_options options;
    ps4_kit_heap_plan plan;
    memset(&options, 0, sizeof(options));
    options.source_order = PS4_KIT_HEAP_DIRECT_FIRST;
    options.direct_max = 64u * MiB;
    options.overflow_direct_max = 64u * MiB;
    options.max_overflow_heaps = 8;
    options.overflow_direct_reserve = 256u * MiB;
    options.auto_arena_size = 2u * MiB;
    options.max_auto_arenas = 6;
    options.regular_reserve = SIZE_MAX - 32u * MiB;

    assert(ps4_kit_heap_make_plan(&options, &plan) == 0);
    assert(plan.main_attempt_count == 1);
    assert(plan.main_attempts[0].pool == PS4_KIT_POOL_DIRECT);
    assert(plan.main_attempts[0].size == 64u * MiB);
    assert(plan.overflow_pool == PS4_KIT_POOL_DIRECT);
    assert(plan.overflow_heap_size == 64u * MiB);
    assert(plan.max_overflow_heaps == 8);
    assert(plan.overflow_reserve == 256u * MiB);
    assert(plan.auto_arena_size == 2u * MiB);
    assert(plan.max_auto_arenas == 6);
    assert(plan.regular_reserve == SIZE_MAX - 32u * MiB);
}

static void test_outside_process_policy(void)
{
    ps4_kit_heap_options options;
    ps4_kit_heap_plan plan;
    memset(&options, 0, sizeof(options));
    options.regular_only = 1;
    options.regular_max = 16u * MiB;
    options.max_overflow_heaps = 3;

    assert(ps4_kit_heap_make_plan(&options, &plan) == 0);
    assert(plan.main_attempt_count == 1);
    assert(plan.main_attempts[0].pool == PS4_KIT_POOL_REGULAR_FLEXIBLE);
    assert(plan.main_attempts[0].size == 16u * MiB);
    assert(plan.overflow_pool == PS4_KIT_POOL_REGULAR_FLEXIBLE);
    assert(plan.overflow_heap_size == 16u * MiB);
    assert(plan.max_overflow_heaps == 3);
}

static void test_rejects_inconsistent_policy(void)
{
    ps4_kit_heap_options options;
    ps4_kit_heap_plan plan;
    memset(&options, 0, sizeof(options));
    options.regular_only = 1;
    options.regular_max = 16u * MiB;
    options.direct_max = 64u * MiB;
    assert(ps4_kit_heap_make_plan(&options, &plan) == -1);

    memset(&options, 0, sizeof(options));
    options.source_order = PS4_KIT_HEAP_DIRECT_FIRST;
    options.max_overflow_heaps = 1;
    assert(ps4_kit_heap_make_plan(&options, &plan) == -1);
}

int main(void)
{
    test_desktop_policy();
    test_wine_program_policy();
    test_outside_process_policy();
    test_rejects_inconsistent_policy();
    puts("ps4-kit heap policy host tests passed");
    return 0;
}
