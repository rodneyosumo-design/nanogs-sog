// Codec edge cases: float<->half against the hardware conversion, and encoder corner cases.
#include "SOGCodec.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        ++failures;
    }
}

bool IsNaNHalf(uint16_t h) { return (h & 0x7C00u) == 0x7C00u && (h & 0x3FFu); }

} // namespace

int main()
{
    // Every half round-trips through float.
    size_t halfBad = 0;
    for (uint32_t h = 0; h < 65536; ++h) {
        const uint16_t back = sog::FloatToHalf(sog::HalfToFloat(uint16_t(h)));
        if (IsNaNHalf(uint16_t(h)) ? !IsNaNHalf(back) : back != h) {
            ++halfBad;
        }
    }
    Check(halfBad == 0, "all 65,536 halves round-trip through float");

#if defined(__aarch64__)
    // Compare with the hardware float->half conversion over a sweep of every 256th float bit pattern.
    size_t hwBad = 0;
    for (uint64_t bits = 0; bits <= 0xFFFFFFFFull; bits += 256) {
        float f;
        const uint32_t b = uint32_t(bits);
        std::memcpy(&f, &b, 4);
        const _Float16 hw = static_cast<_Float16>(f);
        uint16_t hwBits;
        std::memcpy(&hwBits, &hw, 2);
        const uint16_t sw = sog::FloatToHalf(f);
        if (std::isnan(f) ? !(IsNaNHalf(hwBits) && IsNaNHalf(sw)) : hwBits != sw) {
            ++hwBad;
        }
    }
    Check(hwBad == 0, "FloatToHalf matches the M4's hardware conversion (16.8M floats)");
#endif

    // Encoder corner cases on a synthetic asset.
    sog::DecodedAsset a;
    a.count = 1;
    for (int k = 0; k < 3; ++k) {
        a.meanMin[k] = -2.0;
        a.meanMax[k] = 2.0;
    }
    for (int i = 0; i < 256; ++i) {
        a.scaleCodebook[i] = -10.0f + 10.0f * float(i) / 255.0f;
        a.sh0Codebook[i] = -1.0f + 2.0f * float(i) / 255.0f;
    }
    sog::BuildGpuTables(a);
    const sog::Encoder enc(a);
    sog::SplatFloat s;
    s.scale[0] = s.scale[1] = s.scale[2] = 0.01f;
    s.opacity = 0.5f;

    s.rotation[0] = 1; s.rotation[1] = s.rotation[2] = s.rotation[3] = 0;
    sog::PackedSplat p = enc.Encode(s, 7, sog::kFlagNoSH);
    a.splats = {p};
    sog::SplatFloat d;
    sog::DecodeSplat(a, 0, d);
    Check((p.quat >> 24) == 0 && std::fabs(d.rotation[0] - 1.0f) < 1e-4f, "identity rotation encodes with w dropped");
    Check((p.meanZ_label >> 16) == 7 && (p.dc >> 24) == sog::kFlagNoSH && d.shCoeffs == 0,
          "label and no-SH flag are packed, flag suppresses SH");

    s.rotation[0] = -0.1f; s.rotation[1] = 0.2f; s.rotation[2] = -0.9f; s.rotation[3] = 0.3f;
    p = enc.Encode(s, 0, 0);
    a.splats = {p};
    sog::DecodeSplat(a, 0, d);
    const float n = std::sqrt(0.01f + 0.04f + 0.81f + 0.09f);
    float dot = 0;
    for (int k = 0; k < 4; ++k) {
        dot += d.rotation[k] * (k == 0 ? -0.1f : k == 1 ? 0.2f : k == 2 ? -0.9f : 0.3f) / n;
    }
    Check((p.quat >> 24) == 2 && std::fabs(std::fabs(dot) - 1.0f) < 1e-4f, "negative largest component flips sign, same rotation");

    s.rotation[0] = s.rotation[1] = s.rotation[2] = s.rotation[3] = 0;
    p = enc.Encode(s, 0, 0);
    Check((p.quat >> 24) == 0, "zero quaternion encodes as identity");

    s.position[0] = 1e9f; s.position[1] = -1e9f; s.position[2] = 0.0f;
    p = enc.Encode(s, 0, 0);
    Check((p.meanXY & 0xFFFF) == 65535 && (p.meanXY >> 16) == 0, "positions outside the range clamp");

    s.opacity = 2.0f;
    s.scale[0] = 1e-20f;
    s.dc[0] = 5.0f;
    p = enc.Encode(s, 0, 0);
    Check((p.scaleOpacity >> 24) == 255 && (p.scaleOpacity & 0xFF) == 0 && (p.dc & 0xFF) == 255,
          "opacity, scale and DC clamp to the codebook ends");

    std::printf("%s\n", failures ? "FAILED" : "all codec checks passed");
    return failures ? 1 : 0;
}
