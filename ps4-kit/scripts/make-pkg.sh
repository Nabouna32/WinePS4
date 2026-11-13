#!/usr/bin/env bash
# Reconstructed OpenOrbis fake-PKG builder for WinePS4.
#
# Interface preserved from app/package.sh:
#   make-pkg.sh <stage-dir> <TITLE_ID> <title> <version XX.YY> <content-label> <out-dir>
#
# Uses only the user's local OpenOrbis installation. No SDK binaries or PRXs
# are stored in this repository.

set -euo pipefail

if [[ $# -ne 6 ]]; then
    echo "usage: $0 <stage-dir> <TITLE_ID> <title> <version XX.YY> <content-label> <out-dir>" >&2
    exit 2
fi

STAGE_INPUT=$1
TITLE_ID=$2
TITLE=$3
VERSION=$4
LABEL_INPUT=$5
OUT_INPUT=$6

[[ -d "$STAGE_INPUT" ]] || { echo "make-pkg: stage directory not found: $STAGE_INPUT" >&2; exit 1; }
STAGE="$(cd "$STAGE_INPUT" && pwd -P)"
mkdir -p "$OUT_INPUT"
OUT="$(cd "$OUT_INPUT" && pwd -P)"

[[ "$TITLE_ID" =~ ^[A-Z]{4}[0-9]{5}$ ]] || {
    echo "make-pkg: TITLE_ID must look like ABCD12345 (got '$TITLE_ID')" >&2
    exit 1
}
[[ "$VERSION" =~ ^[0-9]{2}\.[0-9]{2}$ ]] || {
    echo "make-pkg: version must look like 01.00 (got '$VERSION')" >&2
    exit 1
}
[[ -n "$TITLE" ]] || { echo "make-pkg: title must not be empty" >&2; exit 1; }

[[ -f "$STAGE/eboot.bin" ]] || { echo "make-pkg: missing $STAGE/eboot.bin" >&2; exit 1; }
[[ -f "$STAGE/sce_sys/icon0.png" ]] || { echo "make-pkg: missing $STAGE/sce_sys/icon0.png" >&2; exit 1; }

OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"
TOOLS="$OPENORBIS/bin/linux"
PKGTOOL="$TOOLS/PkgTool.Core"
CREATE_GP4="$TOOLS/create-gp4"
for tool in "$PKGTOOL" "$CREATE_GP4"; do
    [[ -f "$tool" ]] || { echo "make-pkg: required OpenOrbis packaging tool not found: $tool" >&2; exit 1; }
done
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

# The content label is ASCII alphanumeric, uppercase and exactly 16 characters.
LABEL="$(printf '%s' "$LABEL_INPUT" | tr '[:lower:]' '[:upper:]' | tr -cd 'A-Z0-9')"
[[ -n "$LABEL" ]] || { echo "make-pkg: content label must contain letters or digits" >&2; exit 1; }
LABEL="$(printf '%-16s' "${LABEL:0:16}" | tr ' ' '0')"
CONTENT_ID="IV0000-${TITLE_ID}_00-${LABEL}"

# WinePS4 passes NO_RIGHT_SPRX=1 because it does not use the sample's shell
# information panel. If enabled, use only the copy shipped with the local SDK.
if [[ "${NO_RIGHT_SPRX:-0}" != 1 ]]; then
    RIGHT_SPRX="$OPENORBIS/samples/piglet/sce_sys/about/right.sprx"
    if [[ ! -f "$RIGHT_SPRX" ]]; then
        echo "make-pkg: $RIGHT_SPRX is missing (or set NO_RIGHT_SPRX=1)" >&2
        exit 1
    fi
    mkdir -p "$STAGE/sce_sys/about"
    cp "$RIGHT_SPRX" "$STAGE/sce_sys/about/right.sprx"
fi

# Preserve the historical kit behavior of adding OpenOrbis' sample Piglet
# modules when they are present. They remain local SDK inputs, never repo files.
if [[ "${NO_PIGLET_SPRX:-0}" != 1 ]]; then
    MODULE_DIR="$OPENORBIS/samples/piglet/sce_module"
    if [[ -d "$MODULE_DIR" ]]; then
        shopt -s nullglob
        MODULES=("$MODULE_DIR"/*.prx)
        shopt -u nullglob
        if (( ${#MODULES[@]} )); then
            mkdir -p "$STAGE/sce_module"
            cp "${MODULES[@]}" "$STAGE/sce_module/"
        fi
    fi
fi

# WinePS4 sets SFO_ATTRIBUTE=0x800002 in app/package.sh. The plain OpenOrbis
# SFO profile is therefore the default; an explicit retroarch profile is kept
# only for compatibility with other public consumers of the shared recipe.
SFO="$STAGE/sce_sys/param.sfo"
rm -f "$SFO"
"$PKGTOOL" sfo_new "$SFO"
set_entry() {
    "$PKGTOOL" sfo_setentry "$SFO" "$1" --type "$2" --maxsize "$3" --value "$4" >/dev/null
}

SFO_STYLE="${SFO_STYLE:-plain}"
if [[ "$SFO_STYLE" == retroarch ]]; then
    set_entry APP_TYPE Integer 4 0
    set_entry ATTRIBUTE Integer 4 0x20814016
    set_entry ATTRIBUTE2 Integer 4 0x6
    set_entry CATEGORY Utf8 4 gde
    set_entry FORMAT Utf8 4 obs
    set_entry SYSTEM_VER Integer 4 0x3fc
elif [[ "$SFO_STYLE" == plain ]]; then
    set_entry APP_TYPE Integer 4 1
    set_entry ATTRIBUTE Integer 4 "${SFO_ATTRIBUTE:-0}"
    set_entry CATEGORY Utf8 4 gd
    set_entry SYSTEM_VER Integer 4 0
else
    echo "make-pkg: unsupported SFO_STYLE '$SFO_STYLE' (expected plain or retroarch)" >&2
    exit 1
fi
set_entry APP_VER Utf8 8 "$VERSION"
set_entry CONTENT_ID Utf8 48 "$CONTENT_ID"
set_entry DOWNLOAD_DATA_SIZE Integer 4 0
set_entry TITLE Utf8 128 "$TITLE"
set_entry TITLE_ID Utf8 12 "$TITLE_ID"
set_entry VERSION Utf8 8 "$VERSION"

# PkgTool.Core has failed on empty directory entries in some OpenOrbis builds.
find "$STAGE" -depth -type d -empty -delete
rm -f "$STAGE/pkg.gp4"
(
    cd "$STAGE"
    "$CREATE_GP4" -out pkg.gp4 --content-id="$CONTENT_ID" --path .
)
"$PKGTOOL" pkg_build "$STAGE/pkg.gp4" "$OUT"
rm -f "$STAGE/pkg.gp4"

PKG="$OUT/$CONTENT_ID.pkg"
[[ -f "$PKG" ]] || {
    echo "make-pkg: PkgTool.Core returned without creating expected package: $PKG" >&2
    exit 1
}
echo "$PKG"
