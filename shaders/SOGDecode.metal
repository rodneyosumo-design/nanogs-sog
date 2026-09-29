// SOGDecode.metal
// GPU decode of the 20-byte SOG splat record. CPU mirror: SOGTypes.h.
// Validated on an Apple M4 GPU against the Python reference decoder over all 2,575,004 splats of
// scene_nosky (tools/phase0, docs/phase0-results.md).
#ifndef SOG_DECODE_METAL
#define SOG_DECODE_METAL

#include <metal_stdlib>
using namespace metal;

struct SOGSplat {
    uint meanXY;        // qx | qy << 16                          16-bit log-domain position
    uint meanZ_label;   // qz | shN palette label << 16
    uint quat;          // a | b << 8 | c << 16 | mode << 24        smallest-three, mode = quats.A - 252
    uint scaleOpacity;  // sx | sy << 8 | sz << 16 | opacity << 24  scale codebook indices, opacity 0..255
    uint dc;            // r | g << 8 | b << 16 | flags << 24        sh0 codebook indices, flags (bit 0: no SH)
};
static_assert(sizeof(SOGSplat) == 20, "SOGSplat must be 20 bytes");

// Flags in the dc word's top byte (SOGTypes.h kFlagNoSH): NanoGS's merged LOD splats have no SH.
constant uint kSOGFlagNoSH = 1u;

struct SOGAssetConstants {
    float4x4      fileToLocal;               // column-major; SOG file frame -> engine local (may reflect)
    packed_float3 meanMin;  uint count;      // meta.means.mins (log-domain)
    packed_float3 meanMax;  uint shCoeffs;   // meta.means.maxs; 0, 3, 8 or 15
    uint antialias; uint pad0; uint pad1; uint pad2;
};
static_assert(sizeof(SOGAssetConstants) == 112, "SOGAssetConstants must be 112 bytes");

// Import-time tables (see SOGTypes.h):
//   scaleLUT[i] = exp(meta.scales.codebook[i])
//   dcLUT[i]    = 0.5 + 0.28209479 * meta.sh0.codebook[i]
//   palette     = half4[paletteCount * shCoeffs], rgb = meta.shN.codebook[centroid texel], a unused

inline float3 sog_mean(SOGSplat s, constant SOGAssetConstants& k)
{
    float3 q = float3(uint3(s.meanXY & 0xFFFFu, s.meanXY >> 16, s.meanZ_label & 0xFFFFu)) * (1.0f / 65535.0f);
    float3 n = mix(float3(k.meanMin), float3(k.meanMax), q);
    return sign(n) * (exp(abs(n)) - 1.0f);                  // undo the symmetric log
}

inline float4 sog_quat_wxyz(uint p)                          // smallest-three, components (w, x, y, z)
{
    float3 abc = (float3(uint3(p & 0xFFu, (p >> 8) & 0xFFu, (p >> 16) & 0xFFu)) * (1.0f / 255.0f) - 0.5f) * M_SQRT2_F;
    float d = sqrt(max(0.0f, 1.0f - dot(abc, abc)));
    switch (p >> 24) {                                       // index of the dropped component
        case 0:  return float4(d, abc);
        case 1:  return float4(abc.x, d, abc.yz);
        case 2:  return float4(abc.xy, d, abc.z);
        default: return float4(abc, d);
    }
}

inline float3 sog_scale(uint p, constant float* scaleLUT)
{
    return float3(scaleLUT[p & 0xFFu], scaleLUT[(p >> 8) & 0xFFu], scaleLUT[(p >> 16) & 0xFFu]);
}

inline float sog_opacity(uint p) { return float(p >> 24) * (1.0f / 255.0f); }

inline uint sog_label(SOGSplat s) { return s.meanZ_label >> 16; }

// Covariance in the engine's local frame: Σ_file = R diag(s²) Rᵀ, then M Σ_file Mᵀ with M the upper
// 3x3 of fileToLocal. Valid when M is a reflection (NanoGS: det = -1), so quaternions stay untouched.
inline float3x3 sog_covariance_local(float4 q, float3 s, constant SOGAssetConstants& k)
{
    const float w = q.x, x = q.y, y = q.z, z = q.w;
    float3x3 R = float3x3(                                   // columns
        float3(1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + w * z), 2.0f * (x * z - w * y)),
        float3(2.0f * (x * y - w * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + w * x)),
        float3(2.0f * (x * z + w * y), 2.0f * (y * z - w * x), 1.0f - 2.0f * (x * x + y * y)));
    float3x3 RS = float3x3(R[0] * s.x, R[1] * s.y, R[2] * s.z);
    float3x3 M = float3x3(k.fileToLocal[0].xyz, k.fileToLocal[1].xyz, k.fileToLocal[2].xyz);
    float3x3 MRS = M * RS;
    return MRS * transpose(MRS);
}

// meta.antialias == true: Mip-Splatting opacity compensation, applied with the usual 2D dilation.
// cov2d = (a, b, c) of [[a, b], [b, c]] before dilation.
inline float sog_antialias_compensation(float3 cov2d, float dilation)
{
    float det0 = cov2d.x * cov2d.z - cov2d.y * cov2d.y;
    float det1 = (cov2d.x + dilation) * (cov2d.z + dilation) - cov2d.y * cov2d.y;
    return sqrt(max(det0 / max(det1, 1e-12f), 0.0f));
}

// View-dependent colour, 3DGS convention: dir = normalize(mean - cameraPos), both in the SOG file
// frame (the frame the coefficients were trained in). Evaluate once per splat in compute.
// maxCoeffs caps the order at runtime: 0 = DC only, 3 = band 1, 8 = band 2, 15 = band 3.
// Records flagged kSOGFlagNoSH (NanoGS LOD splats) are DC only.
inline float3 sog_color(SOGSplat s, float3 dir, constant float* dcLUT,
                        device const half4* palette, uint shCoeffs, uint maxCoeffs)
{
    float3 c = float3(dcLUT[s.dc & 0xFFu], dcLUT[(s.dc >> 8) & 0xFFu], dcLUT[(s.dc >> 16) & 0xFFu]);
    const uint n = min(shCoeffs, maxCoeffs);
    if (n < 3 || ((s.dc >> 24) & kSOGFlagNoSH) != 0u) {
        return max(c, 0.0f);
    }
    device const half4* sh = palette + sog_label(s) * shCoeffs;
    const float x = dir.x, y = dir.y, z = dir.z;
    c += 0.4886025119f * (-y * float3(sh[0].rgb) + z * float3(sh[1].rgb) - x * float3(sh[2].rgb));
    if (n >= 8) {
        const float xx = x * x, yy = y * y, zz = z * z, xy = x * y, yz = y * z, xz = x * z;
        c += 1.0925484306f * xy * float3(sh[3].rgb)
           - 1.0925484306f * yz * float3(sh[4].rgb)
           + 0.3153915653f * (2.0f * zz - xx - yy) * float3(sh[5].rgb)
           - 1.0925484306f * xz * float3(sh[6].rgb)
           + 0.5462742153f * (xx - yy) * float3(sh[7].rgb);
        if (n >= 15) {
            c += -0.5900435899f * y * (3.0f * xx - yy) * float3(sh[8].rgb)
               + 2.8906114426f * xy * z * float3(sh[9].rgb)
               - 0.4570457995f * y * (4.0f * zz - xx - yy) * float3(sh[10].rgb)
               + 0.3731763326f * z * (2.0f * zz - 3.0f * xx - 3.0f * yy) * float3(sh[11].rgb)
               - 0.4570457995f * x * (4.0f * zz - xx - yy) * float3(sh[12].rgb)
               + 1.4453057213f * z * (xx - yy) * float3(sh[13].rgb)
               - 0.5900435899f * x * (xx - 3.0f * yy) * float3(sh[14].rgb);
        }
    }
    return max(c, 0.0f);
}

#endif // SOG_DECODE_METAL
