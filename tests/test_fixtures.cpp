// Loads every fixture from tools/phase1/make_fixtures.py and checks the error code (and, for valid
// fixtures, every record) against fixtures.json.
// usage: test_fixtures <fixtures_dir>
#include "SOGJson.h"
#include "SOGLoader.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <fixtures_dir>\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    std::ifstream f(dir + "/fixtures.json", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(f)), {});
    sog::json::Value manifest;
    std::string err;
    if (!sog::json::Parse(text.data(), text.size(), manifest, &err) || !manifest.IsObject()) {
        std::fprintf(stderr, "bad fixtures.json: %s\n", err.c_str());
        return 2;
    }
    int failures = 0;
    for (const auto& item : manifest.object) {
        const std::string& name = item.first;
        const std::string path = dir + "/" + item.second.Find("path")->string;
        const std::string expected = item.second.Find("error")->string;
        std::string message;
        sog::DecodedAsset asset;
        auto source = sog::OpenPath(path, &message);
        const sog::Error e = source ? sog::LoadSOG(*source, asset, &message) : sog::Error::NotZipOrFolder;
        bool ok = expected == sog::ErrorName(e);
        if (ok && e == sog::Error::Ok) {
            const sog::json::Value* records = item.second.Find("records");
            ok = records && records->array.size() == asset.splats.size();
            for (size_t i = 0; ok && i < asset.splats.size(); ++i) {
                const uint32_t* got = &asset.splats[i].meanXY;
                for (size_t k = 0; k < 5; ++k) {
                    ok = ok && uint32_t(records->array[i].array[k].number) == got[k];
                }
            }
        }
        std::printf("  %-20s expected %-18s got %-18s %s%s%s\n", name.c_str(), expected.c_str(), sog::ErrorName(e),
                    ok ? "ok" : "FAIL", message.empty() ? "" : "  // ", message.c_str());
        failures += ok ? 0 : 1;
    }
    std::printf("%s\n", failures ? "FAILED" : "all fixtures passed");
    return failures ? 1 : 0;
}
