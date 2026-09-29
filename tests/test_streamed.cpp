// Streamed SOG: lod-meta.json parsing (malformed manifests) and, when Phase 4's campus data exists, a full merge
// checked against the source chunks splat by splat.
// usage: test_streamed [<lod-meta.json or its folder> [<k-means iterations>]]
#include "SOGCodec.h"
#include "SOGStreamed.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

int gFailures = 0;

void Check(bool ok, const char* what)
{
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    gFailures += ok ? 0 : 1;
}

void ParallelFor(size_t count, const std::function<void(size_t)>& body)
{
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&]() {
            for (size_t i; (i = next.fetch_add(1)) < count;) {
                body(i);
            }
        });
    }
    for (std::thread& t : pool) {
        t.join();
    }
}

const char* kLeaf = R"({"bound": {"min": [0, 0, 0], "max": [1, 1, 1]}, "lods": {"0": {"file": 0, "offset": 0, "count": 4}}})";

std::string Manifest(const std::string& body)
{
    return R"({"version": 1, "lodLevels": 1, "filenames": ["0_0/meta.json"], )" + body + "}";
}

void ParseCases()
{
    struct Case { const char* name; std::string text; sog::Error expected; };
    const std::string leaf = kLeaf;
    const Case cases[] = {
        {"valid leaf", Manifest(R"("tree": )" + leaf), sog::Error::Ok},
        {"valid interior node", Manifest(R"("tree": {"bound": {"min": [0,0,0], "max": [1,1,1]}, "children": [)" + leaf + "," + leaf + "]}"), sog::Error::Ok},
        {"pre-versioned file", R"({"lodLevels": 1, "filenames": ["a/meta.json"], "tree": )" + leaf + "}", sog::Error::Ok},
        {"not json", "{", sog::Error::BadLodMeta},
        {"version 2", R"({"version": 2, "lodLevels": 1, "filenames": ["a/meta.json"], "tree": )" + leaf + "}", sog::Error::BadLodMeta},
        {"no lodLevels", R"({"filenames": ["a/meta.json"], "tree": )" + leaf + "}", sog::Error::BadLodMeta},
        {"no filenames", R"({"lodLevels": 1, "tree": )" + leaf + "}", sog::Error::BadLodMeta},
        {"parent path in filenames", R"({"lodLevels": 1, "filenames": ["../x/meta.json"], "tree": )" + leaf + "}", sog::Error::BadLodMeta},
        {"absolute filename", R"({"lodLevels": 1, "filenames": ["/x/meta.json"], "tree": )" + leaf + "}", sog::Error::BadLodMeta},
        {"no tree", Manifest(R"("count": 4)"), sog::Error::BadLodMeta},
        {"file index out of range", Manifest(R"("tree": {"bound": {"min": [0,0,0], "max": [1,1,1]}, "lods": {"0": {"file": 1, "offset": 0, "count": 4}}})"), sog::Error::BadLodMeta},
        {"level out of range", Manifest(R"("tree": {"bound": {"min": [0,0,0], "max": [1,1,1]}, "lods": {"1": {"file": 0, "offset": 0, "count": 4}}})"), sog::Error::BadLodMeta},
        {"errors wrong length", Manifest(R"("tree": {"bound": {"min": [0,0,0], "max": [1,1,1]}, "lods": {"0": {"file": 0, "count": 4}}, "errors": [0, 1]})"), sog::Error::BadLodMeta},
        {"negative error", Manifest(R"("tree": {"bound": {"min": [0,0,0], "max": [1,1,1]}, "lods": {"0": {"file": 0, "count": 4}}, "errors": [-1]})"), sog::Error::BadLodMeta},
        {"bad bound", Manifest(R"("tree": {"bound": {"min": [0,0], "max": [1,1,1]}, "lods": {"0": {"file": 0, "count": 4}}})"), sog::Error::BadLodMeta},
        {"neither lods nor children", Manifest(R"("tree": {"bound": {"min": [0,0,0], "max": [1,1,1]}})"), sog::Error::BadLodMeta},
        {"fractional count", Manifest(R"("tree": {"bound": {"min": [0,0,0], "max": [1,1,1]}, "lods": {"0": {"file": 0, "count": 4.5}}})"), sog::Error::BadLodMeta},
    };
    for (const Case& c : cases) {
        sog::LodMeta meta;
        std::string message;
        const sog::Error e = sog::ParseLodMeta(c.text.data(), c.text.size(), meta, &message);
        char line[160];
        std::snprintf(line, sizeof(line), "parse: %s -> %s", c.name, sog::ErrorName(e));
        Check(e == c.expected, line);
    }
}

double Seconds(std::chrono::steady_clock::time_point since)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

void MergeScene(const std::string& path, uint32_t iterations)
{
    std::printf("== merge %s\n", path.c_str());
    sog::StreamedOptions options;
    if (iterations > 0) {
        options.iterations = iterations;
    }
    options.parallelFor = ParallelFor;
    options.keepSources = true;
    const auto start = std::chrono::steady_clock::now();
    auto stage = start;
    options.progress = [&](const char* name) {
        std::printf("    %-22s at %6.2f s\n", name, Seconds(start));
        stage = std::chrono::steady_clock::now();
    };
    sog::StreamedAsset asset;
    std::string message;
    const sog::Error e = sog::LoadStreamedPath(path, options, asset, &message);
    std::printf("  load: %s %s (%.1f s)\n", sog::ErrorName(e), message.c_str(), Seconds(start));
    Check(e == sog::Error::Ok, "streamed SOG loads and merges");
    if (e != sog::Error::Ok) {
        return;
    }
    const sog::DecodedAsset& m = asset.merged;
    std::printf("  %u splats, %u levels, %zu leaves, %zu clusters, palette %u (from %llu chunk entries), SH bands %u\n",
                m.count, asset.lodLevels, asset.leaves.size(), asset.clusters.size(), m.paletteCount,
                (unsigned long long)asset.sourcePaletteEntries, m.shBands);
    for (uint32_t l = 0; l < asset.lodLevels; ++l) {
        std::printf("    level %u: %llu splats\n", l, (unsigned long long)asset.levelCounts[l]);
    }

    // Structure: leaf runs tile the records; clusters tile them in order
    std::vector<uint8_t> covered(m.count, 0);
    bool runsOk = true;
    for (const sog::StreamedLeaf& leaf : asset.leaves) {
        runsOk = runsOk && leaf.levels.size() == asset.lodLevels && leaf.errors.size() == asset.lodLevels;
        for (uint32_t l = 0; l < asset.lodLevels && runsOk; ++l) {
            const sog::StreamedLevel& r = leaf.levels[l];
            runsOk = uint64_t(r.start) + r.count <= m.count;
            for (uint32_t i = r.start; runsOk && i < r.start + r.count; ++i) {
                runsOk = covered[i]++ == 0;
            }
            runsOk = runsOk && (l == 0 || leaf.errors[l] >= leaf.errors[l - 1]);
        }
    }
    runsOk = runsOk && std::all_of(covered.begin(), covered.end(), [](uint8_t c) { return c == 1; });
    Check(runsOk, "leaf runs cover every record once; errors non-decreasing");
    bool clustersOk = true;
    uint32_t cursor = 0;
    uint32_t prevLevel = asset.lodLevels;
    for (const sog::StreamedCluster& c : asset.clusters) {
        const sog::StreamedLevel& r = asset.leaves[c.leaf].levels[c.level];
        clustersOk = clustersOk && c.start == cursor && c.count >= 1 && c.count <= 128 &&
                     c.start >= r.start && c.start + c.count <= r.start + r.count && c.level <= prevLevel;
        prevLevel = c.level;
        cursor += c.count;
    }
    Check(clustersOk && cursor == m.count, "clusters tile the records, coarsest level first, <= 128 splats");
    const uint64_t coarsest = asset.levelCounts[asset.lodLevels - 1];
    bool prefixOk = true;
    for (const sog::StreamedLeaf& leaf : asset.leaves) {
        const sog::StreamedLevel& r = leaf.levels[asset.lodLevels - 1];
        prefixOk = prefixOk && (r.count == 0 || uint64_t(r.start) + r.count <= coarsest);
    }
    Check(prefixOk, "coarsest level is a prefix of the records");

    // Accuracy: every record against its source splat
    std::string dir = path;
    if (dir.size() > 14 && dir.compare(dir.size() - 14, 14, "/lod-meta.json") == 0) {
        dir.resize(dir.size() - 14);
    }
    std::string text;
    {
        FILE* f = std::fopen((dir + "/lod-meta.json").c_str(), "rb");
        if (f) {
            char buf[65536];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
                text.append(buf, n);
            }
            std::fclose(f);
        }
    }
    sog::LodMeta meta;
    sog::ParseLodMeta(text.data(), text.size(), meta, &message);
    std::map<uint32_t, sog::DecodedAsset> chunks;
    for (uint64_t src : asset.sources) {
        chunks.emplace(uint32_t(src >> 32), sog::DecodedAsset());
    }
    std::vector<uint32_t> ids;
    for (auto& kv : chunks) {
        ids.push_back(kv.first);
    }
    ParallelFor(ids.size(), [&](size_t i) {
        auto source = sog::OpenPath(dir + "/" + meta.filenames[ids[i]], nullptr);
        sog::LoadSOG(*source, chunks[ids[i]], nullptr);
    });

    const size_t n = m.count;
    // Per-splat errors; percentiles show whether large errors are rare outliers (codebook tails) or systematic
    std::vector<float> posErr(n), scaleErr(n), scaleErrCm(n), dcErr(n);
    std::vector<double> shSq(n), shRefSq(n);
    std::vector<uint8_t> bitsDiffer(n);
    ParallelFor((n + 65535) / 65536, [&](size_t block) {
        sog::SplatFloat a, b;
        for (size_t i = block * 65536; i < std::min(n, (block + 1) * 65536); ++i) {
            const uint64_t src = asset.sources[i];
            const sog::DecodedAsset& chunk = chunks.at(uint32_t(src >> 32));
            sog::DecodeSplat(m, i, a);
            sog::DecodeSplat(chunk, size_t(uint32_t(src)), b);
            const sog::PackedSplat& pa = m.splats[i];
            const sog::PackedSplat& pb = chunk.splats[uint32_t(src)];
            bitsDiffer[i] = (pa.quat != pb.quat || (pa.scaleOpacity >> 24) != (pb.scaleOpacity >> 24)) ? 1 : 0;
            double d2 = 0, ls = 0, lsCm = 0, dc = 0;
            for (int k = 0; k < 3; ++k) {
                d2 += double(a.position[k] - b.position[k]) * double(a.position[k] - b.position[k]);
                // Visible error only: axes under 1 mm and colour outside 0..1 (clamped on screen) don't show
                if (b.scale[k] >= 1e-3f) {
                    ls = std::max(ls, std::fabs(std::log(double(a.scale[k]) / double(b.scale[k]))));
                }
                if (b.scale[k] >= 1e-2f) {
                    lsCm = std::max(lsCm, std::fabs(std::log(double(a.scale[k]) / double(b.scale[k]))));
                }
                const double ca = std::clamp(0.5 + sog::kSHC0 * double(a.dc[k]), 0.0, 1.0);
                const double cb = std::clamp(0.5 + sog::kSHC0 * double(b.dc[k]), 0.0, 1.0);
                dc = std::max(dc, std::fabs(ca - cb));
            }
            posErr[i] = float(std::sqrt(d2));
            scaleErr[i] = float(ls);
            scaleErrCm[i] = float(lsCm);
            dcErr[i] = float(dc);
            double sq = 0, ref = 0;
            for (uint32_t c = 0; c < b.shCoeffs; ++c) {
                for (int ch = 0; ch < 3; ++ch) {
                    const double d = double(a.sh[c][ch]) - double(b.sh[c][ch]);
                    sq += d * d;
                    ref += double(b.sh[c][ch]) * double(b.sh[c][ch]);
                }
            }
            shSq[i] = sq;
            shRefSq[i] = ref;
        }
    });
    auto percentiles = [&](std::vector<float> v, const char* what, double scale, const char* unit) {
        std::sort(v.begin(), v.end());
        auto at = [&](double q) { return double(v[std::min(v.size() - 1, size_t(q * double(v.size())))]) * scale; };
        std::printf("  %-26s p50 %.3g  p99 %.3g  p99.9 %.3g  p99.99 %.3g  max %.3g %s\n", what,
                    at(0.5), at(0.99), at(0.999), at(0.9999), double(v.back()) * scale, unit);
        return std::vector<double>{ at(0.999), double(v.back()) * scale };
    };
    const auto pos = percentiles(posErr, "position error", 1000.0, "mm");
    const auto scale = percentiles(scaleErr, "|log scale ratio|, >= 1 mm", 1.0, "");
    percentiles(scaleErrCm, "|log scale ratio|, >= 1 cm", 1.0, "");
    const auto dc = percentiles(dcErr, "DC colour error, clamped", 1.0, "");
    double sq = 0, ref = 0;
    for (size_t i = 0; i < n; ++i) {
        sq += shSq[i];
        ref += shRefSq[i];
    }
    const double shRel = ref > 0 ? std::sqrt(sq / ref) : 0.0;
    std::printf("  SH AC relative RMS error vs per-chunk palettes: %.3f\n", shRel);
    Check(std::none_of(bitsDiffer.begin(), bitsDiffer.end(), [](uint8_t b) { return b != 0; }),
          "quaternions and opacities copied bit for bit");
    Check(pos[1] < 1.0, "positions within 1 mm");
    Check(scale[0] < 0.05, "scales: 99.9% within 5%");
    Check(dc[0] < 0.01, "DC colour: 99.9% within 0.01");
    Check(shRel < 0.5, "SH within 50% relative RMS (merged palette)");
}

} // namespace

int main(int argc, char** argv)
{
    std::printf("== lod-meta.json parsing\n");
    ParseCases();
    if (argc >= 2) {
        MergeScene(argv[1], argc >= 3 ? uint32_t(std::atoi(argv[2])) : 0u);
    } else {
        std::printf("== merge: skipped (no path)\n");
    }
    std::printf(gFailures ? "FAILED: %d\n" : "all passed\n", gFailures);
    return gFailures ? 1 : 0;
}
