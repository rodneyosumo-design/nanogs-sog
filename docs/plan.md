# Engineering plan: SOG for NanoGS on iOS

*As proposed on 2026-09-28, before Phase 0.*

**Recommendation:** extend the NanoGS fork. Treat `.sog` as the import format: decode it once in the
editor, then keep SOG's quantized layout on the GPU at about 20 bytes per splat instead of today's ~144.
For the 3.43M-splat scene that frees roughly 420 MB on the iPhone 13 Pro. Forking LCC isn't viable, and a
from-scratch Metal renderer only makes sense outside Unreal.

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
- The spec says right-handed, y-up, −z forward; NanoGS's PLY reader assumes y-down. Don't guess the
  conversion; derive it from a round-trip of the project's own PLY (Phase 0).
- `antialias: true` needs the Mip-Splatting opacity compensation; NanoGS doesn't implement it yet.

## 2. Fork vs. build

| | Fork LCC (XGRIDS) | Extend the NanoGS fork | Build new (Metal/Swift) |
|---|---|---|---|
| Runs in the iOS app | No: the Unreal plugin supports Windows and Linux only; the web SDK is Three.js/Cesium | Yes, already ships on the 13 Pro | Not inside Unreal without sharing depth/colour textures around the RHI |
| SOG decoding work | Its plugin reads SOG, but it's a Free/Pro codebase with unclear source availability | One importer plus one shader decode path; culling, LOD, sort and draw are reused | Loader is easy; everything after it is new |
| Shader control | Whatever XGRIDS exposes | Full | Full, from zero |
| Maintenance | Vendor releases plus Pro licensing | Existing fork, plus an importer, a storage mode and a shader variant | A second renderer indefinitely |
| **Verdict** | Not viable | **Recommended** | Only for a standalone non-Unreal viewer (fork MetalSplatter) |

NanoGS already declares compressed tiers (`EGaussianQualityLevel`, 16/11/6-bit positions, a 64k-entry
clustered SH format), but the importer hard-codes Float32 positions, Float16 colour and Float16 SH
(`GaussianSplatAsset.cpp:148-150`), and the shaders only decode 16-bit positions. SOG's layout is
effectively the compressed tier NanoGS never finished. The 3.43M splats currently cost about 495 MB
(330 MB of it full SH3), even though phones only evaluate band 1.

## 3. Execution plan

**Phase 0: ground truth (no code).** Convert `SourceData/scene_building_nosky.ply` to `.sog` with
PlayCanvas's `splat-transform`, and convert it back to `.ply`. That gives SOG-authored content in Unreal
through the existing PLY importer, and a matched pair to derive the file-to-Unreal matrix and validate
Phase 1.

**Phase 1: ingestion and parser (NanoGSEditor).**
- Import factory for `.sog`, SOG folders and `lod-meta.json`; zips via Unreal's `FZipArchiveReader`.
- Vendor the libwebp decoder (BSD) into the editor module only; Unreal 5.8's image decoder has no WebP.
- Pack each splat into the 20-byte record (section 4), keeping Morton order; pre-apply the tables and
  decode the SH palette to half floats; store as bulk data under a new SOG storage mode, plus the
  `fileToLocal` matrix from Phase 0.
- NanoGS LOD splats: requantize positions, snap scale/DC to the nearest codebook entry, inherit the SH label
  from the highest-weight child.
- Done when the SOG import matches the PLY import (bounds within 1 cm, overlaid screenshots) and splat
  memory is about 20 bytes per splat.

**Phase 2: GPU decode and sorting.**
- SOG shader variant in `CalcViewData`; it writes the same per-splat view data, so nothing downstream changes.
- Keep NanoGS's reduce-then-scan radix sort (safe on Apple GPUs); add 16-bit keys on mobile: 2 passes
  instead of 4 (`GaussianSplatRenderer.cpp:306`).
- Shrink the 64-byte per-splat view data to 32 bytes on mobile.
- Done when a 13 Pro capture shows the splat pass under the 18.3 ms baseline.

**Phase 3: rasterization for tile-based GPUs.**
- Opacity-aware quad radius (σ·√(2·ln(255α))); cull < 1/255 and sub-pixel splats before sorting.
- Reduced-resolution splat target (`gs.ScreenPercentage` ≈ 0.7 on phones), upscaled in the composite.
- Premultiplied blending, read-only depth test against scene depth, no depth writes.
- Antialias compensation for `antialias: true` assets.
- Done when the Quad view with the crowd holds 30 fps on the 13 Pro.

**Phase 4: API, streaming, memory.**
- Keep the component plus console-variable API; add `gs.SortKeyBits` and `gs.ScreenPercentage`.
- Streamed SOG: map `lod-meta.json` leaf runs onto NanoGS clusters, use `errors` as the LOD metric, cook
  chunks as separately streamed bulk data, LRU residency budget, coarsest level pinned.
- A native iOS viewer, if ever needed: fork MetalSplatter and reuse the same packed format and decode.

## 4. Blueprint

```metal
struct SOGSplat {
    uint meanXY;        // qx | qy << 16            (16-bit, log-domain)
    uint meanZ_label;   // qz | shN label << 16
    uint quat;          // a | b << 8 | c << 16 | mode << 24   (mode = quats.A - 252)
    uint scaleOpacity;  // sx | sy << 8 | sz << 16 | opacity << 24
    uint dc;            // r | g << 8 | b << 16     (sh0 codebook indices)
};

struct SOGAssetConstants {
    float4x4      fileToLocal;               // SOG frame -> engine local (may reflect)
    packed_float3 meanMin;  uint count;      // log-domain
    packed_float3 meanMax;  uint shCoeffs;   // 0, 3, 8 or 15
    uint antialias; uint pad0, pad1, pad2;
};

inline float3 sog_mean(SOGSplat s, constant SOGAssetConstants& k) {
    float3 q = float3(uint3(s.meanXY & 0xFFFFu, s.meanXY >> 16, s.meanZ_label & 0xFFFFu)) * (1.0f / 65535.0f);
    float3 n = mix(float3(k.meanMin), float3(k.meanMax), q);
    return sign(n) * (exp(abs(n)) - 1.0f);
}

inline float4 sog_quat_wxyz(uint p) {
    float3 abc = (float3(uint3(p & 0xFFu, (p >> 8) & 0xFFu, (p >> 16) & 0xFFu)) * (1.0f / 255.0f) - 0.5f) * M_SQRT2_F;
    float d = sqrt(max(0.0f, 1.0f - dot(abc, abc)));
    switch (p >> 24) {
        case 0:  return float4(d, abc);
        case 1:  return float4(abc.x, d, abc.yz);
        case 2:  return float4(abc.xy, d, abc.z);
        default: return float4(abc, d);
    }
}
```

```cpp
namespace sog {
struct PackedSplat { uint32_t meanXY, meanZ_label, quat, scaleOpacity, dc; };
static_assert(sizeof(PackedSplat) == 20, "must match SOGSplat");

struct AssetConstants {
    float    fileToLocal[16];
    float    meanMin[3]; uint32_t count;
    float    meanMax[3]; uint32_t shCoeffs;
    uint32_t antialias;  uint32_t pad[3];
};
static_assert(sizeof(AssetConstants) == 112, "must match SOGAssetConstants");
}
```

```swift
// Only for a standalone (non-Unreal) viewer built on MetalSplatter.
public final class SOGSplatView: MTKView {
    public var splatBudget = 500_000          // mirrors gs.MaxRenderBudget
    public var maxSHBands = 1                 // mirrors gs.OverrideSHOrder
    public var lodErrorThreshold: Float = 0.08
    public func load(_ url: URL) async throws { /* .sog, SOG folder, or lod-meta.json */ }
    public func purgeCaches() { /* call from didReceiveMemoryWarning */ }
}
```

Sources: [SOG spec](https://developer.playcanvas.com/user-manual/gaussian-splatting/formats/sog/),
[Streamed SOG spec](https://developer.playcanvas.com/user-manual/gaussian-splatting/formats/streamed-sog/),
[splat-transform](https://github.com/playcanvas/splat-transform),
[XGRIDS repositories](https://github.com/orgs/xgrids/repositories),
[LCC Unreal plugin](https://github.com/xgrids/LCC-3DGS-Unreal-Plugin),
[MetalSplatter](https://github.com/scier/MetalSplatter).
