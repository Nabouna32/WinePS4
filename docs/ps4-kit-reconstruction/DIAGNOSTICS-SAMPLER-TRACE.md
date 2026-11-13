# Diagnostics, thread watcher and sampler API trace

Date: 2026-11-13

This audit follows the direct-consumer patch for TTT2 into the public
DolphinPS4, Resonance, HENkaku and Conker runtime implementations. It identifies
the missing symbols that prevent a complete TTT2 link. The implementations in
those public repositories are GPL-2.0 project trees and are used here as
behavioral references only; this reconstruction must not copy their code
without a separate licence review.

## 1. Confirmed kit symbols required by TTT2

The TTT2 PS4 patch explicitly references these private-kit symbols:

| Symbol | How TTT2 uses it | Public reference / status |
|---|---|---|
| ps4_watch_thread(name) | Registers the main thread for watchdog stack dumps | DolphinPS4/Resonance/HENkaku/Conker sampler |
| ps4_dump_thread_stacks(path, reason) | Watchdog writes all watched stacks to /data/TTT2/hang.log | DolphinPS4/Resonance/HENkaku/Conker sampler |
| ps4_thread_busy_report(out, size, min_percent) | GPU monitor reports the busiest watched threads and their CPU percentages | Resonance/HENkaku/Conker sampler; absent from the older DolphinPS4 revision |
| ps4_sampler_register_thread(name) | Registers the render and command-processor threads for profiling | DolphinPS4/Resonance/HENkaku/Conker sampler |
| ps4_sampler_set_code_dumps(on) | TTT2 passes 0 because it has no Dolphin-style JIT/guest-memory dump ranges | TTT2-only private-kit API; not found in the public sampler revisions checked |
| ps4_sampler_start() | Starts 1 kHz SIGPROF sampling when TTT2_PROFILE=2 | DolphinPS4/Resonance/HENkaku sampler |
| ps4_hot_profiler_set_enabled(on) | Starts the 20 ms per-thread hot-spot profiler when TTT2_PROFILE=1 | Conker-PS4 sampler; not in the older DolphinPS4/Resonance versions |
| PS4_DIAG_DATA_DIR(path) | Sets the per-application diagnostics directory to /data/TTT2 | TTT2 port/ps4/win32_extra.cpp |

TTT2's project code declares the sampler functions with extern C, includes
ps4_kit_diag.h in its watchdog and runtime glue, and calls the profiler APIs
from its command-processor/render threads. The target name is ttt2; its CMake
project name is ttt2_ps4. Its CMakeLists includes $HOME/ps4-kit/ps4-kit.cmake,
adds $PS4_KIT_DIR/runtime to the platform object library, then calls
ps4_kit_link_mesa(ttt2) and ps4_kit_app(ttt2).

## 2. Public implementation evolution

### DolphinPS4 (commit 879b3e57f3d3180bcb5f2234db3903688b28cd7d)

File: [port/ps4_sampler.cpp](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/port/ps4_sampler.cpp)

- ps4_watch_thread registers a thread, copies its name, stores its kernel thread ID and a safe upper stack bound.
- ps4_dump_thread_stacks sends SIGUSR1 (FreeBSD signal 30), waits for each handler and writes the interrupted RIP plus code addresses from the stack.
- When the thread is blocked in the FreeBSD _umtx_op syscall, the handler records its wait arguments and the dumper reads the mutex/condition-variable object words only after checking memory protection.
- ps4_sampler_register_thread adds the thread to a bounded sampler list and calls ps4_watch_thread.
- ps4_sampler_start installs SIGPROF (FreeBSD signal 27), starts a helper thread, sends SIGPROF at 1 kHz, and writes 10-second sample windows.
- The sampler can record eboot, system-module and JIT addresses. JIT/guest RAM dumps are Dolphin-specific and must be optional.
- It also exposes per-thread CPU-time and wait counters for Dolphin's own monitor.

### Resonance / HENkaku

The later sampler revisions add ps4_thread_busy_report:

- Tracks each watched thread's CPU time using ps4_thread_cpu_tick() or pthread_getcpuclockid().
- Computes CPU time divided by elapsed wall time since the previous report.
- Returns names and percentages at or above min_percent, sorted busiest first, without allocating.

This is the public implementation lineage most relevant to TTT2's GPU monitor.

### Conker-PS4

File: [port/ps4_sampler.cpp](https://github.com/iHaiDeeZ/Conker-PS4/blob/main/port/ps4_sampler.cpp)

Conker adds the switchable hot-spot profiler:

- ps4_hot_profiler_set_enabled(1) creates a helper thread once, then samples watched threads every 20 ms.
- The profiler reuses the watched-thread signal capture path, distinguishes code in the eboot from system-library time, and attributes system-library samples to callers where possible.
- It writes profile.log every 10 seconds.
- It retains ps4_thread_busy_report and the hang-dump API.

## 3. Data-directory macro contract

TTT2 calls PS4_DIAG_DATA_DIR("/data/TTT2") once from its platform glue. The
runtime must use that value for samples.log and profile.log, while
ps4_dump_thread_stacks must honor the explicit path supplied by the watchdog.

The current branch's macro only emits a C++ namespace-scope const object. A
plain namespace-scope const in C++ may have internal linkage, so this is not
yet a reliable exported runtime contract. The header should emit an extern-C
visible symbol and the diagnostics runtime should read it, with a safe
fallback directory when no application override is present.

## 4. Licensing and implementation strategy

- DolphinPS4 and Resonance top-level trees are GPL-2.0; HENkaku and Conker also
  use GPL-2.0 project trees. No separate permissive file notice was found on
  the sampler files in this audit.
- Do not copy their sampler source into WinePS4's shared runtime just because
  ShiroKlein is credited as the author. Keep a clean-room implementation from
  the public API contract and measured behavior, or obtain explicit
  relicensing/permission.
- Do not transplant Dolphin's ps4_heap_private_arena(name) call into the
  generic ps4_watch_thread API. It is emulator-specific; Wine's heap policy
  has its own automatic arena controls.
- Preserve the signal numbers and mcontext offsets as target-specific
  constants, isolate them behind a PS4-only compilation guard, and validate
  on the console. Do not treat a host build as proof of signal-context ABI.

## 5. Current reconstruction gap

The branch currently provides only the PS4_DIAG_DATA_DIR macro. It does not
yet implement any of the sampler, hot-profiler, thread-busy or stack-dump
symbols above. TTT2 therefore cannot complete its final link yet. The next
runtime milestone is a clean-room diagnostics component with:

1. exported per-app data directory and bounded thread registry;
2. safe SIGUSR1 stack snapshots and a watchdog dump routine;
3. per-thread CPU-time busy report;
4. optional 1 kHz sampler and 20 ms hot profiler;
5. optional code/JIT dump hooks, disabled by default for TTT2;
6. host tests for registry limits, report formatting, ring-buffer accounting
   and path selection, plus separate target-only signal-context tests.
