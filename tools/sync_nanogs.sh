#!/usr/bin/env bash
# Copy this repo's SOG library, HLSL decode and libwebp into the NanoGS plugin, or check the copies are identical.
# usage: tools/sync_nanogs.sh [--check] [<NanoGS plugin dir>]
# The plugin files are Perforce-controlled: open them for edit (p4 edit) before syncing.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CHECK=0
if [ "${1:-}" = "--check" ]; then
    CHECK=1
    shift
fi
PLUGIN="${1:-/Volumes/External SSD/Unreal Projects/SHUTourDemo/Plugins/NanoGS}"
SRC="$PLUGIN/Source"

# repo file -> plugin location (Source/ unless the destination starts with Shaders/)
PAIRS=(
    "shaders/SOGDecode.ush|Shaders/Private/SOGDecode.ush"
    "sog/SOGTypes.h|NanoGS/Public/SOG/SOGTypes.h"
    "sog/SOGCodec.h|NanoGS/Public/SOG/SOGCodec.h"
    "sog/SOGCodec.cpp|NanoGS/Private/SOG/SOGCodec.cpp"
    "sog/SOGJson.h|NanoGSEditor/Private/SOG/SOGJson.h"
    "sog/SOGJson.cpp|NanoGSEditor/Private/SOG/SOGJson.cpp"
    "sog/SOGZip.h|NanoGSEditor/Private/SOG/SOGZip.h"
    "sog/SOGZip.cpp|NanoGSEditor/Private/SOG/SOGZip.cpp"
    "sog/SOGLoader.h|NanoGSEditor/Private/SOG/SOGLoader.h"
    "sog/SOGLoader.cpp|NanoGSEditor/Private/SOG/SOGLoader.cpp"
)
WEBP_DST="$SRC/NanoGSEditor/Private/ThirdParty/libwebp"

status=0
for pair in "${PAIRS[@]}"; do
    from="$ROOT/${pair%%|*}"
    dst="${pair##*|}"
    case "$dst" in
        Shaders/*) to="$PLUGIN/$dst" ;;
        *) to="$SRC/$dst" ;;
    esac
    if [ "$CHECK" = 1 ]; then
        if ! cmp -s "$from" "$to"; then
            echo "differs: $dst"
            status=1
        fi
    else
        cp "$from" "$to"
    fi
done
if [ "$CHECK" = 1 ]; then
    if ! diff -rq -x README.NanoGS.md "$ROOT/third_party/libwebp" "$WEBP_DST" > /dev/null; then
        echo "differs: libwebp"
        status=1
    fi
    [ "$status" = 0 ] && echo "NanoGS copies match this repo"
    exit "$status"
fi
rsync -a --delete --exclude README.NanoGS.md "$ROOT/third_party/libwebp/" "$WEBP_DST/"
echo "synced into $SRC"
