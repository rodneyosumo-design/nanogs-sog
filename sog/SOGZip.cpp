// SOGZip.cpp — see SOGZip.h. Uses zlib for deflate and CRC-32.
#include "SOGZip.h"

#include <zlib.h>

#include <cstring>

namespace sog::zip {

namespace {

uint16_t Read16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Read32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

constexpr uint32_t kLocalSig = 0x04034b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kEndSig = 0x06054b50;

bool Fail(std::string* error, const std::string& message)
{
    if (error) {
        *error = "zip: " + message;
    }
    return false;
}

std::string BaseName(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

bool LooksLikeZip(const uint8_t* data, size_t size)
{
    return size >= 4 && Read32(data) == kLocalSig;
}

bool Archive::Open(std::vector<uint8_t> bytes, std::string* error)
{
    Data = std::move(bytes);
    EntryList.clear();
    const size_t size = Data.size();
    if (size < 22) {
        return Fail(error, "file too small");
    }
    // The end-of-central-directory record sits in the last 22 + 65535 (comment) bytes.
    size_t eocd = size_t(-1);
    const size_t lowest = size > 22 + 65535 ? size - 22 - 65535 : 0;
    for (size_t pos = size - 22 + 1; pos-- > lowest;) {
        if (Read32(&Data[pos]) == kEndSig) {
            eocd = pos;
            break;
        }
    }
    if (eocd == size_t(-1)) {
        return Fail(error, "end of central directory not found");
    }
    const uint16_t entries = Read16(&Data[eocd + 10]);
    const uint32_t cdSize = Read32(&Data[eocd + 12]);
    const uint32_t cdOffset = Read32(&Data[eocd + 16]);
    if (entries == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu) {
        return Fail(error, "ZIP64 archives are not supported");
    }
    if (size_t(cdOffset) + cdSize > eocd) {
        return Fail(error, "central directory out of range");
    }
    size_t pos = cdOffset;
    for (uint16_t i = 0; i < entries; ++i) {
        if (pos + 46 > eocd || Read32(&Data[pos]) != kCentralSig) {
            return Fail(error, "bad central directory entry");
        }
        const uint16_t flags = Read16(&Data[pos + 8]);
        Entry e;
        e.method = Read16(&Data[pos + 10]);
        e.crc32 = Read32(&Data[pos + 16]);
        e.compressedSize = Read32(&Data[pos + 20]);
        e.uncompressedSize = Read32(&Data[pos + 24]);
        const uint16_t nameLen = Read16(&Data[pos + 28]);
        const uint16_t extraLen = Read16(&Data[pos + 30]);
        const uint16_t commentLen = Read16(&Data[pos + 32]);
        e.localHeaderOffset = Read32(&Data[pos + 42]);
        if (pos + 46 + nameLen > eocd) {
            return Fail(error, "entry name out of range");
        }
        e.name.assign(reinterpret_cast<const char*>(&Data[pos + 46]), nameLen);
        if (flags & 1u) {
            return Fail(error, "encrypted entry " + e.name);
        }
        pos += 46 + size_t(nameLen) + extraLen + commentLen;
        if (!e.name.empty() && e.name.back() != '/') {
            EntryList.push_back(std::move(e));
        }
    }
    return true;
}

const Entry* Archive::Find(const std::string& name) const
{
    for (const Entry& e : EntryList) {
        if (e.name == name) {
            return &e;
        }
    }
    const Entry* match = nullptr;
    for (const Entry& e : EntryList) {
        if (BaseName(e.name) == name) {
            if (match) {
                return nullptr;                                  // ambiguous
            }
            match = &e;
        }
    }
    return match;
}

bool Archive::Extract(const Entry& e, std::vector<uint8_t>& out, std::string* error) const
{
    const size_t p = e.localHeaderOffset;
    if (p + 30 > Data.size() || Read32(&Data[p]) != kLocalSig) {
        return Fail(error, "bad local header for " + e.name);
    }
    const size_t begin = p + 30 + Read16(&Data[p + 26]) + Read16(&Data[p + 28]);
    if (begin + e.compressedSize > Data.size()) {
        return Fail(error, "data out of range for " + e.name);
    }
    const uint8_t* src = Data.data() + begin;
    out.resize(e.uncompressedSize);
    if (e.method == 0) {
        if (e.compressedSize != e.uncompressedSize) {
            return Fail(error, "stored size mismatch for " + e.name);
        }
        if (e.uncompressedSize) {
            std::memcpy(out.data(), src, e.uncompressedSize);
        }
    } else if (e.method == 8) {
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
            return Fail(error, "inflateInit failed");
        }
        zs.next_in = const_cast<Bytef*>(src);
        zs.avail_in = e.compressedSize;
        zs.next_out = out.data();
        zs.avail_out = e.uncompressedSize;
        const int rc = inflate(&zs, Z_FINISH);
        const uLong produced = zs.total_out;
        inflateEnd(&zs);
        if (rc != Z_STREAM_END || produced != e.uncompressedSize) {
            return Fail(error, "inflate failed for " + e.name);
        }
    } else {
        return Fail(error, "unsupported compression method " + std::to_string(e.method) + " for " + e.name);
    }
    const uLong crc = crc32(0L, out.empty() ? Z_NULL : out.data(), uInt(out.size()));
    if (uint32_t(crc) != e.crc32) {
        return Fail(error, "CRC mismatch for " + e.name);
    }
    return true;
}

} // namespace sog::zip
