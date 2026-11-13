# Reconstructed ps4-kit

This directory is a clean-room reconstruction of the shared PS4 build/runtime
base described by the developer's public ports. Its first compatibility target
is the unchanged WinePS4 build, but the architecture is being widened to support
the confirmed direct consumers MGR-PS4, MP3-PS4, TTT2-PS4, ConkerLR-PS4
and Godot's PS4 branch, plus the related SSBU / DolphinPS4 / Crema / Resonance
build-tree lineage.

It is deliberately separate from WinePS4's `app/`, `port/`, `shell/` and
Wine source. It does not contain Sony SDK files, proprietary libraries, game
data, private keys or leaked binaries. Project-specific adapters and WSI
profiles must remain explicit; a Wine desktop window backend is not a native
VideoOut backend.

## Install without editing WinePS4

From a checkout at `$HOME/wineps4`:

```bash
ln -sfn "$HOME/wineps4/ps4-kit" "$HOME/ps4-kit"
export OPENORBIS=/opt/pacbrew/ps4/openorbis
# LLVM 21 is the documented modern public-port baseline; override if needed.
export CLANG_VER=21
source "$HOME/ps4-kit/toolchain/env.sh"
```

The public DolphinPS4, Crema, Resonance, M33, SMW, MGR, MP3 and TTT2 build
documentation uses LLVM/Clang 21 with OpenOrbis and libc++ 21. WinePS4 documents private kit
revision `56b3d86`, but the exact compiler revision used by that private kit
has not been recovered. The default in `env.sh` is therefore a public modern
baseline, not a claim that the private revision used the same toolchain.

## Check the ReXGlue project-injection contract

MGR-PS4 and MP3-PS4 use the same kit include at different CMake project names:
`CMAKE_PROJECT_sa2_INCLUDE` and `CMAKE_PROJECT_mp3_INCLUDE`. Both explicitly
pass `PS4_KIT_NETFS=OFF`. A host-only regression test exercises the hook timing,
runtime target, helper definitions and generated linker script:

```bash
bash "$PS4_KIT/tests/test-project-injection.sh"
```

This does not compile or link the PS4 application and does not implement the
private NetFS component. See
[DIRECT-CONSUMER-AUDIT.md](../docs/ps4-kit-reconstruction/DIRECT-CONSUMER-AUDIT.md)
for the source audit and remaining integration gates.

## Build the Mesa/RADV prerequisite

The WinePS4 CMake helper links static RADV archives; it does not build Mesa by
itself. Prepare the external Mesa 26.0.8-family Git checkout at
`$PS4_MESA_SOURCE`, the header-only libdrm source at
`$PS4_BUILD_ROOT/src/libdrm`, the host Meson venv at
`$PS4_BUILD_ROOT/mesa-venv`, and host `glslangValidator` at
`$PS4_BUILD_ROOT/host-tools/bin/glslangValidator`. The target sysroot must
contain libc++/libc++abi and the OpenOrbis libc overlay.

Then run:

```bash
bash "$PS4_KIT/scripts/build-mesa.sh" setup
```

The script verifies the Mesa patch with forward/reverse `git apply --check`,
stages the MIT GNM winsys, generates the PS4 Meson cross file, and builds the
public RADV static targets. It stops without modifying the Mesa tree if the
patch does not match. The final WinePS4 link step consumes the 16-archive
closure in `ps4-kit/mesa/archives.cmake`. This is a public Mesa 26.0.8
candidate, not proof of the exact Mesa revision used by private kit commit
`56b3d86`.

## Current implementation map

- `toolchain/env.sh`: OpenOrbis/PacBrew environment, build paths, versioned
  LLVM tools and the `ps4_cmake` helper.
- `ps4-kit.cmake`: compatibility entry point used by the unchanged WinePS4
  app and launcher CMake files; delegates to the clean-room interface under
  `toolchain/ps4-kit-compat/`.
- `include/ps4_kit.h` / `ps4_kit_diag.h`: source-level declarations
  reconstructed from WinePS4's own call sites. The heap-options layout is a
  new paired clean-room contract, not a recovered private binary ABI.
- `runtime/ps4_kit_runtime.c`: initial thread-priority, minimum-stack,
  system-module and Wine Vulkan callback implementations. Host tests use
  mocked Sce APIs.
- `include/ps4_kit_heap.h` and `runtime/ps4_kit_heap_policy.c`: a pure policy
  planner with a provisional shared-port default (512 MiB system-flexible
  main heap; 512 MiB direct overflow size when a consumer enables overflow),
  plus Wine's desktop/program/outside-process overrides and Godot's eight
  overflow heaps. The defaults are inferred from public callbacks/runtime,
  not recovered private ABI. It performs no PS4 allocations; the actual
  mspace/kernel mapping backend remains a blocker. Run its host tests with
  `bash ps4-kit/runtime/tests/run-heap-policy-tests.sh`.
- `scripts/make-pkg.sh`: initial OpenOrbis `PkgTool.Core` / `create-gp4`
  implementation, with the SFO attributes expected by WinePS4.
- `mesa/ac_ps4_drm.c` and `mesa/patches/mesa-ps4.patch`: MIT/per-file PS4
  GNM winsys and Mesa 26.0.8 integration patch from DolphinPS4 commit
  `879b3e57f3d3180bcb5f2234db3903688b28cd7d`, with separate MIT notices.
- `mesa/ps4_vk_wsi_window.cpp`: clean-room Wine window-desktop WSI using the
  existing desktop callback ABI.
- `mesa/ps4_vk_wsi_videoout.cpp`: first-pass clean-room native VideoOut WSI;
  2 MiB buffer alignment, four buffer banks with deferred retirement,
  VideoOut flip queue, GNM GPU-flip submission with CPU fallback, and binary
  semaphore filtering while preserving timeline semaphores. It is not yet
  target-compiled or validated on retail hardware.
- `scripts/build-mesa.sh`: Mesa 26.0.8-family cross-build driver based on the
  public DolphinPS4 recipe.

## Multi-project direction

See [PROJECT-COMPATIBILITY-MATRIX.md](../docs/ps4-kit-reconstruction/PROJECT-COMPATIBILITY-MATRIX.md)
for the evidence-backed project matrix and required profiles.

The shared kit must separate:

1. host compiler/build tools;
2. OpenOrbis target toolchain and sysroot;
3. shared PS4 runtime services;
4. common Mesa/RADV/GNM build outputs;
5. presentation profiles;
6. packaging and per-project adapters.

At minimum, presentation needs two distinct profiles:

- `window-desktop`: WinePS4 programs have no VideoOut; the headless Vulkan
  swapchain forwards completed frames to the desktop callback;
- `native-videoout`: a native app owns VideoOut buffers, flip queue and
  display lifecycle, as used by the emulator/recompilation ports.

Do not link the native VideoOut backend into WinePS4 or change its callback
contract. Share only low-level GNM/RADV primitives whose lifecycle and licence
have been checked.

## Known blockers

- The actual OpenOrbis target build, Mesa cross-build, final WinePS4 link and
  eboot/package output have not yet been validated end-to-end.
- The Wine-specific heap backend is not present: the current heap planner is
  policy-only and does not map PS4 memory or provide mspaces/overflow arenas.
- Diagnostics now have a first-pass runtime, but thread dumps capture only
  RIP/RSP, samplers log counts/last RIP rather than address histograms, JIT code
  dumps are absent, and the PS4 signal ABI still needs hardware validation.
- The native VideoOut source is present as a first-pass implementation, but it
  has not been compiled with OpenOrbis/Mesa or tested on retail hardware.
- `ps4_kit_link_mesa()` links only the generic RADV archive closure.
  `ps4_kit_app()` selects `window-desktop` automatically for WinePS4 and
  `native-videoout` for other Mesa apps. Host tests verify profile separation
  and the native source's required VideoOut/GNM hooks.
- MGR's private kit revision `79fba0f` is distinct from WinePS4's
  `56b3d86`; its exact CMake/runtime API still needs a complete audit.
- The package script has only fake-tool smoke coverage; it has not been run
  against a real OpenOrbis installation.

No claim is made yet that all projects configure, link, package or run on
hardware. Each profile needs its own reproducible acceptance test.


## Additional direct-consumer blockers found in TTT2

The TTT2 patch confirms that the shared CMake entry point must export
PS4_KIT_DIR as the root of this directory; the compatibility entry point now
does so. TTT2 also requires a diagnostics runtime, not just the
PS4_DIAG_DATA_DIR macro:

- ps4_watch_thread and ps4_dump_thread_stacks;
- ps4_thread_busy_report;
- ps4_sampler_register_thread, ps4_sampler_set_code_dumps and ps4_sampler_start;
- ps4_hot_profiler_set_enabled.

The branch exports ps4_data_dir() with C linkage and default visibility, and
includes a first-pass clean-room watcher, busy report, SIGPROF sampler and hot
profiler. It is intentionally not feature-equivalent yet; see the diagnostics
trace for exact gaps. Public DolphinPS4/Resonance/HENkaku/Conker code is
GPL-2.0 project code, so it is used as behavioral reference rather than copied. See
[DIAGNOSTICS-SAMPLER-TRACE.md](../docs/ps4-kit-reconstruction/DIAGNOSTICS-SAMPLER-TRACE.md).
