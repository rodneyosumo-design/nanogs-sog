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
| 1 | Editor importer: `.sog` / `meta.json` / `lod-meta.json` → cooked 20-byte records + tables | Not started |
| 2 | GPU decode path in NanoGS's compute pass, 16-bit sort keys on mobile | Not started |
| 3 | Mobile rasterization tuning (quad extents, reduced-resolution splat target) | Not started |
| 4 | Streamed SOG LOD, residency budget, API/device-profile knobs | Not started |

Full plan: [docs/plan.md](docs/plan.md).

## Layout

| Path | Contents |
|---|---|
| `docs/` | Engineering plan, phase write-ups, comparison images |
| `sog/SOGTypes.h` | CPU layout of the GPU-resident format (20-byte record, 112-byte constants, `PackSplat`) |
| `shaders/SOGDecode.metal` | GPU decode: position, quaternion, scale, opacity, SH0–SH3 colour, local-space covariance |
| `tools/phase0/` | Reference SOG v2 decoder (Python), analysis, render comparison, blueprint tests |
| `results/` | Phase 0 measurements (JSON), golden test vectors, the real asset's `meta.json` |
| `data/` | Large generated files (SOG, PLYs, renders, test binaries). Git-ignored; recreate with the script below |

The Unreal integration itself (importer, shader permutation) will land in the NanoGS plugin in the
SHUTourDemo Perforce depot. This repo holds the format library, reference tools, tests and docs.

## Reproduce Phase 0

Needs Node 20+ (tested with 26.10), Python 3.9+, and Xcode's Metal toolchain.

```bash
tools/phase0/run_phase0.sh "/Volumes/External SSD/Unreal Projects/SHUTourDemo/SourceData/scene_building_nosky.ply"
```

It installs the pinned `@playcanvas/splat-transform` (3.7.0) and a Python venv, encodes the PLY to SOG,
round-trips it, runs the analysis and render comparison, and runs the Metal (GPU) and C++ blueprint tests.
