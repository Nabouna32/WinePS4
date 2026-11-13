# ps4-kit packaging scripts

WinePS4's unchanged `app/package.sh` calls:

```bash
$PS4_KIT/scripts/make-pkg.sh <stage> <title-id> <title> <version> <content-label> <output-dir>
```

## `make-pkg.sh`

The first clean-room implementation now exists at `make-pkg.sh`. It:

1. validates the six-argument interface and staged `eboot.bin` / `sce_sys/icon0.png`;
2. creates the SFO using the local OpenOrbis `PkgTool.Core`;
3. uses the plain homebrew SFO profile by default and honours WinePS4's existing `SFO_ATTRIBUTE=0x800002`;
4. creates the GP4 with `create-gp4` and builds the fake-PKG with `PkgTool.Core pkg_build`;
5. prints the resulting package path as its final output line, as expected by `app/package.sh`.

The implementation uses only tools from the user's local OpenOrbis installation. It does not store SDK binaries, keys, PRXs or other proprietary assets in this repository. `NO_RIGHT_SPRX=1` is the setting already passed by WinePS4; optional Piglet modules are copied only from the local OpenOrbis sample tree when present. The package script can be tested without OpenOrbis using the fake-tool smoke test:

```bash
bash "$PS4_KIT/scripts/tests/test_make_pkg.sh"
```

## Evidence and limits

The command shape and SFO/GP4 workflow are independently visible in public PS4 ports, including the public DolphinPS4 packaging helper and PS4SX2's `SFO_ATTRIBUTE` variant:

- https://github.com/iHaiDeeZ/DolphinPS4/blob/879b3e57f3d3180bcb5f2234db3903688b28cd7d/scripts/make-pkg.sh
- https://github.com/iHaiDeeZ/PS4SX2/blob/7afa5c43e7827193ac165eecd5b36d34d07761be/ps4/make-pkg.sh

These are implementation references, not proof that their exact SFO defaults match WinePS4's original private kit. The plain profile is selected because WinePS4 explicitly supplies `SFO_ATTRIBUTE`; the exact resulting SFO and PKG still need inspection with the user's OpenOrbis version. The fake-tool test checks the script contract only, not package validity or console acceptance.
