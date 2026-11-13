/*
 * Clean-room ps4-kit runtime subset for WinePS4.
 *
 * Implements only the public PS4 primitives needed by the current source:
 * pthread priority control, a minimum stack size for new threads, and internal
 * system-module loading. The heap allocator and diagnostics backend remain
 * separate reconstruction tasks.
 */

#include "ps4_kit.h"

#include <pthread.h>
#include <stdlib.h>

extern int scePthreadSetprio(pthread_t thread, int priority);
extern int scePthreadGetprio(pthread_t thread, int *priority);
extern int sceSysmoduleLoadModuleInternal(uint32_t module_id);

static size_t g_min_thread_stack;
static __thread int g_new_thread_priority = -1;

struct ps4_kit_thread_start {
    void *(*start)(void *);
    void *argument;
    int priority;
};

static void *ps4_kit_thread_trampoline(void *opaque)
{
    struct ps4_kit_thread_start start = *(struct ps4_kit_thread_start *)opaque;
    free(opaque);
    (void)scePthreadSetprio(pthread_self(), start.priority);
    return start.start(start.argument);
}

void ps4_kit_set_min_thread_stack(size_t bytes)
{
    __atomic_store_n(&g_min_thread_stack, bytes, __ATOMIC_RELEASE);
}

void ps4_kit_set_thread_priority(int priority)
{
    (void)scePthreadSetprio(pthread_self(), priority);
}

void ps4_kit_set_new_thread_priority(int priority)
{
    g_new_thread_priority = priority;
}

int ps4_kit_thread_priority(void)
{
    int priority = -1;
    if (scePthreadGetprio(pthread_self(), &priority) != 0)
        return -1;
    return priority;
}

int ps4_kit_load_module(uint32_t module_id, const char *module_name)
{
    (void)module_name; /* retained for source/API compatibility and diagnostics extensions */
    return sceSysmoduleLoadModuleInternal(module_id);
}

/*
 * Link with --wrap=pthread_create. The wrapper applies the app's requested
 * minimum stack to every new thread and, when set_new_thread_priority() has
 * selected a value, installs it before the thread's start routine executes.
 */
int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                          void *(*start)(void *), void *argument);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                          void *(*start)(void *), void *argument)
{
    pthread_attr_t local_attributes;
    const pthread_attr_t *effective_attributes = attributes;
    int owns_attributes = 0;
    size_t minimum = __atomic_load_n(&g_min_thread_stack, __ATOMIC_ACQUIRE);
    size_t current_stack = 0;

    if (!effective_attributes && pthread_attr_init(&local_attributes) == 0) {
        effective_attributes = &local_attributes;
        owns_attributes = 1;
    }

    if (effective_attributes && minimum &&
        pthread_attr_getstacksize(effective_attributes, &current_stack) == 0 &&
        current_stack < minimum) {
        /* OpenOrbis' pthread attributes are mutable POD handles; this mirrors
         * the established PS4 porting convention for a const pthread_attr_t*. */
        (void)pthread_attr_setstacksize((pthread_attr_t *)effective_attributes, minimum);
    }

    int result;
    int priority = g_new_thread_priority;
    if (priority >= 0) {
        struct ps4_kit_thread_start *context = malloc(sizeof(*context));
        if (context) {
            context->start = start;
            context->argument = argument;
            context->priority = priority;
            result = __real_pthread_create(thread, effective_attributes,
                                           ps4_kit_thread_trampoline, context);
            if (result != 0)
                free(context);
        } else {
            result = __real_pthread_create(thread, effective_attributes, start, argument);
        }
    } else {
        result = __real_pthread_create(thread, effective_attributes, start, argument);
    }

    if (owns_attributes)
        (void)pthread_attr_destroy(&local_attributes);
    return result;
}
