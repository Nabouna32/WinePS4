# Historical traces across iHaiDeeZ's public PS4 repositories

Chronological findings relevant to reconstructing WinePS4's unpublished ps4-kit. These are public commit messages and repository files; they do not expose the private kit itself.

## 2026-09-26 — love-ps4: OpenOrbis baseline on real hardware

- [Add PS4 platform support](https://github.com/iHaiDeeZ/love-ps4/commit/4410b32411d10fa567c74ed74929d653e869721d)
  - Explicitly says the port is built with the OpenOrbis toolchain.
  - Documents PacBrew/OpenOrbis-based CMake support, static liblove, Piglet GLES2, SDL and PS4-specific runtime behavior.
- [Run prioritized constructors](https://github.com/iHaiDeeZ/love-ps4/commit/75aa9799d0a3d354df67acb4ca6a5c625fa95160)
  - Records a real hardware failure caused by the OpenOrbis linker script collecting only plain `.init_array`, leaving prioritized constructors orphaned.
  - Fix: generate a modified linker script.

This is the earliest public lineage found in this search for the toolchain/linker work later reused by other ports.

## 2026-10-01 — DolphinPS4: toolchain and hardware probe

- [PS4 toolchain, packaging and hardware probe](https://github.com/iHaiDeeZ/DolphinPS4/commit/a60339661046bf5dc3f45999cc92da1042b40c06)
  - Commit message explicitly describes a love-ps4-style build using PacBrew `ps4.cmake`, patched `link.x`, heap wrapping and optional clang 21/libc++ 21.
  - Adds a probe for memory, JIT, fault handling and Piglet GLES support.
- [Dolphin PS4 port: build scripts and source patch](https://github.com/iHaiDeeZ/DolphinPS4/commit/a52d6ffd72e60c7c46e9f2efecaef9dc6d00007b)
  - Describes PS4 build scripts, compiler wrappers and FreeBSD header overlays.

## 2026-10-02 — DolphinPS4: second linker-script defect

- [Collect per-symbol data sections](https://github.com/iHaiDeeZ/DolphinPS4/commit/fdd639f50b055ff94db788c85e6cae0caedda9d6)
  - Commit message says OpenOrbis `link.x` gathered only plain `.data`, `.tdata`, `.tbss`.
  - C++ statics, stdio FILE objects and sections emitted by `-fdata-sections` could be placed beyond the data segment's file image and appear zero-initialized at runtime.
  - Fix: collect the suffixed section families too.

## 2026-10-15 — xemu-ps4: reusable toolchain/runtime template

- [Toolchain, PS4 runtime and dependency builds from the Resonance template](https://github.com/iHaiDeeZ/xemu-ps4/commit/7961525546ee3b91c37c61b819be3966cd8d4054)
  - Describes reuse of a common toolchain/runtime template and a sysroot overlay.
- [xemu builds, links and packages for PS4](https://github.com/iHaiDeeZ/xemu-ps4/commit/fa43482b504efda710ec89390a954f451af7b6f8)
  - Records the more complete build path: clang, stub-free libc probes, compatibility library, generated link script, SDL3, eboot/OELF checks and package creation.

The current public xemu-ps4 `toolchain/env.sh` makes the split explicit: clang 21/libc++ 21 sits on top of the OpenOrbis C runtime. This is a newer custom layer, not the stock OpenOrbis toolchain itself.

## 2026-11-05 — MGR-PS4: ps4-kit still separate and unpublished

- [MGR-PS4 v00.09](https://github.com/iHaiDeeZ/MGR-PS4/commit/5c6293b6262d0a2815dcaa4f985872c98c6e0e3c)
  - Its credits distinguish OpenOrbis/PacBrew from ps4-kit.
  - ps4-kit is described as toolchain setup, runtime, Mesa RADV, VideoOut swapchain, crash log, profiler and packaging, and explicitly says it is not published.
  - The build script uses `$HOME/ps4-kit/toolchain/env.sh`, `ps4_cmake`, `ps4-kit.cmake`, `ps4_kit_app`, `PS4_KIT_NETFS`, and `scripts/make-pkg.sh`.

## Practical reconstruction consequences

1. Start with PacBrew OpenOrbis and its own `ps4.cmake`, `ps4vars.sh`, CRT, `link.x` and `create-fself`.
2. Recreate the known linker-script corrections as an independent generator with strict input checks.
3. Keep the later clang/libc++ sysroot and runtime overlay optional; WinePS4's actual ps4-kit revision must determine which compiler ABI it expects.
4. Treat ps4-kit-only runtime, RADV/WSI and package behavior as separate unknown interfaces. Do not infer their exact code from the public ports' similarly named helpers.
