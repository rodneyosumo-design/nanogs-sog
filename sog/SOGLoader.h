// SOGLoader.h — load a SOG v2 asset (bundled .sog zip or unbundled folder) into a DecodedAsset.
// Needs libwebp (decoder) and zlib.
#pragma once

#include "SOGTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sog {

// Supplies the files of one SOG asset by name ("meta.json", "means_l.webp", ...).
class FileSource {
public:
    virtual ~FileSource() = default;
    virtual bool Read(const std::string& name, std::vector<uint8_t>& out, std::string* error) = 0;
};

// A bundled .sog held in memory.
std::unique_ptr<FileSource> MakeZipSource(std::vector<uint8_t> zipBytes, std::string* error);

// Standalone convenience: a .sog file, a folder, or a path to its meta.json (std::ifstream-based;
// engines with their own file APIs should implement FileSource instead).
std::unique_ptr<FileSource> OpenPath(const std::string& utf8Path, std::string* error);

Error LoadSOG(FileSource& source, DecodedAsset& out, std::string* message = nullptr);

// Decode one image to RGBA8 with libwebp, rejecting lossy or animated files.
Error DecodeWebP(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& rgba, int& width, int& height,
                 std::string* message = nullptr);

} // namespace sog
