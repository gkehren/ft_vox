#pragma once

#include <Chunk/BiomeRegionGrid.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <utils.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <vector>
#include <glm/glm.hpp>

/// Biome map colors indexed by BiomeType (from legacy UIManager).
inline constexpr unsigned char kBiomeColors[BIOME_COUNT][3] = {
	{100, 150, 200}, // FROZEN_OCEAN
	{220, 230, 240}, // SNOWY_TUNDRA
	{130, 160, 180}, // SNOWY_TAIGA
	{160, 210, 230}, // ICE_SPIKES
	{30, 100, 180},	 // OCEAN
	{210, 200, 150}, // BEACH
	{140, 200, 90},	 // PLAINS
	{50, 130, 50},	 // FOREST
	{100, 170, 80},	 // BIRCH_FOREST
	{30, 80, 30},	 // DARK_FOREST
	{80, 100, 60},	 // SWAMP
	{60, 130, 200},	 // RIVER
	{220, 200, 100}, // DESERT
	{190, 170, 80},	 // SAVANNA
	{40, 160, 40},	 // JUNGLE
	{200, 100, 30},	 // BADLANDS
	{150, 150, 150}, // MOUNTAINS
	{220, 220, 230}, // SNOWY_MOUNTAINS
	{150, 215, 95},	 // FLOWER_MEADOW
	{230, 130, 165}, // CHERRY_GROVE
	{190, 105, 45},	 // AUTUMN_FOREST
	{45, 85, 50},	 // REDWOOD_FOREST
	{55, 85, 55},	 // MANGROVE_SWAMP
	{70, 180, 65},	 // BAMBOO_JUNGLE
	{105, 105, 70},	 // MOOR
	{155, 210, 225}, // GLACIER
	{125, 185, 220}, // FROZEN_RIVER
	{70, 55, 50},	 // VOLCANIC
	{80, 185, 85},	 // OASIS
	{145, 90, 150},	 // MUSHROOM_FIELDS
	{45, 175, 185},	 // CORAL_REEF
};

struct BiomeMapRequest
{
	uint64_t requestId{0};
	uint64_t worldGenerationId{0};
	int seed{0};
	glm::vec2 center{0.f, 0.f};
	int size{256};
	float zoom{0.5f};
	std::shared_ptr<std::atomic<bool>> cancelToken;
	/// Optional injectable test seam hook invoked before sampling.
	std::function<void()> onCheckpoint;
	/// Owner-controlled scratch for the sequential/small-map path. Parallel
	/// tiles never share it; each worker uses bounded thread-local scratch.
	/// When set, the
	/// job reuses it across refreshes (retention bounded by the scratch's
	/// retainedPointsCap); when null, getBiomeRegion falls back to an
	/// internal thread-local scratch. Shared ownership keeps the scratch
	/// alive for the duration of the job even if the owner goes away.
	/// Access relies on the single-flight biome-map job contract (at most
	/// one in-flight request at a time), so no synchronization is needed.
	std::shared_ptr<TerrainGenerator::BiomeRegionScratch> scratch;
};

struct BiomeMapResult
{
	uint64_t requestId{0};
	uint64_t worldGenerationId{0};
	int seed{0};
	glm::vec2 center{0.f, 0.f};
	float zoom{1.f};
	/// Canonical grid describing pixel <-> world <-> voxel-column mapping
	/// for this map (step == 1/zoom, center == request center). The player
	/// marker is placed through it instead of reconstructing the mapping.
	BiomeRegionGrid grid;
	int size{0};
	std::vector<unsigned char> rgba;
	bool valid{false};
	double elapsedMs{0.0}; // scheduled request to completed CPU result, including queue time
};

/// Pending GPU upload request for streamed biome map data. The grid travels
/// with the pixels ON PURPOSE (issue #191 review): pixels and their mapping
/// are one atomic logical unit, so the World panel's published grid can only
/// ever describe the texture that was actually recorded to the GPU.
struct BiomeMapUpload
{
	std::vector<uint8_t> rgba;
	uint32_t width{0};
	uint32_t height{0};
	uint64_t requestId{0};
	/// Canonical grid of these pixels (validated upstream by
	/// isBiomeMapResultAcceptable before staging). Invalid while empty.
	BiomeRegionGrid grid;
};

/// True when a staged upload must be dropped because a newer request
/// superseded it while it waited for staging space (issue #191 review).
/// Dropping must never touch the PUBLISHED grid: the older texture stays on
/// screen and keeps its own mapping.
inline bool isBiomeMapUploadSuperseded(const BiomeMapUpload &pending, uint64_t currentRequestId)
{
	return pending.requestId != 0 && pending.requestId != currentRequestId;
}

/// Validate that a biome map upload request contains well-formed pixel data.
inline bool isBiomeMapUploadValid(const BiomeMapUpload &upload)
{
	return upload.width > 0 &&
		   upload.height > 0 &&
		   upload.requestId > 0 &&
		   upload.rgba.size() == static_cast<size_t>(upload.width) * static_cast<size_t>(upload.height) * 4;
}

/// Check if a biome map result matches active world generation, seed, request ID,
/// and internal invariant checks before GPU publication.
inline bool isBiomeMapResultAcceptable(const BiomeMapResult &result,
										  uint64_t currentWorldGenId,
										  int currentSeed,
										  uint64_t currentRequestId)
{
	return result.valid &&
		   result.worldGenerationId == currentWorldGenId &&
		   result.seed == currentSeed &&
		   result.requestId == currentRequestId &&
		   result.size > 0 &&
		   result.zoom > 0.0f &&
		   result.grid.valid() &&
		   result.grid.width == result.size &&
		   result.grid.height == result.size &&
		   result.rgba.size() == static_cast<size_t>(result.size) * static_cast<size_t>(result.size) * 4;
}

/// Check whether the existing GPU backing resources can be reused without destruction/recreation.
/// Semantic content validity (m_mapHasTexture) does not affect backing resource reuse.
inline bool canReuseBiomeTexture(bool imageExists,
								 bool descriptorExists,
								 int existingSize,
								 int requestedSize)
{
	return imageExists && descriptorExists && (existingSize == requestedSize) && (requestedSize > 0);
}

/// Determine if player movement should trigger superseding the active biome map request.
inline bool shouldSupersedeBiomeMap(glm::vec2 player,
									glm::vec2 lastPlayer,
									bool follow)
{
	if (!follow)
		return false;
	return glm::length(player - lastPlayer) > 8.0f;
}

/// Continuous (float) map-pixel coordinates for a world position on a
/// published biome grid — the pixel-center convention of BiomeRegionGrid
/// without the display rounding: row 0 is the min-Z edge, `center` falls at
/// pixel size*0.5 (even sizes). Pure so tests and the World-panel draw-list
/// overlays share one mapping (issue #186); callers map this into screen
/// space with the drawn image rect.
inline glm::vec2 biomeMapContinuousPixel(const BiomeRegionGrid &grid, glm::vec2 world)
{
	return {(world.x - grid.center.x) / grid.step + static_cast<float>(grid.width) * 0.5f,
			(world.y - grid.center.y) / grid.step + static_cast<float>(grid.height) * 0.5f};
}

/// Paint the player indicator dot (black outline with white center) into the
/// RGBA buffer. The dot pixel is grid.pixelForWorld(playerXZ) (nearest display
/// pixel); when it falls outside the grid nothing is painted.
void paintBiomeMapPlayerDot(std::vector<unsigned char> &rgba,
							const BiomeRegionGrid &grid,
							glm::vec2 playerXZ);

/// Sample biomes and convert them to an RGBA pixel buffer using thread-local generator.
/// Fully self-contained: owns its buffers and does not retain raw pointers to Engine state.
/// Cancellation is checked before sampling, during region sampling (between
/// tiles), and after sampling — an interrupted build returns an invalid
/// result and never publishes partial pixel data.
BiomeMapResult generateBiomeMap(const BiomeMapRequest &req);

class ThreadPool;
/// Engine-owned Low-priority tile scheduling. No worker waits for children.
/// Request data lives until every tile retires; the last tile publishes.
/// The pool must outlive submission/execution. GameUI enforces single-flight.
std::future<BiomeMapResult> submitBiomeMap(ThreadPool &pool, BiomeMapRequest req);

