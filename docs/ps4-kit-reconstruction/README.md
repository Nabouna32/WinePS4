# ps4-kit — reconstruction notes

This directory tracks a **clean-room compatibility effort** for the build interface used by WinePS4.

It is not the original ps4-kit, and it does not contain Sony SDK files, proprietary libraries, leaked binaries, or code copied from repositories without a clear redistribution licence. The goal is to document public interfaces and, later, implement independent replacements where legally and technically possible.

## Why this exists

The WinePS4 build currently depends on an external, unpublished kit:

- `app/CMakeLists.txt` includes `$HOME/ps4-kit/ps4-kit.cmake`.
- It calls `ps4_kit_app(wineps4)` and `ps4_kit_link_mesa(wineps4)`.
- It uses kit-provided build configuration such as `PS4_LINK_SCRIPT` and `PS4_SYSROOT`.
- `docs/BUILDING.md` says the kit is not published and records the version used for the v00.71 build (`56b3d86`).

A public repository by the same author, MGR-PS4, describes ps4-kit as providing toolchain setup, runtime, PS4 Mesa RADV, a VideoOut swapchain, crash logging, a profiler, and packaging. It also says the kit is linked at build time and is not published yet.

## Current strategy

Rebuild the **minimum compatible interface**, not an imagined byte-for-byte copy of the private kit:

1. Inventory every CMake function, variable, header, library and script WinePS4 expects.
2. Separate build-host tools from PS4 runtime components.
3. Identify which components can use OpenOrbis and public upstream projects.
4. Write independent CMake/toolchain glue and small compatibility implementations.
5. Keep unavailable pieces explicitly marked as blockers until there is a lawful, testable replacement.
6. Build and test incrementally on a PS4 development environment.

## Status

See [PUBLIC-TRACES.md](PUBLIC-TRACES.md) for the evidence collected so far, [NETWORK-API-MAP.md](NETWORK-API-MAP.md) for the Winsock/NSI/DNS/NetCtl dependency map, and [INTERFACE-AUDIT.md](INTERFACE-AUDIT.md) for the build/runtime boundary.

No replacement implementation is claimed to work yet. In particular, the graphics/RADV integration, PS4 linker script, sysroot, packaging/signing path and runtime must be validated rather than guessed.

## Rules for contributions

- Prefer original implementations and documented public APIs.
- Record the source URL, commit, file path and licence for every reused component.
- Do not commit Sony SDK files, private keys, console credentials, or proprietary binaries.
- Do not describe this directory as the original ps4-kit.
- Keep experimental code separate from WinePS4's production build until it passes a real build.
