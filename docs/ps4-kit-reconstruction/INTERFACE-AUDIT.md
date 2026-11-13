# ps4-kit interface audit (WinePS4)

Status: static audit of the public WinePS4 source. This is an interface inventory, **not** an implementation of ps4-kit and not proof that a replacement can link or boot on a PS4.

Audited against WinePS4 `main`:
- `app/CMakeLists.txt`
- `app/launcher/CMakeLists.txt`
- `app/build.sh`
- `app/package.sh`
- `docs/BUILDING.md`

## 1. Required entry points

| Interface | Observed use | What a compatible layer must eventually provide |
|---|---|---|
| `$HOME/ps4-kit/ps4-kit.cmake` | Included by both app CMake projects | CMake toolchain/configuration glue and the two functions below |
| `ps4_kit_app(target)` | Called for `wineps4` and `wineps4_launcher` | PS4 executable/eboot target setup, link settings, output layout; exact behavior still unknown |
| `ps4_kit_link_mesa(target)` | Called for `wineps4` only | Mesa/RADV and PS4 window-system linkage; exact libraries and flags still unknown |
| `PS4_LINK_SCRIPT` | Read and modified by `app/CMakeLists.txt` | Path to a writable linker script compatible with the selected PS4 linker |
| `PS4_SYSROOT` | Used for headers and static `libz.a`/`libcrypto.a` | Root containing the expected target headers and libraries |
| `$HOME/ps4-kit/toolchain/env.sh` | Sourced by build and package scripts | Exports the build environment and helper commands below |
| `ps4_cmake` | Configures both CMake projects | Wrapper that selects the PS4 toolchain and passes required CMake settings |
| `PSB` | Build, logs, host tools, outputs, staging paths | Workspace root used by the scripts |
| `CLANG_VER` | Selects versioned `ld.lld` and `llvm-objcopy` executables | Tool version suffix compatible with installed LLVM tools |
| `JOBS` | Passed to Ninja's `-j` | Parallel build count |
| `$PS4_KIT/scripts/make-pkg.sh` | Creates the final PKG from a staging directory | Packaging helper and its expected arguments/environment |

## 2. Build flow observed

1. `app/build.sh` sources `toolchain/env.sh`, adds `$PSB/host-tools/bin` to `PATH`, and exports `CLANG_VER`.
2. It configures the main app with `ps4_cmake -S app -B $PSB/build/wineps4 -G Ninja`.
3. CMake builds a target named `wineps4`; the script also configures and builds the separate launcher project.
4. The CMake project uses versioned `ld.lld-$CLANG_VER -r` and `llvm-objcopy-$CLANG_VER` for intermediate object consolidation/localization.
5. `app/package.sh` stages the launcher eboot, app eboot, Wine payload, prefix, fonts, wallpapers and tests.
6. It invokes `make-pkg.sh` with `NO_RIGHT_SPRX=1`, `SFO_ATTRIBUTE=0x800002`, and arguments: stage directory, title ID `WINE00002`, title `WinePS4`, version, content ID `WINEPS4`, output directory.

## 3. Important linker-script behavior

The main CMake file expects `PS4_LINK_SCRIPT` to exist and be writable. It inserts:

```ld
. = 0x60000000;
```

inside the script's `SECTIONS {` block unless that exact assignment is already present. The package script then converts the built `wineps4.bin` to `ET_SCE_EXEC`. This means a guessed generic ELF linker script is not a safe substitute: the expected PS4 executable format, segment layout, entry point, and packaging conventions must be established first.

## 4. Other target dependencies (not automatically supplied by CMake glue)

The app directly links PS4 system libraries including `SceVideoOut`, `ScePad`, `SceUserService`, `SceMouse`, `SceKeyboard`, `SceIme`, `SceImeDialog`, `SceCommonDialog`, `SceNetCtl`, and `SceAudioOut`.

It also expects prepared external builds under the user's home directory:
- `~/wine-ps4-build/src/wine`
- `~/wine-ps4-build/build/wine-wow64/include`
- `~/wine-ps4-build/install/wine-wow64` (packaging)
- `~/wineps4-build/freetype`
- `~/wineps4-build/ffmpeg-img`
- `~/wineps4-build/dxvk-3.1.1` (packaging)

A ps4-kit compatibility module alone therefore cannot make the full project build without the PS4 sysroot, compatible system libraries, Mesa/RADV stack, and these separately prepared dependencies.

## 5. Safe implementation sequence

- [x] Inventory public call sites and environment variables.
- [ ] Obtain a reproducible, legally redistributable PS4 toolchain/sysroot and record its provenance.
- [ ] Determine the required compiler target/triple, compiler drivers, flags, and CMake toolchain variables from a working independent environment.
- [ ] Determine the PS4 linker script and executable/eboot conversion requirements from public, licensed sources.
- [ ] Implement a minimal CMake compatibility module that validates explicit prerequisites and fails clearly when they are missing.
- [ ] Add the executable helper only after its exact link and output contract is known.
- [ ] Treat RADV/Mesa and the window-system integration as a separate, substantial milestone.
- [ ] Validate each stage with a tiny independent test program before attempting WinePS4.

## 6. What remains unknown

The public WinePS4 call sites do **not** reveal:
- compiler executable names, target triple, default compile/link flags, or CMake toolchain file;
- contents and semantics of `ps4_cmake`;
- the implementation of `ps4_kit_app` (linker options, output target, post-link steps);
- the exact Mesa libraries, generated sources, include paths, and PS4 window-system ABI;
- the linker script contents and the PKG builder's internal requirements.

Do not fill these gaps with guessed flags or copied proprietary SDK material. The next useful evidence is a build log plus the generated CMake cache/build.ninja from a legitimate working setup, with secrets and private paths redacted, or public documentation/source for an independently redistributable toolchain.
