#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

// Cryptomatte (https://github.com/Psyop/Cryptomatte, specification 1.2): a name's id is its MurmurHash3_x86_32 (seed
// 0), stored as the bits of a float32. UE's Movie Render Queue Object ID pass hashes names the same way
// (MoviePipelineMaskRenderPass, MoviePipelineHashUtils.h).
inline uint32_t HdUECryptomatteHash(std::string_view name) {
    const auto rotl = [](uint32_t x, int r) { return (x << r) | (x >> (32 - r)); };
    const auto mix = [&](uint32_t k) { return rotl(k * 0xcc9e2d51u, 15) * 0x1b873593u; };
    uint32_t h = 0;
    const size_t blocks = name.size() / 4;
    for (size_t i = 0; i < blocks; ++i) {
        uint32_t k = 0;
        for (int b = 0; b < 4; ++b) k |= uint32_t(uint8_t(name[i * 4 + b])) << (8 * b);
        h = rotl(h ^ mix(k), 13) * 5 + 0xe6546b64u;
    }
    uint32_t tail = 0;
    for (size_t b = name.size() & 3; b-- > 0;) tail = tail << 8 | uint8_t(name[blocks * 4 + b]);
    if (name.size() & 3) h ^= mix(tail);
    h ^= uint32_t(name.size());
    h = (h ^ h >> 16) * 0x85ebca6bu;
    h = (h ^ h >> 13) * 0xc2b2ae35u;
    h ^= h >> 16;
    // Denormals, infinities and NaNs do not survive float conversions: flip the lowest exponent bit, as the
    // specification and MRQ do. No id is then 0, which marks "nothing" below.
    if (const uint32_t exponent = h >> 23 & 0xff; exponent == 0 || exponent == 0xff) h ^= 1u << 23;
    return h;
}

// The Cryptomatte layer a render var holds: CryptoObject<NN> (prim paths) is kind 0, CryptoMaterial<NN> (bound
// material paths) kind 1, layer NN; (-1, -1) for other names.
inline std::pair<int, int> HdUECryptomatteLayer(std::string_view name) {
    const std::string_view kinds[] = {"CryptoObject", "CryptoMaterial"};
    for (int kind = 0; kind < 2; ++kind)
        if (name.size() == kinds[kind].size() + 2 && name.substr(0, kinds[kind].size()) == kinds[kind] &&
            unsigned(name[name.size() - 2] - '0') < 10 && unsigned(name.back() - '0') < 10)
            return {kind, (name[name.size() - 2] - '0') * 10 + (name.back() - '0')};
    return {-1, -1};
}

inline float HdUECryptomatteFloat(uint32_t hash) {
    float value;
    std::memcpy(&value, &hash, sizeof(value));
    return value;
}

// One pixel's Cryptomatte ranks from the ids its samples hit (0: nothing): (id, coverage) pairs, most coverage first
// (ties: lower id), each sample covering 1 / count of the pixel (a box filter). Writes 2 * ranks floats, (0, 0) past
// the ids present; layer NN of a Cryptomatte holds ranks 2NN and 2NN + 1 as RGBA.
inline void HdUECryptomatteRanks(const uint32_t* ids, size_t count, float* out, size_t ranks) {
    std::vector<std::pair<uint32_t, uint32_t>> hits;  // (id, samples)
    for (size_t i = 0; i < count; ++i) {
        if (!ids[i]) continue;
        auto it = std::find_if(hits.begin(), hits.end(), [&](const auto& hit) { return hit.first == ids[i]; });
        if (it == hits.end())
            hits.emplace_back(ids[i], 1);
        else
            ++it->second;
    }
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    for (size_t r = 0; r < ranks; ++r) {
        out[2 * r] = r < hits.size() ? HdUECryptomatteFloat(hits[r].first) : 0.f;
        out[2 * r + 1] = r < hits.size() ? float(hits[r].second) / float(count) : 0.f;
    }
}
