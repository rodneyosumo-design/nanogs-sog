// SOGStreamed.h — Streamed SOG (lod-meta.json, version 1): parse the manifest, load its chunk SOGs and merge
// every level into one DecodedAsset with shared codebooks and a single SH palette, plus the LOD tree over the
// merged records. Import-time only: the renderer keeps one 20-byte record per splat and one palette, as for a
// single .sog file.
#pragma once

#include "SOGLoader.h"
#include "SOGTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sog {

// ---- lod-meta.json ----

struct LodRun { uint32_t file = 0, offset = 0, count = 0; };   // count 0: the leaf has no splats at this level

struct LodNode {
    float boundMin[3] = {0, 0, 0};
    float boundMax[3] = {0, 0, 0};
    std::vector<int32_t> children;      // interior node (splat-transform writes two)
    std::vector<LodRun> lods;           // leaf: one run per level, lodLevels entries; empty for interior nodes
    std::vector<float> errors;          // leaf: per-level errors as written, or empty
    bool IsLeaf() const { return !lods.empty(); }
};

struct LodMeta {
    uint32_t version = 0;               // 0: a pre-versioned file
    uint32_t lodLevels = 0;             // level 0 is the finest
    uint64_t count = 0;                 // total splats over all levels; 0 when absent
    std::vector<uint64_t> counts;       // per level; empty when absent
    bool lodErrors = false;             // the writer measured per-leaf errors
    std::string environment;            // optional environment chunk outside the tree (not merged)
    std::vector<std::string> filenames; // chunk meta.json paths relative to the lod-meta.json folder
    std::vector<LodNode> nodes;         // depth first; nodes[0] is the root
};

Error ParseLodMeta(const char* text, size_t length, LodMeta& out, std::string* message = nullptr);

// ---- Merged asset ----

struct StreamedLevel { uint32_t start = 0, count = 0; };      // merged records [start, start + count)

struct StreamedLeaf {
    float boundMin[3] = {0, 0, 0};      // file frame, from lod-meta.json
    float boundMax[3] = {0, 0, 0};
    std::vector<float> errors;          // per level; non-decreasing, 0 at the finest level present
    std::vector<StreamedLevel> levels;  // per level; count 0 where the leaf has no splats
};

struct StreamedCluster {
    uint32_t leaf = 0, level = 0, start = 0, count = 0;       // count <= clusterSize
    float boundMin[3] = {0, 0, 0};      // file frame: splat centres padded by 3 sigma of their largest axis
    float boundMax[3] = {0, 0, 0};
};

struct StreamedAsset {
    // Every level's splats. Records are ordered by level (coarsest first), then leaf (tree order), then
    // Morton order within the leaf, so each (leaf, level) run and the whole coarsest level are contiguous.
    DecodedAsset merged;
    uint32_t lodLevels = 0;
    bool errorsFromFile = false;        // false: derived from splat counts, log(finest / count), as PlayCanvas does
    std::vector<StreamedLeaf> leaves;   // lod-meta.json leaves in tree order
    std::vector<StreamedCluster> clusters;
    std::vector<uint64_t> levelCounts;  // merged splats per level
    uint64_t sourcePaletteEntries = 0;  // chunk palette entries in use before merging
    std::vector<uint64_t> sources;      // with StreamedOptions::keepSources: chunk << 32 | splat, per record
};

struct StreamedOptions {
    uint32_t paletteSize = 65536;       // merged SH palette entries; records keep 16-bit labels
    uint32_t iterations = 8;            // Lloyd iterations for each k-means level
    uint32_t clusterSize = 128;         // splats per cluster
    bool keepSources = false;           // fill StreamedAsset::sources (tests)
    // Runs body(i) for every i in [0, count), possibly concurrently. Null runs serially.
    std::function<void(size_t count, const std::function<void(size_t)>& body)> parallelFor;
    std::function<void(const char* stage)> progress;          // optional
};

// Opens one chunk given its path from filenames (relative to the lod-meta.json folder); must be thread safe
// when options.parallelFor runs concurrently.
using ChunkOpener = std::function<std::unique_ptr<FileSource>(const std::string& path, std::string* error)>;

// Loads every chunk the tree references and merges them:
//  - positions are requantized into the union of the chunks' ranges;
//  - scales and DC colours snap to 256-entry codebooks fitted to all chunks (weighted by use);
//  - the chunks' SH palettes (one per chunk, about one entry per splat) are clustered into one palette of
//    options.paletteSize entries with two-level weighted k-means;
//  - quaternions and opacities are copied unchanged.
Error LoadStreamed(const LodMeta& meta, const ChunkOpener& open, const StreamedOptions& options,
                   StreamedAsset& out, std::string* message = nullptr);

// Standalone convenience: parse <path>/lod-meta.json (or the file itself) and load its chunks from disk.
Error LoadStreamedPath(const std::string& lodMetaPath, const StreamedOptions& options, StreamedAsset& out,
                       std::string* message = nullptr);

} // namespace sog
