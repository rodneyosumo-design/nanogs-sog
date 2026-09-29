// SOGLoader.cpp — see SOGLoader.h.
#include "SOGLoader.h"

#include "SOGCodec.h"
#include "SOGJson.h"
#include "SOGZip.h"

#include "src/webp/decode.h"

#include <cstring>
#include <fstream>

namespace sog {

namespace {

bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return false;
    }
    const std::streamoff size = f.tellg();
    if (size < 0) {
        return false;
    }
    out.resize(size_t(size));
    f.seekg(0);
    return size == 0 || bool(f.read(reinterpret_cast<char*>(out.data()), size));
}

bool IsDirectory(const std::string& path)
{
    std::ifstream probe(path + "/meta.json", std::ios::binary);
    return bool(probe);
}

class ZipSource final : public FileSource {
public:
    zip::Archive Archive;
    bool Read(const std::string& name, std::vector<uint8_t>& out, std::string* error) override
    {
        const zip::Entry* e = Archive.Find(name);
        if (!e) {
            if (error) *error = "missing " + name;
            return false;
        }
        return Archive.Extract(*e, out, error);
    }
};

class FolderSource final : public FileSource {
public:
    explicit FolderSource(std::string dir) : Dir(std::move(dir)) {}
    bool Read(const std::string& name, std::vector<uint8_t>& out, std::string* error) override
    {
        if (name.find("..") != std::string::npos) {
            if (error) *error = "refusing path " + name;
            return false;
        }
        if (!ReadFile(Dir + "/" + name, out)) {
            if (error) *error = "missing " + name;
            return false;
        }
        return true;
    }
private:
    std::string Dir;
};

Error Fail(std::string* message, Error e, const std::string& text)
{
    if (message) {
        *message = text;
    }
    return e;
}

bool GetNumbers(const json::Value* v, size_t count, double* out)
{
    if (!v || !v->IsArray() || v->array.size() != count) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!v->array[i].IsNumber()) {
            return false;
        }
        out[i] = v->array[i].number;
    }
    return true;
}

bool GetCodebook(const json::Value* section, float* out)
{
    double values[256];
    if (!section || !GetNumbers(section->Find("codebook"), 256, values)) {
        return false;
    }
    for (int i = 0; i < 256; ++i) {
        out[i] = float(values[i]);
    }
    return true;
}

bool GetFile(const json::Value* section, size_t index, std::string& out)
{
    const json::Value* files = section ? section->Find("files") : nullptr;
    if (!files || !files->IsArray() || files->array.size() <= index || !files->array[index].IsString()) {
        return false;
    }
    out = files->array[index].string;
    return !out.empty();
}

struct Image {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
};

Error LoadImage(FileSource& src, const std::string& name, Image& img, std::string* message)
{
    std::vector<uint8_t> bytes;
    std::string why;
    if (!src.Read(name, bytes, &why)) {
        return Fail(message, Error::MissingImage, why);
    }
    const Error e = DecodeWebP(bytes, img.rgba, img.width, img.height, &why);
    if (e != Error::Ok) {
        return Fail(message, e, name + ": " + why);
    }
    return Error::Ok;
}

} // namespace

Error DecodeWebP(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& rgba, int& width, int& height,
                 std::string* message)
{
    WebPBitstreamFeatures features;
    if (WebPGetFeatures(bytes.data(), bytes.size(), &features) != VP8_STATUS_OK) {
        return Fail(message, Error::BadImage, "not a WebP image");
    }
    if (features.has_animation) {
        return Fail(message, Error::BadImage, "animated WebP");
    }
    if (features.format != 2) {                                  // 1 = lossy, 0 = mixed/undefined
        return Fail(message, Error::LossyImage, "lossy WebP (SOG requires lossless)");
    }
    uint8_t* pixels = WebPDecodeRGBA(bytes.data(), bytes.size(), &width, &height);
    if (!pixels) {
        return Fail(message, Error::BadImage, "WebP decode failed");
    }
    rgba.assign(pixels, pixels + size_t(width) * size_t(height) * 4);
    WebPFree(pixels);
    return Error::Ok;
}

std::unique_ptr<FileSource> MakeZipSource(std::vector<uint8_t> zipBytes, std::string* error)
{
    auto src = std::make_unique<ZipSource>();
    if (!src->Archive.Open(std::move(zipBytes), error)) {
        return nullptr;
    }
    return src;
}

std::unique_ptr<FileSource> OpenPath(const std::string& path, std::string* error)
{
    const std::string metaSuffix = "meta.json";
    if (path.size() >= metaSuffix.size() && path.compare(path.size() - metaSuffix.size(), metaSuffix.size(), metaSuffix) == 0) {
        const size_t slash = path.find_last_of("/\\");
        return std::make_unique<FolderSource>(slash == std::string::npos ? "." : path.substr(0, slash));
    }
    if (IsDirectory(path)) {
        return std::make_unique<FolderSource>(path);
    }
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes)) {
        if (error) *error = "cannot read " + path;
        return nullptr;
    }
    if (!zip::LooksLikeZip(bytes.data(), bytes.size())) {
        if (error) *error = path + " is not a zip archive or a SOG folder";
        return nullptr;
    }
    return MakeZipSource(std::move(bytes), error);
}

Error LoadSOG(FileSource& src, DecodedAsset& out, std::string* message)
{
    out = DecodedAsset();
    std::vector<uint8_t> metaBytes;
    std::string why;
    if (!src.Read("meta.json", metaBytes, &why)) {
        return Fail(message, Error::MissingMeta, why);
    }
    json::Value meta;
    if (!json::Parse(reinterpret_cast<const char*>(metaBytes.data()), metaBytes.size(), meta, &why) || !meta.IsObject()) {
        return Fail(message, Error::BadMeta, "meta.json: " + (why.empty() ? std::string("not an object") : why));
    }
    const json::Value* version = meta.Find("version");
    if (!version || !version->IsNumber() || version->number != 2.0) {
        return Fail(message, Error::BadVersion, "unsupported SOG version (need 2)");
    }
    const json::Value* count = meta.Find("count");
    if (!count || !count->IsNumber() || count->number < 1.0 || count->number > 2147483647.0 ||
        count->number != double(uint32_t(count->number))) {
        return Fail(message, Error::BadMeta, "meta.json: bad count");
    }
    out.count = uint32_t(count->number);
    if (const json::Value* aa = meta.Find("antialias")) {
        out.antialias = aa->type == json::Value::Type::Bool && aa->boolean;
    }

    const json::Value* means = meta.Find("means");
    const json::Value* scales = meta.Find("scales");
    const json::Value* quats = meta.Find("quats");
    const json::Value* sh0 = meta.Find("sh0");
    const json::Value* shN = meta.Find("shN");
    std::string fMeansL, fMeansU, fScales, fQuats, fSh0, fCentroids, fLabels;
    if (!means || !GetNumbers(means->Find("mins"), 3, out.meanMin) || !GetNumbers(means->Find("maxs"), 3, out.meanMax) ||
        !GetFile(means, 0, fMeansL) || !GetFile(means, 1, fMeansU)) {
        return Fail(message, Error::BadMeta, "meta.json: bad means");
    }
    if (!GetCodebook(scales, out.scaleCodebook) || !GetFile(scales, 0, fScales)) {
        return Fail(message, Error::BadMeta, "meta.json: bad scales");
    }
    if (!GetFile(quats, 0, fQuats)) {
        return Fail(message, Error::BadMeta, "meta.json: bad quats");
    }
    if (!GetCodebook(sh0, out.sh0Codebook) || !GetFile(sh0, 0, fSh0)) {
        return Fail(message, Error::BadMeta, "meta.json: bad sh0");
    }
    if (shN) {
        const json::Value* bands = shN->Find("bands");
        const json::Value* palette = shN->Find("count");
        if (!bands || !bands->IsNumber() || !palette || !palette->IsNumber() ||
            !GetCodebook(shN, out.shNCodebook) || !GetFile(shN, 0, fCentroids) || !GetFile(shN, 1, fLabels)) {
            return Fail(message, Error::BadMeta, "meta.json: bad shN");
        }
        if (bands->number < 1.0 || bands->number > 3.0 || bands->number != double(int(bands->number)) ||
            palette->number < 1.0 || palette->number > 65536.0 || palette->number != double(int(palette->number))) {
            return Fail(message, Error::BadMeta, "meta.json: shN bands must be 1-3 and count 1-65536");
        }
        static const uint32_t kCoeffs[4] = {0, 3, 8, 15};
        out.shBands = uint32_t(bands->number);
        out.shCoeffs = kCoeffs[out.shBands];
        out.paletteCount = uint32_t(palette->number);
    }

    Image meansL, meansU, quatImg, scaleImg, sh0Img, labelImg, centroidImg;
    struct Pending { const std::string* file; Image* img; } perSplat[] = {
        {&fMeansL, &meansL}, {&fMeansU, &meansU}, {&fQuats, &quatImg}, {&fScales, &scaleImg}, {&fSh0, &sh0Img},
        {&fLabels, &labelImg},
    };
    for (const Pending& p : perSplat) {
        if (p.file->empty()) {
            continue;                                                // labels only exist with shN
        }
        const Error e = LoadImage(src, *p.file, *p.img, message);
        if (e != Error::Ok) {
            return e;
        }
        if (uint64_t(p.img->width) * uint64_t(p.img->height) < out.count) {
            return Fail(message, Error::ImageSizeMismatch, *p.file + " has fewer pixels than count");
        }
        if (p.img->width != meansL.width) {
            return Fail(message, Error::ImageSizeMismatch, *p.file + " width differs from " + fMeansL);
        }
    }
    if (out.shCoeffs) {
        const Error e = LoadImage(src, fCentroids, centroidImg, message);
        if (e != Error::Ok) {
            return e;
        }
        if (uint32_t(centroidImg.width) < 64 * out.shCoeffs ||
            uint64_t(centroidImg.height) * 64 < out.paletteCount) {
            return Fail(message, Error::ImageSizeMismatch, fCentroids + " is too small for the palette");
        }
    }

    const uint8_t* ml = meansL.rgba.data();
    const uint8_t* mu = meansU.rgba.data();
    const uint8_t* q = quatImg.rgba.data();
    const uint8_t* s = scaleImg.rgba.data();
    const uint8_t* c = sh0Img.rgba.data();
    const uint8_t* lb = out.shCoeffs ? labelImg.rgba.data() : nullptr;
    out.splats.resize(out.count);
    for (uint32_t i = 0; i < out.count; ++i) {
        const size_t o = size_t(i) * 4;
        if (q[o + 3] < 252) {
            return Fail(message, Error::BadQuatMode, "quats alpha outside 252..255 at splat " + std::to_string(i));
        }
        if (lb && uint32_t(lb[o] | (lb[o + 1] << 8)) >= out.paletteCount) {
            return Fail(message, Error::LabelOutOfRange, "shN label out of range at splat " + std::to_string(i));
        }
        out.splats[i] = PackSplat(ml + o, mu + o, q + o, s + o, c + o, lb ? lb + o : nullptr);
    }

    if (out.shCoeffs) {
        out.paletteIndices.resize(size_t(out.paletteCount) * out.shCoeffs * 3);
        const size_t rowBytes = size_t(centroidImg.width) * 4;
        for (uint32_t e = 0; e < out.paletteCount; ++e) {
            const uint8_t* row = centroidImg.rgba.data() + size_t(e / 64) * rowBytes;
            for (uint32_t k = 0; k < out.shCoeffs; ++k) {
                const uint8_t* px = row + (size_t(e % 64) * out.shCoeffs + k) * 4;
                uint8_t* dst = &out.paletteIndices[(size_t(e) * out.shCoeffs + k) * 3];
                dst[0] = px[0];
                dst[1] = px[1];
                dst[2] = px[2];
            }
        }
    }
    BuildGpuTables(out);
    return Error::Ok;
}

} // namespace sog
