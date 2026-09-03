#pragma once

#include <cmath>
#include <glm/glm.hpp>

/// Continuous world position -> voxel column (canonical convention).
/// This is THE single convention shared by the HUD, getBiomeAt(),
/// getBiomeRegion(), the biome map, and the player marker: a world
/// coordinate belongs to the voxel column at its floor().
inline int worldToVoxelColumn(float world)
{
	return static_cast<int>(std::floor(world));
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
	const float fx = (world - center) / step + static_cast<float>(size) * 0.5f;
	if (!std::isfinite(fx))
		return -2147483648;
	if (fx >= 2147483520.0f) // 2^31-128: safely inside int range before rounding
		return 2147483647;
	if (fx <= -2147483520.0f)
		return -2147483648;
	return static_cast<int>(std::round(fx));
}

/// Canonical description of a biome-map sampling grid. It is the single
/// source of truth mapping pixels <-> world positions <-> voxel columns for
/// both TerrainGenerator::getBiomeRegion() and the GameUI biome map.
///
/// Pixel (0,0) samples worldAt(0,0) (the minimum corner) and pixel
/// (width-1,height-1) samples worldAt(width-1,height-1). For even
/// dimensions the grid center falls between the four central pixels; for
/// odd dimensions it lands exactly on the central pixel.
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
