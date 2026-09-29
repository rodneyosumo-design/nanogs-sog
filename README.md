# nanogs-sog

SOG support for NanoGS, the Nanite-style Gaussian splat renderer in the SHUTourDemo Unreal 5.8 project,
with iOS (Metal) as the constraining target.

SOG is PlayCanvas's **Spatially Ordered Gaussians** format: a zip (or folder) of lossless WebP images plus
`meta.json`, roughly 15x smaller than a 3DGS PLY. The plan is to import SOG in the Unreal editor and keep
its quantized layout resident on the GPU (20 bytes per splat plus a small SH palette) instead of NanoGS's
current storage, most of which is Float16 spherical harmonics.

| Phase | Scope | Status |
|---|---|---|
| 0 | Ground truth: encode the real scene, validate the format, frame, fidelity and GPU layout | In progress |
| 1 | Editor importer: `.sog` / `meta.json` / `lod-meta.json` → cooked 20-byte records + tables | Not started |
| 2 | GPU decode path in NanoGS's compute pass, 16-bit sort keys on mobile | Not started |
| 3 | Mobile rasterization tuning (quad extents, reduced-resolution splat target) | Not started |
| 4 | Streamed SOG LOD, residency budget, API/device-profile knobs | Not started |

Full plan: [docs/plan.md](docs/plan.md).

## Layout

| Path | Contents |
|---|---|
| `docs/plan.md` | Engineering plan: format analysis, fork-vs-build comparison, phased execution plan, blueprint |

The Unreal integration itself (importer, shader permutation) will land in the NanoGS plugin in the
SHUTourDemo Perforce depot. This repo holds the format library, reference tools, tests and docs.
