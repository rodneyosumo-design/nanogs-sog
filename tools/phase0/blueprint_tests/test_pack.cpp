// Checks sog::PackSplat reproduces the reference records for every splat.
// build: clang++ -std=c++17 -O2 -I ../../../blueprint test_pack.cpp -o test_pack
// usage: ./test_pack <test_data_dir>
#include "SOGTypes.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static std::vector<uint8_t> ReadAll(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <test_data_dir>\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    const std::vector<uint8_t> texels = ReadAll(dir + "/texels.bin");
    const std::vector<uint8_t> records = ReadAll(dir + "/records.bin");
    const size_t n = records.size() / sizeof(sog::PackedSplat);
    if (n == 0 || texels.size() != n * 24) {
        std::fprintf(stderr, "bad test data (%zu records, %zu texel bytes)\n", n, texels.size());
        return 2;
    }
    size_t mismatches = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* t = texels.data() + i * 24;       // means_l, means_u, quats, scales, sh0, labels
        const sog::PackedSplat p = sog::PackSplat(t, t + 4, t + 8, t + 12, t + 16, t + 20);
        if (std::memcmp(&p, records.data() + i * sizeof(p), sizeof(p)) != 0) {
            if (++mismatches <= 5) {
                std::fprintf(stderr, "mismatch at splat %zu\n", i);
            }
        }
    }
    std::printf("{\"splats\": %zu, \"pack_mismatches\": %zu, \"sizeof_PackedSplat\": %zu, \"sizeof_AssetConstants\": %zu}\n",
                n, mismatches, sizeof(sog::PackedSplat), sizeof(sog::AssetConstants));
    return mismatches == 0 ? 0 : 1;
}
