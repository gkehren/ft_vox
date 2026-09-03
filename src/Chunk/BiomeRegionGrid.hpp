#pragma once

#include <cmath>
#include <limits>
#include <glm/glm.hpp>

/// Continuous world position -> voxel column (canonical convention).
/// This is THE single convention shared by the HUD, getBiomeAt(),
/// getBiomeRegion(), the biome map, and the player marker: a world
/// coordinate belongs to the voxel column at its floor(). Non-finite or
/// int-unrepresentable inputs clamp to the nearest representable column
/// (INT_MIN direction for NaN) instead of invoking UB; such positions are
/// far outside any reachable world.
inline int worldToVoxelColumn(float world)
{
	if (!std::isfinite(world))
		return std::numeric_limits<int>::min();
	// Floor in double, then range-check: float cannot represent the int
	// endpoints exactly, and static_cast<int>(floor(huge)) would be UB.
	const double w = std::floor(static_cast<double>(world));
	constexpr double kIntMin = static_cast<double>(std::numeric_limits<int>::min());
	constexpr double kIntMax = static_cast<double>(std::numeric_limits<int>::max());
	if (w <= kIntMin)
		return std::numeric_limits<int>::min();
	if (w >= kIntMax)
		return std::numeric_limits<int>::max();
	return static_cast<int>(w);
}

inline glm::ivec2 worldToVoxelColumn(glm::vec2 world)
{
	return {worldToVoxelColumn(world.x), worldToVoxelColumn(world.y)};
}

/// World position -> nearest biome-map pixel index along one axis.
/// Visualization-only rounding (round to nearest pixel); never use this to
/// resolve a canonical voxel column. Values outside the grid (or NaN input)
/// resolve to out-of-range indices; callers must bounds-check.
inline int worldToNearestMapPixel(float world, float center, float step, int size)
{
	const double fx = (static_cast<double>(world) - static_cast<double>(center)) /
						  static_cast<double>(step) +
					  static_cast<double>(size) * 0.5;
	if (!std::isfinite(fx))
		return std::numeric_limits<int>::min();
	const double rounded = std::round(fx);
	constexpr double kIntMin = static_cast<double>(std::numeric_limits<int>::min());
	constexpr double kIntMax = static_cast<double>(std::numeric_limits<int>::max());
	if (rounded <= kIntMin)
		return std::numeric_limits<int>::min();
	if (rounded >= kIntMax)
		return std::numeric_limits<int>::max();
	return static_cast<int>(rounded);
}

/// Canonical description of a biome-map sampling grid. It is the single
/// source of truth mapping pixels <-> world positions <-> voxel columns for
/// both TerrainGenerator::getBiomeRegion() and the GameUI biome map.
///
/// With `worldAt(x) = center + (x - size * 0.5f) * step`, the parity
/// contract is deliberately asymmetric and assumed as-is:
/// - even size: pixel `size/2` samples exactly `center`. Example
///   width=4, center=0, step=1 -> pixels sample -2, -1, 0, 1.
/// - odd size: `center` falls between the two central pixels. Example
///   width=5, center=0, step=1 -> pixels sample -2.5, -1.5, -0.5,
///   0.5, 1.5, so no pixel samples the center itself.
///
/// Lightweight and dependency-free (glm only): no FastNoise/Vulkan/ImGui.
struct BiomeRegionGrid
{
	glm::vec2 center{0.0f, 0.0f}; // world-space center of the grid
	float step{1.0f};             // world units per pixel; must be > 0 and finite
	int width{0};
	int height{0};

	/// World coordinate sampled by pixel (x, z).
	glm::vec2 worldAt(int x, int z) const
	{
		return center + glm::vec2(
							(static_cast<float>(x) - static_cast<float>(width) * 0.5f) * step,
							(static_cast<float>(z) - static_cast<float>(height) * 0.5f) * step);
	}

	/// Voxel column a pixel resolves to (canonical floor convention).
	glm::ivec2 columnAt(int x, int z) const
	{
		return worldToVoxelColumn(worldAt(x, z));
	}

	/// Nearest display pixel for a world position. May fall outside
	/// [0,width)x[0,height) when the position lies outside the grid.
	glm::ivec2 pixelForWorld(glm::vec2 world) const
	{
		return {worldToNearestMapPixel(world.x, center.x, step, width),
				worldToNearestMapPixel(world.y, center.y, step, height)};
	}

	/// True when the grid is usable for sampling.
	bool valid() const
	{
		return width > 0 && height > 0 &&
			   std::isfinite(step) && step > 0.0f &&
			   std::isfinite(center.x) && std::isfinite(center.y);
	}
};

/// Single factory for biome-map grids so every producer (TerrainGenerator,
/// GameUI, tests) builds the same description from the same inputs.
inline BiomeRegionGrid makeBiomeRegionGrid(float centerX, float centerZ,
										   float step, int width, int height)
{
	return BiomeRegionGrid{glm::vec2(centerX, centerZ), step, width, height};
}
