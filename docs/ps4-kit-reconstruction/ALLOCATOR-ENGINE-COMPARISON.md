# Allocator engine comparison: DolphinPS4 vs WinePS4

Date: 2026-11-13

This note records a source-level comparison of the public DolphinPS4 allocator and the heap policy actually requested by WinePS4. The public code is a valuable technical reference, not a drop-in implementation: DolphinPS4 is GPL-2.0 and its allocator adapter is not covered automatically by the MIT-0 notice on the vendored dlmalloc engine.

## 1. Confirmed public allocator architecture

Source inspected: [DolphinPS4 port/ps4_runtime.cpp](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/port/ps4_runtime.cpp), especially its heap setup, overflow heap, wrappers and `ps4_heap_private_arena()`; [DolphinPS4 port/ps4_dlmalloc.c](https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/port/ps4_dlmalloc.c).

- It replaces the OpenOrbis libc malloc family because the default libc's first allocation attempts a 2.5 GiB system-flexible-memory mapping, which fails on retail consoles.
- It creates a dlmalloc mspace over a kernel-mapped region. Its primary search prefers system flexible memory (512, 384, 256, 192, 128 MiB), then regular flexible memory while preserving 320 MiB for Piglet, then direct memory (256, 128 MiB).
- Before mapping flexible memory, it reserves virtual address space and puts a guard page in front of the heap. It deliberately chooses addresses above the area where kernel pthread objects were observed to land, after an allocator control block was overwritten by those objects.
- The direct-memory overflow heap is lazy and one-shot: it tries 512, 384, 256, then 128 MiB; maps at a high virtual address; publishes the mspace atomically; and uses dlmalloc FOOTERS so a chunk can be freed/reallocated through the correct owning mspace.
- The allocator has corruption/invalid-pointer diagnostics, caller addresses and stack traces. It also implements the system libc malloc-replacement hooks, so allocations from Sony modules such as Piglet are routed into the replacement heap.
- Per-thread arenas are opt-in by thread name, not automatic for every thread. Dolphin's named busy threads receive an 8 MiB mspace, with a maximum of 12. The arena is carved from the main heap. On thread exit it is put on a protected free list and reused by later threads; outstanding chunks remain valid because FOOTERS records the originating arena.
- A full private arena falls back to the shared main heap. malloc/calloc/memalign try private arena → main heap → direct overflow. The arena optimization exists to reduce lock contention on CPU, video, Vulkan submission, shader compiler, DVD and audio threads.

## 2. Kernel APIs available in the public OpenOrbis headers

The current OpenOrbis `include/orbis/libkernel.h` declares these relevant entry points:

- Direct memory: `sceKernelAllocateDirectMemory`, `sceKernelMapDirectMemory`, `sceKernelReleaseDirectMemory`, `sceKernelGetDirectMemorySize`.
- Flexible memory: `sceKernelAvailableFlexibleMemorySize`, `sceKernelMapFlexibleMemory`, `sceKernelMapNamedFlexibleMemory`, `sceKernelMapNamedSystemFlexibleMemory`, `sceKernelReleaseFlexibleMemory`.
- Virtual address management: `sceKernelReserveVirtualRange`, `sceKernelMunmap`, `sceKernelSetVirtualRangeName`.
- Direct-memory alias support: `sceKernelEnableDmemAliasing` is referenced by the WinePS4 notes/Mesa patch lineage, not treated here as a generic heap requirement.

References: [OpenOrbis libkernel documentation](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/docs/MD/PS4%20Libraries/Libkernel.md), [OpenOrbis libkernel.h](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/libkernel.h).

The signatures in the OpenOrbis header are useful declarations, but the header often labels comments as empty and is not a substitute for testing error codes, flags, alignment and ownership rules on the target runtime.

## 3. Why Dolphin's policy must not be copied into WinePS4

| Concern | DolphinPS4 public runtime | WinePS4 contract inferred from its checked-in callback |
|---|---|---|
| Main pool order | system flexible first | desktop and ordinary Wine process: direct memory first |
| System pool | preferred main pool | ordinary Wine process sets system pool maximum to zero |
| Main heap size | adaptive large pool sizes | ordinary Wine process starts at 64 MiB |
| Overflow | one lazy direct heap, size search 512→128 MiB | up to eight direct overflow heaps, each 64 MiB, with a configured direct-memory reserve |
| Private arenas | 8 MiB, max 12, selected by thread name | automatic 2 MiB arenas, max six |
| Process roles | emulator-specific policy | desktop, normal Wine process, and process outside seven local-process slots have distinct budgets |
| libc replacement | explicit Sony system-libc replacement hooks | not yet proven to be part of the private kit's heap contract |

The private kit's historical behavior is constrained by WinePS4 `NOTES.md`: `a306bf5` introduced 64 MiB direct overflow increments up to eight; `bb4a45c` fixed attempts for direct maxima below 256 MiB and disabled the system pool for programs. Those details make a generic "Dolphin heap with different constants" unsafe.

## 4. Reconstruction implications

1. **Reuse dlmalloc as an allocator engine only after confirming the exact vendored version and MIT-0 notice.** Do not copy DolphinPS4's GPL-2.0 adapter into the clean-room implementation without a separate file-specific licence basis.
2. Implement a Wine-owned memory-source backend with explicit source policy: direct-first, system-pool disabled where requested, regular-only process mode, reserved memory and exact overflow count/size.
3. Track each mapped region independently: source type, physical allocation token where applicable, virtual base, mapped length, allocator mspace, and cleanup state. Never release a direct-memory allocation until its mapping and all chunks are no longer in use.
4. Use mspace FOOTERS or an equivalently proven ownership mechanism if chunks may be freed by a different thread or from a different arena. Add tests for cross-thread free/realloc and for allocations crossing arena/main/overflow boundaries.
5. Treat virtual-address placement and guard pages as optional, separately tested hardening. Dolphin's observed `0x200000000` collision and high-address hints are specific empirical facts, not universal addresses to hardcode in Wine.
6. Keep all kernel memory calls behind a host-mockable interface. Host tests can validate policy, overflow progression, rollback and allocator routing; only a real PS4 target can validate the kernel's actual memory availability and mapping behavior.

## 5. Outstanding questions

- Does the private kit install the Sony system-libc malloc-replacement table, or does it only wrap application-linked malloc symbols?
- What exact dlmalloc revision/configuration and FOOTERS behavior did the private kit use?
- Does Wine's automatic arena assignment depend on thread creation, runtime thread registration, or a later allocation/role signal? The 2 MiB / six-arena values are visible, but the trigger is not.
- How does the private allocator reclaim/reuse overflow heaps and arenas, and what happens if an overflow map succeeds but mspace initialization fails?
- What exact direct-memory type/protection and alignment choices are required by Wine's Vulkan placed-memory mappings?

Until those questions are answered, this is an evidence map and architecture constraint, not a claim that the Wine heap has been reconstructed.
