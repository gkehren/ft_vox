#include "Engine/GameUIBiomeMap.hpp"
#include <Chunk/TerrainGenerator.hpp>

#include <cmath>

void paintBiomeMapPlayerDot(std::vector<unsigned char> &rgba,
							const BiomeRegionGrid &grid,
							glm::vec2 playerXZ)
{
	// Visualization rounding only: nearest display pixel. The canonical
	// voxel column of the player is worldToVoxelColumn(playerXZ); the marker
	// does not redefine it.
	const glm::ivec2 dot = grid.pixelForWorld(playerXZ);
	const int size = grid.width;
	auto paint = [&](int px, int py, unsigned char r, unsigned char g, unsigned char b) {
		if (px < 0 || py < 0 || px >= size || py >= grid.height)
			return;
		const int idx = (py * size + px) * 4;
		if (static_cast<size_t>(idx + 3) < rgba.size())
		{
			rgba[idx + 0] = r;
			rgba[idx + 1] = g;
			rgba[idx + 2] = b;
			rgba[idx + 3] = 255;
		}
	};
	for (int dy = -4; dy <= 4; ++dy)
		for (int dx = -4; dx <= 4; ++dx)
			if (dx * dx + dy * dy <= 16)
				paint(dot.x + dx, dot.y + dy, 0, 0, 0);
	for (int dy = -2; dy <= 2; ++dy)
		for (int dx = -2; dx <= 2; ++dx)
			if (dx * dx + dy * dy <= 4)
				paint(dot.x + dx, dot.y + dy, 255, 255, 255);
}

BiomeMapResult generateBiomeMap(const BiomeMapRequest &req)
{
	auto cancelled = [&]() {
		return req.cancelToken && req.cancelToken->load(std::memory_order_relaxed);
	};

	// Checkpoint 1: before generator lookup
	if (cancelled())
		return {};

	if (req.size <= 0 || req.zoom <= 0.0f)
		return {};

	// Single canonical grid for the whole pipeline: sampling, result
	// description, and player-marker placement all share it.
	const BiomeRegionGrid grid =
		makeBiomeRegionGrid(req.center.x, req.center.y, 1.0f / req.zoom, req.size, req.size);

	if (req.onCheckpoint)
		req.onCheckpoint();

	// Checkpoint 2: before getBiomeRegion()
	if (cancelled())
		return {};

	TerrainGenerator &gen = TerrainGenerator::getThreadLocal(req.seed);
	std::vector<BiomeType> biomes;
	// Propagate cancellation into the region sampling so a long zoomed-out
	// build is abandoned between tiles instead of running to completion.
	if (!gen.getBiomeRegion(grid, biomes, [&] { return cancelled(); }))
		return {};

	// Checkpoint 3: before RGBA allocation
	if (cancelled())
		return {};

	const size_t totalPixels = static_cast<size_t>(req.size * req.size);
	std::vector<unsigned char> rgba(totalPixels * 4);

	// Checkpoint 4: conversion loop with periodic check every 1024 iterations
	constexpr size_t kCheckInterval = 1024;
	for (size_t i = 0; i < totalPixels; ++i)
	{
		if ((i % kCheckInterval) == 0 && cancelled())
			return {};

		const int b = static_cast<int>(biomes[i]);
		const int bi = (b >= 0 && b < BIOME_COUNT) ? b : 0;
		rgba[i * 4 + 0] = kBiomeColors[bi][0];
		rgba[i * 4 + 1] = kBiomeColors[bi][1];
		rgba[i * 4 + 2] = kBiomeColors[bi][2];
		rgba[i * 4 + 3] = 255;
	}

	// Checkpoint 5: before publication
	if (cancelled())
		return {};

	BiomeMapResult res;
	res.requestId = req.requestId;
	res.worldGenerationId = req.worldGenerationId;
	res.seed = req.seed;
	res.center = req.center;
	res.zoom = req.zoom;
	res.grid = grid;
	res.size = req.size;
	res.rgba = std::move(rgba);
	res.valid = true;
	return res;
}
