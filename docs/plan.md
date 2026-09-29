# Engineering plan: SOG for NanoGS on iOS

**Recommendation:** extend the NanoGS fork. Treat `.sog` as the import format: decode it once in the
editor, then keep SOG's quantized layout on the GPU at 20 bytes per splat plus an SH palette. Forking
XGRIDS' LCC plugin isn't viable, and a from-scratch Metal renderer only makes sense outside Unreal.

Corrections to the original brief:

- **SOG is PlayCanvas's "Spatially Ordered Gaussians"**, not "Self-Organized Gaussians". It borrows ideas
  from the Self-Organizing Gaussians paper but is its own, fully specified container.
- **LCC is XGRIDS' format** (Lixel CyberColor), not Luma AI's.
- **NanoGS isn't a Metal-native iOS renderer.** It's an Unreal plugin that reaches Metal through Unreal's
  RHI, which is why it already runs on the iPhone 13 Pro. The real "lightweight Metal-native" option is
  MetalSplatter (MIT, Swift/Metal): it loads PLY, SPZ and `.splat` but not SOG, and documents no LOD.

## 1. Format

A `.sog` is a zip, or an unzipped folder, of `meta.json` plus lossless WebP images. Each image holds one
pixel per splat, row-major from the top-left: splat *i* is at `(i % W, i / W)`. Splats are stored in Morton
order. Version 2:

| File | Channels | Encoding | Decode |
|---|---|---|---|
| `means_l.webp`, `means_u.webp` | RGB | 16 bits per axis (low/high bytes), symmetric log domain | `q = u<<8 \| l`; `n = lerp(mins, maxs, q/65535)`; `p = sign(n)·(exp(\|n\|) − 1)` |
| `quats.webp` | RGBA | Smallest-three: RGB = 3 components in 8 bits, A = 252 + index of the dropped one, order (w,x,y,z) | `c = (v/255 − 0.5)·√2`; dropped = `√(1 − a² − b² − c²)` |
| `scales.webp` | RGB | Index into a 256-entry log-scale codebook | `s = exp(codebook[idx])` |
| `sh0.webp` | RGBA | RGB = index into the DC codebook; A = opacity (already sigmoided) | `col = 0.5 + 0.28209479·codebook[idx]`; `α = A/255` |
| `shN_labels.webp` | RG | 16-bit index into the SH palette | `label = R \| G<<8` |
| `shN_centroids.webp` | RGB | Palette of ≤ 65,536 entries, 3/8/15 coefficients per entry for bands 1/2/3, 64 entries per row, each value an index into a 256-entry codebook | texel `(label%64)·coeffs + c`, row `label/64` |

Base SOG has no spatial chunking. **Streamed SOG** adds it: `lod-meta.json` holds a binary tree of bounds
whose leaves map LOD levels to runs of splats (`file, offset, count`) in chunk folders that are ordinary
SOG files, with optional per-level errors.

Gotchas:

- Decode WebP with libwebp (`WebPDecodeRGBA`), never ImageIO/CoreGraphics: CoreGraphics premultiplies
  8-bit alpha, which corrupts the quaternion mode byte and opacity. Reject lossy files.
- Validate `count ≤ W·H`, `quats.A` in 252–255, labels below `shN.count`.
- The spec declares y-up, but **files written by splat-transform keep the source PLY's axes** (Phase 0).
  Other tools may differ, so the importer needs a frame option.
- `antialias: true` needs the Mip-Splatting opacity compensation; NanoGS doesn't implement it yet.

## 2. Fork vs. build

| | Fork LCC (XGRIDS) | Extend the NanoGS fork | Build new (Metal/Swift) |
|---|---|---|---|
| Runs in the iOS app | No: the Unreal plugin supports Windows and Linux only; the web SDK is Three.js/Cesium | Yes, already ships on the 13 Pro | Not inside Unreal without sharing depth/colour textures around the RHI |
| SOG decoding work | Its plugin reads SOG, but it's a Free/Pro codebase with unclear source availability | One importer plus one shader decode path; culling, LOD, sort and draw are reused | Loader is easy; everything after it is new |
| Shader control | Whatever XGRIDS exposes | Full | Full, from zero |
| Maintenance | Vendor releases plus Pro licensing | Existing fork, plus an importer, a storage mode and a shader variant | A second renderer indefinitely |
| **Verdict** | Not viable | **Recommended** | Only for a standalone non-Unreal viewer (fork MetalSplatter) |

**Memory today (corrected in Phase 0).** NanoGS already packs the core attributes to 16 bytes per splat on
the GPU (sRGB8 colour, float16 positions, octahedral quaternion, 8-bit log scales) but keeps all three SH
bands as Float16: 96 bytes per splat, 86% of its splat memory, even though phones only evaluate band 1.
For `scene_nosky` (3.43M splats including NanoGS's LOD splats) that is ~384 MB on the GPU, plus a 494 MB
cooked bulk payload read at load. The asset's quality-level and "Cluster" SH enums exist but aren't wired
up (the importer hard-codes Float32/Float16/Float16 bulk formats). SOG storage would be ~77 MB on the GPU.

## 3. Execution plan

**Phase 0: ground truth — done.** See [phase0-results.md](phase0-results.md).

**Phase 1: ingestion and parser (NanoGSEditor).**
- Import factory for `.sog`, SOG folders and `lod-meta.json`; zips via Unreal's `FZipArchiveReader`.
- Vendor the libwebp decoder (BSD) into the editor module only; Unreal 5.8's image decoder has no WebP.
- Pack each splat into the 20-byte record (`sog/SOGTypes.h`), keeping Morton order; build the
  scale/DC tables and the half-float SH palette; store as bulk data under a new SOG storage mode, plus
  `fileToLocal` (default `kNanoGSFileToLocal`, with a y-up option).
- NanoGS LOD splats: requantize positions, snap scale/DC to the nearest codebook entry, inherit the SH label
  from the highest-weight child (a brute-force palette search would take trillions of operations).
- Decision to make first (see Phase 0 §6): full 20-byte record vs. NanoGS's 16-byte core + SOG SH label.
- Done when the SOG import matches the PLY import and matches `results/golden_scene_nosky.json`.

**Phase 2: GPU decode and sorting.**
- SOG shader variant in `CalcViewData` using `shaders/SOGDecode.metal` (ported to .usf); it writes the
  same per-splat view data, so nothing downstream changes.
- Keep NanoGS's reduce-then-scan radix sort (safe on Apple GPUs, where Metal gives no cross-threadgroup
  forward-progress guarantee for Onesweep-style look-back). Add 16-bit keys on mobile: 2 passes instead of
  4 (`GaussianSplatRenderer.cpp:306`).
- Shrink the 64-byte per-splat view data to 32 bytes on mobile.
- Done when a 13 Pro capture shows the splat pass under the 18.3 ms baseline (2026-09-03 profile).

**Phase 3: rasterization for tile-based GPUs.**
- Opacity-aware quad radius (σ·√(2·ln(255α))) instead of a fixed extent; cull < 1/255 and sub-pixel
  splats before sorting.
- Reduced-resolution splat target (`gs.ScreenPercentage` ≈ 0.7 on phones), upscaled in the composite.
- Premultiplied blending, read-only depth test against scene depth, no depth writes.
- Antialias compensation for `antialias: true` assets.
- Done when the Quad view with the crowd holds 30 fps on the 13 Pro.

**Phase 4: API, streaming, memory.**
- Keep the component plus console-variable API; add `gs.SortKeyBits` and `gs.ScreenPercentage` to device
  profiles. The palette makes full SH3 nearly free in memory.
- Streamed SOG: map `lod-meta.json` leaf runs onto NanoGS clusters (128-splat groups), use `errors` as the
  LOD metric, cook chunks as separately streamed bulk data, LRU residency budget, coarsest level pinned.
- A native iOS viewer, if ever needed: fork MetalSplatter and reuse `sog/` and `shaders/`.

## 4. Blueprint

- `sog/SOGTypes.h` — 20-byte `PackedSplat`, 112-byte `AssetConstants`, `PackSplat`,
  `kNanoGSFileToLocal`, Streamed SOG structs, loader declarations.
- `shaders/SOGDecode.metal` — `sog_mean`, `sog_quat_wxyz`, `sog_scale`, `sog_opacity`,
  `sog_covariance_local`, `sog_antialias_compensation`, `sog_color` (SH0–SH3 with a runtime order cap).

Both are compiled and tested against the reference decoder over the full scene
(`tools/phase0/blueprint_tests`).

Sources: [SOG spec](https://developer.playcanvas.com/user-manual/gaussian-splatting/formats/sog/),
[Streamed SOG spec](https://developer.playcanvas.com/user-manual/gaussian-splatting/formats/streamed-sog/),
[splat-transform](https://github.com/playcanvas/splat-transform),
[XGRIDS repositories](https://github.com/orgs/xgrids/repositories),
[LCC Unreal plugin](https://github.com/xgrids/LCC-3DGS-Unreal-Plugin),
[MetalSplatter](https://github.com/scier/MetalSplatter).
