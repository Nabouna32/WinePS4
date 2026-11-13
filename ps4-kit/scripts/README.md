# ps4-kit packaging scripts

WinePS4's unchanged `app/package.sh` calls:

```bash
$PS4_KIT/scripts/make-pkg.sh <stage> <title-id> <title> <version> <content-id> <output-dir>
```

The implementation is not present yet. Do not substitute a script that only
prints a fake output path: the caller expects a real installable PS4 PKG.

Reconstruction must establish the exact OpenOrbis/LibOrbisPkg tool versions,
SFO fields, content ID handling, package layout, and return-value convention
from public evidence before implementing this entry point.
