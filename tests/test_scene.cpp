// Full-scene checks for the SOG library against Phase 0's reference data.
// usage: test_scene <data_dir>
//   <data_dir>/scene_nosky.sog, sog_unzipped/, scene_nosky_roundtrip.ply, blueprint_test/*.bin
#include "SOGCodec.h"
#include "SOGLoader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> ReadAll(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

struct Ply {
    size_t count = 0;
    size_t stride = 0;
    std::map<std::string, size_t> offsets;
    std::vector<uint8_t> data;
    size_t dataOffset = 0;

    float Get(size_t i, const std::string& name) const
    {
        float v;
        std::memcpy(&v, data.data() + dataOffset + i * stride + offsets.at(name), sizeof(v));
        return v;
    }
};

bool LoadPly(const std::string& path, Ply& ply)
{
    ply.data = ReadAll(path);
    const std::string marker = "end_header\n";
    const auto it = std::search(ply.data.begin(), ply.data.end(), marker.begin(), marker.end());
    if (it == ply.data.end()) {
        return false;
    }
    ply.dataOffset = size_t(it - ply.data.begin()) + marker.size();
    const std::string header(ply.data.begin(), ply.data.begin() + ply.dataOffset);
    size_t pos = 0;
    while (pos < header.size()) {
        const size_t eol = header.find('\n', pos);
        const std::string line = header.substr(pos, eol - pos);
        pos = eol + 1;
        char a[64], b[64];
        unsigned long n = 0;
        if (std::sscanf(line.c_str(), "element vertex %lu", &n) == 1) {
            ply.count = n;
        } else if (std::sscanf(line.c_str(), "property %63s %63s", a, b) == 2) {
            if (std::string(a) != "float") {
                return false;
            }
            ply.offsets[b] = ply.stride;
            ply.stride += 4;
        }
    }
    return ply.data.size() >= ply.dataOffset + ply.count * ply.stride;
}

double Seconds(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}

int failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        ++failures;
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <data_dir>\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    std::string err;

    // Load the bundled file and the unbundled folder.
    const auto t0 = std::chrono::steady_clock::now();
    auto zipSource = sog::OpenPath(dir + "/scene_nosky.sog", &err);
    sog::DecodedAsset asset;
    const sog::Error e = zipSource ? sog::LoadSOG(*zipSource, asset, &err) : sog::Error::NotZipOrFolder;
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("load scene_nosky.sog: %s %s (%.2f s, %u splats, %u palette entries, %u coeffs)\n",
                sog::ErrorName(e), err.c_str(), Seconds(t0, t1), asset.count, asset.paletteCount, asset.shCoeffs);
    if (e != sog::Error::Ok) {
        return 1;
    }

    auto folderSource = sog::OpenPath(dir + "/sog_unzipped", &err);
    sog::DecodedAsset fromFolder;
    const bool folderOk = folderSource && sog::LoadSOG(*folderSource, fromFolder, &err) == sog::Error::Ok;

    std::printf("records and tables vs Phase 0 reference:\n");
    const std::vector<uint8_t> refRecords = ReadAll(dir + "/blueprint_test/records.bin");
    Check(refRecords.size() == asset.splats.size() * sizeof(sog::PackedSplat) &&
          std::memcmp(refRecords.data(), asset.splats.data(), refRecords.size()) == 0,
          "20-byte records identical to the Python reference");
    const std::vector<uint8_t> refScale = ReadAll(dir + "/blueprint_test/scale_lut.bin");
    const std::vector<uint8_t> refDc = ReadAll(dir + "/blueprint_test/dc_lut.bin");
    double lutErr = 0;
    for (int i = 0; i < 256; ++i) {
        float s, d;
        std::memcpy(&s, refScale.data() + i * 4, 4);
        std::memcpy(&d, refDc.data() + i * 4, 4);
        lutErr = std::max(lutErr, std::fabs(double(asset.scaleLUT[i]) - s) / s);
        lutErr = std::max(lutErr, std::fabs(double(asset.dcLUT[i]) - d));
    }
    Check(refScale.size() == 1024 && lutErr < 1e-6, "scale / DC lookup tables within 1e-6");
    const std::vector<uint8_t> refPalette = ReadAll(dir + "/blueprint_test/palette_half4.bin");
    Check(refPalette.size() == asset.paletteHalf4.size() * 2 &&
          std::memcmp(refPalette.data(), asset.paletteHalf4.data(), refPalette.size()) == 0,
          "half-float SH palette identical to numpy's float16");
    Check(folderOk && fromFolder.splats.size() == asset.splats.size() &&
          std::memcmp(fromFolder.splats.data(), asset.splats.data(), asset.splats.size() * 20) == 0 &&
          fromFolder.paletteIndices == asset.paletteIndices,
          "unbundled folder loads identically to the .sog");

    // CPU decode vs splat-transform's own decode (the round-trip PLY is in SOG order).
    std::printf("CPU decode vs splat-transform round-trip PLY:\n");
    Ply ply;
    if (!LoadPly(dir + "/scene_nosky_roundtrip.ply", ply) || ply.count != asset.count) {
        std::printf("  cannot read round-trip PLY\n");
        return 1;
    }
    double ePos = 0, eRot = 0, eScale = 0, eOpacity = 0, eDc = 0, eSh = 0;
    sog::SplatFloat s;
    for (size_t i = 0; i < asset.count; ++i) {
        sog::DecodeSplat(asset, i, s);
        ePos = std::max({ePos, std::fabs(double(s.position[0]) - ply.Get(i, "x")),
                         std::fabs(double(s.position[1]) - ply.Get(i, "y")), std::fabs(double(s.position[2]) - ply.Get(i, "z"))});
        double r[4] = { ply.Get(i, "rot_0"), ply.Get(i, "rot_1"), ply.Get(i, "rot_2"), ply.Get(i, "rot_3") };
        const double rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
        double dot = 0;
        for (int k = 0; k < 4; ++k) {
            dot += r[k] / rl * s.rotation[k];
        }
        eRot = std::max(eRot, 1.0 - std::fabs(dot));
        for (int k = 0; k < 3; ++k) {
            const double ref = std::exp(double(ply.Get(i, "scale_" + std::to_string(k))));
            eScale = std::max(eScale, std::fabs(s.scale[k] - ref) / ref);
            eDc = std::max(eDc, std::fabs(double(s.dc[k]) - ply.Get(i, "f_dc_" + std::to_string(k))));
        }
        const double op = 1.0 / (1.0 + std::exp(-double(ply.Get(i, "opacity"))));
        eOpacity = std::max(eOpacity, std::fabs(s.opacity - op));
        for (uint32_t c = 0; c < s.shCoeffs; ++c) {
            for (int ch = 0; ch < 3; ++ch) {
                eSh = std::max(eSh, std::fabs(double(s.sh[c][ch]) - ply.Get(i, "f_rest_" + std::to_string(ch * 15 + c))));
            }
        }
    }
    std::printf("  max errors: position %.3g m, rotation 1-|dot| %.3g, scale %.3g rel, opacity %.3g, dc %.3g, sh %.3g\n",
                ePos, eRot, eScale, eOpacity, eDc, eSh);
    Check(ePos <= 2e-6 && eRot <= 1e-6 && eScale <= 1e-6 && eOpacity <= 1e-6 && eDc == 0 && eSh == 0,
          "every splat matches splat-transform's decode");

    // Encoder: decode -> encode must reproduce the record.
    std::printf("encoder round trip:\n");
    const auto t2 = std::chrono::steady_clock::now();
    sog::Encoder encoder(asset);
    size_t badPos = 0, badQuat = 0, badScale = 0, badOpacity = 0, badDc = 0, badLabel = 0;
    double worstQuatAngle = 0;
    for (size_t i = 0; i < asset.count; ++i) {
        sog::DecodeSplat(asset, i, s);
        const sog::PackedSplat& ref = asset.splats[i];
        const sog::PackedSplat p = encoder.Encode(s, uint16_t(ref.meanZ_label >> 16), uint8_t(ref.dc >> 24));
        badPos += (p.meanXY != ref.meanXY) || ((p.meanZ_label & 0xFFFF) != (ref.meanZ_label & 0xFFFF));
        badLabel += (p.meanZ_label >> 16) != (ref.meanZ_label >> 16);
        badScale += (p.scaleOpacity & 0xFFFFFF) != (ref.scaleOpacity & 0xFFFFFF);
        badOpacity += (p.scaleOpacity >> 24) != (ref.scaleOpacity >> 24);
        badDc += p.dc != ref.dc;
        if (p.quat != ref.quat) {
            ++badQuat;
            sog::DecodedAsset one;                                   // decode the re-encoded quaternion
            one = sog::DecodedAsset();
            one.count = 1;
            one.splats.push_back(p);
            sog::SplatFloat t;
            sog::DecodeSplat(one, 0, t);
            double dot = 0;
            for (int k = 0; k < 4; ++k) {
                dot += double(t.rotation[k]) * s.rotation[k];
            }
            worstQuatAngle = std::max(worstQuatAngle, 2.0 * std::acos(std::min(1.0, std::fabs(dot))) * 57.29577951308232);
        }
    }
    const auto t3 = std::chrono::steady_clock::now();
    std::printf("  %zu splats in %.2f s; mismatches: position %zu, quaternion %zu (worst %.3f deg), scale %zu, "
                "opacity %zu, dc %zu, label %zu\n", size_t(asset.count), Seconds(t2, t3), badPos, badQuat,
                worstQuatAngle, badScale, badOpacity, badDc, badLabel);
    Check(badPos == 0 && badScale == 0 && badOpacity == 0 && badDc == 0 && badLabel == 0,
          "positions, scales, opacity, colour and labels re-encode exactly");
    Check(worstQuatAngle < 0.5, "re-encoded quaternions within 0.5 degrees");

    std::printf("%s\n", failures ? "FAILED" : "all scene checks passed");
    return failures ? 1 : 0;
}
