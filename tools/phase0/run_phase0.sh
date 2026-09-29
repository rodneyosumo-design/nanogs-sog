#!/usr/bin/env bash
# Reproduces Phase 0: encode a 3DGS PLY to SOG, round-trip it, analyse it, compare renders, and run
# the blueprint tests (Metal on the GPU, C++ packing).
# usage: tools/phase0/run_phase0.sh <source.ply>        (FORCE=1 re-encodes an existing SOG)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PLY="${1:?usage: run_phase0.sh <source.ply>}"
DATA="$ROOT/data"
RESULTS="$ROOT/results"
TEST="$DATA/blueprint_test"
SOG="$DATA/scene_nosky.sog"
mkdir -p "$DATA" "$RESULTS" "$TEST"

cd "$HERE"
echo "== tools"
npm ci --silent
[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/pip install -q -r requirements.txt

echo "== encode and round-trip"
if [ ! -f "$SOG" ] || [ "${FORCE:-0}" = "1" ]; then
    npx splat-transform -w "$PLY" "$SOG"
fi
npx splat-transform -w -q "$SOG" "$DATA/scene_nosky_roundtrip.ply"
rm -rf "$DATA/sog_unzipped" && mkdir -p "$DATA/sog_unzipped" && unzip -o -q "$SOG" -d "$DATA/sog_unzipped"
cp "$DATA/sog_unzipped/meta.json" "$RESULTS/scene_nosky_meta.json"

echo "== analysis"
.venv/bin/python analyze.py "$PLY" "$SOG" "$DATA/scene_nosky_roundtrip.ply" "$RESULTS"
.venv/bin/python frame_check.py "$SOG" "$RESULTS/frame_check.json"
.venv/bin/python render_compare.py "$PLY" "$SOG" "$DATA/renders"
cp "$DATA/renders/render_compare.json" "$RESULTS/"
mkdir -p "$ROOT/docs/img/phase0" && cp "$DATA"/renders/*_compare.jpg "$ROOT/docs/img/phase0/"

echo "== blueprint tests"
cd "$HERE/blueprint_tests"
../.venv/bin/python export_test_data.py "$SOG" "$TEST"
xcrun -sdk macosx metal -Werror -I "$ROOT/blueprint" -c sog_golden_test.metal -o "$TEST/sog_golden_test.air"
xcrun -sdk macosx metallib "$TEST/sog_golden_test.air" -o "$TEST/sog_golden_test.metallib"
swift run_metal_test.swift "$TEST" "$TEST/sog_golden_test.metallib" > "$RESULTS/metal_test_result.json"
clang++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$ROOT/blueprint" test_pack.cpp -o "$TEST/test_pack"
"$TEST/test_pack" "$TEST" > "$RESULTS/pack_test_result.json"

echo "== done: results in $RESULTS"
