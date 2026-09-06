#pragma once

/// Single source of truth for the per-frame VoxelDrawData table layout
/// (issue #110): every indirect voxel command owns one table entry, indexed
/// in the shader through firstInstance / gl_InstanceIndex. Regions are
/// disjoint so OpaquePass / WaterPass / ShadowPass can write their entries
/// into the same frame buffer without coordination.

#include <cstdint>

#include <Renderer/ShadowCascades.hpp>

namespace voxel_draw
{
/// Per-pass indirect-command budget: also the allocation size of each pass's
/// host-visible indirect buffer (kCommandsPerPass * 16 bytes per frame).
inline constexpr uint32_t kCommandsPerPass = 65536;

inline constexpr uint32_t kOpaqueBase = 0;
inline constexpr uint32_t kWaterBase = kOpaqueBase + kCommandsPerPass;
inline constexpr uint32_t kShadowBase = kWaterBase + kCommandsPerPass;

constexpr uint32_t shadowBase(uint32_t cascade)
{
	return kShadowBase + cascade * kCommandsPerPass;
}

/// Total table entries: opaque + water + one region per shadow cascade.
inline constexpr uint32_t kEntryCount =
	kCommandsPerPass * (2u + static_cast<uint32_t>(shadow::kCascadeCount));

static_assert(kOpaqueBase + kCommandsPerPass == kWaterBase, "opaque/water regions must not overlap");
static_assert(kWaterBase + kCommandsPerPass == kShadowBase, "water/shadow regions must not overlap");
static_assert(kEntryCount == 327680u, "layout contract: 2 passes + 3 cascades of 65536 entries");
} // namespace voxel_draw
