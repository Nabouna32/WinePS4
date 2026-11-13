# Heap and runtime reconstruction: public lineage and WinePS4 contract

Date: 2026-11-13

This audit follows the actual WinePS4 callers and the author's public runtime sources. It is intended to prevent the reconstruction from substituting a generic PS4 allocator for the much more specific process-aware heap that WinePS4 expects.

## 1. Proven public lineage

WinePS4's own [CREDITS.md](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/CREDITS.md) identifies the unpublished `ps4-kit` as the shared base merged from Resonance, DolphinPS4, M33, Crema and other ports. It names runtime, heap, threads, RADV, WSI and packaging.

The public implementation lineage is explicit in [Resonance `port/ps4_runtime.cpp`](https://github.com/iHaiDeeZ/Resonance/blob/6b87389033d4f239e97d36c605bf7eea384d1e23/port/ps4_runtime.cpp): it says it is based on [love-ps4 `ps4_heap.cpp`](https://github.com/iHaiDeeZ/love-ps4/blob/main/src/common/ps4_heap.cpp) and [`ps4.cpp`](https://github.com/iHaiDeeZ/love-ps4/blob/main/src/common/ps4.cpp).

- love-ps4 uses `sceLibcMspace` over named flexible memory, redirects malloc/calloc/realloc/free/memalign with linker wrappers, and implements `__cxa_thread_atexit_impl` using a pthread-key destructor list.
- Resonance/DolphinPS4/M33/Crema replace `sceLibcMspace` with dlmalloc 2.8.6 mspaces because the former stopped allocating after about 800 small allocations during Dolphin static initialization. Their runtime maps the heap itself and adds error diagnostics, overflow memory, and private per-thread arenas.
- [DolphinPS4 `port/ps4_dlmalloc.c`](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/port/ps4_dlmalloc.c) configures dlmalloc as mspaces-only, no mmap/sbrk, with locks and footers. The vendored [`malloc.c`](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/port/third_party/dlmalloc/malloc.c) is MIT-0.
- The allocator's PS4 adapter is not automatically MIT-0. DolphinPS4's repository is GPL-2.0; verify a file-specific notice before copying the adapter. The LÖVE source carries its own permissive notice requiring retention and marking modified source. A clean implementation of the Wine-specific allocator policy remains the safer route unless the relevant adapter's licence is established.

Independent confirmation: [HENkaku-PS4 credits](https://github.com/iHaiDeeZ/HENkaku-PS4/blob/main/CREDITS.md) say that its PS4 runtime (heap, crash/boot logs, profiler, DNS) and VideoOut presentation come from the same four projects, and that its LLVM 21/libc++ 21 sysroot libraries, including FFmpeg 7.1, come from M33's build. This confirms the runtime is a reusable shared lineage across more than WinePS4.

## 2. The Wine heap contract is more advanced than the public emulator heap

The checked-in [WinePS4 `port/ps4_main.c`](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/port/ps4_main.c#L104-L180) provides `ps4_kit_configure_heap()` before `_start`, before `main`, and before any allocation. It selects a different heap policy according to process role:

| Process role | Main heap policy | Growth / isolation policy |
|---|---|---|
| Desktop | direct memory first; up to 384 MiB, system pool only as a fallback (128 MiB) | desktop owns the app's display and long-lived services |
| Normal Wine program | direct memory first; 64 MiB main heap | up to eight 64 MiB direct-memory overflow heaps; reserve 256 MiB for the rest of the app; automatic 2 MiB arenas, six maximum; no system-pool heap |
| Program outside the PS4's seven local-process slots | regular flexible memory only; 16 MiB initial heap | up to three 16 MiB overflow heaps; no direct or system pool |

The `ps4_kit_heap_options` structure in our branch is still a provisional source contract inferred from those assignments, not a recovered private ABI. The implementation must honor the policy, not merely accept the structure.

### Historical kit changes recovered from WinePS4's public engineering notes

WinePS4's [NOTES.md](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/NOTES.md) exposes several private kit commit prefixes and the behavior they introduced:

| Private kit revision | Evidence in notes | Reconstruction implication |
|---|---|---|
| `195da48` | window-mode `set_window_present`: headless surface, requested swapchain size, linear host-visible images, present waits for GPU then calls a frame hook ([v00.58 notes](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/NOTES.md#L320-L330)) | confirms the Wine WSI is a distinct callback-driven window path, not the native VideoOut flip WSI |
| `a306bf5` | `overflow_direct_max`: 64 MiB main heap can grow in 64 MiB increments, up to eight ([v00.59 notes](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/NOTES.md#L368-L376)) | overflow heap count, allocation size and reserve are part of the observable allocator contract |
| `bb4a45c` | a `direct_max` below 256 MiB must be attempted as the main heap, not skipped; programs set `system_pool_max=0` ([v00.60 notes](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/NOTES.md#L386-L399)) | heap-size search order is correctness-critical; an incorrect threshold starved the system flexible pool and closed the app |
| `941b771` | Mesa patch 0030 implements placed Vulkan memory mappings by aliasing direct memory at the application's requested address ([v00.63 notes](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/NOTES.md#L476-L489)) | the Mesa port needs `VK_EXT_map_memory_placed`-compatible fixed mappings, not just ordinary RADV allocations |
| `53cf427` | Mesa patch 0031 enables direct-memory aliasing with `sceKernelEnableDmemAliasing` before the first placed map ([v00.67 notes](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/NOTES.md#L640-L650)) | placed-map implementation also has a process-level kernel prerequisite |
| later kit notes | per-thread heap/stdio depth counters are read by signal-time freeze diagnostics; per-thread priority APIs expose current and default-new-thread priority ([v00.64 and v00.71 notes](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/NOTES.md#L520-L532)) | diagnostics and thread-priority behavior are part of the kit surface beyond allocator basics |

The commit prefixes are recorded as references from the application's own notes; they are not full Git object IDs and do not prove that the private kit repository is publicly reachable.

## 3. Runtime interfaces consumed by WinePS4

The current Wine source confirms these requirements:

- `ps4_kit_configure_heap(options)` is called before normal runtime startup. The desktop, normal program, and outside-process cases have materially different memory budgets.
- `ps4_kit_set_min_thread_stack(WINE_KERNEL_STACK_SIZE)` is called before Wine starts. `WINE_KERNEL_STACK_SIZE` is 1 MiB; a larger default minimum would grow Wine's own 1 MiB NTDLL stacks beyond their intended size. The wrapper must only raise a smaller requested stack, never shrink a larger caller-specified stack.
- `ps4_kit_set_new_thread_priority(priority)` and `ps4_kit_set_thread_priority(priority)` are called at process-role transitions. New threads must inherit the selected priority before their start routine runs.
- `ps4_kit_thread_priority()` reports the current kernel priority, used to capture the desktop's initial priority.
- `ps4_kit_load_module(id,name)` is used by Wine's network/system-module setup. The name is useful diagnostic context even if the underlying OpenOrbis import accepts only the numeric module ID.
- Wine's Vulkan loader calls `ps4kit_vk_get_instance_proc_addr()`; this dispatch and the window-present callback are separate from the generic thread/heap runtime.

Relevant public callers:
- [WinePS4 `ps4_main.c`](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/port/ps4_main.c#L1500-L1515) sets the 1 MiB minimum stack before process setup.
- [WinePS4 `ps4_main.c`](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/port/ps4_main.c#L630-L680) applies process-specific current and default-new-thread priorities.
- [WinePS4 `ps4_ifaddrs.c`](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/port/ps4_ifaddrs.c) calls the kit module loader for network control setup.
- [WinePS4 `ps4_unixlibs.c`](https://github.com/iHaiDeeZ/WinePS4/blob/fd9595f98a9a495f19da295fe07d11f20578d50f/port/ps4_unixlibs.c#L90-L120) obtains the Vulkan instance/device dispatch through the kit's WSI loader.

## 4. What we should reuse, and what must remain Wine-specific

1. Reuse the public LÖVE thread-destructor implementation only with its original copyright/licence notice and clear modification marking. Test destructor order, pthread-key iteration, and allocation during thread exit.
2. Use dlmalloc 2.8.6 MIT-0 as a candidate allocator engine, with an independently written Wine heap adapter unless the adapter's exact licence is verified. The mspace footers behavior is important because pointers may be freed from threads other than the allocating thread.
3. Reconstruct the heap policy from Wine's actual callback: direct-first and direct-size search, overflow allocation and reserve, regular-only mode, system-pool prohibition, and automatic arenas. Do not transplant the emulator's fixed 512 MiB heap or name-selected per-thread arenas; Wine's policies differ.
4. Preserve Wine's priority model and 1 MiB minimum stack. The current branch's pthread wrappers are only a first host-tested implementation; verify target TLS, attr mutability and failure behavior with the OpenOrbis runtime.
5. Keep Wine's window-mode WSI: no VideoOut call in a program process. The native emulator WSI and the Wine desktop callback have different ownership and frame transport.


## 5. First executable reconstruction step: deterministic heap policy planner

The current WinePS4 `port/ps4_main.c` callback was fetched directly from the
public repository at the pinned base revision. Its actual values and comments
confirm the three policies without relying on inferred summaries:

- Desktop: direct-first, 384 MiB direct; 128 MiB system-flexible only if direct fails.
- Normal Wine program: direct-first, 64 MiB main; up to eight 64 MiB direct overflow heaps; preserve 256 MiB direct-memory reserve; six 2 MiB automatic arenas; system and regular flexible pools disabled (`regular_reserve = SIZE_MAX - 32 MiB`).
- Program launched by the desktop outside the seven local-process slots: regular flexible only, 16 MiB main and up to three further 16 MiB heaps; no direct or system pool.

Added clean-room files:

- `ps4-kit/include/ps4_kit_heap.h`: explicit plan and pool types.
- `ps4-kit/runtime/ps4_kit_heap_policy.c`: validates options and emits an ordered main-pool attempt list plus overflow/arena limits.
- `ps4-kit/runtime/tests/test_heap_policy.c`: regression tests for all three exact Wine profiles and invalid combinations.
- `ps4-kit/runtime/tests/run-heap-policy-tests.sh`: standalone host runner.

The planner is intentionally pure: no syscall, mapping, allocator or memory
reservation occurs here. This separates a testable policy contract from the
still-unimplemented target backend and prevents host tests from pretending
that PS4 memory allocation works. The new CI runtime job runs this test.

Important next backend requirements:

1. Add a target-only mspace allocator over a separately mapped region; dlmalloc
   2.8.6 is a candidate engine because its upstream allocator source is MIT-0,
   but the PS4 adapter must be independently written and separately licensed.
2. Implement direct/system/regular pool acquisition with checked alignment,
   map rollback and exact size limits; do not treat total direct-memory size as
   currently available memory.
3. Enforce the 256 MiB direct reserve when growing overflow heaps; the
   planner carries the value but does not yet enforce it against kernel state.
4. Implement per-thread automatic arenas only after cross-thread free/realloc
   behavior and arena lifetime are proven with mspace footers or an equivalent
   ownership mechanism.
5. Preserve the current Wine process callback and all `port/` files unchanged.

## 6. Remaining evidence gaps

- The private kit repository and full object IDs for the prefixes above are not public in the evidence examined here.
- The exact allocator data structures, lock strategy, auto-arena trigger policy and fallback order are not recovered. Wine's notes constrain externally visible behavior but do not reveal implementation.
- The current reconstruction has not been target-built or run on hardware. Heap allocation, fixed direct-memory aliases, pthread wrappers, and RADV placed maps still need console tests.
- No local SDK or LLVM installation was performed during this research.

