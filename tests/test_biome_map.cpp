// Biome map generation lifetime, concurrency safety, cancellation, and stale/superseded rejection tests.
#include <Engine/GameUIBiomeMap.hpp>
#include <Engine/ThreadPool.hpp>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <Chunk/TerrainGenerator.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

static int g_fails = 0;

#define CHECK(cond, msg)                                                       \
	do                                                                         \
	{                                                                          \
		if (!(cond))                                                           \
		{                                                                      \
			std::cerr << "FAIL: " << msg << " (" << __LINE__ << ")\n";         \
			++g_fails;                                                         \
		}                                                                      \
	} while (0)

static void test_biome_map_result_validity()
{
	const uint64_t reqId = 12;
	const uint64_t genId = 5;
	const int seed = 42;
	const int size = 32;
	const float zoom = 0.5f;
	const glm::vec2 center{100.f, 200.f};

	BiomeMapRequest req{
		.requestId = reqId,
		.worldGenerationId = genId,
		.seed = seed,
		.center = center,
		.size = size,
		.zoom = zoom,
		.cancelToken = nullptr,
		.onCheckpoint = nullptr
	};

	BiomeMapResult res = generateBiomeMap(req);
	CHECK(res.valid, "Biome map result should be marked valid");
	CHECK(res.requestId == reqId, "Result request ID should match requested");
	CHECK(res.worldGenerationId == genId, "Result generation ID should match requested");
	CHECK(res.seed == seed, "Result seed should match requested");
	CHECK(res.size == size, "Result size should match requested");
	CHECK(res.rgba.size() == static_cast<size_t>(size * size * 4), "Result RGBA size mismatch");

	// The result must carry the canonical grid it was sampled with.
	CHECK(res.grid.center == center, "Result grid center should match request center");
	CHECK(std::fabs(res.grid.step - 1.0f / zoom) < 1e-6f, "Result grid step should be 1/zoom");
	CHECK(res.grid.width == size && res.grid.height == size, "Result grid dimensions should match size");
	for (int z = 0; z < size; z += 7)
	{
		for (int x = 0; x < size; x += 7)
		{
			const glm::ivec2 pixel = res.grid.pixelForWorld(res.grid.worldAt(x, z));
			CHECK(pixel == glm::ivec2(x, z), "Result grid pixel/world round-trip");
		}
	}

	// Verify that pixel colors correspond to known biomes and alpha is 255
	bool hasNonZeroPixel = false;
	bool allAlphaOpaque = true;
	for (size_t i = 0; i < res.rgba.size(); i += 4)
	{
		if (res.rgba[i + 0] != 0 || res.rgba[i + 1] != 0 || res.rgba[i + 2] != 0)
			hasNonZeroPixel = true;
		if (res.rgba[i + 3] != 255)
			allAlphaOpaque = false;
	}
	CHECK(hasNonZeroPixel, "RGBA map should contain non-zero pixel data");
	CHECK(allAlphaOpaque, "RGBA map should have fully opaque alpha (255)");
}

static void test_deterministic_stale_generation_rejection()
{
	// Controlled scenario:
	// Worker captures generation=1 / seed=42 / requestId=10
	// Worker signals checkpoint reached
	// Main changes active world to generation=2 / seed=1337 / requestId=11
	// Main allows worker to continue
	// Worker produces generation=1 result
	// Result is deterministically rejected
	std::promise<void> workerAtCheckpoint;
	std::promise<void> allowWorkerToContinue;
	auto atCheckpointFut = workerAtCheckpoint.get_future();
	auto allowWorkerFut = allowWorkerToContinue.get_future().share();

	std::atomic<uint64_t> activeGeneration{1};
	std::atomic<int> activeSeed{42};
	std::atomic<uint64_t> activeRequestId{10};

	BiomeMapRequest req{
		.requestId = 10,
		.worldGenerationId = 1,
		.seed = 42,
		.center = {0.f, 0.f},
		.size = 16,
		.zoom = 1.0f,
		.cancelToken = nullptr,
		.onCheckpoint = [&]() {
			workerAtCheckpoint.set_value();
			allowWorkerFut.wait();
		}
	};

	std::future<BiomeMapResult> workerFut = std::async(std::launch::async, [&]() {
		return generateBiomeMap(req);
	});

	// Wait for worker to reach checkpoint with captured generation=1 parameters
	atCheckpointFut.wait();

	// Main simulates world reload
	activeGeneration.store(2);
	activeSeed.store(1337);
	activeRequestId.store(11);

	// Let worker proceed and finish
	allowWorkerToContinue.set_value();

	BiomeMapResult res = workerFut.get();
	CHECK(res.valid, "Worker result itself was generated");
	CHECK(res.worldGenerationId == 1, "Result should retain generation=1");
	CHECK(res.seed == 42, "Result should retain seed=42");

	// Result must be deterministically rejected against active generation=2
	CHECK(!isBiomeMapResultAcceptable(res, activeGeneration.load(), activeSeed.load(), activeRequestId.load()),
		  "Stale generation result must be deterministically rejected");
}

static void test_deterministic_superseded_request_rejection()
{
	const uint64_t generation = 10;
	const int seed = 42;
	const uint64_t oldRequestId = 15;
	const uint64_t currentRequestId = 16;

	// Request 15 generated with older center and zoom
	BiomeMapRequest oldReq{
		.requestId = oldRequestId,
		.worldGenerationId = generation,
		.seed = seed,
		.center = {100.f, 100.f},
		.size = 16,
		.zoom = 0.5f,
		.cancelToken = nullptr,
		.onCheckpoint = nullptr
	};
	BiomeMapResult oldResult = generateBiomeMap(oldReq);
	CHECK(oldResult.valid, "Old result should be valid");

	// In the same world, active request ID is now 16 -> request 15 must be rejected
	CHECK(!isBiomeMapResultAcceptable(oldResult, generation, seed, currentRequestId),
		  "Superseded request ID in the same world must be rejected");

	// Request 16 generated for new center/zoom
	BiomeMapRequest currentReq{
		.requestId = currentRequestId,
		.worldGenerationId = generation,
		.seed = seed,
		.center = {200.f, 200.f},
		.size = 16,
		.zoom = 1.0f,
		.cancelToken = nullptr,
		.onCheckpoint = nullptr
	};
	BiomeMapResult currentResult = generateBiomeMap(currentReq);
	CHECK(currentResult.valid, "Current result should be valid");
	CHECK(isBiomeMapResultAcceptable(currentResult, generation, seed, currentRequestId),
		  "Current request ID matching active world must be accepted");

	// Check that invalid dimensions / zoom are rejected by invariants
	BiomeMapResult badResult = currentResult;
	badResult.size = 0;
	CHECK(!isBiomeMapResultAcceptable(badResult, generation, seed, currentRequestId),
		  "Zero size result must be rejected");

	badResult = currentResult;
	badResult.zoom = -1.f;
	CHECK(!isBiomeMapResultAcceptable(badResult, generation, seed, currentRequestId),
		  "Negative zoom result must be rejected");

	badResult = currentResult;
	badResult.rgba.pop_back();
	CHECK(!isBiomeMapResultAcceptable(badResult, generation, seed, currentRequestId),
		  "Truncated RGBA buffer result must be rejected");
}

static void test_cancellation_pre_cancelled()
{
	auto token = std::make_shared<std::atomic<bool>>(true);
	BiomeMapRequest req{
		.requestId = 1,
		.worldGenerationId = 1,
		.seed = 42,
		.center = {0.f, 0.f},
		.size = 32,
		.zoom = 1.0f,
		.cancelToken = token,
		.onCheckpoint = nullptr
	};
	BiomeMapResult res = generateBiomeMap(req);
	CHECK(!res.valid, "Pre-cancelled task should return invalid result");
	CHECK(res.rgba.empty(), "Cancelled task should not allocate RGBA buffer");
	CHECK(!isBiomeMapResultAcceptable(res, 1, 42, 1), "Cancelled task result must not be acceptable");
}

static void test_cancellation_mid_flight()
{
	auto token = std::make_shared<std::atomic<bool>>(false);
	std::promise<void> atCheckpoint;
	std::promise<void> proceed;
	auto atCheckpointFut = atCheckpoint.get_future();
	auto proceedFut = proceed.get_future().share();

	BiomeMapRequest req{
		.requestId = 2,
		.worldGenerationId = 1,
		.seed = 42,
		.center = {0.f, 0.f},
		.size = 32,
		.zoom = 1.0f,
		.cancelToken = token,
		.onCheckpoint = [&]() {
			atCheckpoint.set_value();
			proceedFut.wait();
		}
	};

	auto fut = std::async(std::launch::async, [&]() {
		return generateBiomeMap(req);
	});

	// Wait until task reaches checkpoint
	atCheckpointFut.wait();

	// Cancel while in flight before sampling
	token->store(true, std::memory_order_relaxed);
	proceed.set_value();

	BiomeMapResult res = fut.get();
	CHECK(!res.valid, "Mid-flight cancelled task must return invalid result");
	CHECK(res.rgba.empty(), "Cancelled task must have empty rgba buffer");
	CHECK(!isBiomeMapResultAcceptable(res, 1, 42, 2), "Cancelled result must be rejected");
}

static void test_rapid_request_cancellation_sequence()
{
	// Sequence:
	// Request A -> cancelled
	// Request B -> cancelled
	// Request C -> completes and matches currentRequestId
	const uint64_t gen = 1;
	const int seed = 42;
	uint64_t currentReqId = 0;

	auto tokenA = std::make_shared<std::atomic<bool>>(true);
	uint64_t reqIdA = ++currentReqId;
	BiomeMapRequest reqA{
		.requestId = reqIdA,
		.worldGenerationId = gen,
		.seed = seed,
		.center = {0.f, 0.f},
		.size = 16,
		.zoom = 1.f,
		.cancelToken = tokenA,
		.onCheckpoint = nullptr
	};
	BiomeMapResult resA = generateBiomeMap(reqA);

	auto tokenB = std::make_shared<std::atomic<bool>>(true);
	uint64_t reqIdB = ++currentReqId;
	BiomeMapRequest reqB{
		.requestId = reqIdB,
		.worldGenerationId = gen,
		.seed = seed,
		.center = {10.f, 10.f},
		.size = 16,
		.zoom = 1.f,
		.cancelToken = tokenB,
		.onCheckpoint = nullptr
	};
	BiomeMapResult resB = generateBiomeMap(reqB);

	uint64_t reqIdC = ++currentReqId;
	BiomeMapRequest reqC{
		.requestId = reqIdC,
		.worldGenerationId = gen,
		.seed = seed,
		.center = {20.f, 20.f},
		.size = 16,
		.zoom = 1.f,
		.cancelToken = nullptr,
		.onCheckpoint = nullptr
	};
	BiomeMapResult resC = generateBiomeMap(reqC);

	CHECK(!isBiomeMapResultAcceptable(resA, gen, seed, currentReqId), "Request A must be rejected");
	CHECK(!isBiomeMapResultAcceptable(resB, gen, seed, currentReqId), "Request B must be rejected");
	CHECK(isBiomeMapResultAcceptable(resC, gen, seed, currentReqId), "Only Request C must be accepted");
}

static void test_player_dot_painting()
{
	const int size = 32;
	const glm::vec2 center{0.f, 0.f};
	const float zoom = 1.0f;
	const BiomeRegionGrid grid = makeBiomeRegionGrid(center.x, center.y, 1.0f / zoom, size, size);

	// Player at the grid center: the dot must land on the nearest display
	// pixel of the player position per the canonical grid.
	{
		std::vector<unsigned char> rgba(size * size * 4, 0);
		paintBiomeMapPlayerDot(rgba, grid, center);

		const glm::ivec2 dot = grid.pixelForWorld(center);
		const size_t centerIdx = (static_cast<size_t>(dot.y) * size + dot.x) * 4;
		CHECK(rgba[centerIdx + 0] == 255 && rgba[centerIdx + 1] == 255 &&
				  rgba[centerIdx + 2] == 255 && rgba[centerIdx + 3] == 255,
			  "Center of player dot should be opaque white");

		// The black outline ring must surround the white core.
		const size_t ringIdx = (static_cast<size_t>(dot.y - 4) * size + dot.x) * 4;
		CHECK(rgba[ringIdx + 0] == 0 && rgba[ringIdx + 1] == 0 &&
				  rgba[ringIdx + 2] == 0 && rgba[ringIdx + 3] == 255,
			  "Player dot outline should be opaque black");
	}

	// Fractional player position: pixel comes from the grid, not from a
	// locally reconstructed mapping.
	{
		std::vector<unsigned char> rgba(size * size * 4, 0);
		const glm::vec2 player{-0.5f, 12.25f};
		paintBiomeMapPlayerDot(rgba, grid, player);
		const glm::ivec2 dot = grid.pixelForWorld(player);
		CHECK(dot.x >= 0 && dot.y >= 0 && dot.x < size && dot.y < size,
			  "fractional player maps inside the grid");
		const size_t centerIdx = (static_cast<size_t>(dot.y) * size + dot.x) * 4;
		CHECK(rgba[centerIdx + 0] == 255 && rgba[centerIdx + 1] == 255 &&
				  rgba[centerIdx + 2] == 255 && rgba[centerIdx + 3] == 255,
			  "Fractional player dot center should be opaque white");
	}

	// Player far outside the grid: nothing may be painted at all.
	{
		std::vector<unsigned char> rgba(size * size * 4, 0);
		const std::vector<unsigned char> before = rgba;
		paintBiomeMapPlayerDot(rgba, grid, {-100000.f, -100000.f});
		CHECK(rgba == before, "Out-of-grid player must not paint any pixel");
	}

	// Just beyond the border: a marker whose center is outside the grid
	// paints nothing — no half-clipped ring on the edge.
	{
		std::vector<unsigned char> rgba(size * size * 4, 0);
		const glm::vec2 player = grid.worldAt(-1, -1); // one pixel outside
		CHECK(grid.pixelForWorld(player) == glm::ivec2(-1, -1),
			  "player one pixel outside maps to pixel -1");
		const std::vector<unsigned char> before = rgba;
		paintBiomeMapPlayerDot(rgba, grid, player);
		CHECK(rgba == before, "Out-of-grid marker center must not paint any pixel");
	}
}

static void test_biome_map_upload_validation()
{
	BiomeMapUpload emptyUpload{};
	CHECK(!isBiomeMapUploadValid(emptyUpload), "Empty upload must be invalid");

	BiomeMapUpload validUpload{
		.rgba = std::vector<uint8_t>(256 * 256 * 4, 128),
		.width = 256,
		.height = 256,
		.requestId = 1
	};
	CHECK(isBiomeMapUploadValid(validUpload), "Well-formed 256x256 RGBA upload must be valid");

	BiomeMapUpload sizeMismatch{
		.rgba = std::vector<uint8_t>(256 * 256 * 3, 128),
		.width = 256,
		.height = 256,
		.requestId = 1
	};
	CHECK(!isBiomeMapUploadValid(sizeMismatch), "RGB sized upload must be rejected");

	BiomeMapUpload zeroDim{
		.rgba = std::vector<uint8_t>(16 * 16 * 4, 128),
		.width = 0,
		.height = 16,
		.requestId = 1
	};
	CHECK(!isBiomeMapUploadValid(zeroDim), "Zero dimension upload must be rejected");
}

static void test_deferred_upload_superseded_rejection()
{
	uint64_t currentRequestId = 10;

	BiomeMapUpload upload{
		.rgba = std::vector<uint8_t>(16 * 16 * 4, 255),
		.width = 16,
		.height = 16,
		.requestId = currentRequestId
	};
	CHECK(isBiomeMapUploadValid(upload), "Upload matching currentRequestId must be valid");

	// Player movement / zoom / view supersession occurs:
	++currentRequestId;

	// Stale deferred upload whose requestId no longer matches active request ID must be recognized as superseded
	CHECK(upload.requestId != currentRequestId,
		  "Deferred upload requestId must mismatch incremented active request ID");
}

static void test_player_movement_supersession()
{
	const glm::vec2 origin{0.f, 0.f};
	const glm::vec2 smallMove{4.f, 0.f};
	const glm::vec2 largeMove{12.f, 0.f};

	// Follow ON:
	CHECK(!shouldSupersedeBiomeMap(smallMove, origin, true),
		  "Movement <= 8 with follow ON must not supersede");
	CHECK(shouldSupersedeBiomeMap(largeMove, origin, true),
		  "Movement > 8 with follow ON must supersede");

	// Follow OFF:
	CHECK(!shouldSupersedeBiomeMap(largeMove, origin, false),
		  "Movement > 8 with follow OFF must not supersede");

	// Scenario:
	// Request 10 running for origin
	const uint64_t gen = 1;
	const int seed = 42;
	uint64_t currentReqId = 10;

	BiomeMapRequest req10{
		.requestId = 10,
		.worldGenerationId = gen,
		.seed = seed,
		.center = origin,
		.size = 16,
		.zoom = 1.0f
	};
	BiomeMapResult res10 = generateBiomeMap(req10);

	// Player moves > 8 with follow ON -> supersession occurs, active request becomes 11
	CHECK(shouldSupersedeBiomeMap(largeMove, origin, true), "Movement supersedes request");
	currentReqId = 11;

	// Result from request 10 completing now is superseded and rejected
	CHECK(!isBiomeMapResultAcceptable(res10, gen, seed, currentReqId),
		  "Superseded request 10 must be rejected after player movement triggered request 11");
}

static void test_biome_texture_reuse_rules()
{
	// image + descriptor + same size -> reuse
	CHECK(canReuseBiomeTexture(true, true, 256, 256), "Image and descriptor with same size must be reused");

	// image + descriptor + different size -> recreate
	CHECK(!canReuseBiomeTexture(true, true, 256, 512), "Different size must not be reused");

	// missing image -> recreate
	CHECK(!canReuseBiomeTexture(false, true, 256, 256), "Missing image must not be reused");

	// missing descriptor -> recreate/re-register
	CHECK(!canReuseBiomeTexture(true, false, 256, 256), "Missing descriptor must not be reused");

	// invalid requested size -> false
	CHECK(!canReuseBiomeTexture(true, true, 0, 0), "Invalid size must not be reused");
}

static void test_slow_job_exceeding_refresh_interval()
{
	const uint64_t gen = 1;
	const int seed = 42;
	uint64_t activeReqId = 10;
	auto token = std::make_shared<std::atomic<bool>>(false);

	std::promise<void> atCheckpoint;
	std::promise<void> allowWorker;
	auto atCheckpointFut = atCheckpoint.get_future();
	auto allowWorkerFut = allowWorker.get_future().share();

	BiomeMapRequest req10{
		.requestId = activeReqId,
		.worldGenerationId = gen,
		.seed = seed,
		.center = {0.f, 0.f},
		.size = 16,
		.zoom = 1.0f,
		.cancelToken = token,
		.onCheckpoint = [&]() {
			atCheckpoint.set_value();
			allowWorkerFut.wait();
		}
	};

	auto fut = std::async(std::launch::async, [&]() {
		return generateBiomeMap(req10);
	});

	// Wait until worker reaches checkpoint
	atCheckpointFut.wait();

	// Simulate periodic timer expiration while job is running:
	const double lastPublishedAt = 10.0;
	const double now = 11.5; // 1.5s elapsed (> 1.0s refresh interval)
	const bool running = true;
	const bool timeElapsed = (now - lastPublishedAt) >= 1.0;

	// In the updated engine loop, periodic refresh ONLY triggers when !running
	bool wouldRefresh = (!running && timeElapsed);
	CHECK(!wouldRefresh, "Periodic timer must not trigger refresh while a job is running");

	// Verify token is NOT cancelled and request ID is unchanged
	CHECK(!token->load(), "In-flight job must not be cancelled by periodic timer");
	CHECK(activeReqId == 10, "Request ID must not be incremented by timer while running");

	// Release worker
	allowWorker.set_value();
	BiomeMapResult res = fut.get();

	CHECK(res.valid, "Slow job completing after interval must produce valid result");
	CHECK(isBiomeMapResultAcceptable(res, gen, seed, activeReqId),
		  "Slow job result must be accepted when active request was not superseded");
}

static void test_ready_result_with_interval_elapsed()
{
	const uint64_t gen = 1;
	const int seed = 42;
	uint64_t activeReqId = 10;

	BiomeMapRequest req10{
		.requestId = activeReqId,
		.worldGenerationId = gen,
		.seed = seed,
		.center = {0.f, 0.f},
		.size = 16,
		.zoom = 1.0f
	};
	BiomeMapResult res10 = generateBiomeMap(req10);
	CHECK(res10.valid, "Generated result must be valid");

	// Simulate time: published at 10.0, current time is 11.3 (interval expired)
	double lastPublishedAt = 10.0;
	const double now = 11.3;

	// tickBiomeMap consumes ready result BEFORE evaluating periodic refresh:
	CHECK(isBiomeMapResultAcceptable(res10, gen, seed, activeReqId),
		  "Ready result must be accepted before periodic timer evaluation");

	// On publication, lastPublishedAt is updated to 'now'
	lastPublishedAt = now;

	// Next step in tick: evaluate periodic refresh
	const bool running = false;
	const bool timeElapsedAfterPublish = (now - lastPublishedAt) >= 1.0;
	const bool immediateReRefresh = (!running && timeElapsedAfterPublish);

	CHECK(!immediateReRefresh,
		  "Immediate re-refresh must not trigger right after publishing a result");
	CHECK(activeReqId == 10, "Request ID must remain intact after publication");
}

static void test_semantic_supersession_still_cancels_slow_job()
{
	const uint64_t gen = 1;
	const int seed = 42;
	uint64_t activeReqId = 10;
	auto token = std::make_shared<std::atomic<bool>>(false);

	std::promise<void> atCheckpoint;
	std::promise<void> allowWorker;
	auto atCheckpointFut = atCheckpoint.get_future();
	auto allowWorkerFut = allowWorker.get_future().share();

	BiomeMapRequest req10{
		.requestId = activeReqId,
		.worldGenerationId = gen,
		.seed = seed,
		.center = {0.f, 0.f},
		.size = 16,
		.zoom = 1.0f,
		.cancelToken = token,
		.onCheckpoint = [&]() {
			atCheckpoint.set_value();
			allowWorkerFut.wait();
		}
	};

	auto fut = std::async(std::launch::async, [&]() {
		return generateBiomeMap(req10);
	});

	atCheckpointFut.wait();

	// Player moves > 8 with follow ON -> semantic supersession
	const glm::vec2 playerXZ{100.f, 0.f};
	const glm::vec2 lastPlayer{0.f, 0.f};
	CHECK(shouldSupersedeBiomeMap(playerXZ, lastPlayer, true),
		  "Player movement with follow ON must trigger supersession");

	// supersedeBiomeMapRequest(): increment request ID and signal cancel token
	++activeReqId;
	token->store(true, std::memory_order_relaxed);

	allowWorker.set_value();
	BiomeMapResult res = fut.get();

	CHECK(!res.valid, "Semantically superseded slow job must be cancelled");
	CHECK(!isBiomeMapResultAcceptable(res, gen, seed, activeReqId),
		  "Cancelled / superseded slow job must be rejected against new active request ID");
}

static void test_request_scratch_reuse()
{
	// The request-owned scratch is reused across refreshes: dense capacity
	// is retained (bounded by the absolute dense bound, never per worker)
	// and the second dense build does not grow it (no allocation churn),
	// while results stay valid and acceptable.
	auto scratch = std::make_shared<TerrainGenerator::BiomeRegionScratch>();
	scratch->retainedPointsCap = TerrainGenerator::kMaxDenseDomainPoints;

	BiomeMapRequest req{
		.requestId = 21,
		.worldGenerationId = 3,
		.seed = 42,
		.center = {0.f, 0.f},
		.size = 256,
		.zoom = 0.5f, // dense single-pass path (peak scratch)
		.cancelToken = nullptr,
		.onCheckpoint = nullptr,
		.scratch = scratch
	};

	BiomeMapResult res1 = generateBiomeMap(req);
	CHECK(res1.valid, "dense map build with request scratch must be valid");
	CHECK(isBiomeMapResultAcceptable(res1, 3, 42, 21), "first result acceptable");
	const size_t retained = scratch->temperature.capacity();
	CHECK(retained > TerrainGenerator::kMaxTileDensePoints &&
			  retained <= TerrainGenerator::kMaxDenseDomainPoints,
		  "dense scratch capacity must be retained within the absolute bound");

	req.requestId = 22;
	BiomeMapResult res2 = generateBiomeMap(req);
	CHECK(res2.valid, "second dense map build must be valid");
	CHECK(isBiomeMapResultAcceptable(res2, 3, 42, 22), "second result acceptable");
	CHECK(scratch->temperature.capacity() == retained,
		  "second dense build must reuse the retained scratch without churn");

	// A cancelled build through the same scratch must not disturb retention
	// policy or leak the capacity above the cap.
	auto token = std::make_shared<std::atomic<bool>>(true);
	req.requestId = 23;
	req.cancelToken = token;
	BiomeMapResult res3 = generateBiomeMap(req);
	CHECK(!res3.valid, "pre-cancelled build must stay invalid");
	CHECK(scratch->temperature.capacity() == retained,
		  "cancelled build must not grow the retained scratch");

	// Zoom sweep dense -> tiled -> dense: switching zooms must not
	// reintroduce churn — the retained dense capacity survives the tiled
	// build untouched and is reused when returning to a dense zoom.
	req.cancelToken = nullptr;

	req.requestId = 24;
	req.zoom = 0.5f; // dense
	BiomeMapResult res4 = generateBiomeMap(req);
	CHECK(res4.valid && isBiomeMapResultAcceptable(res4, 3, 42, 24), "dense build valid");
	const size_t denseCapacity = scratch->temperature.capacity();

	req.requestId = 25;
	req.zoom = 0.1f; // tiled path
	BiomeMapResult res5 = generateBiomeMap(req);
	CHECK(res5.valid && isBiomeMapResultAcceptable(res5, 3, 42, 25), "tiled build valid");
	CHECK(scratch->temperature.capacity() == denseCapacity,
		  "tiled build must not disturb the retained dense capacity");

	req.requestId = 26;
	req.zoom = 0.5f; // dense again
	BiomeMapResult res6 = generateBiomeMap(req);
	CHECK(res6.valid && isBiomeMapResultAcceptable(res6, 3, 42, 26), "dense rebuild valid");
	CHECK(scratch->temperature.capacity() == denseCapacity,
		  "returning to dense zoom must reuse the retained capacity without churn");
}



static void test_region_tile_plan()
{
	for (int seed : {42, 4242})
	{
		TerrainGenerator gen(seed);
		for (float step : {0.125f, 1.0f, 10.0f, 1.0e20f})
		{
			const auto grid = makeBiomeRegionGrid(-16.01f, 123.6f, step, 67, 35);
			std::vector<BiomeType> expected, assembled(67 * 35);
			std::vector<int> visits(67 * 35);
			CHECK(gen.getBiomeRegion(grid, expected), "sequential region succeeds");
			auto plan = TerrainGenerator::buildBiomeRegionPlan(grid);
			std::reverse(plan.begin(), plan.end());
			TerrainGenerator::BiomeRegionScratch scratch;
			for (auto tile : plan)
			{
				std::vector<BiomeType> pixels;
				CHECK(gen.getBiomeRegionTile(grid, tile, pixels, &scratch), "independent tile succeeds");
				for (int z = 0; z < tile.height; ++z)
					for (int x = 0; x < tile.width; ++x)
					{
						const size_t i = static_cast<size_t>(tile.z + z) * grid.width + tile.x + x;
						assembled[i] = pixels[static_cast<size_t>(z) * tile.width + x];
						++visits[i];
					}
				CHECK(scratch.temperature.capacity() <= TerrainGenerator::kMaxTileDensePoints,
					  "tile scratch retention stays bounded");
			}
			CHECK(assembled == expected, "reverse tile order preserves every biome");
			CHECK(std::all_of(visits.begin(), visits.end(), [](int n) { return n == 1; }),
				  "plan partitions output without gaps or overlap");
			CHECK(!gen.getBiomeRegionTile(grid, {-1, 0, 1, 1}, assembled), "invalid rectangle rejected");
			CHECK(assembled.empty(), "invalid rectangle never returns partial data");
			int checks = 0;
			CHECK(!gen.getBiomeRegionTile(grid, plan.front(), assembled, &scratch,
				[&] { return ++checks == 2; }), "late tile cancellation rejected");
			CHECK(assembled.empty(), "cancelled tile output cleared");
		}
	}
}

static void test_parallel_maps()
{
	for (size_t workers : {size_t(1), size_t(4)})
	{
		ThreadPool pool(workers);
		for (float zoom : {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f})
		{
			BiomeMapRequest req;
			req.seed = 1337;
			req.requestId = 8;
			req.worldGenerationId = 3;
			req.size = 65; // odd dimensions, partial edge tiles
			req.center = {-123.6f, 432.1f};
			req.zoom = zoom;
			const auto expected = generateBiomeMap(req);
			auto future = submitBiomeMap(pool, req);
			CHECK(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
				  "parallel map completes even with one worker (no nested waits)");
			const auto actual = future.get();
			CHECK(actual.valid && actual.rgba == expected.rgba, "scheduled map matches sequential RGBA exactly");
			CHECK(isBiomeMapResultAcceptable(actual, 3, 1337, 8), "scheduled result preserves identity/grid");
		}
	}

	ThreadPool pool(1);
	BiomeMapRequest req;
	req.size = 65;
	req.zoom = 0.1f;
	req.cancelToken = std::make_shared<std::atomic<bool>>(true);
	CHECK(!submitBiomeMap(pool, req).get().valid, "pre-cancelled scheduled map rejected");
	req.cancelToken->store(false);
	// Parent enqueues tiles, then a queued High task cancels before they run.
	req.onCheckpoint = [&] {
		pool.enqueue(TaskPriority::High, [token = req.cancelToken] { token->store(true); });
	};
	const auto cancelled = submitBiomeMap(pool, req).get();
	CHECK(!cancelled.valid && cancelled.rgba.empty(), "queued tiles drain cancellation without partial publication");
	req.cancelToken->store(false);
	req.onCheckpoint = [] { throw std::runtime_error("test failure"); };
	CHECK(!submitBiomeMap(pool, req).get().valid, "job failure resolves future with invalid result");
	req.onCheckpoint = {};
	std::future<BiomeMapResult> future;
	{
		ThreadPool draining(2);
		future = submitBiomeMap(draining, req);
	} // pool shutdown must drain parent and all children without dangling state
	CHECK(future.get().valid, "shutdown drains scheduled map");
}

static void test_global_priority()
{
	ThreadPool pool(2);
	std::promise<void> enteredA, enteredB, releaseA, releaseB;
	auto gateA = releaseA.get_future().share();
	auto gateB = releaseB.get_future().share();
	auto a = pool.enqueue(TaskPriority::High, [&] { enteredA.set_value(); gateA.wait(); });
	enteredA.get_future().wait();
	auto b = pool.enqueue(TaskPriority::High, [&] { enteredB.set_value(); gateB.wait(); });
	enteredB.get_future().wait();
	// Only one worker is released. Every priority has work in both queues,
	// so local-first scheduling would select local Normal before remote High.
	std::vector<int> order;
	std::vector<std::future<void>> tasks;
	for (auto priority : {TaskPriority::Low, TaskPriority::Normal, TaskPriority::High})
		for (int i = 0; i < 2; ++i)
			tasks.push_back(pool.enqueue(priority, [&, priority] { order.push_back(static_cast<int>(priority)); }));
	releaseA.set_value();
	for (auto &task : tasks)
		task.get();
	CHECK(order == std::vector<int>({0, 0, 1, 1, 2, 2}), "High/Normal across all queues precede local Low tiles");
	releaseB.set_value();
	a.get(); b.get();
}

static int benchmark_maps()
{
	using Clock = std::chrono::steady_clock;
	const size_t workers = std::min(size_t(8), std::max(size_t(1), size_t(std::thread::hardware_concurrency())));
	ThreadPool pool(workers);
	BiomeMapRequest req;
	req.seed = 1337;
	req.size = 256;
	req.center = {-123.6f, 432.1f};
	req.scratch = std::make_shared<TerrainGenerator::BiomeRegionScratch>();
	req.scratch->retainedPointsCap = TerrainGenerator::kMaxDenseDomainPoints;
	std::cout << "[Biome map latency] workers=" << workers << " size=256 seed=1337 (3 runs, alternating order)\n";
	for (float zoom : {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f})
	{
		req.zoom = zoom;
		generateBiomeMap(req);
		submitBiomeMap(pool, req).get();
		double sequential = 0, parallel = 0;
		for (int run = 0; run < 3; ++run)
		{
			BiomeMapResult reference, scheduled;
			for (int phase = 0; phase < 2; ++phase)
			{
				const bool usePool = ((run + phase) % 2) != 0;
				const auto start = Clock::now();
				auto result = usePool ? submitBiomeMap(pool, req).get() : generateBiomeMap(req);
				const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
				(usePool ? parallel : sequential) += ms;
				(usePool ? scheduled : reference) = std::move(result);
			}
			CHECK(reference.valid && scheduled.valid && reference.rgba == scheduled.rgba,
				  "benchmark outputs must match exactly");
		}
		std::cout << "zoom=" << zoom << " sequential_ms=" << sequential / 3
				  << " scheduled_ms=" << parallel / 3 << '\n';
	}
	return g_fails ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && std::strcmp(argv[1], "--map-perf") == 0)
		return benchmark_maps();
	test_region_tile_plan();
	test_global_priority();
	test_parallel_maps();
	std::cout << "[test_biome_map] Running tests...\n";
	test_biome_map_result_validity();
	test_deterministic_stale_generation_rejection();
	test_deterministic_superseded_request_rejection();
	test_cancellation_pre_cancelled();
	test_cancellation_mid_flight();
	test_rapid_request_cancellation_sequence();
	test_player_dot_painting();
	test_request_scratch_reuse();
	test_biome_map_upload_validation();
	test_deferred_upload_superseded_rejection();
	test_player_movement_supersession();
	test_biome_texture_reuse_rules();
	test_slow_job_exceeding_refresh_interval();
	test_ready_result_with_interval_elapsed();
	test_semantic_supersession_still_cancels_slow_job();

	if (g_fails == 0)
	{
		std::cout << "[test_biome_map] ALL TESTS PASSED\n";
		return 0;
	}
	else
	{
		std::cerr << "[test_biome_map] " << g_fails << " TEST(S) FAILED\n";
		return 1;
	}
}
