#define _GNU_SOURCE
#include "ps4_kit.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

static __thread int test_priority = -1;

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
