#!/usr/bin/env bash
# Builds the SOG library (with the vendored libwebp decoder) and runs its tests.
# usage: tests/run_tests.sh            (scene checks need Phase 0's data: tools/phase0/run_phase0.sh)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/data/build"
WEBP="$ROOT/third_party/libwebp"
CXXFLAGS=(-std=c++17 -O2 -Wall -Wextra -Wshadow -Wimplicit-fallthrough -Werror -I "$ROOT/sog" -I "$WEBP")
mkdir -p "$BUILD/webp" "$BUILD/sog"

# libwebp: compile only what changed
while IFS= read -r -d '' src; do
    obj="$BUILD/webp/$(echo "${src#"$WEBP"/src/}" | tr / _ | sed "s/\.c$//").o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        cc -std=c11 -O2 -I "$WEBP" -c "$src" -o "$obj"
    fi
done < <(find "$WEBP/src" -name '*.c' -print0)

objs=()
for src in "$ROOT"/sog/*.cpp; do
    obj="$BUILD/sog/$(basename "${src%.cpp}").o"
    c++ "${CXXFLAGS[@]}" -c "$src" -o "$obj"
    objs+=("$obj")
done

for t in test_codec test_fixtures test_scene; do
    c++ "${CXXFLAGS[@]}" "$ROOT/tests/$t.cpp" "${objs[@]}" "$BUILD"/webp/*.o -lz -o "$BUILD/$t"
done

echo "== codec";    "$BUILD/test_codec"
if [ ! -f "$ROOT/data/fixtures/fixtures.json" ]; then
    "$ROOT/tools/phase0/.venv/bin/python" "$ROOT/tools/phase1/make_fixtures.py" "$ROOT/data/fixtures"
fi
echo "== fixtures"; "$BUILD/test_fixtures" "$ROOT/data/fixtures"
echo "== scene";    "$BUILD/test_scene" "$ROOT/data"
