#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

enum class GpuPass : uint32_t { Frame, Upload, Shadow, Opaque, Overlays, Water, Sky, Post, ImGui, Count };
inline constexpr size_t kGpuPassCount = static_cast<size_t>(GpuPass::Count);
inline constexpr std::array<const char *, kGpuPassCount> kGpuPassNames = {
    "GPU Frame", "Uploads", "Shadow", "Opaque", "Overlays", "Water", "Sky", "Post", "ImGui"};

struct GpuFrameSample
{
    uint64_t serial{0};
    uint64_t benchmarkTag{0};
    std::array<float, kGpuPassCount> ms{};
    std::array<bool, kGpuPassCount> present{};
};

// timestampComputeAndGraphics guarantees support across graphics/compute queues.
// For queries on the selected graphics queue, that queue's valid bits are
// authoritative even when the device-wide guarantee is false.
inline bool gpuTimestampSupported(uint32_t validBits, float period)
{
    return validBits > 0 && validBits <= 64 &&
           std::isfinite(period) && period > 0.f;
}

// Unsigned subtraction plus masking handles one counter wrap without ever
// shifting by 64. Multiple wraps cannot be recovered from two timestamps.
inline double gpuTimestampMilliseconds(uint64_t begin, uint64_t end, uint32_t bits, float period)
{
    if (bits == 0 || bits > 64 || !std::isfinite(period) || period <= 0.f)
        return 0.0;
    const uint64_t mask = bits == 64 ? std::numeric_limits<uint64_t>::max() : (uint64_t{1} << bits) - 1;
    return static_cast<double>((end - begin) & mask) * static_cast<double>(period) / 1000000.0;
}
