#define _GNU_SOURCE
#include "ps4_kit.h"
#include "ps4_kit_vulkan.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

static __thread int test_priority = -1;
static unsigned int present_calls;
static unsigned int log_calls;
static unsigned long long last_surface;
static const void *last_pixels;
static unsigned int last_width, last_height, last_row_bytes;
static int last_bgra;

static void test_present(unsigned long long surface, const void *pixels,
                         unsigned width, unsigned height, unsigned row_bytes, int bgra)
{
    present_calls++;
    last_surface = surface;
    last_pixels = pixels;
    last_width = width;
    last_height = height;
    last_row_bytes = row_bytes;
    last_bgra = bgra;
}

static void test_log(const char *line)
{
    assert(line != NULL);
    log_calls++;
}

int scePthreadSetprio(pthread_t thread, int priority)
{
    if (!pthread_equal(thread, pthread_self()))
        return -1;
    test_priority = priority;
    return 0;
}

int scePthreadGetprio(pthread_t thread, int *priority)
{
    if (!pthread_equal(thread, pthread_self()))
        return -1;
    *priority = test_priority;
    return 0;
}

int sceSysmoduleLoadModuleInternal(uint32_t module_id)
{
    return module_id == 0x12345678u ? 0 : -1;
}

static void *check_child_priority(void *opaque)
{
    int *priority = opaque;
    *priority = ps4_kit_thread_priority();
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_attr_t attributes;
    size_t stack_size = 0;
    int child_priority = -1;

    {
        const char pixels[] = "frame";
        ps4kit_set_window_present(test_present);
        ps4kit_set_log(test_log);
        ps4kit_window_present(0x1234ull, pixels, 640, 480, 2560, 1);
        ps4kit_vulkan_log("test log");
        assert(present_calls == 1);
        assert(last_surface == 0x1234ull && last_pixels == pixels);
        assert(last_width == 640 && last_height == 480 && last_row_bytes == 2560 && last_bgra == 1);
        assert(log_calls == 1);
        ps4kit_set_window_present(NULL);
        ps4kit_set_log(NULL);
        ps4kit_window_present(0, pixels, 1, 1, 4, 0);
        ps4kit_vulkan_log("ignored");
        assert(present_calls == 1 && log_calls == 1);
    }

    ps4_kit_set_thread_priority(500);
    assert(ps4_kit_thread_priority() == 500);
    assert(ps4_kit_load_module(0x12345678u, "test-module") == 0);
    assert(ps4_kit_load_module(0, "missing-module") == -1);

    assert(pthread_attr_init(&attributes) == 0);
    assert(pthread_attr_setstacksize(&attributes, PTHREAD_STACK_MIN) == 0);
    ps4_kit_set_min_thread_stack(1024u * 1024u);
    ps4_kit_set_new_thread_priority(420);

    assert(pthread_create(&thread, &attributes, check_child_priority, &child_priority) == 0);
    assert(pthread_attr_getstacksize(&attributes, &stack_size) == 0);
    assert(stack_size >= 1024u * 1024u);
    assert(pthread_join(thread, NULL) == 0);
    assert(child_priority == 420);

    pthread_attr_destroy(&attributes);
    puts("ps4-kit runtime host test passed");
    return 0;
}
