// SOGStreamed.cpp — see SOGStreamed.h.
#include "SOGStreamed.h"

#include "SOGCodec.h"
#include "SOGJson.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <numeric>

namespace sog {

namespace {

using ParallelFor = std::function<void(size_t, const std::function<void(size_t)>&)>;

Error Fail(std::string* message, Error e, const std::string& text)
{
    if (message) {
        *message = text;
    }
    return e;
}

void Run(const ParallelFor& parallelFor, size_t count, const std::function<void(size_t)>& body)
{
    if (parallelFor && count > 1) {
        parallelFor(count, body);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        body(i);
    }
}

// Deterministic across platforms (std:: distributions are not).
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    uint64_t Next()
    {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double Uniform() { return double(Next() >> 11) * (1.0 / 9007199254740992.0); }   // [0, 1)
};

double Unlog(double n)
{
    return n < 0.0 ? -std::expm1(-n) : std::expm1(n);
}

uint32_t Round16(double v)
{
    const double r = std::floor(v + 0.5);
    if (!(r > 0.0)) {
        return 0;
    }
    return r >= 65535.0 ? 65535u : uint32_t(r);
}

// ---- lod-meta.json ----

bool GetUInt(const json::Value* v, double limit, uint64_t& out)
{
    if (!v || !v->IsNumber() || !(v->number >= 0.0) || v->number > limit || v->number != std::floor(v->number)) {
        return false;
    }
    out = uint64_t(v->number);
    return true;
}

bool GetVec3(const json::Value* v, float out[3])
{
    if (!v || !v->IsArray() || v->array.size() != 3) {
        return false;
    }
    for (int k = 0; k < 3; ++k) {
        if (!v->array[k].IsNumber() || !std::isfinite(v->array[k].number)) {
            return false;
        }
        out[k] = float(v->array[k].number);
    }
    return true;
}

bool SafeRelativePath(const std::string& p)
{
    if (p.empty() || p[0] == '/' || p[0] == '\\' || p.find(':') != std::string::npos) {
        return false;
    }
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find_first_of("/\\", start);
        if (end == std::string::npos) {
            end = p.size();
        }
        if (p.compare(start, end - start, "..") == 0 && end - start == 2) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

struct TreeParser {
    LodMeta& meta;
    std::string error;

    int32_t Parse(const json::Value& v, int depth)
    {
        const std::string where = "tree node " + std::to_string(meta.nodes.size());
        if (depth > 64) {
            error = "tree deeper than 64 levels";
            return -1;
        }
        if (!v.IsObject()) {
            error = where + " is not an object";
            return -1;
        }
        const int32_t index = int32_t(meta.nodes.size());
        meta.nodes.emplace_back();
        LodNode node;
        const json::Value* bound = v.Find("bound");
        if (!bound || !GetVec3(bound->Find("min"), node.boundMin) || !GetVec3(bound->Find("max"), node.boundMax)) {
            error = where + ": bad bound";
            return -1;
        }
        const json::Value* lods = v.Find("lods");
        const json::Value* children = v.Find("children");
        if (lods && lods->type != json::Value::Type::Null) {
            if (!lods->IsObject()) {
                error = where + ": lods is not an object";
                return -1;
            }
            node.lods.resize(meta.lodLevels);
            for (const auto& item : lods->object) {
                char* end = nullptr;
                const long level = std::strtol(item.first.c_str(), &end, 10);
                if (item.first.empty() || *end != '\0' || level < 0 || level >= long(meta.lodLevels)) {
                    error = where + ": bad level \"" + item.first + "\"";
                    return -1;
                }
                const json::Value& run = item.second;
                uint64_t file = 0, offset = 0, count = 0;
                if (!run.IsObject() || !GetUInt(run.Find("file"), 4294967295.0, file) ||
                    (run.Find("offset") && !GetUInt(run.Find("offset"), 4294967295.0, offset)) ||
                    (run.Find("count") && !GetUInt(run.Find("count"), 4294967295.0, count))) {
                    error = where + ": bad run for level " + item.first;
                    return -1;
                }
                if (file >= meta.filenames.size()) {
                    error = where + ": file index " + std::to_string(file) + " out of range";
                    return -1;
                }
                node.lods[size_t(level)] = { uint32_t(file), uint32_t(offset), uint32_t(count) };
            }
            if (const json::Value* errors = v.Find("errors")) {
                if (!errors->IsArray() || errors->array.size() != meta.lodLevels) {
                    error = where + ": errors must have lodLevels entries";
                    return -1;
                }
                for (const json::Value& e : errors->array) {
                    if (!e.IsNumber() || !std::isfinite(e.number) || e.number < 0.0) {
                        error = where + ": errors must be finite and non-negative";
                        return -1;
                    }
                    node.errors.push_back(float(e.number));
                }
            }
            if (node.lods.empty()) {                         // lodLevels >= 1, so only reachable with no levels
                error = where + ": leaf without levels";
                return -1;
            }
        } else if (children) {
            if (!children->IsArray() || children->array.empty()) {
                error = where + ": children must be a non-empty array";
                return -1;
            }
            for (const json::Value& child : children->array) {
                const int32_t c = Parse(child, depth + 1);
                if (c < 0) {
                    return -1;
                }
                node.children.push_back(c);
            }
        } else {
            error = where + " has neither lods nor children";
            return -1;
        }
        meta.nodes[size_t(index)] = std::move(node);
        return index;
    }
};

// ---- Clustering ----

constexpr size_t kLanes = 8;          // row stride multiple; lets the dot product vectorize without fast-math

// Rows of D codes into one float codebook, stored with stride S (a multiple of kLanes); values past D decode as 0.
struct Rows {
    const uint8_t* codes = nullptr;   // n * S
    const float* weight = nullptr;    // n
    const float* codebook = nullptr;  // 256 values
    size_t S = 0;
    size_t D = 0;                     // meaningful values per row

    void Decode(uint32_t row, float* out) const
    {
        const uint8_t* c = codes + size_t(row) * S;
        size_t s = 0;
        for (; s < D; ++s) {
            out[s] = codebook[c[s]];
        }
        for (; s < S; ++s) {
            out[s] = 0.0f;
        }
    }
};

inline float Dot(const float* a, const float* b, size_t S)
{
    float acc[kLanes] = {};
    for (size_t s = 0; s < S; s += kLanes) {
        for (size_t l = 0; l < kLanes; ++l) {
            acc[l] += a[s + l] * b[s + l];
        }
    }
    return ((acc[0] + acc[1]) + (acc[2] + acc[3])) + ((acc[4] + acc[5]) + (acc[6] + acc[7]));
}

// Nearest centroid by |c|^2 - 2 x.c; returns the squared distance.
inline uint32_t Nearest(const float* x, float xNorm, const float* C, const float* cNorm, uint32_t K, size_t S, float& dist)
{
    float best = std::numeric_limits<float>::max();
    uint32_t bestK = 0;
    for (uint32_t k = 0; k < K; ++k) {
        const float d = cNorm[k] - 2.0f * Dot(x, C + size_t(k) * S, S);
        if (d < best) {
            best = d;
            bestK = k;
        }
    }
    dist = std::max(0.0f, best + xNorm);
    return bestK;
}

constexpr size_t kBlock = 2048;       // rows per parallel work item
constexpr uint32_t kSamplePerCentroid = 64;

// Weighted k-means over rows `ids`: trains on at most kSamplePerCentroid * K rows (k-means++ then Lloyd), assigns
// every row, and finishes with one weighted-mean update over all rows. K may shrink when rows run out.
void KMeans(const Rows& X, const std::vector<uint32_t>& ids, uint32_t K, uint32_t iterations, uint64_t seed,
            const ParallelFor& parallelFor, std::vector<float>& C, std::vector<uint32_t>& label)
{
    const size_t n = ids.size();
    const size_t S = X.S;
    K = uint32_t(std::min<size_t>(K, n));
    C.clear();
    label.assign(n, 0);
    if (K == 0) {
        return;
    }
    std::vector<float> row(S);
    if (K == n) {
        C.resize(size_t(K) * S);
        for (size_t i = 0; i < n; ++i) {
            X.Decode(ids[i], &C[i * S]);
            label[i] = uint32_t(i);
        }
        return;
    }

    // Training sample: uniform without replacement (weights still apply)
    Rng rng(seed);
    std::vector<uint32_t> train = ids;
    const size_t m = std::min<size_t>(n, size_t(K) * kSamplePerCentroid);
    for (size_t i = 0; i < m; ++i) {
        const size_t j = i + size_t(rng.Next() % uint64_t(n - i));
        std::swap(train[i], train[j]);
    }
    train.resize(m);
    std::vector<float> T(m * S), tNorm(m), tWeight(m);
    for (size_t i = 0; i < m; ++i) {
        X.Decode(train[i], &T[i * S]);
        tNorm[i] = Dot(&T[i * S], &T[i * S], S);
        tWeight[i] = X.weight[train[i]];
    }

    // k-means++ (weighted D^2 sampling)
    C.assign(size_t(K) * S, 0.0f);
    std::vector<float> d2(m, std::numeric_limits<float>::max());
    auto pick = [&](const std::vector<double>& mass, double total) {
        double r = rng.Uniform() * total;
        for (size_t i = 0; i < m; ++i) {
            r -= mass[i];
            if (r < 0.0) {
                return i;
            }
        }
        return m - 1;
    };
    std::vector<double> mass(m);
    double total = 0.0;
    for (size_t i = 0; i < m; ++i) {
        mass[i] = double(tWeight[i]);
        total += mass[i];
    }
    uint32_t k = 0;
    std::copy_n(&T[pick(mass, total) * S], S, &C[0]);
    for (k = 1; k < K; ++k) {
        const float* last = &C[size_t(k - 1) * S];
        const float lastNorm = Dot(last, last, S);
        total = 0.0;
        for (size_t i = 0; i < m; ++i) {
            const float d = std::max(0.0f, tNorm[i] + lastNorm - 2.0f * Dot(&T[i * S], last, S));
            d2[i] = std::min(d2[i], d);
            mass[i] = double(tWeight[i]) * double(d2[i]);
            total += mass[i];
        }
        if (!(total > 0.0)) {
            break;                                           // every sample sits on a centroid
        }
        std::copy_n(&T[pick(mass, total) * S], S, &C[size_t(k) * S]);
    }
    K = k;
    C.resize(size_t(K) * S);

    std::vector<float> cNorm(K);
    auto updateNorms = [&]() {
        for (uint32_t c = 0; c < K; ++c) {
            cNorm[c] = Dot(&C[size_t(c) * S], &C[size_t(c) * S], S);
        }
    };
    std::vector<double> sum(size_t(K) * S);
    std::vector<double> wsum(K);
    std::vector<uint32_t> tLabel(m);
    std::vector<float> tDist(m);
    for (uint32_t it = 0; it < iterations; ++it) {
        updateNorms();
        for (size_t i = 0; i < m; ++i) {
            tLabel[i] = Nearest(&T[i * S], tNorm[i], C.data(), cNorm.data(), K, S, tDist[i]);
        }
        std::fill(sum.begin(), sum.end(), 0.0);
        std::fill(wsum.begin(), wsum.end(), 0.0);
        for (size_t i = 0; i < m; ++i) {
            const double w = tWeight[i];
            double* acc = &sum[size_t(tLabel[i]) * S];
            const float* x = &T[i * S];
            for (size_t s = 0; s < S; ++s) {
                acc[s] += w * x[s];
            }
            wsum[tLabel[i]] += w;
        }
        for (uint32_t c = 0; c < K; ++c) {
            if (wsum[c] > 0.0) {
                for (size_t s = 0; s < S; ++s) {
                    C[size_t(c) * S + s] = float(sum[size_t(c) * S + s] / wsum[c]);
                }
            } else {
                // Empty centroid: move it to the sample farthest (weighted) from its own centroid
                size_t far = 0;
                for (size_t i = 1; i < m; ++i) {
                    if (tWeight[i] * tDist[i] > tWeight[far] * tDist[far]) {
                        far = i;
                    }
                }
                std::copy_n(&T[far * S], S, &C[size_t(c) * S]);
                tDist[far] = 0.0f;
            }
        }
    }

    // Assign every row, then move each centroid to the weighted mean of all its rows
    updateNorms();
    const size_t blocks = (n + kBlock - 1) / kBlock;
    Run(parallelFor, blocks, [&](size_t b) {
        std::vector<float> x(S);
        float dist;
        for (size_t i = b * kBlock, e = std::min(n, (b + 1) * kBlock); i < e; ++i) {
            X.Decode(ids[i], x.data());
            label[i] = Nearest(x.data(), Dot(x.data(), x.data(), S), C.data(), cNorm.data(), K, S, dist);
        }
    });
    std::fill(sum.begin(), sum.end(), 0.0);
    std::fill(wsum.begin(), wsum.end(), 0.0);
    for (size_t i = 0; i < n; ++i) {
        X.Decode(ids[i], row.data());
        const double w = X.weight[ids[i]];
        double* acc = &sum[size_t(label[i]) * S];
        for (size_t s = 0; s < S; ++s) {
            acc[s] += w * row[s];
        }
        wsum[label[i]] += w;
    }
    for (uint32_t c = 0; c < K; ++c) {
        if (wsum[c] > 0.0) {
            for (size_t s = 0; s < S; ++s) {
                C[size_t(c) * S + s] = float(sum[size_t(c) * S + s] / wsum[c]);
            }
        }
    }
}

// Largest-remainder split of `total` centroids by weight, at least one per non-empty group and at most its size.
std::vector<uint32_t> Apportion(uint32_t total, const std::vector<double>& weight, const std::vector<size_t>& size)
{
    const size_t g = weight.size();
    std::vector<uint32_t> out(g, 0);
    const double wTotal = std::accumulate(weight.begin(), weight.end(), 0.0);
    std::vector<std::pair<double, size_t>> remainder;
    uint64_t used = 0;
    for (size_t i = 0; i < g; ++i) {
        if (size[i] == 0) {
            continue;
        }
        const double quota = wTotal > 0.0 ? double(total) * weight[i] / wTotal : double(total) / double(g);
        uint32_t a = std::max<uint32_t>(1, uint32_t(std::floor(quota)));
        a = uint32_t(std::min<size_t>(a, size[i]));
        out[i] = a;
        used += a;
        remainder.emplace_back(quota - std::floor(quota), i);
    }
    std::sort(remainder.begin(), remainder.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    // Hand out what's left by remainder, then round-robin to groups with room
    for (bool progress = true; used < total && progress;) {
        progress = false;
        for (const auto& r : remainder) {
            if (used >= total) {
                break;
            }
            if (out[r.second] < size[r.second]) {
                ++out[r.second];
                ++used;
                progress = true;
            }
        }
    }
    // The one-per-group minimum can overshoot: take back from the largest shares
    while (used > total) {
        const size_t j = size_t(std::max_element(out.begin(), out.end()) - out.begin());
        if (out[j] <= 1) {
            break;
        }
        --out[j];
        --used;
    }
    return out;
}

// Hierarchical weighted k-means: K centroids for rows `ids`. At most kLeafK centroids are fitted directly;
// larger K first splits the rows into up to kLeafK groups and shares K among them by weight.
constexpr uint32_t kLeafK = 256;

void ClusterRows(const Rows& X, const std::vector<uint32_t>& ids, uint32_t K, uint32_t iterations, uint64_t seed,
                 const ParallelFor& parallelFor, std::vector<float>& C, std::vector<uint32_t>& label)
{
    if (K <= kLeafK || ids.size() <= K) {
        KMeans(X, ids, K, iterations, seed, parallelFor, C, label);
        return;
    }
    const uint32_t groups = std::min(kLeafK, (K + kLeafK - 1) / kLeafK);
    std::vector<float> coarse;
    std::vector<uint32_t> coarseLabel;
    KMeans(X, ids, groups, iterations, seed, parallelFor, coarse, coarseLabel);
    const size_t G = coarse.size() / X.S;
    std::vector<std::vector<uint32_t>> members(G);
    std::vector<std::vector<uint32_t>> memberPos(G);
    std::vector<double> weight(G, 0.0);
    for (size_t i = 0; i < ids.size(); ++i) {
        members[coarseLabel[i]].push_back(ids[i]);
        memberPos[coarseLabel[i]].push_back(uint32_t(i));
        weight[coarseLabel[i]] += X.weight[ids[i]];
    }
    std::vector<size_t> sizes(G);
    for (size_t g = 0; g < G; ++g) {
        sizes[g] = members[g].size();
    }
    const std::vector<uint32_t> share = Apportion(K, weight, sizes);
    std::vector<std::vector<float>> subC(G);
    std::vector<std::vector<uint32_t>> subLabel(G);
    Run(parallelFor, G, [&](size_t g) {
        if (share[g] > 0) {
            ClusterRows(X, members[g], share[g], iterations, seed * 7919 + g + 1, nullptr, subC[g], subLabel[g]);
        }
    });
    C.clear();
    label.assign(ids.size(), 0);
    uint32_t base = 0;
    for (size_t g = 0; g < G; ++g) {
        C.insert(C.end(), subC[g].begin(), subC[g].end());
        for (size_t j = 0; j < memberPos[g].size(); ++j) {
            label[memberPos[g][j]] = base + subLabel[g][j];
        }
        base += uint32_t(subC[g].size() / X.S);
    }
}

// Weighted 1-D k-means into exactly 256 sorted values (repeats when there are fewer distinct values).
void Codebook256(std::vector<std::pair<float, double>> values, uint32_t iterations, float out[256])
{
    std::sort(values.begin(), values.end());
    // Merge equal values
    std::vector<std::pair<float, double>> v;
    for (const auto& p : values) {
        if (p.second <= 0.0) {
            continue;
        }
        if (!v.empty() && v.back().first == p.first) {
            v.back().second += p.second;
        } else {
            v.push_back(p);
        }
    }
    if (v.empty()) {
        std::fill(out, out + 256, 0.0f);
        return;
    }
    if (v.size() <= 256) {
        for (size_t i = 0; i < 256; ++i) {
            out[i] = v[std::min(i, v.size() - 1)].first;
        }
        return;
    }
    // Initialize at quantiles of weight^(1/3) (MSE-optimal codebooks space entries with density^(1/3), so the tails
    // keep entries), then run Lloyd to convergence with a sorted sweep
    double total = 0.0;
    for (const auto& p : v) {
        total += std::cbrt(p.second);
    }
    std::vector<double> c(256);
    {
        double acc = 0.0;
        size_t j = 0;
        for (size_t i = 0; i < v.size() && j < 256; ++i) {
            acc += std::cbrt(v[i].second);
            while (j < 256 && acc >= total * double(j) / 255.0) {
                c[j++] = v[i].first;
            }
        }
        for (; j < 256; ++j) {
            c[j] = v.back().first;
        }
    }
    std::vector<double> sum(256), wsum(256);
    for (uint32_t it = 0; it < iterations * 32; ++it) {
        std::fill(sum.begin(), sum.end(), 0.0);
        std::fill(wsum.begin(), wsum.end(), 0.0);
        size_t k = 0;
        for (const auto& p : v) {
            while (k + 1 < 256 && std::fabs(double(p.first) - c[k + 1]) <= std::fabs(double(p.first) - c[k])) {
                ++k;
            }
            sum[k] += p.second * double(p.first);
            wsum[k] += p.second;
        }
        bool moved = false;
        for (size_t j = 0; j < 256; ++j) {
            if (wsum[j] > 0.0) {
                const double next = sum[j] / wsum[j];
                moved = moved || next != c[j];
                c[j] = next;
            }
        }
        std::sort(c.begin(), c.end());
        if (!moved) {
            break;
        }
    }
    for (size_t i = 0; i < 256; ++i) {
        out[i] = float(c[i]);
    }
}

uint8_t NearestIn(const float sorted[256], float v)
{
    const float* it = std::lower_bound(sorted, sorted + 256, v);
    const int hi = int(it - sorted);
    if (hi <= 0) {
        return 0;
    }
    if (hi >= 256) {
        return 255;
    }
    return (v - sorted[hi - 1] <= sorted[hi] - v) ? uint8_t(hi - 1) : uint8_t(hi);
}

uint32_t Morton10(uint32_t v)
{
    v &= 0x3FFu;
    v = (v | (v << 16)) & 0x030000FFu;
    v = (v | (v << 8)) & 0x0300F00Fu;
    v = (v | (v << 4)) & 0x030C30C3u;
    v = (v | (v << 2)) & 0x09249249u;
    return v;
}

} // namespace

Error ParseLodMeta(const char* text, size_t length, LodMeta& out, std::string* message)
{
    out = LodMeta();
    json::Value root;
    std::string why;
    if (!json::Parse(text, length, root, &why) || !root.IsObject()) {
        return Fail(message, Error::BadLodMeta, "lod-meta.json: " + (why.empty() ? std::string("not an object") : why));
    }
    uint64_t value = 0;
    if (const json::Value* v = root.Find("version")) {
        if (!GetUInt(v, 1e6, value) || value != 1) {
            return Fail(message, Error::BadLodMeta, "lod-meta.json: unsupported version (need 1)");
        }
        out.version = 1;
    }
    if (!GetUInt(root.Find("lodLevels"), 32.0, value) || value < 1) {
        return Fail(message, Error::BadLodMeta, "lod-meta.json: lodLevels must be 1-32");
    }
    out.lodLevels = uint32_t(value);
    if (const json::Value* v = root.Find("count")) {
        if (!GetUInt(v, 9007199254740992.0, out.count)) {
            return Fail(message, Error::BadLodMeta, "lod-meta.json: bad count");
        }
    }
    if (const json::Value* v = root.Find("counts")) {
        if (!v->IsArray() || v->array.size() != out.lodLevels) {
            return Fail(message, Error::BadLodMeta, "lod-meta.json: counts must have lodLevels entries");
        }
        for (const json::Value& c : v->array) {
            if (!GetUInt(&c, 9007199254740992.0, value)) {
                return Fail(message, Error::BadLodMeta, "lod-meta.json: bad counts");
            }
            out.counts.push_back(value);
        }
    }
    if (const json::Value* v = root.Find("lodErrors")) {
        out.lodErrors = v->type == json::Value::Type::Bool && v->boolean;
    }
    if (const json::Value* v = root.Find("environment")) {
        if (!v->IsString() || !SafeRelativePath(v->string)) {
            return Fail(message, Error::BadLodMeta, "lod-meta.json: bad environment path");
        }
        out.environment = v->string;
    }
    const json::Value* files = root.Find("filenames");
    if (!files || !files->IsArray() || files->array.empty()) {
        return Fail(message, Error::BadLodMeta, "lod-meta.json: filenames must be a non-empty array");
    }
    for (const json::Value& f : files->array) {
        if (!f.IsString() || !SafeRelativePath(f.string)) {
            return Fail(message, Error::BadLodMeta, "lod-meta.json: bad chunk path in filenames");
        }
        out.filenames.push_back(f.string);
    }
    const json::Value* tree = root.Find("tree");
    if (!tree) {
        return Fail(message, Error::BadLodMeta, "lod-meta.json: missing tree");
    }
    TreeParser parser{out, {}};
    if (parser.Parse(*tree, 0) < 0) {
        return Fail(message, Error::BadLodMeta, "lod-meta.json: " + parser.error);
    }
    return Error::Ok;
}

Error LoadStreamed(const LodMeta& meta, const ChunkOpener& open, const StreamedOptions& options,
                   StreamedAsset& out, std::string* message)
{
    out = StreamedAsset();
    const uint32_t L = meta.lodLevels;
    const ParallelFor& parallelFor = options.parallelFor;
    auto progress = [&](const char* stage) {
        if (options.progress) {
            options.progress(stage);
        }
    };
    if (L == 0 || meta.nodes.empty()) {
        return Fail(message, Error::BadLodMeta, "lod-meta.json: empty tree");
    }
    if (options.paletteSize < 1 || options.paletteSize > 65536 || options.clusterSize < 1) {
        return Fail(message, Error::BadLodMeta, "bad merge options");
    }

    // Leaves with at least one splat, in tree order
    std::vector<const LodNode*> leaves;
    std::vector<uint8_t> used(meta.filenames.size(), 0);
    for (const LodNode& node : meta.nodes) {
        if (!node.IsLeaf()) {
            continue;
        }
        bool any = false;
        for (const LodRun& run : node.lods) {
            if (run.count > 0) {
                used[run.file] = 1;
                any = true;
            }
        }
        if (any) {
            leaves.push_back(&node);
        }
    }
    if (leaves.empty()) {
        return Fail(message, Error::BadLodMeta, "lod-meta.json: no leaf has splats");
    }

    // Load the referenced chunks
    progress("Loading chunks");
    struct Chunk {
        DecodedAsset asset;
        Error code = Error::Ok;
        std::string error;
    };
    std::vector<Chunk> chunks(meta.filenames.size());
    Run(parallelFor, chunks.size(), [&](size_t i) {
        if (!used[i]) {
            return;
        }
        Chunk& c = chunks[i];
        std::string why;
        std::unique_ptr<FileSource> source = open(meta.filenames[i], &why);
        if (!source) {
            c.code = Error::BadChunk;
            c.error = meta.filenames[i] + ": " + why;
            return;
        }
        const Error e = LoadSOG(*source, c.asset, &why);
        if (e != Error::Ok) {
            c.code = Error::BadChunk;
            c.error = meta.filenames[i] + ": " + ErrorName(e) + (why.empty() ? "" : " (" + why + ")");
        }
        std::vector<uint16_t>().swap(c.asset.paletteHalf4);   // only the palette indices are needed
    });
    for (const Chunk& c : chunks) {
        if (c.code != Error::Ok) {
            return Fail(message, c.code, c.error);
        }
    }

    // Validate runs; count what each chunk's codebook entries and palette entries are used for
    uint64_t total = 0;
    uint32_t shCoeffs = 0;
    bool antialias = true;
    struct Usage {
        std::vector<double> scale, dc;
        std::vector<float> label;
    };
    std::vector<Usage> usage(chunks.size());
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (!used[i]) {
            continue;
        }
        usage[i].scale.assign(256, 0.0);
        usage[i].dc.assign(256, 0.0);
        usage[i].label.assign(chunks[i].asset.paletteCount, 0.0f);
        shCoeffs = std::max(shCoeffs, chunks[i].asset.shCoeffs);
        antialias = antialias && chunks[i].asset.antialias;
    }
    for (const LodNode* leaf : leaves) {
        for (uint32_t level = 0; level < L; ++level) {
            const LodRun& run = leaf->lods[level];
            if (run.count == 0) {
                continue;
            }
            const DecodedAsset& a = chunks[run.file].asset;
            if (uint64_t(run.offset) + run.count > a.count) {
                return Fail(message, Error::BadChunk, meta.filenames[run.file] + ": a run ends past the chunk's " +
                            std::to_string(a.count) + " splats");
            }
            total += run.count;
            Usage& u = usage[run.file];
            for (uint32_t i = run.offset; i < run.offset + run.count; ++i) {
                const PackedSplat& s = a.splats[i];
                for (int k = 0; k < 3; ++k) {
                    u.scale[(s.scaleOpacity >> (8 * k)) & 0xFFu] += 1.0;
                    u.dc[(s.dc >> (8 * k)) & 0xFFu] += 1.0;
                }
                if (a.shCoeffs > 0 && !((s.dc >> 24) & kFlagNoSH)) {
                    u.label[s.meanZ_label >> 16] += 1.0f;
                }
            }
        }
    }
    if (total >= (uint64_t(1) << 31)) {
        return Fail(message, Error::BadLodMeta, "lod-meta.json: more than 2^31 splats");
    }

    DecodedAsset& m = out.merged;
    m.count = uint32_t(total);
    m.antialias = antialias;

    // Shared codebooks for scale and DC, fitted to the chunks' entries weighted by use
    progress("Fitting codebooks");
    {
        std::vector<std::pair<float, double>> scales, dcs;
        for (size_t i = 0; i < chunks.size(); ++i) {
            if (!used[i]) {
                continue;
            }
            for (int e = 0; e < 256; ++e) {
                // Scales: weight by linear size too, so large (visible) splats keep precise codebook entries
                const float logScale = chunks[i].asset.scaleCodebook[e];
                scales.emplace_back(logScale, usage[i].scale[size_t(e)] * std::exp(double(logScale)));
                dcs.emplace_back(chunks[i].asset.sh0Codebook[e], usage[i].dc[size_t(e)]);
            }
        }
        Codebook256(std::move(scales), options.iterations, m.scaleCodebook);
        Codebook256(std::move(dcs), options.iterations, m.sh0Codebook);
    }
    std::vector<std::array<uint8_t, 256>> scaleMap(chunks.size()), dcMap(chunks.size());
    for (size_t i = 0; i < chunks.size(); ++i) {
        for (int e = 0; e < 256 && used[i]; ++e) {
            scaleMap[i][size_t(e)] = NearestIn(m.scaleCodebook, chunks[i].asset.scaleCodebook[e]);
            dcMap[i][size_t(e)] = NearestIn(m.sh0Codebook, chunks[i].asset.sh0Codebook[e]);
        }
    }

    // One SH palette for all chunks
    std::vector<std::vector<uint32_t>> labelMap(chunks.size());   // chunk palette entry -> merged label
    if (shCoeffs > 0) {
        progress("Merging SH palettes");
        static const uint32_t kBands[16] = {0, 0, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 3};
        m.shCoeffs = shCoeffs;
        m.shBands = kBands[shCoeffs];
        const size_t D = size_t(shCoeffs) * 3;
        const size_t S = (D + kLanes - 1) / kLanes * kLanes;

        // Chunk AC codebooks -> one input codebook (so rows stay one byte per value)
        float input[256];
        {
            std::vector<std::pair<float, double>> values;
            for (size_t i = 0; i < chunks.size(); ++i) {
                const DecodedAsset& a = chunks[i].asset;
                if (!used[i] || a.shCoeffs == 0) {
                    continue;
                }
                std::vector<double> codeUse(256, 0.0);
                for (uint32_t e = 0; e < a.paletteCount; ++e) {
                    const float w = usage[i].label[e];
                    if (w > 0.0f) {
                        const uint8_t* idx = &a.paletteIndices[size_t(e) * a.shCoeffs * 3];
                        for (size_t v = 0; v < size_t(a.shCoeffs) * 3; ++v) {
                            codeUse[idx[v]] += w;
                        }
                    }
                }
                for (int e = 0; e < 256; ++e) {
                    values.emplace_back(a.shNCodebook[e], codeUse[size_t(e)]);
                }
            }
            Codebook256(std::move(values), options.iterations, input);
        }

        // Rows: every chunk palette entry in use; padding codes decode through Rows::D, never the codebook
        std::vector<uint8_t> codes;
        std::vector<float> weights;
        for (size_t i = 0; i < chunks.size(); ++i) {
            const DecodedAsset& a = chunks[i].asset;
            if (!used[i] || a.shCoeffs == 0) {
                continue;
            }
            uint8_t remap[256];
            for (int e = 0; e < 256; ++e) {
                remap[e] = NearestIn(input, a.shNCodebook[e]);
            }
            labelMap[i].assign(a.paletteCount, 0);
            for (uint32_t e = 0; e < a.paletteCount; ++e) {
                const float w = usage[i].label[e];
                if (w <= 0.0f) {
                    continue;
                }
                labelMap[i][e] = uint32_t(weights.size());
                const uint8_t* idx = &a.paletteIndices[size_t(e) * a.shCoeffs * 3];
                const size_t base = codes.size();
                codes.resize(base + S, 0);
                for (size_t v = 0; v < size_t(a.shCoeffs) * 3; ++v) {
                    codes[base + v] = remap[idx[v]];
                }
                // Fewer bands than the merged palette: the missing coefficients are zero. Zero isn't a code,
                // so give those values the code nearest 0 (exact when the codebook contains 0).
                const uint8_t zero = NearestIn(input, 0.0f);
                for (size_t v = size_t(a.shCoeffs) * 3; v < D; ++v) {
                    codes[base + v] = zero;
                }
                weights.push_back(w);
            }
            std::vector<uint8_t>().swap(chunks[i].asset.paletteIndices);
        }
        out.sourcePaletteEntries = weights.size();

        Rows rows;
        rows.codes = codes.data();
        rows.weight = weights.data();
        rows.codebook = input;
        rows.S = S;
        rows.D = D;
        std::vector<uint32_t> ids(weights.size());
        std::iota(ids.begin(), ids.end(), 0u);
        std::vector<float> centroids;
        std::vector<uint32_t> rowLabel;
        ClusterRows(rows, ids, options.paletteSize, options.iterations, 1, parallelFor, centroids, rowLabel);
        const uint32_t K = uint32_t(centroids.size() / S);
        for (size_t i = 0; i < chunks.size(); ++i) {
            for (uint32_t& l : labelMap[i]) {
                l = rowLabel[l];                             // unused entries map to 0 and are never read
            }
        }

        // Quantize the merged palette with its own AC codebook
        std::vector<double> centroidWeight(K, 0.0);
        for (size_t r = 0; r < rowLabel.size(); ++r) {
            centroidWeight[rowLabel[r]] += weights[r];
        }
        std::vector<std::pair<float, double>> values;
        values.reserve(size_t(K) * D);
        for (uint32_t k = 0; k < K; ++k) {
            for (size_t v = 0; v < D; ++v) {
                values.emplace_back(centroids[size_t(k) * S + v], centroidWeight[k]);
            }
        }
        Codebook256(std::move(values), options.iterations, m.shNCodebook);
        m.paletteCount = K;
        m.paletteIndices.resize(size_t(K) * D);
        for (uint32_t k = 0; k < K; ++k) {
            for (size_t v = 0; v < D; ++v) {
                m.paletteIndices[size_t(k) * D + v] = NearestIn(m.shNCodebook, centroids[size_t(k) * S + v]);
            }
        }
    }

    // Merged position range: the union of the chunks' (log-domain) ranges
    for (int k = 0; k < 3; ++k) {
        m.meanMin[k] = std::numeric_limits<double>::max();
        m.meanMax[k] = -std::numeric_limits<double>::max();
    }
    for (size_t i = 0; i < chunks.size(); ++i) {
        for (int k = 0; k < 3 && used[i]; ++k) {
            m.meanMin[k] = std::min(m.meanMin[k], chunks[i].asset.meanMin[k]);
            m.meanMax[k] = std::max(m.meanMax[k], chunks[i].asset.meanMax[k]);
        }
    }

    // Records: coarsest level first, then leaves in tree order, Morton order inside each run
    progress("Building records");
    m.splats.resize(m.count);
    if (options.keepSources) {
        out.sources.resize(m.count);
    }
    out.lodLevels = L;
    out.levelCounts.assign(L, 0);
    out.leaves.resize(leaves.size());
    struct Job { uint32_t leaf, level, start; };
    std::vector<Job> jobs;
    uint32_t cursor = 0;
    for (uint32_t li = L; li-- > 0;) {
        for (size_t f = 0; f < leaves.size(); ++f) {
            StreamedLeaf& leaf = out.leaves[f];
            if (leaf.levels.empty()) {
                leaf.levels.resize(L);
                std::copy_n(leaves[f]->boundMin, 3, leaf.boundMin);
                std::copy_n(leaves[f]->boundMax, 3, leaf.boundMax);
            }
            const uint32_t count = leaves[f]->lods[li].count;
            leaf.levels[li] = { cursor, count };
            if (count > 0) {
                jobs.push_back({ uint32_t(f), li, cursor });
                cursor += count;
                out.levelCounts[li] += count;
            }
        }
    }
    std::vector<std::vector<StreamedCluster>> jobClusters(jobs.size());
    Run(parallelFor, jobs.size(), [&](size_t j) {
        const Job& job = jobs[j];
        const LodNode& node = *leaves[job.leaf];
        const LodRun& run = node.lods[job.level];
        const DecodedAsset& a = chunks[run.file].asset;
        const std::array<uint8_t, 256>& sMap = scaleMap[run.file];
        const std::array<uint8_t, 256>& cMap = dcMap[run.file];
        const bool chunkSH = a.shCoeffs > 0;

        struct Item { uint32_t morton; uint32_t index; float pos[3]; float radius; };
        std::vector<Item> items(run.count);
        float extent[3];
        for (int k = 0; k < 3; ++k) {
            extent[k] = std::max(node.boundMax[k] - node.boundMin[k], 1e-6f);
        }
        for (uint32_t i = 0; i < run.count; ++i) {
            const PackedSplat& s = a.splats[run.offset + i];
            const uint32_t q[3] = { s.meanXY & 0xFFFFu, s.meanXY >> 16, s.meanZ_label & 0xFFFFu };
            Item& it = items[i];
            uint32_t cell[3];
            for (int k = 0; k < 3; ++k) {
                const double n = a.meanMin[k] + (a.meanMax[k] - a.meanMin[k]) * (double(q[k]) / 65535.0);
                it.pos[k] = float(Unlog(n));
                const double t = (double(it.pos[k]) - node.boundMin[k]) / extent[k];
                cell[k] = uint32_t(std::clamp(t, 0.0, 1.0) * 1023.0);
            }
            float largest = 0.0f;
            for (int k = 0; k < 3; ++k) {
                largest = std::max(largest, a.scaleCodebook[(s.scaleOpacity >> (8 * k)) & 0xFFu]);
            }
            it.radius = 3.0f * std::exp(largest);
            it.morton = Morton10(cell[0]) | (Morton10(cell[1]) << 1) | (Morton10(cell[2]) << 2);
            it.index = run.offset + i;
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.morton < y.morton; });

        for (uint32_t i = 0; i < run.count; ++i) {
            const Item& it = items[i];
            const PackedSplat& s = a.splats[it.index];
            const uint32_t q[3] = { s.meanXY & 0xFFFFu, s.meanXY >> 16, s.meanZ_label & 0xFFFFu };
            uint32_t g[3];
            for (int k = 0; k < 3; ++k) {
                const double n = a.meanMin[k] + (a.meanMax[k] - a.meanMin[k]) * (double(q[k]) / 65535.0);
                const double range = m.meanMax[k] - m.meanMin[k];
                g[k] = range > 0.0 ? Round16((n - m.meanMin[k]) / range * 65535.0) : 0u;
            }
            uint32_t flags = (s.dc >> 24) & 0xFFu;
            uint32_t label = 0;
            if (m.shCoeffs > 0) {
                if (chunkSH && !(flags & kFlagNoSH)) {
                    label = labelMap[run.file][s.meanZ_label >> 16];
                } else {
                    flags |= kFlagNoSH;
                }
            }
            const uint32_t so = s.scaleOpacity;
            if (options.keepSources) {
                out.sources[job.start + i] = (uint64_t(run.file) << 32) | it.index;
            }
            PackedSplat& d = m.splats[job.start + i];
            d.meanXY = g[0] | (g[1] << 16);
            d.meanZ_label = g[2] | (label << 16);
            d.quat = s.quat;
            d.scaleOpacity = uint32_t(sMap[so & 0xFFu]) | (uint32_t(sMap[(so >> 8) & 0xFFu]) << 8) |
                             (uint32_t(sMap[(so >> 16) & 0xFFu]) << 16) | (so & 0xFF000000u);
            d.dc = uint32_t(cMap[s.dc & 0xFFu]) | (uint32_t(cMap[(s.dc >> 8) & 0xFFu]) << 8) |
                   (uint32_t(cMap[(s.dc >> 16) & 0xFFu]) << 16) | (flags << 24);
        }

        for (uint32_t first = 0; first < run.count; first += options.clusterSize) {
            StreamedCluster c;
            c.leaf = job.leaf;
            c.level = job.level;
            c.start = job.start + first;
            c.count = std::min(options.clusterSize, run.count - first);
            for (int k = 0; k < 3; ++k) {
                c.boundMin[k] = std::numeric_limits<float>::max();
                c.boundMax[k] = -std::numeric_limits<float>::max();
            }
            for (uint32_t i = first; i < first + c.count; ++i) {
                for (int k = 0; k < 3; ++k) {
                    c.boundMin[k] = std::min(c.boundMin[k], items[i].pos[k] - items[i].radius);
                    c.boundMax[k] = std::max(c.boundMax[k], items[i].pos[k] + items[i].radius);
                }
            }
            jobClusters[j].push_back(c);
        }
    });
    for (auto& list : jobClusters) {
        out.clusters.insert(out.clusters.end(), list.begin(), list.end());
    }

    // Per-leaf errors: the file's when every leaf has them, else PlayCanvas's count-based fallback
    bool fileErrors = meta.lodErrors;
    for (const LodNode* leaf : leaves) {
        fileErrors = fileErrors && leaf->errors.size() == L;
    }
    out.errorsFromFile = fileErrors;
    for (size_t f = 0; f < leaves.size(); ++f) {
        const LodNode& node = *leaves[f];
        StreamedLeaf& leaf = out.leaves[f];
        leaf.errors.assign(L, 0.0f);
        uint32_t reference = 0;
        for (uint32_t li = 0; li < L && reference == 0; ++li) {
            reference = node.lods[li].count;
        }
        float previous = 0.0f;
        for (uint32_t li = 0; li < L; ++li) {
            const uint32_t count = node.lods[li].count;
            float e = 0.0f;
            if (fileErrors) {
                e = node.errors[li];
            } else if (count > 0) {
                e = float(std::log(double(reference) / double(count)));
            }
            previous = std::max(previous, e);                // non-decreasing toward coarser levels
            leaf.errors[li] = previous;
        }
    }

    BuildGpuTables(m);
    progress("Done");
    return Error::Ok;
}

Error LoadStreamedPath(const std::string& lodMetaPath, const StreamedOptions& options, StreamedAsset& out,
                       std::string* message)
{
    std::string file = lodMetaPath;
    {
        std::ifstream probe(file + "/lod-meta.json", std::ios::binary);
        if (probe) {
            file += "/lod-meta.json";
        }
    }
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        return Fail(message, Error::BadLodMeta, "cannot read " + file);
    }
    const std::string text((std::istreambuf_iterator<char>(f)), {});
    LodMeta meta;
    const Error e = ParseLodMeta(text.data(), text.size(), meta, message);
    if (e != Error::Ok) {
        return e;
    }
    const size_t slash = file.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? "." : file.substr(0, slash);
    ChunkOpener opener = [dir](const std::string& path, std::string* error) {
        return OpenPath(dir + "/" + path, error);
    };
    return LoadStreamed(meta, opener, options, out, message);
}

} // namespace sog
