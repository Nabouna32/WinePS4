# Compatibility prototype status

## Implemented on `research/ps4-kit-reconstruction`

- `toolchain/ps4-kit-compat/generate-link-script.py`: clean-room generator for the publicly documented OpenOrbis linker-script fixes:
  - priority-ordered `.init_array.*` and `.ctors.*`;
  - priority-ordered `.fini_array.*` and `.dtors.*`;
  - suffixed `.data.*`, `.tdata.*`, and `.tbss.*` sections.
- `toolchain/ps4-kit-compat/ps4-linker.cmake`: invokes the generator, publishes `PS4_LINK_SCRIPT`, and redirects the CMake executable linker flags to the generated copy.
- `toolchain/ps4-kit-compat/ps4-kit.cmake`: provisional compatibility surface:
  - `ps4_kit_app(target)` generates the `<target>_eboot/eboot.bin` and `<target>_eboot/<target>.oelf` outputs expected by WinePS4's package script, using OpenOrbis `create-fself`;
  - `check-oelf.cmake` checks for exactly one read-write PT_LOAD before packaging;
  - `ps4_kit_link_mesa(target)` intentionally stops with a clear error. The private VideoOut WSI is not reproduced, and linking a public RADV archive set alone would not be equivalent.
- `toolchain/ps4-kit-compat/tests/test_linker_script.py`: host-side regression tests against a synthetic linker-script fixture.

## Evidence and design basis

The eboot output layout is directly required by WinePS4's existing `app/package.sh`. The create-fself invocation and OELF segment check follow public OpenOrbis/DolphinPS4 build conventions:
- https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/toolchain/ps4-love-style.cmake
- https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/toolchain/check-oelf.cmake
- https://github.com/iHaiDeeZ/xemu-ps4/blob/3738f4d7389055e9727c7ccf79af64e562eeb369/toolchain/make-link-script.py

The implementation is independent and does not copy those files.

## Networking audit added

- `docs/ps4-kit-reconstruction/NETWORK-API-MAP.md` now maps Wine's Winsock/NSI/DNS layers, WinePS4's `ps4_net.c` and `ps4_ifaddrs.c`, and the remaining private-kit boundaries.
- Confirmed in the public app CMake that WinePS4 compiles `ws2_32`, `nsiproxy` and its own DNS Unix half, and links `SceNetCtl`.
- Identified the explicit private runtime call `ps4_kit_load_module()` for Net/NetCtl and the resolver dependency that still needs an independent implementation or verified sysroot support.
- This is a source audit only. No network code was changed and no PS4 networking test has been run.

## Not yet implemented or verified

- The private kit's exact compiler and linker command contract for WinePS4's pinned kit revision `56b3d86`.
- WinePS4's exact PAID/auth-info requirements; current values are configurable defaults drawn from a public PS4 port and must be confirmed for this application.
- The private RADV + VideoOut WSI interface and its archive dependency closure.
- Crash logging, profiling, stdio/runtime hooks and the private packaging script.
- A real OpenOrbis configure/link test, eboot validation on a PS4, and a complete WinePS4 build.

## Test command

```sh
python3 -m unittest discover -s toolchain/ps4-kit-compat/tests -v
```

The unit tests cover generator transformations and fail-closed behavior only. They are not a substitute for running the generator against the exact `link.x` installed on the target build host.
