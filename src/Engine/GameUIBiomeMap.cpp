#include "Engine/GameUIBiomeMap.hpp"
#include <Chunk/TerrainGenerator.hpp>

#include <cmath>
#include <algorithm>
#include <Engine/ThreadPool.hpp>
#include <chrono>

void paintBiomeMapPlayerDot(std::vector<unsigned char> &rgba,
							const BiomeRegionGrid &grid,
							glm::vec2 playerXZ)
{
	// Visualization rounding only: nearest display pixel. The canonical
	// voxel column of the player is worldToVoxelColumn(playerXZ); the marker
	// does not redefine it.
	const glm::ivec2 dot = grid.pixelForWorld(playerXZ);
	// Contract: a marker whose center falls outside the grid draws nothing —
	// no half-clipped ring on the border, and no arithmetic on extreme
	// pixel indices below.
	if (dot.x < 0 || dot.y < 0 || dot.x >= grid.width || dot.y >= grid.height)
		return;
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

static BiomeMapResult colorBiomeMap(const BiomeMapRequest &req,
                                    const BiomeRegionGrid &grid,
                                    const std::vector<BiomeType> &biomes)
{
	auto cancelled = [&]() {
		return req.cancelToken && req.cancelToken->load(std::memory_order_relaxed);
	};
	// Checkpoint 3: before RGBA allocation
	if (cancelled())
		return {};

	const size_t totalPixels = static_cast<size_t>(req.size) * static_cast<size_t>(req.size);
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
	// Scratch provided by the request's owner when available: one shared
	// scratch whose retainedPointsCap policy controls retention (the biome
	// map retains dense capacity for churn-free refreshes). When absent,
	// getBiomeRegion falls back to its internal thread-local scratch, which
	// keeps only tiled-sized capacity. Either way, getBiomeRegion's exit
	// guard enforces the bound after every attempt - successful or cancelled.
	TerrainGenerator::BiomeRegionScratch *scratch =
		req.scratch ? req.scratch.get() : nullptr;
	// Cancellation is checked before, during (between tiles), and after the
	// region sampling; an interrupted build publishes nothing.
	if (!gen.getBiomeRegion(grid, biomes, scratch, [&] { return cancelled(); }))
		return {};

	return colorBiomeMap(req, grid, biomes);
}

namespace
{
struct BiomeMapBuild : std::enable_shared_from_this<BiomeMapBuild>
{
	BiomeMapRequest request;
	const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	BiomeRegionGrid grid;
	std::vector<BiomeType> biomes;
	std::vector<TerrainGenerator::BiomeRegionTile> plan;
	std::atomic<size_t> nextTile{0};
	std::promise<BiomeMapResult> promise;
	// Submission sentinel prevents publication while more work is enqueued.
	std::atomic<size_t> remaining{1};
	std::atomic<bool> failed{false};

	bool cancelled() const
	{
		return failed.load(std::memory_order_relaxed) ||
			(request.cancelToken && request.cancelToken->load(std::memory_order_relaxed));
	}

	void publish(BiomeMapResult result)
	{
		result.elapsedMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - start).count();
		promise.set_value(std::move(result));
	}

	void finish()
	{
		// The last tile acquires all disjoint writes before converting/publishing.
		if (remaining.fetch_sub(1, std::memory_order_acq_rel) != 1)
			return;
		BiomeMapResult result;
		try
		{
			if (!cancelled())
				result = colorBiomeMap(request, grid, biomes);
		}
		catch (...) {} // Failed builds are invalid, never partially published.
		publish(std::move(result));
	}

	void runTile(ThreadPool &pool, TerrainGenerator::BiomeRegionTile tile)
	{
		try
		{
			if (!cancelled())
			{
				auto &gen = TerrainGenerator::getThreadLocal(request.seed);
				std::vector<BiomeType> pixels;
				// Dedicated thread-local tile scratch, never the shared request
				// scratch. Retention stays below ~0.7 MiB per worker.
				if (!gen.getBiomeRegionTile(grid, tile, pixels, nullptr,
					[&] { return cancelled(); }))
					failed.store(true, std::memory_order_relaxed);
				else
					for (int z = 0; z < tile.height; ++z)
						std::copy_n(pixels.begin() + static_cast<size_t>(z) * tile.width,
							tile.width, biomes.begin() +
							static_cast<size_t>(tile.z + z) * grid.width + tile.x);
			}
		}
		catch (...) { failed.store(true, std::memory_order_relaxed); }
		// Re-enqueue each next tile at Low priority: yield to streaming between
		// tiles rather than running a whole lane as one non-preemptible job.
		enqueueNext(pool);
		finish();
	}

	void enqueueNext(ThreadPool &pool)
	{
		if (cancelled())
			return;
		const size_t index = nextTile.fetch_add(1, std::memory_order_relaxed);
		if (index >= plan.size())
			return;
		remaining.fetch_add(1, std::memory_order_relaxed);
		try
		{
			pool.enqueue(TaskPriority::Low, [build = shared_from_this(), &pool, index] {
				build->runTile(pool, build->plan[index]);
			});
		}
		catch (...)
		{
			failed.store(true, std::memory_order_relaxed);
			finish(); // undo this unsubmitted tile; caller still holds one count
		}
	}

	void enqueueTiles(ThreadPool &pool)
	{
		plan = TerrainGenerator::buildBiomeRegionPlan(grid);
		biomes.resize(static_cast<size_t>(grid.width) * grid.height);
		// Bound map pressure to eight lanes even on large machines. The map
		// remains one single-flight build and adds no native worker threads.
		const size_t lanes = std::min({size_t(8), pool.workerCount(), plan.size()});
		for (size_t i = 0; i < lanes; ++i)
			enqueueNext(pool);
	}

	void run(ThreadPool &pool)
	{
		try
		{
			grid = makeBiomeRegionGrid(request.center.x, request.center.y,
				1.0f / request.zoom, request.size, request.size);
			if (!grid.valid() || cancelled())
				failed.store(true, std::memory_order_relaxed);
			else if (static_cast<double>(request.size) * grid.step <= 128.0)
			{
				// Small maps stay a single Low job, reusing the owner's scratch.
				publish(generateBiomeMap(request));
				return;
			}
			else
			{
				if (request.onCheckpoint)
					request.onCheckpoint();
				if (!cancelled())
					enqueueTiles(pool);
			}
		}
		catch (...) { failed.store(true, std::memory_order_relaxed); }
		finish(); // release submission sentinel; never wait for children
	}
};
}

std::future<BiomeMapResult> submitBiomeMap(ThreadPool &pool, BiomeMapRequest request)
{
	auto build = std::make_shared<BiomeMapBuild>();
	build->request = std::move(request);
	auto future = build->promise.get_future();
	try
	{
		pool.enqueue(TaskPriority::Low, [build, &pool] { build->run(pool); });
	}
	catch (...)
	{
		build->failed.store(true, std::memory_order_relaxed);
		build->finish();
	}
	return future;
}
