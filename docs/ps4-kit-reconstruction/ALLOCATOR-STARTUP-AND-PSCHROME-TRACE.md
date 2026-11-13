# Allocator startup and PSChrome lineage

Date: 2026-11-13

This document records the allocator implementations found by following the
credits and source links recursively. It is a source audit, not a claim that
these implementations are ABI-compatible with the unpublished WinePS4 kit.

## 1. LÖVE / love-ps4: the first working heap model

The public source is
[`iHaiDeeZ/love-ps4/src/common/ps4_heap.cpp`](https://github.com/iHaiDeeZ/love-ps4/blob/main/src/common/ps4_heap.cpp).
Its own source notice is the LÖVE project's permissive zlib-style licence; it
must be retained, and modifications must be marked.

Observed behavior:

- OpenOrbis libc's lazy heap asks for a 2.5 GiB system-flexible mapping. On
  retail hardware this fails; its fallback calls `sceLibcMspaceMalloc(NULL,` and
  faults before `main()`.
- The LÖVE port redirects the malloc family with linker `--wrap` and creates
  its own mspace on first allocation.
- It calls `sceKernelAvailableFlexibleMemorySize()` to measure the actual
  available flexible pool. It first tries system-flexible heap sizes of 1024,
  768, 512, 384, 256, 192 and 128 MiB.
- If that fails, it maps regular flexible memory while leaving 320 MiB for
  Piglet, and halves the candidate size down to 32 MiB.
- It also calls `sceKernelGetDirectMemorySize()` and
  `sceKernelAvailableDirectMemorySize()` for diagnostics; the latter is used
  to report the largest free direct-memory block, not merely total memory.
- `love-ps4/src/common/ps4.cpp` supplies `__cxa_thread_atexit_impl` with a
  pthread-key destructor list, because the PS4 libc++abi path expects a symbol
  the platform libc does not provide.

This is a valuable first working reference, but it is not sufficient for Wine:
the Wine process policy explicitly avoids the system pool for ordinary
programs, and it requires direct-memory overflow heaps and automatic arenas.

## 2. PSChrome: early replacement allocator and direct-memory discovery

SSBU's credits explicitly attribute its PS4 random source and malloc fixes to
**PSChrome**, and its build scripts use a local source/build tree at
`~/pschrome-build`. A standalone public PSChrome repository was not found in
this pass. Its implementation is therefore only recoverable through the
published downstream code; do not invent a repository URL or treat the
downstream copy as the complete original.

The downstream copy is
[`SSBU/toolchain/runtime/ps4_malloc.c`](https://github.com/iHaiDeeZ/SSBU/blob/7e83c0eb9367c657960876f1ea2b4b5491d99099/toolchain/runtime/ps4_malloc.c).
It records the failure mode and exposes a useful mapping recipe:

- Try direct memory first: get total size with
  `sceKernelGetDirectMemorySize()`, query the largest available region with
  `sceKernelAvailableDirectMemorySize()`, use 2 MiB alignment, leave a 2560
  MiB reserve, and cap the heap at 2048 MiB (minimum 64 MiB).
- If direct memory cannot supply it, use half the currently available regular
  flexible memory, aligned to 16 KiB.
- If that fails, try anonymous mmap from 256 MiB down to 64 MiB.
- Create a Sony `sceLibcMspace` on the chosen block. A 16-byte header records
  the base pointer and requested size so aligned allocations, realloc and
  usable-size reporting can work without a non-exported Sony query.
- Initialization is lazy and uses an atomic state. The source is an early
  fallback implementation, not the later full dlmalloc/mspace adapter.

The comments also identify firmware 12.00/13.52 as cases where OpenOrbis's
fixed 2.5 GiB allocation failed. This is useful historical evidence, not a
universal guarantee about every firmware or pool size.

## 3. DolphinPS4 / Resonance: dlmalloc mspaces and allocator lifecycle

The later implementation is in:

- [DolphinPS4 `port/ps4_dlmalloc.c`](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/port/ps4_dlmalloc.c)
- [DolphinPS4 `port/ps4_runtime.cpp`](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/port/ps4_runtime.cpp)
- [DolphinPS4 `toolchain/ps4-love-style.cmake`](https://github.com/iHaiDeeZ/DolphinPS4/blob/main/toolchain/ps4-love-style.cmake)
- [Resonance `port/ps4_runtime.cpp`](https://github.com/iHaiDeeZ/Resonance/blob/main/port/ps4_runtime.cpp)

The adapter configures dlmalloc 2.8.6 with:

- `ONLY_MSPACES=1`, `MSPACES=1`;
- `USE_LOCKS=1`, `FOOTERS=1`;
- `HAVE_MMAP=0`, `HAVE_MREMAP=0`, `HAVE_MORECORE=0`;
- 16 KiB page size and 64 KiB default granularity;
- corruption and invalid-free callbacks.

The runtime creates a mspace over a block it maps itself. It checks the
allocator with a small self-test allocation before accepting the heap. It
reserves virtual address space away from the kernel's pthread-object region
(early hints around 0x2 GiB collided with kernel allocations and corrupted
mspace control data), leaves a guard page before the heap, and logs the pool,
base, size, caller and stack on failure.

Resonance's main heap search is system-flexible memory first (512, 384, 256,
192, 128 MiB), then regular flexible memory while preserving 320 MiB for the
system/Piglet, then direct-memory fallbacks (256 and 128 MiB). The exact
sequence differs by project and evolved over time: SSBU/SMW use a smaller
system-pool cap because many live mutexes and shader objects consume the same
pool. Therefore do not copy one project's static size list into the Wine
backend.

Resonance also has a direct-memory overflow heap, per-thread arenas, arena
reuse on thread exit, footer-based cross-thread free/realloc, heap integrity
checks, and error diagnostics. Its source documents the practical reason for
per-thread arenas: one global locked heap made busy CPU/video/Vulkan/shader
threads contend on a spin lock.

## 4. SSBU/DolphinPS4: system libc replacement is a separate requirement

A crucial finding: linker `--wrap=malloc` is not the whole story.

DolphinPS4's `toolchain/ps4-love-style.cmake` first tries to patch the
OpenOrbis `crt1.o` data section to declare a larger `sceLibcHeapSize`. The
hardware test showed no effect. The actual fix is a replacement startup
object, [`port/ps4_crt1.S`](https://github.com/iHaiDeeZ/DolphinPS4/blob/main/port/ps4_crt1.S),
which fills the process parameter's `_sceLibcMallocReplace` table
(`_sceLibcParam + 0x30`). The table is version 1 and carries init/finalize,
malloc/free/calloc/realloc/memalign/reallocalign/posix_memalign and statistics
callbacks. The system libc then forwards process-wide allocations—including
Sony system modules such as Piglet and its shader compiler—to the port's
replacement functions.

The documented default system libc heap was about 12.7 MiB on hardware and
was exhausted by Piglet's program links, shader compiles and command buffers.
Changing `sceLibcHeapSize` did not solve it; the malloc-replacement table did.

The build also uses linker `--wrap` for the application's malloc family,
`pthread_once`, `mmap` and `clock_gettime`, each for a separate OpenOrbis
runtime issue. These are not interchangeable with the system-libc replacement
table.

**Wine reconstruction consequence:** Wine's `ps4_kit_configure_heap()` is
called while system libraries start, before `_start`, `main` or any
allocation. This timing makes the startup-object/process-parameter path a
high-priority hypothesis for the private kit. However, WinePS4's public source
does not expose the private kit's exact `crt1` implementation. Before writing
a replacement object, inspect the kit's expected process parameter layout,
OpenOrbis CRT revision and the target's symbol/import behavior. Do not assume
that a plain `--wrap` implementation can intercept Sony's system modules.

## 5. Wine's heap is not the public emulator heap

Wine's actual callback and engineering notes specify a different policy:

| Role | Main heap | Overflow | Other rules |
|---|---|---|---|
| Desktop | direct-first, 384 MiB | no overflow specified in the callback | system flexible fallback 128 MiB |
| Normal Wine program | direct-first, 64 MiB | eight direct heaps of 64 MiB | keep 256 MiB direct reserve; six automatic 2 MiB arenas; never system or regular flexible memory |
| Outside process | regular flexible only, 16 MiB | three regular heaps of 16 MiB | no direct or system pool |

The public notes explain that the 64 MiB main heap must be attempted even
though it is below 256 MiB; an older kit version skipped it and instead used
128 MiB from the system pool per process, exhausting the pool and closing the
whole app. They also explain why program processes cannot take the system pool
and why the 256 MiB direct reserve must remain available to the app's other
memory consumers.

The current branch's deterministic planner expresses this policy but is not
the allocator backend. It does not reserve/map memory, create mspaces, route
malloc-family calls, install the system libc replacement table, grow overflow
heaps, detect contention, or handle cross-thread frees.

## 6. Implementation checklist before target allocator code

1. Recover the exact OpenOrbis `crt1.o` and process parameter ABI used by the
   consumer toolchain. Determine whether Wine's kit needs both application
   `--wrap` and system `_sceLibcMallocReplace` hooks.
2. Define an explicit memory-region ownership record for each main/overflow
   heap: virtual reservation, mapped address, physical direct-memory offset,
   pool kind, size, alignment and release state.
3. Query available direct memory with `sceKernelAvailableDirectMemorySize`
   over the valid search range. Never derive free memory from total direct
   memory alone.
4. Check every allocation, map and mspace-creation result. If mapping fails
   after direct-memory allocation, release the physical allocation. If the
   kernel returns an address other than the required fixed address, reject it
   and roll back. Record the mapping semantics rather than assuming a successful
   API return means the requested address was honored.
5. Implement Wine's exact 64 MiB main/overflow sizes and 256 MiB reserve.
   The overflow allocator must not consume the reserve, and a failed main
   allocation must not silently fall back to the system pool.
6. Implement automatic 2 MiB arenas based on the kit's actual contention
   policy (not hard-coded emulator thread names). A chunk allocated in an
   arena must be freeable/reallocatable by another thread; dlmalloc footers
   are one possible mechanism.
7. Ensure initialization is safe before `main()`, including recursive
   allocations made by kernel/system calls during heap creation. The first
   Wine callback runs before ordinary application startup.
8. Add host tests for arithmetic, overflow bounds, reserve enforcement,
   allocator-selection order, reentrant initialization, ownership lookup and
   rollback. These do not replace retail-hardware tests.
9. Keep the allocator engine's upstream license separate from the PS4 adapter's
   license. DolphinPS4's credits identify the vendored dlmalloc as CC0/MIT-0;
   that does not automatically relicense the adapter or surrounding runtime.
