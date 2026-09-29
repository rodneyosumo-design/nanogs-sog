# nanogs-sog

SOG support for NanoGS, the Nanite-style Gaussian splat renderer in the SHUTourDemo Unreal 5.8 project,
with iOS (Metal) as the constraining target.

SOG is PlayCanvas's **Spatially Ordered Gaussians** format: a zip (or folder) of lossless WebP images plus
`meta.json`, roughly 15x smaller than a 3DGS PLY. The plan is to import SOG in the Unreal editor and keep
its quantized layout resident on the GPU (20 bytes per splat plus a small SH palette) instead of NanoGS's
current ~112 bytes per splat, most of which is Float16 spherical harmonics.

| Phase | Scope | Status |
|---|---|---|
| 0 | Ground truth: encode the real scene, validate the format, frame, fidelity and GPU layout | **Done** — [results](docs/phase0-results.md) |
| 1 | Editor importer: `.sog` / SOG `meta.json` → cooked 20-byte records + tables (decision A) | **Done** — [results](docs/phase1-results.md); NanoGS side in pending Perforce changelist 363 |
| 2 | GPU decode path in NanoGS's compute pass, 16-bit sort keys on mobile | **Implemented** — [results](docs/phase2-results.md); same changelist 363; iPhone 13 Pro capture still to do |
| 3 | Mobile rasterization tuning (quad extents, reduced-resolution splat target) | Not started |
| 4 | Streamed SOG (`lod-meta.json`) import and LOD, residency budget, API/device-profile knobs | Not started |

Full plan: [docs/plan.md](docs/plan.md).

## Layout

| Path | Contents |
|---|---|
| `docs/` | Engineering plan, phase write-ups, comparison images |
| `sog/` | Engine-independent C++17 SOG library: types, CPU decode/encode, loader (zip, JSON, libwebp) |
| `shaders/SOGDecode.metal` | GPU decode for Metal: position, quaternion, scale, opacity, SH0–SH3 colour, local-space covariance |
| `shaders/SOGDecode.ush` | The same decode in HLSL, as used by NanoGS's `CalcViewData` (synced into the plugin) |
| `third_party/libwebp` | libwebp v1.6.0 decoder only (BSD-3) |
| `tests/` | Library tests: `tests/run_tests.sh` (codec, 14 fixtures, full scene) |
| `integration/` | Review snapshots of the NanoGS (Perforce) changes, one patch per phase |
| `tools/sync_nanogs.sh` | Copies `sog/`, `shaders/SOGDecode.ush` and libwebp into the NanoGS plugin, or `--check`s the copies |
| `tools/phase0/`, `tools/phase1/` | Reference SOG v2 decoder (Python), analysis, render comparison, blueprint tests, fixture generator |
| `tools/phase2/` | Editor A/B renders and GPU timing over the editor's MCP server and Python remote execution |
| `results/` | Phase 0 measurements (JSON), golden test vectors, the real asset's `meta.json` |
| `data/` | Large generated files (SOG, PLYs, renders, test binaries). Git-ignored; recreate with the script below |

The Unreal integration itself (importer, GPU decode permutation, sorting changes) lives in the NanoGS plugin
in the SHUTourDemo Perforce depot. This repo holds the format library, shaders, reference tools, tests and docs.

## Reproduce Phase 0

Needs Node 20+ (tested with 26.10), Python 3.9+, and Xcode's Metal toolchain.

```bash
tools/phase0/run_phase0.sh "/Volumes/External SSD/Unreal Projects/SHUTourDemo/SourceData/scene_building_nosky.ply"
```

It installs the pinned `@playcanvas/splat-transform` (3.7.0) and a Python venv, encodes the PLY to SOG,
round-trips it, runs the analysis and render comparison, and runs the Metal (GPU) and C++ blueprint tests.

## Library tests (Phase 1)

```bash
tests/run_tests.sh
```

Builds `sog/` with the vendored libwebp (clang, `-Wall -Wextra -Wshadow -Werror`) and runs the codec, fixture
and full-scene suites. The full-scene suite needs Phase 0's data. The Unreal-side tests
(`NanoGS.SOG.*`) are described in [docs/phase1-results.md](docs/phase1-results.md).
