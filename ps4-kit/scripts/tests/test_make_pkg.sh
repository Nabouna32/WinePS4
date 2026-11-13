#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

OO="$TMP/openorbis"
STAGE="$TMP/stage"
OUT="$TMP/out"
mkdir -p "$OO/bin/linux" "$STAGE/sce_sys" "$OUT"
: > "$STAGE/eboot.bin"
: > "$STAGE/sce_sys/icon0.png"

cat > "$OO/bin/linux/PkgTool.Core" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "$PKGTOOL_LOG"
case "$1" in
    sfo_new) : > "$2" ;;
    sfo_setentry) : ;;
    pkg_build)
        content_id="$(sed -n 's/^CONTENT_ID=//p' "$2")"
        : > "$3/$content_id.pkg"
        ;;
    *) echo "unexpected PkgTool.Core command: $*" >&2; exit 9 ;;
esac
STUB
cat > "$OO/bin/linux/create-gp4" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
content_id=""
for arg in "$@"; do
    case "$arg" in --content-id=*) content_id="${arg#*=}" ;; esac
done
printf 'CONTENT_ID=%s\n' "$content_id" > pkg.gp4
STUB
chmod +x "$OO/bin/linux/PkgTool.Core" "$OO/bin/linux/create-gp4"

export OPENORBIS="$OO"
export PKGTOOL_LOG="$TMP/pkgtool.log"
NO_RIGHT_SPRX=1 SFO_ATTRIBUTE=0x800002 \
    bash "$HERE/make-pkg.sh" "$STAGE" WINE00002 WinePS4 00.71 WINEPS4 "$OUT" > "$TMP/result"

EXPECTED="$OUT/IV0000-WINE00002_00-WINEPS4000000000.pkg"
[[ "$(cat "$TMP/result")" == "$EXPECTED" ]]
[[ -f "$EXPECTED" ]]
grep -Fq 'sfo_setentry' "$PKGTOOL_LOG"
grep -Fq 'ATTRIBUTE --type Integer --maxsize 4 --value 0x800002' "$PKGTOOL_LOG"
grep -Fq 'CONTENT_ID --type Utf8 --maxsize 48 --value IV0000-WINE00002_00-WINEPS4000000000' "$PKGTOOL_LOG"
[[ ! -e "$STAGE/pkg.gp4" ]]
echo "make-pkg smoke test passed"
