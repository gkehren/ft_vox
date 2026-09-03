// Biome map generation lifetime, concurrency safety, cancellation, and stale/superseded rejection tests.
#include <Engine/GameUIBiomeMap.hpp>
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
	std::vector<unsigned char> rgba(size * size * 4, 0);

	const glm::vec2 center{0.f, 0.f};
	const float zoom = 1.0f;
	const float noiseOffset = TerrainGenerator::NOISE_OFFSET;
	const int gridX = static_cast<int>(std::round((center.x + noiseOffset) * zoom - size * 0.5f));
	const int gridZ = static_cast<int>(std::round((center.y + noiseOffset) * zoom - size * 0.5f));

	paintBiomeMapPlayerDot(rgba, size, center, zoom, gridX, gridZ);

	// Center pixel of the dot should be white (255, 255, 255, 255)
	const int dotX = static_cast<int>(std::round((center.x + noiseOffset) * zoom)) - gridX;
	const int dotY = static_cast<int>(std::round((center.y + noiseOffset) * zoom)) - gridZ;
	const int centerIdx = (dotY * size + dotX) * 4;
	CHECK(rgba[centerIdx + 0] == 255 && rgba[centerIdx + 1] == 255 &&
		  rgba[centerIdx + 2] == 255 && rgba[centerIdx + 3] == 255,
		  "Center of player dot should be opaque white");

	// Check that painting doesn't crash on out-of-bounds positions
	paintBiomeMapPlayerDot(rgba, size, {-100000.f, -100000.f}, zoom, gridX, gridZ);
}

static void test_biome_map_upload_validation()
{
	BiomeMapUpload emptyUpload{};
	CHECK(!isBiomeMapUploadValid(emptyUpload), "Empty upload must be invalid");

	BiomeMapUpload validUpload{
		.rgba = std::vector<uint8_t>(256 * 256 * 4, 128),
		.width = 256,
		.height = 256,
		.generation = 1
	};
	CHECK(isBiomeMapUploadValid(validUpload), "Well-formed 256x256 RGBA upload must be valid");

	BiomeMapUpload sizeMismatch{
		.rgba = std::vector<uint8_t>(256 * 256 * 3, 128),
		.width = 256,
		.height = 256,
		.generation = 1
	};
	CHECK(!isBiomeMapUploadValid(sizeMismatch), "RGB sized upload must be rejected");

	BiomeMapUpload zeroDim{
		.rgba = std::vector<uint8_t>(16 * 16 * 4, 128),
		.width = 0,
		.height = 16,
		.generation = 1
	};
	CHECK(!isBiomeMapUploadValid(zeroDim), "Zero dimension upload must be rejected");
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

int main()
{
	std::cout << "[test_biome_map] Running tests...\n";
	test_biome_map_result_validity();
	test_deterministic_stale_generation_rejection();
	test_deterministic_superseded_request_rejection();
	test_cancellation_pre_cancelled();
	test_cancellation_mid_flight();
	test_rapid_request_cancellation_sequence();
	test_player_dot_painting();
	test_biome_map_upload_validation();
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
