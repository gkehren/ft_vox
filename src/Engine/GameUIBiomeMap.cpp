#include "Engine/GameUIBiomeMap.hpp"
#include <Chunk/TerrainGenerator.hpp>

#include <cmath>

void paintBiomeMapPlayerDot(std::vector<unsigned char> &rgba,
						   int size,
						   glm::vec2 playerXZ,
						   float zoom,
						   int gridX,
						   int gridZ)
{
	const float noiseOffset = TerrainGenerator::NOISE_OFFSET;
	const int dotX = static_cast<int>(std::round((playerXZ.x + noiseOffset) * zoom)) - gridX;
	const int dotY = static_cast<int>(std::round((playerXZ.y + noiseOffset) * zoom)) - gridZ;
	auto paint = [&](int px, int py, unsigned char r, unsigned char g, unsigned char b) {
		if (px < 0 || py < 0 || px >= size || py >= size)
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
				paint(dotX + dx, dotY + dy, 0, 0, 0);
	for (int dy = -2; dy <= 2; ++dy)
		for (int dx = -2; dx <= 2; ++dx)
			if (dx * dx + dy * dy <= 4)
				paint(dotX + dx, dotY + dy, 255, 255, 255);
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

	const float step = 1.0f / req.zoom;
	const float noiseOffset = TerrainGenerator::NOISE_OFFSET;
	const int gridX = static_cast<int>(std::round((req.center.x + noiseOffset) * req.zoom - req.size * 0.5f));
	const int gridZ = static_cast<int>(std::round((req.center.y + noiseOffset) * req.zoom - req.size * 0.5f));

	if (req.onCheckpoint)
		req.onCheckpoint();

	// Checkpoint 2: before getBiomeRegion()
	if (cancelled())
		return {};

	TerrainGenerator &gen = TerrainGenerator::getThreadLocal(req.seed);
	std::vector<BiomeType> biomes;
	gen.getBiomeRegion(req.center.x, req.center.y, step, req.size, req.size, biomes);

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
	res.gridX = gridX;
	res.gridZ = gridZ;
	res.size = req.size;
	res.rgba = std::move(rgba);
	res.valid = true;
	return res;
}
