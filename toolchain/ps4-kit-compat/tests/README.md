Run the host-side regression tests with:

```sh
python3 -m unittest discover -s toolchain/ps4-kit-compat/tests -v
```

The suite includes:

- generator tests against a synthetic linker-script fixture (priority arrays, suffixed data sections, and fail-closed markers);
- a configure-only CMake smoke test with a temporary fake OpenOrbis tree, checking that the compatibility module generates the patched linker script, redirects the executable link command, and declares the eboot target.

These tests do not prove compatibility with every OpenOrbis release. The real installed `link.x`, actual `create-fself` invocation, target sysroot, final OELF and PS4 hardware behavior still require separate validation.


The same test suite also checks the candidate Mesa archive manifest in
`ps4-kit/mesa/archives.cmake`: it resolves the complete 16-archive closure
from a synthetic build tree and fails with a useful diagnostic when an archive
is missing.
