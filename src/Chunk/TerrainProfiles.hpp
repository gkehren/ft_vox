#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace worldgen
{
enum class Relief : uint8_t { Rolling, Plateau, Massif, Canyon, Cliffs, Basin, Count };
enum class Palette : uint8_t { Temperate, Cold, Arid, Wet, Alpine, Volcanic, Marine, Fungal };
enum class Decoration : uint8_t { Meadow, Woodland, Conifer, Tropical, Wetland, Desert, Alpine, Marine, Fungal };

inline float blend(float lo, float hi, float value)
{
    const float t = std::clamp((value - lo) / (hi - lo), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// Regional relief is independent of the land/sea field. Adjacent profiles
// interpolate instead of changing height at a biome classification threshold.
inline std::array<float, 6> reliefWeights(float erosion, float weirdness)
{
    const float region = std::clamp((erosion + 0.6f) * (5.f / 1.2f), 0.f, 5.f);
    std::array<float, 6> weights{};
    const int a = std::min(4, static_cast<int>(region));
    const float t = blend(0.f, 1.f, region - a);
    weights[a] = 1.f - t;
    weights[a + 1] = t;
    // Broad inhabited landscapes between the exceptional formations.
    const float wild = 0.55f + 0.45f * blend(-0.35f, 0.55f, weirdness);
    for (auto &w : weights) w *= wild;
    weights[0] += 1.f - wild;
    return weights;
}

inline float riverWidth(float weirdness)
{
    return 0.05f + 0.06f * std::clamp((weirdness + 1.f) * 0.5f, 0.f, 1.f);
}

struct FeatureBounds
{
    int radius;
    int height;
    float maxSlope;
    uint32_t priority;
};
inline constexpr FeatureBounds smallTree{4, 20, 2.5f, 2};
inline constexpr FeatureBounds matureTree{8, 38, 1.5f, 3};
inline constexpr FeatureBounds groundProp{3, 4, 1.5f, 1};
inline constexpr int maxFeatureRadius = matureTree.radius;
inline constexpr int candidateCellSize = 4;

// Mathematical floor division, including negative world coordinates.
inline int cellAt(int coordinate)
{
    const int q = coordinate / candidateCellSize;
    return q - (coordinate % candidateCellSize < 0 ? 1 : 0);
}
} // namespace worldgen
