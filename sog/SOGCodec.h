// SOGCodec.h
// Dependency-free CPU decode and encode of SOG records. Decode mirrors shaders/SOGDecode.metal and
// the Python reference (tools/phase0/sog_decode.py) in double precision; encode is its inverse,
// used for splats that don't come from the file (NanoGS's merged LOD splats).
#pragma once

#include "SOGTypes.h"

#include <cstddef>
#include <cstdint>

namespace sog {

// One splat in the SOG file frame, linear units.
struct SplatFloat {
    float    position[3] = {0, 0, 0};
    float    rotation[4] = {1, 0, 0, 0};    // w, x, y, z (unit)
    float    scale[3] = {0, 0, 0};          // linear
    float    opacity = 0;                   // 0..1
    float    dc[3] = {0, 0, 0};             // SH DC coefficients (PLY f_dc)
    float    sh[15][3] = {};                // AC coefficients, coefficient-major; zero past shCoeffs
    uint32_t shCoeffs = 0;                  // valid AC coefficients (0 when kFlagNoSH is set)
};

SOG_API void DecodeSplat(const DecodedAsset& asset, size_t index, SplatFloat& out);

// Fill scaleLUT, dcLUT and paletteHalf4 from the codebooks and palette indices.
SOG_API void BuildGpuTables(DecodedAsset& asset);

// Quantizes a splat against the asset's existing ranges and codebooks. Positions outside
// [meanMin, meanMax] (log-domain) clamp; scale and DC snap to the nearest codebook entry.
class SOG_API Encoder {
public:
    explicit Encoder(const DecodedAsset& asset);
    PackedSplat Encode(const SplatFloat& s, uint16_t label, uint8_t flags) const;

private:
    struct Sorted { float value[256]; uint8_t index[256]; };
    static void Prepare(const float* codebook, Sorted& out);
    static uint8_t Nearest(const Sorted& s, float v);

    const DecodedAsset& Asset;
    Sorted ScaleSorted;
    Sorted DcSorted;
};

SOG_API uint16_t FloatToHalf(float f);
SOG_API float HalfToFloat(uint16_t h);

} // namespace sog
