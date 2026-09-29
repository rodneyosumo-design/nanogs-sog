// SOGCodec.cpp — see SOGCodec.h.
#include "SOGCodec.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sog {

namespace {

constexpr double kSqrt2 = 1.4142135623730950488;

double Unlog(double n)
{
    return n < 0.0 ? -std::expm1(-n) : std::expm1(n);      // sign(n) * (exp(|n|) - 1)
}

double Log(double p)
{
    return p < 0.0 ? -std::log1p(-p) : std::log1p(p);      // sign(p) * log(|p| + 1)
}

uint32_t Round(double v, uint32_t maxValue)
{
    const double r = std::floor(v + 0.5);
    if (!(r > 0.0)) {                                         // also catches NaN
        return 0;
    }
    return r >= double(maxValue) ? maxValue : uint32_t(r);
}

} // namespace

const char* ErrorName(Error e)
{
    switch (e) {
        case Error::Ok:                return "Ok";
        case Error::NotZipOrFolder:    return "NotZipOrFolder";
        case Error::UnsupportedZip:    return "UnsupportedZip";
        case Error::MissingMeta:       return "MissingMeta";
        case Error::BadMeta:           return "BadMeta";
        case Error::BadVersion:        return "BadVersion";
        case Error::MissingImage:      return "MissingImage";
        case Error::BadImage:          return "BadImage";
        case Error::LossyImage:        return "LossyImage";
        case Error::ImageSizeMismatch: return "ImageSizeMismatch";
        case Error::BadQuatMode:       return "BadQuatMode";
        case Error::LabelOutOfRange:   return "LabelOutOfRange";
    }
    return "Unknown";
}

AssetConstants DecodedAsset::MakeConstants(const float fileToLocal[16]) const
{
    AssetConstants c{};
    std::memcpy(c.fileToLocal, fileToLocal, sizeof(c.fileToLocal));
    for (int k = 0; k < 3; ++k) {
        c.meanMin[k] = float(meanMin[k]);
        c.meanMax[k] = float(meanMax[k]);
    }
    c.count = count;
    c.shCoeffs = shCoeffs;
    c.antialias = antialias ? 1u : 0u;
    return c;
}

void DecodeSplat(const DecodedAsset& a, size_t index, SplatFloat& o)
{
    const PackedSplat& s = a.splats[index];

    const uint32_t q[3] = { s.meanXY & 0xFFFFu, s.meanXY >> 16, s.meanZ_label & 0xFFFFu };
    for (int k = 0; k < 3; ++k) {
        const double n = a.meanMin[k] + (a.meanMax[k] - a.meanMin[k]) * (double(q[k]) / 65535.0);
        o.position[k] = float(Unlog(n));
    }

    const uint32_t p = s.quat;
    const double qa = (double(p & 0xFFu) / 255.0 - 0.5) * kSqrt2;
    const double qb = (double((p >> 8) & 0xFFu) / 255.0 - 0.5) * kSqrt2;
    const double qc = (double((p >> 16) & 0xFFu) / 255.0 - 0.5) * kSqrt2;
    const double qd = std::sqrt(std::max(0.0, 1.0 - (qa * qa + qb * qb + qc * qc)));
    double r[4];
    switch (p >> 24) {                                       // index of the dropped component
        case 0:  r[0] = qd; r[1] = qa; r[2] = qb; r[3] = qc; break;
        case 1:  r[0] = qa; r[1] = qd; r[2] = qb; r[3] = qc; break;
        case 2:  r[0] = qa; r[1] = qb; r[2] = qd; r[3] = qc; break;
        default: r[0] = qa; r[1] = qb; r[2] = qc; r[3] = qd; break;
    }
    for (int k = 0; k < 4; ++k) {
        o.rotation[k] = float(r[k]);
    }

    const uint32_t so = s.scaleOpacity;
    o.scale[0] = float(std::exp(double(a.scaleCodebook[so & 0xFFu])));
    o.scale[1] = float(std::exp(double(a.scaleCodebook[(so >> 8) & 0xFFu])));
    o.scale[2] = float(std::exp(double(a.scaleCodebook[(so >> 16) & 0xFFu])));
    o.opacity = float(double(so >> 24) / 255.0);

    o.dc[0] = a.sh0Codebook[s.dc & 0xFFu];
    o.dc[1] = a.sh0Codebook[(s.dc >> 8) & 0xFFu];
    o.dc[2] = a.sh0Codebook[(s.dc >> 16) & 0xFFu];

    std::memset(o.sh, 0, sizeof(o.sh));
    o.shCoeffs = 0;
    const uint32_t flags = s.dc >> 24;
    if (a.shCoeffs > 0 && !(flags & kFlagNoSH)) {
        const uint32_t label = s.meanZ_label >> 16;
        const uint8_t* idx = a.paletteIndices.data() + size_t(label) * a.shCoeffs * 3;
        for (uint32_t c = 0; c < a.shCoeffs; ++c) {
            for (int ch = 0; ch < 3; ++ch) {
                o.sh[c][ch] = a.shNCodebook[idx[c * 3 + ch]];
            }
        }
        o.shCoeffs = a.shCoeffs;
    }
}

void BuildGpuTables(DecodedAsset& a)
{
    for (int i = 0; i < 256; ++i) {
        a.scaleLUT[i] = std::exp(a.scaleCodebook[i]);
        a.dcLUT[i] = 0.5f + kSHC0 * a.sh0Codebook[i];
    }
    const size_t entries = size_t(a.paletteCount) * a.shCoeffs;
    a.paletteHalf4.assign(entries * 4, 0);
    for (size_t e = 0; e < entries; ++e) {
        for (int ch = 0; ch < 3; ++ch) {
            a.paletteHalf4[e * 4 + ch] = FloatToHalf(a.shNCodebook[a.paletteIndices[e * 3 + ch]]);
        }
    }
}

Encoder::Encoder(const DecodedAsset& asset)
    : Asset(asset)
{
    Prepare(asset.scaleCodebook, ScaleSorted);
    Prepare(asset.sh0Codebook, DcSorted);
}

void Encoder::Prepare(const float* codebook, Sorted& out)
{
    uint8_t order[256];
    for (int i = 0; i < 256; ++i) {
        order[i] = uint8_t(i);
    }
    std::stable_sort(order, order + 256, [codebook](uint8_t x, uint8_t y) { return codebook[x] < codebook[y]; });
    for (int i = 0; i < 256; ++i) {
        out.index[i] = order[i];
        out.value[i] = codebook[order[i]];
    }
}

uint8_t Encoder::Nearest(const Sorted& s, float v)
{
    const float* it = std::lower_bound(s.value, s.value + 256, v);
    const int hi = int(it - s.value);
    if (hi <= 0) {
        return s.index[0];
    }
    if (hi >= 256) {
        return s.index[255];
    }
    return (v - s.value[hi - 1] <= s.value[hi] - v) ? s.index[hi - 1] : s.index[hi];
}

PackedSplat Encoder::Encode(const SplatFloat& s, uint16_t label, uint8_t flags) const
{
    uint32_t q[3];
    for (int k = 0; k < 3; ++k) {
        const double range = Asset.meanMax[k] - Asset.meanMin[k];
        const double t = range > 0.0 ? (Log(double(s.position[k])) - Asset.meanMin[k]) / range : 0.0;
        q[k] = Round(t * 65535.0, 65535);
    }

    double r[4] = { s.rotation[0], s.rotation[1], s.rotation[2], s.rotation[3] };
    const double len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    if (!(len > 0.0)) {
        r[0] = 1.0; r[1] = r[2] = r[3] = 0.0;
    } else {
        for (double& c : r) {
            c /= len;
        }
    }
    int largest = 0;
    for (int k = 1; k < 4; ++k) {
        if (std::fabs(r[k]) > std::fabs(r[largest])) {
            largest = k;
        }
    }
    const double sign = r[largest] < 0.0 ? -1.0 : 1.0;          // q and -q are the same rotation
    uint32_t packedQuat = uint32_t(largest) << 24;
    for (int k = 0, slot = 0; k < 4; ++k) {
        if (k != largest) {
            packedQuat |= Round((sign * r[k] / kSqrt2 + 0.5) * 255.0, 255) << (8 * slot++);
        }
    }

    uint32_t scaleOpacity = Round(double(s.opacity) * 255.0, 255) << 24;
    for (int k = 0; k < 3; ++k) {
        const float ls = std::log(std::max(s.scale[k], 1e-30f));
        scaleOpacity |= uint32_t(Nearest(ScaleSorted, ls)) << (8 * k);
    }

    uint32_t dc = uint32_t(flags) << 24;
    for (int k = 0; k < 3; ++k) {
        dc |= uint32_t(Nearest(DcSorted, s.dc[k])) << (8 * k);
    }

    return { q[0] | (q[1] << 16), q[2] | (uint32_t(label) << 16), packedQuat, scaleOpacity, dc };
}

uint16_t FloatToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000u;
    uint32_t mant = x & 0x007FFFFFu;
    const int32_t exp = int32_t((x >> 23) & 0xFFu);
    if (exp == 0xFF) {                                          // inf / NaN
        return uint16_t(sign | 0x7C00u | (mant ? 0x200u | (mant >> 13) : 0u));
    }
    const int32_t e = exp - 127 + 15;
    if (e >= 0x1F) {                                            // overflow -> inf
        return uint16_t(sign | 0x7C00u);
    }
    if (e <= 0) {                                               // subnormal or zero
        if (e < -10) {
            return uint16_t(sign);
        }
        mant |= 0x00800000u;
        const uint32_t shift = uint32_t(14 - e);
        uint32_t half = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (half & 1u))) {
            ++half;
        }
        return uint16_t(sign | half);
    }
    uint32_t half = (uint32_t(e) << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) {
        ++half;                                                 // a carry into the exponent is correct
    }
    return uint16_t(sign | half);
}

float HalfToFloat(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {                                                // renormalize a subnormal
            int32_t e = -1;
            do {
                ++e;
                mant <<= 1;
            } while (!(mant & 0x400u));
            bits = sign | (uint32_t(127 - 15 - e) << 23) | ((mant & 0x3FFu) << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

} // namespace sog
