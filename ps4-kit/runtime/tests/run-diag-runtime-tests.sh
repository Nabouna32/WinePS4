#!/usr/bin/env bash
# Host test for the clean-room diagnostics thread registry and watchdog dump.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" /tmp/ps4-kit-diag-runtime-test' EXIT
CC="${CC:-cc}"
command -v "$CC" >/dev/null 2>&1

cat > "$TMP/test_diag_runtime.c" <<'EOF'
#define _GNU_SOURCE
#include "ps4_kit_diag.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

PS4_DIAG_DATA_DIR("/tmp/ps4-kit-diag-runtime-test");

static _Atomic int g_stop;
static _Atomic unsigned long g_work;

static void *worker_main(void *unused)
{
    (void)unused;
    ps4_sampler_register_thread("worker");
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed))
        atomic_fetch_add_explicit(&g_work, 1ul, memory_order_relaxed);
    return NULL;
}

int main(void)
{
    pthread_t worker;
    char report[512];
    char dump_path[256];
    char contents[4096];
    FILE *file;
    size_t read_count;

    mkdir(ps4_data_dir(), 0777);
    if (pthread_create(&worker, NULL, worker_main, NULL) != 0)
        return 1;
    ps4_watch_thread("main");

    usleep(50000);
    ps4_thread_busy_report(report, sizeof(report), 0);
    usleep(50000);
    ps4_thread_busy_report(report, sizeof(report), 0);
    if (!strstr(report, "worker")) {
        fprintf(stderr, "busy report omitted worker: %s\n", report);
        return 2;
    }

    snprintf(dump_path, sizeof(dump_path), "%s/hang.log", ps4_data_dir());
    ps4_dump_thread_stacks(dump_path, "host-test");
    file = fopen(dump_path, "rb");
    if (!file)
        return 3;
    read_count = fread(contents, 1, sizeof(contents) - 1u, file);
    fclose(file);
    contents[read_count] = '\0';
    if (!strstr(contents, "host-test") || !strstr(contents, "worker")) {
        fprintf(stderr, "thread dump missing reason or worker:\n%s\n", contents);
        return 4;
    }

    ps4_sampler_set_code_dumps(0);
    ps4_hot_profiler_set_enabled(0);
    atomic_store_explicit(&g_stop, 1, memory_order_relaxed);
    pthread_join(worker, NULL);
    puts("ps4-kit diagnostics host tests passed");
    return 0;
}
EOF

"$CC" -std=c11 -Wall -Wextra -Werror -pthread \
    -I"$ROOT/ps4-kit/include" \
    "$ROOT/ps4-kit/runtime/ps4_kit_diag_runtime.c" \
    "$TMP/test_diag_runtime.c" -o "$TMP/test_diag_runtime"
"$TMP/test_diag_runtime"
