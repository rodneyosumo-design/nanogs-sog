// SOGTypes.h
// CPU-side layout for SOG (PlayCanvas "Spatially Ordered Gaussians", spec v2) assets as they are
// kept on the GPU: one 20-byte record per splat plus small tables. Shared by the NanoGS importer
// and any native loader. Mirrors shaders/SOGDecode.metal byte for byte.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Export macro for shared-library builds (NanoGS defines SOG_API=NANOGS_API); empty for static use.
#ifndef SOG_API
#define SOG_API
#endif

namespace sog {

inline constexpr float kSHC0 = 0.28209479177387814f;

// Bits 24..31 of PackedSplat::dc.
inline constexpr uint32_t kFlagNoSH = 1u;   // skip the AC palette (NanoGS merged LOD splats carry no SH)

struct PackedSplat {
    uint32_t meanXY;        // qx | qy << 16
    uint32_t meanZ_label;   // qz | shN label << 16
    uint32_t quat;          // a | b << 8 | c << 16 | (quats.A - 252) << 24
    uint32_t scaleOpacity;  // sx | sy << 8 | sz << 16 | opacity << 24
    uint32_t dc;            // r | g << 8 | b << 16 | flags << 24
};
static_assert(sizeof(PackedSplat) == 20, "must match SOGSplat in SOGDecode.metal");

struct AssetConstants {
    float    fileToLocal[16];               // column-major: element (row r, col c) at [c * 4 + r]
    float    meanMin[3]; uint32_t count;    // log-domain
    float    meanMax[3]; uint32_t shCoeffs; // 0, 3, 8 or 15
    uint32_t antialias;  uint32_t pad[3];
};
static_assert(sizeof(AssetConstants) == 112, "must match SOGAssetConstants in SOGDecode.metal");

// SOG files written by splat-transform keep the source PLY's axes (Phase 0: identical bounds), so for
// NanoGS the file->local transform equals its PLY importer mapping: local cm = 100 * (z, x, -y).
// It is a reflection (det -1); SOGDecode.metal applies it to the covariance, which Phase 0 showed is
// identical to NanoGS's quaternion conversion (max relative difference 3e-16 over 200k splats).
inline constexpr float kNanoGSFileToLocal[16] = {
    0.0f, 100.0f, 0.0f, 0.0f,     // column 0: image of file x
    0.0f, 0.0f, -100.0f, 0.0f,    // column 1: image of file y
    100.0f, 0.0f, 0.0f, 0.0f,     // column 2: image of file z
    0.0f, 0.0f, 0.0f, 1.0f,
};

struct DecodedAsset {
    uint32_t count = 0;
    double   meanMin[3] = {0, 0, 0};        // meta.means.mins (log-domain, written with 17 digits)
    double   meanMax[3] = {0, 0, 0};        // meta.means.maxs
    bool     antialias = false;
    uint32_t shBands = 0;                   // 0..3
    uint32_t shCoeffs = 0;                  // 0, 3, 8 or 15
    uint32_t paletteCount = 0;

    // Codebooks exactly as stored in meta.json.
    float scaleCodebook[256] = {};          // log scale
    float sh0Codebook[256] = {};            // SH DC coefficient
    float shNCodebook[256] = {};            // SH AC coefficient

    std::vector<PackedSplat> splats;        // Morton order preserved
    std::vector<uint8_t>     paletteIndices;// paletteCount * shCoeffs * 3 shNCodebook indices (r, g, b)

    // Derived GPU tables.
    float scaleLUT[256] = {};               // exp(scaleCodebook)
    float dcLUT[256] = {};                  // 0.5 + SH_C0 * sh0Codebook
    std::vector<uint16_t> paletteHalf4;     // paletteCount * shCoeffs * 4 halves (rgb + unused)
};

SOG_API AssetConstants MakeConstants(const DecodedAsset& asset, const float fileToLocal[16]);

enum class Error {
    Ok, NotZipOrFolder, UnsupportedZip, MissingMeta, BadMeta, BadVersion, MissingImage,
    BadImage, LossyImage, ImageSizeMismatch, BadQuatMode, LabelOutOfRange
};
SOG_API const char* ErrorName(Error e);

// Each pointer is this splat's RGBA8 texel, decoded with libwebp WebPDecodeRGBA (exact bytes; never
// ImageIO/CoreGraphics, which premultiply alpha). The caller has checked q[3] is 252..255.
inline PackedSplat PackSplat(const uint8_t* ml, const uint8_t* mu, const uint8_t* q,
                             const uint8_t* s, const uint8_t* c, const uint8_t* lbl /* null if no SH */)
{
    const uint32_t x = ml[0] | (mu[0] << 8), y = ml[1] | (mu[1] << 8), z = ml[2] | (mu[2] << 8);
    const uint32_t label = lbl ? uint32_t(lbl[0] | (lbl[1] << 8)) : 0u;
    return { x | (y << 16),
             z | (label << 16),
             uint32_t(q[0] | (q[1] << 8) | (q[2] << 16)) | (uint32_t(q[3] - 252) << 24),
             uint32_t(s[0] | (s[1] << 8) | (s[2] << 16)) | (uint32_t(c[3]) << 24),
             uint32_t(c[0] | (c[1] << 8) | (c[2] << 16)) };
}

// Streamed SOG (lod-meta.json)
struct LodRun  { uint32_t file, offset, count; };
struct LodNode {
    float boundMin[3], boundMax[3];
    int32_t child[2] = {-1, -1};                          // interior nodes have exactly two children
    std::vector<std::pair<uint32_t, LodRun>> lods;        // leaf: level -> run
    std::vector<float> errors;                            // optional, non-decreasing per level
};
struct LodMeta {
    uint32_t lodLevels = 0;
    std::vector<uint32_t> counts;
    std::vector<std::string> filenames;                   // chunk meta.json paths
    std::vector<LodNode> nodes;                           // nodes[0] = root
};

} // namespace sog
