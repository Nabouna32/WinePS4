#define _GNU_SOURCE

#include "ps4_kit_diag.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if !defined(__PS4__)
#include <ucontext.h>
#endif

#if defined(__PS4__)
#define PS4_KIT_SA_SIGINFO 0x40
#define PS4_KIT_SA_RESTART 0x0002
#else
#define PS4_KIT_SA_SIGINFO SA_SIGINFO
#define PS4_KIT_SA_RESTART SA_RESTART
#endif

#if defined(__GNUC__)
__attribute__((weak, visibility("default")))
#endif
const char *ps4_data_dir(void)
{
    return "/data/ps4-kit";
}

#define PS4_KIT_DIAG_MAX_THREADS 96u
#define PS4_KIT_DIAG_MAX_SAMPLER_THREADS 4u
#define PS4_KIT_DIAG_NAME_SIZE 32u
#define PS4_KIT_DIAG_REPORT_NS 10000000000ull
#define PS4_KIT_DIAG_WAIT_NS 1000000l

#if defined(PS4_KIT_DIAG_TEST)
#undef PS4_KIT_DIAG_REPORT_NS
#define PS4_KIT_DIAG_REPORT_NS 100000000ull
#endif

typedef struct ps4_kit_diag_thread {
    pthread_t handle;
    char name[PS4_KIT_DIAG_NAME_SIZE];
    int tid;
    uintptr_t stack_limit;
    volatile sig_atomic_t dump_done;
    volatile uintptr_t rip;
    volatile uintptr_t rsp;
    _Atomic int sampler_registered;
    _Atomic uint64_t self_cpu_ns;
    _Atomic unsigned int sample_count;
    _Atomic uintptr_t sample_last_rip;
    _Atomic unsigned int hot_count;
    _Atomic uintptr_t hot_last_rip;
} ps4_kit_diag_thread;

static ps4_kit_diag_thread g_threads[PS4_KIT_DIAG_MAX_THREADS];
static _Atomic unsigned int g_thread_count;
static pthread_mutex_t g_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_busy_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_signal_capture_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int g_tls_thread_index = -1;

static _Atomic int g_sampler_started;
static _Atomic int g_sampler_enabled;
static _Atomic int g_code_dumps_enabled;
static _Atomic int g_hot_started;
static _Atomic int g_hot_enabled;

static pthread_once_t g_dump_handler_once = PTHREAD_ONCE_INIT;
static pthread_once_t g_prof_handler_once = PTHREAD_ONCE_INIT;

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static int current_thread_id(void)
{
#if defined(__PS4__)
    extern int pthread_getthreadid_np(void);
    return pthread_getthreadid_np();
#else
    return (int)(uintptr_t)pthread_self();
#endif
}

static void capture_context(void *context, uintptr_t *rip, uintptr_t *rsp)
{
    *rip = 0;
    *rsp = 0;
#if defined(__PS4__)
    /*
     * OpenOrbis/FreeBSD x86-64 ucontext layout measured by the public PS4
     * runtime lineage: mcontext starts at +0x40, RIP is register slot 20 and
     * RSP is slot 23. Keep this isolated to the PS4 target and hardware-test it.
     */
    const uint64_t *regs = (const uint64_t *)((const char *)context + 0x40);
    *rip = (uintptr_t)regs[20];
    *rsp = (uintptr_t)regs[23];
#elif defined(__x86_64__) && defined(REG_RIP) && defined(REG_RSP)
    const ucontext_t *uc = (const ucontext_t *)context;
    *rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    *rsp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
#else
    (void)context;
#endif
}

static void dump_signal_handler(int signal_number, siginfo_t *info, void *context)
{
    int index = g_tls_thread_index;
    uintptr_t rip;
    uintptr_t rsp;
    (void)signal_number;
    (void)info;

    if (index < 0 || (unsigned int)index >= PS4_KIT_DIAG_MAX_THREADS)
        return;

    capture_context(context, &rip, &rsp);
    g_threads[index].rip = rip;
    g_threads[index].rsp = rsp;
    g_threads[index].dump_done = 1;
}

static void profiler_signal_handler(int signal_number, siginfo_t *info, void *context)
{
    int index = g_tls_thread_index;
    uintptr_t rip;
    uintptr_t rsp;
    (void)signal_number;
    (void)info;

    if (index < 0 || (unsigned int)index >= PS4_KIT_DIAG_MAX_THREADS)
        return;

    capture_context(context, &rip, &rsp);
    (void)rsp;
    if (atomic_load_explicit(&g_threads[index].sampler_registered, memory_order_relaxed)) {
        atomic_store_explicit(&g_threads[index].sample_last_rip, rip, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_threads[index].sample_count, 1u, memory_order_relaxed);
    }
}

static void install_dump_handler_once(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = dump_signal_handler;
    action.sa_flags = PS4_KIT_SA_SIGINFO | PS4_KIT_SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, NULL);
}

static void install_prof_handler_once(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = profiler_signal_handler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF, &action, NULL);
}

static void install_dump_handler(void)
{
    pthread_once(&g_dump_handler_once, install_dump_handler_once);
}

static void install_prof_handler(void)
{
    pthread_once(&g_prof_handler_once, install_prof_handler_once);
}

void ps4_watch_thread(const char *name)
{
    pthread_t self = pthread_self();
    unsigned int count;
    unsigned int i;
    int index = -1;

    install_dump_handler();
    pthread_mutex_lock(&g_thread_lock);
    count = atomic_load_explicit(&g_thread_count, memory_order_relaxed);
    for (i = 0; i < count; i++) {
        if (pthread_equal(g_threads[i].handle, self)) {
            index = (int)i;
            break;
        }
    }

    if (index < 0) {
        if (count >= PS4_KIT_DIAG_MAX_THREADS) {
            pthread_mutex_unlock(&g_thread_lock);
            g_tls_thread_index = -1;
            return;
        }
        index = (int)count;
        g_threads[index].handle = self;
        g_threads[index].name[0] = 0;
        g_threads[index].tid = 0;
        g_threads[index].stack_limit = 0;
        g_threads[index].dump_done = 0;
        g_threads[index].rip = 0;
        g_threads[index].rsp = 0;
        atomic_init(&g_threads[index].sampler_registered, 0);
        atomic_init(&g_threads[index].self_cpu_ns, 0u);
        atomic_init(&g_threads[index].sample_count, 0u);
        atomic_init(&g_threads[index].sample_last_rip, 0);
        atomic_init(&g_threads[index].hot_count, 0u);
        atomic_init(&g_threads[index].hot_last_rip, 0);
        atomic_store_explicit(&g_thread_count, count + 1u, memory_order_release);
    }

    snprintf(g_threads[index].name, sizeof(g_threads[index].name), "%s",
             (name && name[0]) ? name : "?");
    g_threads[index].tid = current_thread_id();
    g_threads[index].stack_limit =
        (uintptr_t)__builtin_frame_address(0) + sizeof(uintptr_t);
    g_tls_thread_index = index;
    pthread_mutex_unlock(&g_thread_lock);
}

void ps4_sampler_register_thread(const char *name)
{
    unsigned int i;
    unsigned int count = 0;
    int already_registered = 0;
    int index;
    ps4_watch_thread(name);
    index = g_tls_thread_index;
    if (index < 0 || (unsigned int)index >= PS4_KIT_DIAG_MAX_THREADS)
        return;

    pthread_mutex_lock(&g_thread_lock);
    for (i = 0; i < atomic_load_explicit(&g_thread_count, memory_order_acquire); i++) {
        if (atomic_load_explicit(&g_threads[i].sampler_registered, memory_order_relaxed)) {
            count++;
            if ((int)i == index)
                already_registered = 1;
        }
    }
    if (already_registered || count < PS4_KIT_DIAG_MAX_SAMPLER_THREADS)
        atomic_store_explicit(&g_threads[index].sampler_registered, 1, memory_order_release);
    pthread_mutex_unlock(&g_thread_lock);
}

static int write_all(int fd, const char *buffer, size_t length)
{
    while (length) {
        ssize_t written = write(fd, buffer, length);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return -1;
        buffer += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static void ensure_data_directory(void)
{
    const char *dir = ps4_data_dir();
    if (dir && dir[0])
        mkdir(dir, 0777);
}

static int open_data_file(const char *name)
{
    char path[256];
    const char *dir = ps4_data_dir();
    if (!dir || !dir[0] || !name || !name[0])
        return -1;
    ensure_data_directory();
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
        return -1;
    return open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
}

static void wait_for_dump_set(unsigned int count, const unsigned char *signaled)
{
    struct timespec pause;
    unsigned int tries;
    pause.tv_sec = 0;
    pause.tv_nsec = PS4_KIT_DIAG_WAIT_NS;
    for (tries = 0; tries < 100u; tries++) {
        unsigned int i;
        int pending = 0;
        for (i = 0; i < count; i++)
            if (signaled[i] && !g_threads[i].dump_done)
                pending = 1;
        if (!pending)
            break;
        nanosleep(&pause, NULL);
    }
}

void ps4_dump_thread_stacks(const char *path, const char *reason)
{
    pthread_t handles[PS4_KIT_DIAG_MAX_THREADS];
    char names[PS4_KIT_DIAG_MAX_THREADS][PS4_KIT_DIAG_NAME_SIZE];
    int tids[PS4_KIT_DIAG_MAX_THREADS];
    unsigned char signaled[PS4_KIT_DIAG_MAX_THREADS] = {0};
    unsigned int count;
    unsigned int i;
    int fd;
    char default_path[256];
    char line[256];

    install_dump_handler();
    ensure_data_directory();
    if (!path || !path[0]) {
        const char *dir = ps4_data_dir();
        if (!dir || snprintf(default_path, sizeof(default_path), "%s/hang.log", dir) >=
                        (int)sizeof(default_path))
            return;
        path = default_path;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;

    snprintf(line, sizeof(line), "==== %s ====\\n", reason ? reason : "thread dump");
    write_all(fd, line, strlen(line));

    pthread_mutex_lock(&g_thread_lock);
    count = atomic_load_explicit(&g_thread_count, memory_order_acquire);
    for (i = 0; i < count; i++) {
        handles[i] = g_threads[i].handle;
        memcpy(names[i], g_threads[i].name, sizeof(names[i]));
        tids[i] = g_threads[i].tid;
    }
    pthread_mutex_unlock(&g_thread_lock);

    pthread_mutex_lock(&g_signal_capture_lock);
    for (i = 0; i < count; i++) {
        g_threads[i].dump_done = 0;
        if (pthread_kill(handles[i], SIGUSR1) == 0)
            signaled[i] = 1;
    }
    wait_for_dump_set(count, signaled);
    pthread_mutex_unlock(&g_signal_capture_lock);

    for (i = 0; i < count; i++) {
        if (!signaled[i]) {
            snprintf(line, sizeof(line), "-- %s tid=%d: signal failed\\n", names[i], tids[i]);
        } else if (g_threads[i].dump_done) {
            snprintf(line, sizeof(line),
                     "-- %s tid=%d rip=0x%llx rsp=0x%llx\\n",
                     names[i], tids[i],
                     (unsigned long long)g_threads[i].rip,
                     (unsigned long long)g_threads[i].rsp);
        } else {
            snprintf(line, sizeof(line), "-- %s tid=%d: signal timed out\\n", names[i], tids[i]);
        }
        write_all(fd, line, strlen(line));
    }
    close(fd);
}

void ps4_thread_cpu_tick(void)
{
    clockid_t clock_id;
    struct timespec cpu;
    int index = g_tls_thread_index;
    uint64_t cpu_ns;
    if (index < 0 || (unsigned int)index >= PS4_KIT_DIAG_MAX_THREADS)
        return;
    if (pthread_getcpuclockid(pthread_self(), &clock_id) != 0 ||
        clock_gettime(clock_id, &cpu) != 0)
        return;
    cpu_ns = (uint64_t)cpu.tv_sec * 1000000000ull + (uint64_t)cpu.tv_nsec;
    atomic_store_explicit(&g_threads[index].self_cpu_ns, cpu_ns, memory_order_relaxed);
}

typedef struct ps4_kit_diag_busy_entry {
    char name[PS4_KIT_DIAG_NAME_SIZE];
    int percent;
} ps4_kit_diag_busy_entry;

void ps4_thread_busy_report(char *out, size_t size, int min_percent)
{
    static uint64_t previous_cpu[PS4_KIT_DIAG_MAX_THREADS];
    static uint64_t previous_wall;
    ps4_kit_diag_busy_entry entries[PS4_KIT_DIAG_MAX_THREADS];
    pthread_t handles[PS4_KIT_DIAG_MAX_THREADS];
    unsigned int count;
    unsigned int i;
    uint64_t wall;
    size_t length = 0;

    if (!out || size == 0)
        return;
    out[0] = '\0';
    wall = monotonic_ns();
    if (!wall)
        return;

    pthread_mutex_lock(&g_busy_lock);
    pthread_mutex_lock(&g_thread_lock);
    count = atomic_load_explicit(&g_thread_count, memory_order_acquire);
    for (i = 0; i < count; i++) {
        handles[i] = g_threads[i].handle;
        memcpy(entries[i].name, g_threads[i].name, sizeof(entries[i].name));
        entries[i].percent = -1;
    }
    pthread_mutex_unlock(&g_thread_lock);

    for (i = 0; i < count; i++) {
        clockid_t clock_id;
        struct timespec cpu;
        uint64_t cpu_ns = atomic_load_explicit(&g_threads[i].self_cpu_ns, memory_order_relaxed);
        if (!cpu_ns) {
            if (pthread_getcpuclockid(handles[i], &clock_id) != 0 ||
                clock_gettime(clock_id, &cpu) != 0)
                continue;
            cpu_ns = (uint64_t)cpu.tv_sec * 1000000000ull + (uint64_t)cpu.tv_nsec;
        }
        if (previous_wall && wall > previous_wall && previous_cpu[i] &&
            cpu_ns >= previous_cpu[i]) {
            uint64_t elapsed = wall - previous_wall;
            uint64_t busy = (cpu_ns - previous_cpu[i]) * 100u / elapsed;
            entries[i].percent = busy > 999u ? 999 : (int)busy;
        }
        previous_cpu[i] = cpu_ns;
    }
    previous_wall = wall;
    pthread_mutex_unlock(&g_busy_lock);

    for (;;) {
        int best = -1;
        for (i = 0; i < count; i++) {
            if (entries[i].percent >= min_percent &&
                (best < 0 || entries[i].percent > entries[best].percent))
                best = (int)i;
        }
        if (best < 0 || length + 40u >= size)
            break;
        {
            int written = snprintf(out + length, size - length, "%s%s %d%%",
                                    length ? ", " : "",
                                    entries[best].name, entries[best].percent);
            if (written < 0)
                break;
            if ((size_t)written >= size - length) {
                length = size - 1u;
                out[length] = '\0';
                break;
            }
            length += (size_t)written;
        }
        entries[best].percent = -1;
    }
}

void ps4_sampler_set_code_dumps(int enabled)
{
    atomic_store_explicit(&g_code_dumps_enabled, enabled != 0, memory_order_release);
}

static void write_sampler_report(void)
{
    unsigned int count;
    unsigned int i;
    int fd = open_data_file("samples.log");
    char line[256];
    if (fd < 0)
        return;
    snprintf(line, sizeof(line), "==== sampler window ====\n");
    write_all(fd, line, strlen(line));

    count = atomic_load_explicit(&g_thread_count, memory_order_acquire);
    for (i = 0; i < count; i++) {
        unsigned int samples;
        uintptr_t last;
        if (!atomic_load_explicit(&g_threads[i].sampler_registered, memory_order_acquire))
            continue;
        samples = atomic_exchange_explicit(&g_threads[i].sample_count, 0u, memory_order_acq_rel);
        last = atomic_load_explicit(&g_threads[i].sample_last_rip, memory_order_relaxed);
        if (!samples)
            continue;
        snprintf(line, sizeof(line), "%s samples=%u last_rip=0x%llx\n",
                 g_threads[i].name, samples, (unsigned long long)last);
        write_all(fd, line, strlen(line));
    }
    if (atomic_load_explicit(&g_code_dumps_enabled, memory_order_acquire)) {
        const char *note = "code dumps requested, but no JIT/guest ranges are registered by this runtime API\n";
        write_all(fd, note, strlen(note));
    }
    close(fd);
}

static void *sampler_thread_main(void *unused)
{
    struct timespec period = {0, 1000000l};
    uint64_t last_report = monotonic_ns();
    (void)unused;
    for (;;) {
        unsigned int count;
        unsigned int i;
        if (atomic_load_explicit(&g_sampler_enabled, memory_order_acquire)) {
            pthread_t handles[PS4_KIT_DIAG_MAX_THREADS];
            unsigned char enabled[PS4_KIT_DIAG_MAX_THREADS];
            pthread_mutex_lock(&g_thread_lock);
            count = atomic_load_explicit(&g_thread_count, memory_order_acquire);
            for (i = 0; i < count; i++) {
                handles[i] = g_threads[i].handle;
                enabled[i] = (unsigned char)atomic_load_explicit(
                    &g_threads[i].sampler_registered, memory_order_relaxed);
            }
            pthread_mutex_unlock(&g_thread_lock);
            for (i = 0; i < count; i++)
                if (enabled[i])
                    pthread_kill(handles[i], SIGPROF);
        }
        if (monotonic_ns() - last_report >= PS4_KIT_DIAG_REPORT_NS) {
            write_sampler_report();
            last_report = monotonic_ns();
        }
        nanosleep(&period, NULL);
    }
    return NULL;
}

static void write_hot_report(void)
{
    unsigned int count;
    unsigned int i;
    int fd = open_data_file("profile.log");
    char line[256];
    if (fd < 0)
        return;
    write_all(fd, "==== hot profiler window ====\n",
              sizeof("==== hot profiler window ====\n") - 1u);
    count = atomic_load_explicit(&g_thread_count, memory_order_acquire);
    for (i = 0; i < count; i++) {
        unsigned int samples = atomic_exchange_explicit(
            &g_threads[i].hot_count, 0u, memory_order_acq_rel);
        uintptr_t last = atomic_load_explicit(
            &g_threads[i].hot_last_rip, memory_order_relaxed);
        if (!samples)
            continue;
        snprintf(line, sizeof(line), "%s samples=%u last_rip=0x%llx\n",
                 g_threads[i].name, samples, (unsigned long long)last);
        write_all(fd, line, strlen(line));
    }
    close(fd);
}

static void *hot_profiler_thread_main(void *unused)
{
    struct timespec period = {0, 20000000l};
    struct timespec pause = {0, 200000l};
    uint64_t last_report = monotonic_ns();
    (void)unused;
    install_dump_handler();
    for (;;) {
        unsigned int count;
        unsigned int i;
        if (atomic_load_explicit(&g_hot_enabled, memory_order_acquire)) {
            pthread_t handles[PS4_KIT_DIAG_MAX_THREADS];
            pthread_mutex_lock(&g_thread_lock);
            count = atomic_load_explicit(&g_thread_count, memory_order_acquire);
            for (i = 0; i < count; i++)
                handles[i] = g_threads[i].handle;
            pthread_mutex_unlock(&g_thread_lock);

            {
                unsigned char signaled[PS4_KIT_DIAG_MAX_THREADS] = {0};
                pthread_mutex_lock(&g_signal_capture_lock);
                for (i = 0; i < count; i++) {
                    if (pthread_equal(handles[i], pthread_self()))
                        continue;
                    g_threads[i].dump_done = 0;
                    if (pthread_kill(handles[i], SIGUSR1) == 0)
                        signaled[i] = 1;
                }
                nanosleep(&pause, NULL);
                for (i = 0; i < count; i++) {
                    if (signaled[i] && g_threads[i].dump_done) {
                        atomic_store_explicit(&g_threads[i].hot_last_rip,
                                              g_threads[i].rip, memory_order_relaxed);
                        atomic_fetch_add_explicit(&g_threads[i].hot_count, 1u,
                                                  memory_order_relaxed);
                    }
                }
                pthread_mutex_unlock(&g_signal_capture_lock);
            }
        }
        if (monotonic_ns() - last_report >= PS4_KIT_DIAG_REPORT_NS) {
            write_hot_report();
            last_report = monotonic_ns();
        }
        nanosleep(&period, NULL);
    }
    return NULL;
}

void ps4_sampler_start(void)
{
    pthread_t thread;
    install_prof_handler();
    atomic_store_explicit(&g_sampler_enabled, 1, memory_order_release);
    if (atomic_exchange_explicit(&g_sampler_started, 1, memory_order_acq_rel))
        return;
    if (pthread_create(&thread, NULL, sampler_thread_main, NULL) == 0)
        pthread_detach(thread);
    else
        atomic_store_explicit(&g_sampler_started, 0, memory_order_release);
}

void ps4_sampler_set_enabled(int enabled)
{
    if (enabled)
        ps4_sampler_start();
    else
        atomic_store_explicit(&g_sampler_enabled, 0, memory_order_release);
}

void ps4_hot_profiler_set_enabled(int enabled)
{
    pthread_t thread;
    install_dump_handler();
    atomic_store_explicit(&g_hot_enabled, enabled != 0, memory_order_release);
    if (!enabled || atomic_exchange_explicit(&g_hot_started, 1, memory_order_acq_rel))
        return;
    if (pthread_create(&thread, NULL, hot_profiler_thread_main, NULL) == 0)
        pthread_detach(thread);
    else
        atomic_store_explicit(&g_hot_started, 0, memory_order_release);
}

int ps4_hot_profiler_enabled(void)
{
    return atomic_load_explicit(&g_hot_enabled, memory_order_acquire) ? 1 : 0;
}
