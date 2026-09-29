// SOGZip.h — read-only zip archive in memory: stored and deflate entries, CRC-checked. No ZIP64
// (SOG bundles are far below 4 GB) and no encryption.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sog::zip {

struct Entry {
    std::string name;
    uint16_t method = 0;                 // 0 = stored, 8 = deflate
    uint32_t crc32 = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint32_t localHeaderOffset = 0;
};

class Archive {
public:
    // Takes ownership of the whole file's bytes.
    bool Open(std::vector<uint8_t> bytes, std::string* error);
    // Exact name first, then a unique match on the file name without directories.
    const Entry* Find(const std::string& name) const;
    bool Extract(const Entry& entry, std::vector<uint8_t>& out, std::string* error) const;
    const std::vector<Entry>& Entries() const { return EntryList; }

private:
    std::vector<uint8_t> Data;
    std::vector<Entry> EntryList;
};

bool LooksLikeZip(const uint8_t* data, size_t size);

} // namespace sog::zip
