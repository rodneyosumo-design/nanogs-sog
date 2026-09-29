#!/usr/bin/env bash
# Build Phase 4's Streamed SOG test data from a splat PLY: splat-transform decimates it to 50%, 25% and 10%
# (moment-matching merges) and bundles the four levels into data/streamed/scene/lod-meta.json with per-leaf
# errors. About ten minutes for scene_building_nosky.ply (2.58M splats) on an M4.
# usage: tools/phase4/make_streamed.sh [<source.ply>]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="${1:-/Volumes/External SSD/Unreal Projects/SHUTourDemo/SourceData/scene_building_nosky.ply}"
ST="$ROOT/tools/phase0/node_modules/.bin/splat-transform"
OUT="$ROOT/data/streamed"
mkdir -p "$OUT"

# Decimation must be the last action and write a .ply
for pct in 50 25 10; do
    "$ST" --no-tty -w "$SRC" -d "$pct%" "$OUT/lod_$pct.ply"
done

# Levels: 0 = source, 1 = 50%, 2 = 25%, 3 = 10%. 64K-splat chunks, 2 m leaves, measured errors.
rm -rf "$OUT/scene"
"$ST" --no-tty -w "$SRC" -l 0 "$OUT/lod_50.ply" -l 1 "$OUT/lod_25.ply" -l 2 "$OUT/lod_10.ply" -l 3 \
    "$OUT/scene/lod-meta.json" --lod-chunk-count 64 --lod-chunk-extent 2 --lod-errors
echo "wrote $OUT/scene/lod-meta.json"
