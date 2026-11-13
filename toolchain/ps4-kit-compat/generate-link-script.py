#!/usr/bin/env python3
"""Generate a validated OpenOrbis PS4 linker script without editing the SDK installation.

The output keeps priority constructors/destructors and suffixed data/TLS sections
inside the PT_LOAD image consumed by create-fself. This is an independent
implementation based on publicly documented linker failures.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys


def replace_once(source: str, old: str, new: str, label: str) -> str:
    count = source.count(old)
    if count != 1:
        raise ValueError(f"expected exactly one {label} marker {old!r}; found {count}")
    return source.replace(old, new, 1)


def generate(source: str) -> str:
    text = source

    # Keep constructor and destructor arrays in priority order. The OpenOrbis
    # script's closing brace for .init_array is moved to close .fini_array.
    old = "*(.init_array);"
    new = (
        "KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.* .ctors.*)));\n"
        "\t\tKEEP(*(.init_array .ctors));\n"
        "\t}\n"
        "\n"
        "\t.fini_array : {\n"
        "\t\tKEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.* .dtors.*)));\n"
        "\t\tKEEP(*(.fini_array .dtors));"
    )
    text = replace_once(text, old, new, "init-array")

    for section in (".data", ".tdata", ".tbss"):
        text = replace_once(
            text,
            f"*({section})",
            f"*({section} {section}.*)",
            f"{section} input-section",
        )

    # Sanity checks: refuse to output a partially patched or unexpected script.
    for marker in ("SORT_BY_INIT_PRIORITY(.init_array.*", "SORT_BY_INIT_PRIORITY(.fini_array.*"):
        if text.count(marker) != 1:
            raise ValueError(f"generated script failed validation for {marker}")
    for section in (".data", ".tdata", ".tbss"):
        if text.count(f"*({section} {section}.*)") != 1:
            raise ValueError(f"generated script failed validation for {section}")
    return text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="OpenOrbis link.x")
    parser.add_argument("output", type=Path, help="generated linker script")
    args = parser.parse_args()

    if not args.input.is_file():
        parser.error(f"input linker script does not exist: {args.input}")
    try:
        generated = generate(args.input.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"ps4-linker: {exc}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generated, encoding="utf-8")
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
