/// Deterministic visual-regression harness for the Vulkan graphics pipeline
/// (issue #142).
///
/// Renders a fixed set of scenes offscreen through the PRODUCTION
/// WorldRenderer pass graph (Shadow → Opaque → Water → Sky → Post) with
/// pinned world seed, camera, dayTime and animation time, then compares the
/// readback against committed tolerant references in
/// tests/visual-references/ (never bit-exact across GPU vendors).
///
/// An additional self-contained numeric check (no references) validates the
/// SSAO camera-motion stability (issue #138): the noon_terrain setup is
/// rendered at two nearby poses with SSAO on and off, and the AO-on motion
/// delta must stay within the no-AO parallax baseline.
///
/// Usage:
///   ft_vox_visual_tests [--update-references] [--smoke]
///                       [--scene NAME]... [--refs DIR] [--out DIR]
///
/// Exit codes: 0 = pass, 1 = failure, 77 = environment skip (no Vulkan).
/// Run from the repository root so ./ressources/ resolves (ctest does).

#include "VisualHarness.hpp"
#include "VisualImage.hpp"

#include "Chunk/TerrainGenerator.hpp"
#include "utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using visual::CompareThresholds;
using visual::ImageMetrics;
using visual::RgbaImage;

namespace
{

// ---------------------------------------------------------------------------
// Small image-stat helpers for targeted per-scene invariants (issue #142 §6).
// ---------------------------------------------------------------------------

struct RegionStats
{
	double meanLuma = 0.0;
	double minLuma = 255.0;
	double maxLuma = 0.0;
	double meanR = 0.0;
	double meanG = 0.0;
	double meanB = 0.0;
};

/// Stats over row range [y0, y1).
RegionStats rowStats(const RgbaImage &image, uint32_t y0, uint32_t y1)
{
	RegionStats s;
	long long count = 0;
	y0 = std::min(y0, image.height);
	y1 = std::min(y1, image.height);
	for (uint32_t y = y0; y < y1; ++y)
	{
		for (uint32_t x = 0; x < image.width; ++x)
		{
			const uint8_t *p = &image.pixels[(size_t(y) * image.width + x) * 4];
			const double luma = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
			s.meanLuma += luma;
			s.meanR += p[0];
			s.meanG += p[1];
			s.meanB += p[2];
			s.minLuma = std::min(s.minLuma, luma);
			s.maxLuma = std::max(s.maxLuma, luma);
			++count;
		}
	}
	if (count)
	{
		s.meanLuma /= count;
		s.meanR /= count;
		s.meanG /= count;
		s.meanB /= count;
	}
	return s;
}

void need(bool condition, std::vector<std::string> &errors, const std::string &message)
{
	if (!condition)
		errors.push_back(message);
}

// ---------------------------------------------------------------------------
// Deterministic spot finders: pure TerrainGenerator sampling, no chunk state,
// so a given seed always resolves the same camera anchor.
// ---------------------------------------------------------------------------

glm::ivec2 findLandColumn(TerrainGenerator &gen, int minHeight, int maxHeight, int laneZ)
{
	for (int x = 0; x <= 8192; x += 8)
	{
		const int h = gen.getTerrainSample(x, laneZ).postErosionHeight;
		if (h >= minHeight && h <= maxHeight)
			return {x, laneZ};
	}
	return {0, laneZ};
}

glm::ivec2 findShoreCrossing(TerrainGenerator &gen, int &outLane)
{
	// A low beach (not a cliff) followed by a persistent body of water: the
	// next four samples must sit below sea level and get deeper. The land
	// must ALSO stay low behind the beach, so a camera standing in the
	// water looking at the coast sees shore + terrain, not a cliff wall.
	// Several lanes are probed; sea shape varies wildly per lane.
	for (int lane : {24, 40, 8, 56, 16, 48, 32, 0, 64})
	{
		for (int x = 16; x <= 16384; x += 8)
		{
			const int land = gen.getTerrainSample(x, lane).postErosionHeight;
			if (land < TerrainGenerator::SEA_LEVEL + 2 || land > TerrainGenerator::SEA_LEVEL + 10)
				continue;
			const int behind1 = gen.getTerrainSample(x - 8, lane).postErosionHeight;
			const int behind2 = gen.getTerrainSample(x - 16, lane).postErosionHeight;
			if (behind1 > TerrainGenerator::SEA_LEVEL + 16 || behind2 > TerrainGenerator::SEA_LEVEL + 26)
				continue;
			bool water = true;
			for (int step = 1; step <= 4; ++step)
			{
				const int h = gen.getTerrainSample(x + 8 * step, lane).postErosionHeight;
				if (h > TerrainGenerator::SEA_LEVEL - 2 || (step == 4 && h > TerrainGenerator::SEA_LEVEL - 4))
					water = false;
			}
			if (water)
			{
				outLane = lane;
				return {x, lane};
			}
		}
	}
	return {0, 0};
}

glm::ivec2 findDeepWaterColumn(TerrainGenerator &gen, int laneZ)
{
	for (int x = 0; x <= 16384; x += 8)
	{
		if (gen.getTerrainSample(x, laneZ).postErosionHeight <= TerrainGenerator::SEA_LEVEL - 8)
			return {x, laneZ};
	}
	return {0, laneZ};
}

/// Chunk-aligned anchor for the sealed cave room (cave_emissive and the
/// auto_exposure scenes): thick rock so the carved room stays sealed; the
/// room is centered inside one chunk (local 8, 38, 8) so light BFS never
/// crosses a border.
glm::vec3 findCaveAnchor(TerrainGenerator &gen)
{
	const glm::ivec2 col = findLandColumn(gen, 85, 130, 8);
	const int baseX = (col.x / CHUNK_SIZE) * CHUNK_SIZE;
	const int baseZ = (col.y / CHUNK_SIZE) * CHUNK_SIZE;
	return glm::vec3(float(baseX + 8), 38.f, float(baseZ + 8));
}

/// Carve the squashed-sphere cavity (dy weighted 1.2): the floor ends up
/// around y = 34 near the center, stepping up towards the rim. Light-source
/// fixtures (lava/magma) are added by the caller, if any.
void carveCaveRoom(ChunkManager &chunks, const glm::vec3 &anchor)
{
	const int cx = int(anchor.x), cy = int(anchor.y), cz = int(anchor.z);
	for (int dz = -6; dz <= 6; ++dz)
		for (int dx = -6; dx <= 6; ++dx)
			for (int dy = -5; dy <= 4; ++dy)
			{
				const float room = float(dx * dx + dz * dz) + float(dy * dy) * 1.44f;
				if (room <= 30.f)
					chunks.deleteVoxel(glm::vec3(float(cx + dx), float(cy + dy), float(cz + dz)));
			}
}

float yawToward(const glm::vec3 &from, const glm::vec3 &to)
{
	return glm::degrees(std::atan2(to.z - from.z, to.x - from.x));
}

/// Highest active voxel in a column of the loaded world — the real surface
/// (post perturbation/vegetation), unlike the sample-based approximation.
int groundHeightAt(ChunkManager &chunks, int x, int z)
{
	for (int y = CHUNK_HEIGHT - 1; y > 0; --y)
		if (chunks.isVoxelActive(glm::vec3(float(x), float(y), float(z))))
			return y;
	return TerrainGenerator::SEA_LEVEL;
}

// ---------------------------------------------------------------------------
// Scene descriptions.
// ---------------------------------------------------------------------------

struct SceneRun
{
	VisualHarness &harness;
	std::vector<entities::MobRenderState> mobs;
	bool worldEdited = false;
	/// Free-form anchor a spot() lambda can leave for its fixture().
	glm::vec3 anchor{0.0f};
};

struct SceneSpec
{
	const char *name = "";
	int seed = 0;
	float dayTime = 0.25f;
	/// Pinned animation time (water waves, foliage wind, star twinkle, grain).
	float time = 0.0f;
	int areaRadiusChunks = 6;
	bool underwater = false;
	/// Render through the AUTO exposure path (issue #140) instead of the
	/// fixed manual exposure the existing golden references were captured
	/// with. See the double-render block in main for how the bit-identical
	/// determinism check is preserved for adapting scenes.
	bool autoExposure = false;
	/// Render an extra base frame with no mobs (entity/terrain consistency).
	bool mobDeltaBase = false;
	CompareThresholds thresholds{};
	/// Camera placement from terrain samples (runs before the area build).
	std::function<void(SceneRun &)> spot;
	/// Optional world fixture: voxel edits / mobs (runs after the build).
	std::function<void(SceneRun &)> fixture;
	/// Targeted numeric assertions on the actual (and optional mob-free) image.
	std::function<std::vector<std::string>(const RgbaImage &actual, const RgbaImage &mobFreeBase)> invariants;
};

/// Shared noon terrain viewpoint (noon_terrain + auto_exposure_noon): a fixed
/// camera over sunlit mixed terrain with tight fog for long shadows.
void spotNoonTerrain(SceneRun &run)
{
	VisualHarness &h = run.harness;
	const glm::ivec2 col = findLandColumn(h.terrain(), 70, 100, 8);
	const float ground = float(h.terrain().getTerrainSample(col.x, col.y).postErosionHeight);
	h.camera().setPosition(glm::vec3(col.x + 10.f, ground + 14.f, col.y + 10.f));
	h.camera().setYawPitch(225.f, -28.f); // look back across the anchor column
	h.shader().fogStart = 60.f;
	h.shader().fogEnd = 135.f;
}

std::vector<SceneSpec> buildSceneTable()
{
	std::vector<SceneSpec> scenes;

	// --- noon_terrain -------------------------------------------------------
	// Grass/stone/sand/foliage in direct sun with visible shadows: color
	// space, mips/filtering, fog, CSM and the base post stack.
	scenes.push_back({});
	SceneSpec &noon = scenes.back();
	noon.name = "noon_terrain";
	noon.seed = 4217;
	noon.dayTime = 0.30f; // full day factor, sun low enough for long shadows
	noon.time = 11.0f;
	noon.spot = spotNoonTerrain;
	noon.invariants = [](const RgbaImage &actual, const RgbaImage &) {
		std::vector<std::string> errors;
		// Top 5% rows stay genuinely sky/clouds for this fixed camera. The wider
		// 15% band used before issue #136 was dominated by tree canopy and
		// distant terrain, whose nearest-sampling aliasing speckle the mip chain
		// removed (band luma 96.6 -> 91.0 at 5%, 74.0 at 15%); ground luma is
		// unchanged (79.7 -> 79.9), so 5% keeps healthy margins on both checks.
		const RegionStats sky = rowStats(actual, 0, actual.height * 5 / 100);
		const RegionStats ground = rowStats(actual, actual.height * 60 / 100, actual.height);
		need(sky.meanLuma > 80.0, errors, "sky band too dark for noon (mean luma " + std::to_string(sky.meanLuma) + ")");
		need(ground.maxLuma - ground.minLuma > 30.0, errors,
			 "ground shows no lit/shadowed dynamic range (spread " + std::to_string(ground.maxLuma - ground.minLuma) + ")");
		need(ground.meanLuma < sky.meanLuma, errors, "ground brighter than sky at noon");
		return errors;
	};

	// --- cascade_transition -------------------------------------------------
	// Geometry crossing the first/second CSM split with a fixed camera:
	// shadow seams and cascade filtering changes.
	scenes.push_back({});
	SceneSpec &cascade = scenes.back();
	cascade.name = "cascade_transition";
	cascade.seed = 4217;
	cascade.dayTime = 0.27f; // lower sun: longer shadows across the split
	cascade.time = 7.0f;
	cascade.spot = [](SceneRun &run) {
		VisualHarness &h = run.harness;
		const glm::ivec2 col = findLandColumn(h.terrain(), 70, 100, 8);
		const float ground = float(h.terrain().getTerrainSample(col.x, col.y).postErosionHeight);
		h.camera().setPosition(glm::vec3(float(col.x), ground + 45.f, float(col.y)));
		h.camera().setYawPitch(0.f, -40.f); // steep view: near ground → horizon
		h.shader().fogStart = 60.f;
		h.shader().fogEnd = 135.f;
	};
	cascade.invariants = [](const RgbaImage &actual, const RgbaImage &) {
		std::vector<std::string> errors;
		const RegionStats ground = rowStats(actual, actual.height * 25 / 100, actual.height);
		need(ground.maxLuma - ground.minLuma > 25.0, errors,
			 "terrain shows no shadow dynamic range (spread " + std::to_string(ground.maxLuma - ground.minLuma) + ")");
		return errors;
	};

	// --- cave_emissive ------------------------------------------------------
	// Hand-carved dark room with a lava pool and magma ring: block-light
	// propagation, emissive blocks, bloom and dark-scene exposure.
	scenes.push_back({});
	SceneSpec &cave = scenes.back();
	cave.name = "cave_emissive";
	cave.seed = 9001;
	cave.dayTime = 0.25f; // irrelevant inside the enclosed room
	cave.time = 3.0f;
	cave.areaRadiusChunks = 3;
	cave.spot = [](SceneRun &run) {
		VisualHarness &h = run.harness;
		run.anchor = findCaveAnchor(h.terrain());
		h.camera().setPosition(run.anchor + glm::vec3(-3.2f, 1.5f, -3.2f));
		h.camera().setYawPitch(45.f, -12.f); // look across the lava pool
	};
	cave.fixture = [](SceneRun &run) {
		ChunkManager &chunks = run.harness.chunks();
		carveCaveRoom(chunks, run.anchor);
		// Lava pool embedded in the center floor (top face exposed by the
		// y=34 cavity), magma ring on the first carved step around it.
		const int cx = int(run.anchor.x), cy = int(run.anchor.y), cz = int(run.anchor.z);
		for (int dz = -4; dz <= 4; ++dz)
			for (int dx = -4; dx <= 4; ++dx)
			{
				const int d2 = dx * dx + dz * dz;
				if (d2 <= 6)
					chunks.placeVoxel(glm::vec3(float(cx + dx), float(cy - 5), float(cz + dz)), LAVA);
				else if (d2 <= 16)
					chunks.placeVoxel(glm::vec3(float(cx + dx), float(cy - 4), float(cz + dz)), MAGMA);
			}
		run.worldEdited = true;
	};
	cave.invariants = [](const RgbaImage &actual, const RgbaImage &) {
		std::vector<std::string> errors;
		const RegionStats all = rowStats(actual, 0, actual.height);
		const RegionStats floorBand = rowStats(actual, actual.height * 55 / 100, actual.height * 92 / 100);
		need(all.meanLuma < 140.0, errors, "cave not dark (mean luma " + std::to_string(all.meanLuma) + ")");
		need(all.maxLuma > 180.0, errors, "no emissive peak from lava (max luma " + std::to_string(all.maxLuma) + ")");
		need(floorBand.meanLuma > all.meanLuma, errors, "lava band not brighter than cave average");
		return errors;
	};

	// --- water_shore --------------------------------------------------------
	// Shoreline crossing viewed from shallow water toward a low beach:
	// refraction, absorption, foam and the #120 water semantics.
	scenes.push_back({});
	SceneSpec &shore = scenes.back();
	shore.name = "water_shore";
	shore.seed = 4217;
	shore.dayTime = 0.35f;
	shore.time = 11.0f;
	shore.spot = [](SceneRun &run) {
		VisualHarness &h = run.harness;
		int lane = 0;
		const glm::ivec2 shore = findShoreCrossing(h.terrain(), lane);
		if (shore.x == 0)
			throw std::runtime_error("no usable shore crossing found on any lane for seed");
		// Stand in the water just off the low beach and look BACK at the
		// coast so a single frame spans deep water → shallow →
		// foam/shoreline → terrain, with the horizon beyond.
		h.camera().setPosition(glm::vec3(float(shore.x + 12), float(TerrainGenerator::SEA_LEVEL) + 10.f,
										float(lane)));
		h.camera().setYawPitch(180.f, -16.f); // face the shore (-X), over the beach
		h.shader().fogStart = 70.f;
		h.shader().fogEnd = 150.f;
	};
	shore.invariants = [](const RgbaImage &actual, const RgbaImage &) {
		std::vector<std::string> errors;
		const RegionStats sky = rowStats(actual, 0, actual.height * 10 / 100);
		need(sky.meanLuma > 80.0, errors, "sky band too dark for day scene");
		// Water and shore share rows (the coast line runs left/right through
		// the frame), so scan row bands: water presence = some band below
		// the sky is blue-shifted; shore presence = some band clearly
		// brighter than that water (beach/terrain instead of open sea).
		bool waterFound = false;
		double waterLuma = 255.0;
		double landLuma = 0.0;
		for (int pct = 30; pct <= 75; pct += 5)
		{
			const RegionStats band =
				rowStats(actual, actual.height * pct / 100, actual.height * (pct + 5) / 100);
			landLuma = std::max(landLuma, band.meanLuma);
			if (band.meanB > band.meanR)
			{
				waterFound = true;
				waterLuma = std::min(waterLuma, band.meanLuma);
			}
		}
		need(waterFound, errors, "no water band (blue-shifted) found in the frame");
		need(landLuma > waterLuma + 5.0, errors, "no terrain band brighter than the water (shore missing)");
		return errors;
	};

	// --- sunset -------------------------------------------------------------
	// Low sun in frame, long shadows, warm fog and god rays.
	scenes.push_back({});
	SceneSpec &sunset = scenes.back();
	sunset.name = "sunset";
	sunset.seed = 4217;
	sunset.dayTime = 0.77f; // sunset atmosphere factors dominate the day fill
	sunset.time = 21.0f;
	sunset.spot = [](SceneRun &run) {
		VisualHarness &h = run.harness;
		const glm::ivec2 col = findLandColumn(h.terrain(), 70, 100, 8);
		const float ground = float(h.terrain().getTerrainSample(col.x, col.y).postErosionHeight);
		h.camera().setPosition(glm::vec3(float(col.x + 6), ground + 12.f, float(col.y)));
		h.camera().setYawPitch(180.f, 0.f); // face the sun on the horizon
		h.shader().fogStart = 45.f;
		h.shader().fogEnd = 120.f;
	};
	sunset.invariants = [](const RgbaImage &actual, const RgbaImage &) {
		std::vector<std::string> errors;
		const RegionStats all = rowStats(actual, 0, actual.height);
		need(all.meanR > all.meanB, errors, "sunset grade is not warm (R " + std::to_string(all.meanR) + " vs B " +
											   std::to_string(all.meanB) + ")");
		return errors;
	};

	// --- midnight -----------------------------------------------------------
	// Moon and stars over dark terrain: night readability and exposure floor.
	scenes.push_back({});
	SceneSpec &midnight = scenes.back();
	midnight.name = "midnight";
	midnight.seed = 4217;
	midnight.dayTime = 0.0f; // deepest night, moon toward -Z
	midnight.time = 33.0f;
	midnight.areaRadiusChunks = 5;
	midnight.spot = [](SceneRun &run) {
		VisualHarness &h = run.harness;
		const glm::ivec2 col = findLandColumn(h.terrain(), 70, 100, 8);
		const float ground = float(h.terrain().getTerrainSample(col.x, col.y).postErosionHeight);
		h.camera().setPosition(glm::vec3(float(col.x), ground + 14.f, float(col.y)));
		h.camera().setYawPitch(-90.f, 25.f); // moon azimuth, raised
		h.shader().fogStart = 30.f;
		h.shader().fogEnd = 90.f;
	};
	midnight.invariants = [](const RgbaImage &actual, const RgbaImage &) {
		std::vector<std::string> errors;
		const RegionStats all = rowStats(actual, 0, actual.height);
		need(all.meanLuma < 100.0, errors, "night scene too bright (mean luma " + std::to_string(all.meanLuma) + ")");
		need(all.meanLuma > 1.0, errors, "night scene crushed to black");
		need(all.maxLuma > 30.0, errors, "no moon/star highlights (max luma " + std::to_string(all.maxLuma) + ")");
		return errors;
	};

	// --- mob_lighting -------------------------------------------------------
	// The four passive mobs on lit terrain beside shadow: entity/terrain
	// lighting consistency. A mob-free base frame proves the mobs actually
	// contribute pixels.
	scenes.push_back({});
	SceneSpec &mob = scenes.back();
	mob.name = "mob_lighting";
	mob.seed = 4217;
	mob.dayTime = 0.33f;
	mob.time = 5.0f;
	mob.areaRadiusChunks = 4;
	mob.mobDeltaBase = true;
	mob.spot = [](SceneRun &run) {
		VisualHarness &h = run.harness;
		const glm::ivec2 col = findLandColumn(h.terrain(), 70, 100, 8);
		run.anchor = glm::vec3(float(col.x), 0.f, float(col.y));
		// Rough pre-build placement: the fixture re-seats camera and mobs on
		// the real voxel surface once the area exists.
		h.camera().setPosition(glm::vec3(float(col.x - 10), 90.f, float(col.y)));
		h.camera().setYawPitch(0.f, -14.f);
		h.shader().fogStart = 60.f;
		h.shader().fogEnd = 135.f;
	};
	mob.fixture = [](SceneRun &run) {
		VisualHarness &h = run.harness;
		ChunkManager &chunks = h.chunks();
		const int z = int(run.anchor.z);
		glm::vec3 mobCenter(0.f);
		float topGround = 0.f;
		for (size_t k = 0; k < entities::kMobSpeciesCount; ++k)
		{
			const int mx = int(run.anchor.x) + 2 + int(k) * 3;
			const int ground = groundHeightAt(chunks, mx, z);
			topGround = std::max(topGround, float(ground));
			const glm::vec3 feet(float(mx) + 0.5f, float(ground) + 1.f, float(z) + 0.5f);
			run.mobs.push_back({entities::MobSpecies(k), feet, 25.f * float(k), 0.f, 0.f, 0.f, 0.f});
			mobCenter = feet;
		}
		const glm::vec3 eye(float(int(run.anchor.x)) - 9.5f, topGround + 7.f, float(z) + 0.5f);
		h.camera().setPosition(eye);
		h.camera().setYawPitch(yawToward(eye, mobCenter), -14.f);
	};
	mob.invariants = [](const RgbaImage &actual, const RgbaImage &mobFree) {
		std::vector<std::string> errors;
		need(mobFree.valid(), errors, "mob-free base frame missing");
		if (!mobFree.valid())
			return errors;
		// The mobs are small in frame (~0.5% of pixels): assert on the count
		// of strongly-changed pixels, not the frame-wide mean.
		const ImageMetrics delta = visual::compareImages(actual, mobFree, 20);
		need(delta.comparable() && delta.hotPixels > 400, errors,
			 "mobs barely changed pixels (hot pixels " + std::to_string(delta.hotPixels) + ", want > 400)");
		return errors;
	};

	// --- underwater ---------------------------------------------------------
	// Submerged camera looking up toward the shore: absorption, surface
	// from below and the underwater post tint.
	scenes.push_back({});
	SceneSpec &underwater = scenes.back();
	underwater.name = "underwater";
	underwater.seed = 4217;
	underwater.dayTime = 0.35f;
	underwater.time = 11.0f;
	underwater.areaRadiusChunks = 4;
	underwater.underwater = true;
	underwater.spot = [](SceneRun &run) {
		VisualHarness &h = run.harness;
		const glm::ivec2 col = findDeepWaterColumn(h.terrain(), 40);
		h.camera().setPosition(glm::vec3(float(col.x), float(TerrainGenerator::SEA_LEVEL) - 4.f, float(col.y)));
		h.camera().setYawPitch(180.f, 18.f); // back toward land, slightly up
		h.shader().fogStart = 20.f;
		h.shader().fogEnd = 70.f;
	};
	underwater.invariants = [](const RgbaImage &actual, const RgbaImage &) {
		std::vector<std::string> errors;
		const RegionStats all = rowStats(actual, 0, actual.height);
		need(all.meanB > all.meanR, errors, "underwater frame not blue-shifted");
		need(all.meanLuma > 2.0 && all.meanLuma < 200.0, errors,
			 "underwater exposure out of range (mean luma " + std::to_string(all.meanLuma) + ")");
		return errors;
	};

	// --- auto_exposure_noon --------------------------------------------------
	// The exact noon_terrain viewpoint/atmosphere through the AUTO exposure
	// path (issue #140): same pinned meter, adaptation seeded from the manual
	// exposure. Existing golden references stay pinned to fixed exposure; auto
	// mode gets dedicated scenes. See the double-render block in main for the
	// off->on seeding that keeps the bit-identical check meaningful here.
	scenes.push_back({});
	SceneSpec &autoNoon = scenes.back();
	autoNoon.name = "auto_exposure_noon";
	autoNoon.seed = 4217;
	autoNoon.dayTime = 0.30f;
	autoNoon.time = 11.0f;
	autoNoon.autoExposure = true;
	autoNoon.spot = spotNoonTerrain;

	// --- auto_exposure_cave --------------------------------------------------
	// The cave_emissive room positioning with the lava kept out: a sealed,
	// unlit interior whose meter reads near the clipping floor, so the
	// adapted exposure climbs toward the max-EV clamp (+4 EV with defaults).
	scenes.push_back({});
	SceneSpec &autoCave = scenes.back();
	autoCave.name = "auto_exposure_cave";
	autoCave.seed = 9001;
	autoCave.dayTime = 0.25f; // irrelevant inside the enclosed room
	autoCave.time = 3.0f;
	autoCave.areaRadiusChunks = 3;
	autoCave.autoExposure = true;
	autoCave.spot = [](SceneRun &run) {
		VisualHarness &h = run.harness;
		run.anchor = findCaveAnchor(h.terrain());
		h.camera().setPosition(run.anchor + glm::vec3(-3.2f, 1.5f, -3.2f));
		h.camera().setYawPitch(45.f, -12.f); // look across the (dark) room
	};
	autoCave.fixture = [](SceneRun &run) {
		carveCaveRoom(run.harness.chunks(), run.anchor);
		run.worldEdited = true;
	};

	return scenes;
}

// ---------------------------------------------------------------------------
// Reference/artifact IO.
// ---------------------------------------------------------------------------

std::string formatMetrics(const ImageMetrics &m, const CompareThresholds &t, bool pass)
{
	std::ostringstream out;
	out << "mean_abs=" << m.meanAbsError << " (max " << t.maxMeanAbsError << ")\n"
		<< "rms=" << m.rmsError << " (max " << t.maxRmsError << ")\n"
		<< "max_abs=" << m.maxAbsError << "\n"
		<< "hot_pixels=" << m.hotPixels << " ratio=" << m.hotPixelRatio << " (max " << t.maxHotPixelRatio
		<< ", threshold " << t.hotPixelThreshold << ")\n"
		<< "verdict=" << (pass ? "PASS" : "FAIL") << "\n";
	return out.str();
}

void writeText(const fs::path &path, const std::string &text)
{
	fs::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary);
	file << text;
}

// ---------------------------------------------------------------------------
// AO camera-motion stability (issue #138): render the noon_terrain setup at
// two nearby camera poses under three configurations — SSAO on (composited
// frame), SSAO off (parallax baseline), and the isolated final-AO buffer via
// the SSAO debug view. The composited no-AO delta is the parallax baseline
// for a smoke comparison; the isolated AO-buffer delta is the targeted
// crawl/shimmer measure (its absolute motion delta must stay small — the
// composited comparison alone cannot separate AO noise from the overall
// darkening AO applies). Purely numeric — no reference images involved.
// ---------------------------------------------------------------------------

int runAoMotionCheck(VisualHarness &harness, const std::vector<SceneSpec> &scenes,
					 const fs::path &outDir)
{
	const SceneSpec *motionScene = nullptr;
	for (const SceneSpec &scene : scenes)
		if (std::string(scene.name) == "noon_terrain")
			motionScene = &scene;
	if (!motionScene)
		return 0;

		std::vector<std::string> errors;
		RgbaImage aoBase, aoMoved, rawBase, rawMoved, dbgBase, dbgMoved;
	try
	{
		harness.beginScene(motionScene->seed);
		SceneRun run{harness, {}, false};
		harness.shader() = ShaderParameters{};
		harness.renderSettings() = RenderSettings{};
		harness.post() = PostProcessSettings{};
		harness.post().underwater = motionScene->underwater;
		harness.shader().dayTime = motionScene->dayTime;
		updateAtmosphereFromDayTime(harness.shader());
		if (motionScene->spot)
			motionScene->spot(run);
		harness.buildArea(harness.camera().getPosition(), motionScene->areaRadiusChunks);
		if (motionScene->fixture)
			motionScene->fixture(run);
		if (run.worldEdited)
			harness.remeshEditedChunks();

		// Second pose: 0.125 world units strafed along the camera right
		// vector, derived from yaw exactly like Camera::updateCameraVectors.
		const glm::vec3 basePos = harness.camera().getPosition();
		const float yawRad = glm::radians(harness.camera().getYaw());
		const glm::vec3 right(-std::sin(yawRad), 0.0f, std::cos(yawRad));
		const glm::vec3 movedPos = basePos + right * 0.125f;

		// Every pose/state renders twice: the bit-identical guard keeps
		// nondeterminism out of the metric (same contract as the scenes).
		const bool ssaoDefault = harness.post().ssaoEnabled;
		harness.post().ssaoEnabled = true;
		aoBase = harness.renderFrame(motionScene->time, run.mobs);
		const RgbaImage aoBaseRepeat = harness.renderFrame(motionScene->time, run.mobs);
		harness.camera().setPosition(movedPos);
		aoMoved = harness.renderFrame(motionScene->time, run.mobs);
		const RgbaImage aoMovedRepeat = harness.renderFrame(motionScene->time, run.mobs);
		harness.camera().setPosition(basePos);
		harness.post().ssaoEnabled = false;
		rawBase = harness.renderFrame(motionScene->time, run.mobs);
		const RgbaImage rawBaseRepeat = harness.renderFrame(motionScene->time, run.mobs);
		harness.camera().setPosition(movedPos);
		rawMoved = harness.renderFrame(motionScene->time, run.mobs);
		const RgbaImage rawMovedRepeat = harness.renderFrame(motionScene->time, run.mobs);
		harness.camera().setPosition(basePos);
		harness.post().ssaoEnabled = ssaoDefault;

		// Final-AO buffer (debug view isolates the AO term from lighting).
		// The composited comparison below cannot fully separate AO crawling
		// from parallax darkening (AO multiplies the frame and lowers its
		// amplitudes), so this absolute metric on the AO output itself is the
		// targeted crawl measure; the composited ratio stays as a smoke check.
		const int debugViewDefault = harness.post().ssaoDebugView;
		harness.post().ssaoEnabled = true;
		harness.post().ssaoDebugView = 1; // AO (final) grayscale
		dbgBase = harness.renderFrame(motionScene->time, run.mobs);
		const RgbaImage dbgBaseRepeat = harness.renderFrame(motionScene->time, run.mobs);
		harness.camera().setPosition(movedPos);
		dbgMoved = harness.renderFrame(motionScene->time, run.mobs);
		const RgbaImage dbgMovedRepeat = harness.renderFrame(motionScene->time, run.mobs);
		harness.camera().setPosition(basePos);
		harness.post().ssaoDebugView = debugViewDefault;

		need(aoBase.valid() && aoMoved.valid() && rawBase.valid() && rawMoved.valid() &&
				 dbgBase.valid() && dbgMoved.valid(),
			 errors, "render produced an invalid image");
		if (dbgBase.valid() && dbgBaseRepeat.valid() &&
			!std::equal(dbgBase.pixels.begin(), dbgBase.pixels.end(), dbgBaseRepeat.pixels.begin()))
			errors.push_back("AO-debug base pose frames differ (nondeterministic render)");
		if (dbgMoved.valid() && dbgMovedRepeat.valid() &&
			!std::equal(dbgMoved.pixels.begin(), dbgMoved.pixels.end(), dbgMovedRepeat.pixels.begin()))
			errors.push_back("AO-debug moved pose frames differ (nondeterministic render)");
		if (aoBase.valid() && aoBaseRepeat.valid() &&
			!std::equal(aoBase.pixels.begin(), aoBase.pixels.end(), aoBaseRepeat.pixels.begin()))
			errors.push_back("AO-on base pose frames differ (nondeterministic render)");
		if (aoMoved.valid() && aoMovedRepeat.valid() &&
			!std::equal(aoMoved.pixels.begin(), aoMoved.pixels.end(), aoMovedRepeat.pixels.begin()))
			errors.push_back("AO-on moved pose frames differ (nondeterministic render)");
		if (rawBase.valid() && rawBaseRepeat.valid() &&
			!std::equal(rawBase.pixels.begin(), rawBase.pixels.end(), rawBaseRepeat.pixels.begin()))
			errors.push_back("AO-off base pose frames differ (nondeterministic render)");
		if (rawMoved.valid() && rawMovedRepeat.valid() &&
			!std::equal(rawMoved.pixels.begin(), rawMoved.pixels.end(), rawMovedRepeat.pixels.begin()))
			errors.push_back("AO-off moved pose frames differ (nondeterministic render)");

			if (errors.empty())
			{
				const ImageMetrics aoOn = visual::compareImages(aoBase, aoMoved, 20);
				const ImageMetrics aoOff = visual::compareImages(rawBase, rawMoved, 20);
				need(aoOn.comparable() && aoOff.comparable(), errors, "motion frames not comparable");
				if (aoOn.comparable() && aoOff.comparable())
				{
					// Smoke PASS: AO-on composited motion delta stays within
					// the no-AO parallax baseline plus 15% / 1-LSB slack.
					const bool pass = aoOn.meanAbsError <= aoOff.meanAbsError * 1.15 + 1.0 / 255.0;

					// Targeted metrics on the isolated AO buffer (grayscale —
					// red channel), per pixel: a global MAE bound alone can
					// hide a small strongly-unstable region, so also bound
					// the tail (p99) and the fraction of moving pixels.
					std::vector<double> aoDeltas;
					aoDeltas.reserve(dbgBase.pixelCount());
					for (size_t i = 0; i + 3 < dbgBase.pixels.size(); i += 4)
						aoDeltas.push_back(std::abs(double(dbgBase.pixels[i]) - double(dbgMoved.pixels[i])) / 255.0);
					std::sort(aoDeltas.begin(), aoDeltas.end());
					const auto fractionAbove = [&](double threshold255) {
						if (aoDeltas.empty())
							return 0.0;
						const auto it = std::lower_bound(aoDeltas.begin(), aoDeltas.end(), threshold255 / 255.0);
						return double(aoDeltas.end() - it) / double(aoDeltas.size());
					};
					const double aoMean = aoDeltas.empty()
											  ? 0.0
											  : std::accumulate(aoDeltas.begin(), aoDeltas.end(), 0.0) / double(aoDeltas.size());
					const double aoP99 = aoDeltas.empty()
											 ? 0.0
											 : aoDeltas[size_t(0.99 * double(aoDeltas.size() - 1))];
					const double fracAbove10 = fractionAbove(10.0);
					const double fracAbove20 = fractionAbove(20.0);
					const bool dbgPass = aoMean <= 20.0 / 255.0 && aoP99 <= 40.0 / 255.0 && fracAbove20 <= 0.01;

					const double aoOn255 = aoOn.meanAbsError * 255.0;
					const double aoOff255 = aoOff.meanAbsError * 255.0;
					const double ratio =
						aoOff.meanAbsError > 1e-12 ? aoOn.meanAbsError / aoOff.meanAbsError : 0.0;
					std::ostringstream line;
					line << std::fixed << std::setprecision(3);
					line << "[motion] " << motionScene->name << " aoOn=" << aoOn255
						 << "/255 aoOff=" << aoOff255 << "/255 ratio=" << ratio
						 << " aoDebug: mean=" << aoMean * 255.0 << " p99=" << aoP99 * 255.0
						 << " frac>10=" << fracAbove10 * 100.0 << "%"
						 << " frac>20=" << fracAbove20 * 100.0 << "% -> "
						 << (pass && dbgPass ? "PASS" : "FAIL");
					std::cout << line.str() << "\n";
					need(pass, errors, "AO-on motion delta exceeds the no-AO baseline (AO crawling/shimmer)");
					need(dbgPass, errors,
						 "isolated AO buffer motion metrics out of bounds (mean/p99/moving-fraction)");
				}
			}
	}
	catch (const std::exception &e)
	{
		errors.push_back(std::string("exception: ") + e.what());
	}

	if (errors.empty())
		return 0;

	for (const std::string &error : errors)
		std::cerr << "  FAIL ao_motion: " << error << "\n";
	const fs::path checkOut = outDir / "ao_motion";
	visual::writePng((checkOut / "ao_on_base.png").string(), aoBase);
	visual::writePng((checkOut / "ao_on_moved.png").string(), aoMoved);
	visual::writePng((checkOut / "ao_off_base.png").string(), rawBase);
	visual::writePng((checkOut / "ao_off_moved.png").string(), rawMoved);
	std::string report;
	for (const std::string &error : errors)
		report += error + "\n";
	writeText(checkOut / "errors.txt", report);
	return 1;
}


// Explicit diagnostic mode: never reads or rewrites golden references.
int runWaterAudit(VisualHarness &h, const fs::path &out) {
    fs::create_directories(out);
    h.beginScene(4217);
    h.camera().setPosition({20.f, 108.f, 0.f});
    h.camera().setYawPitch(180.f, -12.f);
    h.buildArea(h.camera().getPosition(), 4);
    // Closed lake, shallow beach, cliff/tree line, bridge and submerged details.
    for (int x = -24; x <= 40; ++x)
        for (int z = -28; z <= 28; ++z)
            for (int y = 98; y <= 125; ++y) {
                TextureType block = AIR;
                int floor = x < -12 ? 104 : (x < -6 ? 102 : 98);
                if (y <= floor) block = SAND;
                else if (y <= 104) block = WATER;
                if (x >= -20 && x <= -17 && y <= 115) block = STONE;
                if (x >= -14 && x <= -12 && z >= -5 && z <= -3 && y <= 115) block = OAK_LOG;
                if (x >= -16 && x <= -10 && z >= -7 && z <= -1 && y >= 114 && y <= 118) block = OAK_LEAVES;
                if (x >= -2 && x <= 2 && z >= -10 && z <= 10 && y == 110) block = STONE;
                if (x == 8 && z % 4 == 0 && y == 99) block = SEAGRASS;
                if (x == 10 && z % 5 == 0 && y >= 99 && y <= 102) block = y == 102 ? KELP_TOP : KELP;
                h.chunks().placeVoxel(glm::vec3(x, y, z), block);
            }
    h.remeshEditedChunks();
    int errors = 0;
    const long baseline = h.validationErrors();
    // Three interleaved sweeps (ascending / descending / ascending preset
    // order) average out GPU clock ramp and thermal drift that biased a
    // single ordered pass; the published number is the median sweep mean.
    auto medianOf3 = [](double a, double b, double c) {
        return a + b + c - std::max(a, std::max(b, c)) - std::min(a, std::min(b, c));
    };
    const int sweepOrder[3][4] = {{0, 1, 2, 3}, {3, 2, 1, 0}, {0, 1, 2, 3}};
    double sweepWater[3][4] = {};
    double sweepFrame[3][4] = {};
    int sweepSamples[3][4] = {};
    for (int sweep = 0; sweep < 3; ++sweep)
        for (int position = 0; position < 4; ++position) {
            const int tier = sweepOrder[sweep][position];
            h.post().applyPreset(static_cast<GraphicsQualityPreset>(tier));
            h.renderer().applyShadowMapSize(h.post().shadowMapSize);
            h.shader().dayTime = 0.35f;
            updateAtmosphereFromDayTime(h.shader());
            double water = 0, frame = 0;
            int samples = 0;
            for (int i = 0; i < 12; ++i) {
                const auto img = h.renderFrame(11.f, {});
                if (!img.valid() || h.lastNonFiniteSamples()) ++errors;
                if (sweep == 2 && i == 11 &&
                    !visual::writePng((out / ("tier_" + std::to_string(tier) + ".png")).string(), img))
                    ++errors;
                const auto &gpu = h.gpuSample();
                if (i >= 4 && gpu.present[size_t(GpuPass::Water)] && gpu.present[size_t(GpuPass::Frame)]) {
                    water += gpu.ms[size_t(GpuPass::Water)]; frame += gpu.ms[size_t(GpuPass::Frame)]; ++samples;
                }
            }
            if (samples == 0) ++errors;
            sweepWater[sweep][tier] = water / std::max(samples, 1);
            sweepFrame[sweep][tier] = frame / std::max(samples, 1);
            sweepSamples[sweep][tier] = samples;
        }
    std::ostringstream report;
    report << "tier,width,height,water_ms,frame_ms,samples\n";
    for (int tier = 0; tier < 4; ++tier) {
        std::cout << "tier " << tier << " sweep means (ms): water "
                  << sweepWater[0][tier] << '/' << sweepWater[1][tier] << '/' << sweepWater[2][tier]
                  << ", frame " << sweepFrame[0][tier] << '/' << sweepFrame[1][tier] << '/'
                  << sweepFrame[2][tier] << '\n';
        const int totalSamples = sweepSamples[0][tier] + sweepSamples[1][tier] + sweepSamples[2][tier];
        report << tier << ',' << h.extent().width << ',' << h.extent().height << ','
               << medianOf3(sweepWater[0][tier], sweepWater[1][tier], sweepWater[2][tier]) << ','
               << medianOf3(sweepFrame[0][tier], sweepFrame[1][tier], sweepFrame[2][tier]) << ','
               << totalSamples << '\n';
    }
    // Hold post and shadow quality fixed: only toggle SSR to prove scene contribution.
    h.post().qualityPreset = GraphicsQualityPreset::Medium;
    const auto skyOnly = h.renderFrame(11.f, {});
    h.post().qualityPreset = GraphicsQualityPreset::High;
    const auto ssr = h.renderFrame(11.f, {});
    const auto repeated = h.renderFrame(11.f, {});
    if (ssr.pixels != repeated.pixels) { ++errors; std::cerr << "SSR is not deterministic\n"; }
    h.post().qualityPreset = GraphicsQualityPreset::Low;
    const auto unshadowed = h.renderFrame(11.f, {});
    const auto shadowDelta = visual::compareImages(skyOnly, unshadowed, 2);
    if (shadowDelta.hotPixels < 10) { ++errors; std::cerr << "Water shadows did not change scene pixels\n"; }
    h.post().qualityPreset = GraphicsQualityPreset::High;
    auto delta = visual::compareImages(ssr, skyOnly, 2);
    if (delta.hotPixels < 10) { ++errors; std::cerr << "SSR did not change scene pixels\n"; }
    visual::writePng((out / "ssr_difference.png").string(), visual::makeDiffImage(ssr, skyOnly));

    // Temporal stability probe: freeze time so wave animation cannot mask
    // crawling, strafe the camera 0.125 world units and measure the frame
    // pair with SSR (High) and without (Medium; water shadows stay on in
    // both, so SSR is the only difference). The SSR-off pair is the
    // parallax baseline — mirrored views legitimately move more than the
    // direct view under the same strafe. Repeated at wave strength 0.25;
    // the gating probe below covers the 0.45 extreme.
    const float defaultWaveStrength = h.shader().waterWaveStrength;
    const glm::vec3 anchorPos = h.camera().getPosition();
    const float anchorYaw = glm::radians(h.camera().getYaw());
    const glm::vec3 strafedPos =
        anchorPos + glm::vec3(-std::sin(anchorYaw), 0.0f, std::cos(anchorYaw)) * 0.125f;
    auto frozenStrafeDelta = [&](bool ssrOn, float waveStrength) {
        h.shader().waterWaveStrength = waveStrength;
        h.post().qualityPreset = ssrOn ? GraphicsQualityPreset::High : GraphicsQualityPreset::Medium;
        const auto anchor = h.renderFrame(11.f, {});
        const auto anchorRepeat = h.renderFrame(11.f, {});
        h.camera().setPosition(strafedPos);
        const auto strafed = h.renderFrame(11.f, {});
        h.camera().setPosition(anchorPos);
        if (!anchor.valid() || !strafed.valid() || anchorRepeat.pixels != anchor.pixels ||
            h.lastNonFiniteSamples())
            ++errors;
        return visual::compareImages(strafed, anchor, 8);
    };
    auto stabilityCheck = [&](const char *label, const auto &on, const auto &off) {
        if (!on.comparable() || !off.comparable())
        {
            ++errors;
            std::cerr << label << ": stability frames incomparable\n";
            return;
        }
        std::cout << "stability " << label << ": on mean=" << on.meanAbsError
                  << " hot=" << on.hotPixelRatio << " | off mean=" << off.meanAbsError
                  << " hot=" << off.hotPixelRatio << '\n';
        // Measured SSR-on overhead on the reference GPU is ~1.5% hot / 0.0015
        // mean over the SSR-off baseline; the bounds keep a ~3x margin while
        // still catching explosive crawling or shimmer.
        if (on.hotPixelRatio > off.hotPixelRatio + 0.05 ||
            on.meanAbsError > off.meanAbsError + 0.006)
        {
            ++errors;
            std::cerr << label << ": SSR unstable under frozen-time strafe\n";
        }
    };
    const auto onDefault = frozenStrafeDelta(true, defaultWaveStrength);
    const auto offDefault = frozenStrafeDelta(false, defaultWaveStrength);
    const auto onHighWave = frozenStrafeDelta(true, 0.25f);
    const auto offHighWave = frozenStrafeDelta(false, 0.25f);
    stabilityCheck("wave_default", onDefault, offDefault);
    stabilityCheck("wave_0.25", onHighWave, offHighWave);

    // Gating regression probe: with wave strength well above the default
    // (the slider reaches 0.5), a top face must keep both its scene
    // reflection and its shadow reception — gating on the wave-animated
    // shading normal would silently drop SSR and CSM reception across much
    // of the surface.
    h.shader().waterWaveStrength = 0.45f;
    h.post().qualityPreset = GraphicsQualityPreset::Low;
    const auto highWaveUnshadowed = h.renderFrame(11.f, {});
    h.post().qualityPreset = GraphicsQualityPreset::Medium;
    const auto highWaveSky = h.renderFrame(11.f, {});
    h.post().qualityPreset = GraphicsQualityPreset::High;
    const auto highWaveSsr = h.renderFrame(11.f, {});
    h.shader().waterWaveStrength = defaultWaveStrength;
    h.post().qualityPreset = GraphicsQualityPreset::High;
    const auto ssrWaveDelta = visual::compareImages(highWaveSsr, highWaveSky, 2);
    const auto shadowWaveDelta = visual::compareImages(highWaveSky, highWaveUnshadowed, 2);
    std::cout << "wave 0.45 deltas (hot pixels): ssr " << ssrWaveDelta.hotPixels << " vs default "
              << delta.hotPixels << " | shadows " << shadowWaveDelta.hotPixels << " vs default "
              << shadowDelta.hotPixels << '\n';
    // Reference margins (RTX 4070 Ti, 1080p, measured): with geometric
    // gating the wave-0.45 SSR delta keeps ~61% of the default-wave delta
    // (the rest is legitimate wave-tilt fade) while shading-normal gating
    // collapses it to ~15% — the SSR leg is the primary detector at a 40%
    // threshold. The shadow leg is a safety net for shadow-reception
    // collapse and is not confounded by wave tilt (fixed ~90% of default).
    if ((ssrWaveDelta.comparable() && delta.comparable() &&
         double(ssrWaveDelta.hotPixels) < double(delta.hotPixels) * 0.4) ||
        (shadowWaveDelta.comparable() && shadowDelta.comparable() &&
         double(shadowWaveDelta.hotPixels) < double(shadowDelta.hotPixels) * 0.5))
    {
        ++errors;
        std::cerr << "water SSR or shadow contribution collapses at wave strength 0.45 (gating must use the geometric normal)\n";
    }
    visual::writePng((out / "ssr_wave045_difference.png").string(),
                     visual::makeDiffImage(highWaveSsr, highWaveSky));

    const char *names[] = {"lake", "river_edge", "bridge", "shore", "foreground", "sunset", "moon", "surface_crossing", "kelp"};
    for (int scene = 0; scene < 9; ++scene) {
        h.shader().dayTime = scene == 5 ? 0.77f : scene == 6 ? 0.0f : 0.35f;
        updateAtmosphereFromDayTime(h.shader());
        for (int i = 0; i < 12; ++i) {
            float y = scene == 7 ? 104.5f + float(i - 6) * 0.15f : scene == 8 ? 102.f : 108.f;
            float x = scene == 2 ? 5.f : scene == 3 ? 0.f : 20.f;
            float z = scene == 1 ? 12.f : scene == 4 ? -6.f : 0.f;
            h.camera().setPosition({x, y, z + float(i) * 0.06f});
            h.camera().setYawPitch(180.f + float(i) * 0.15f, scene == 8 ? -20.f : -12.f);
            h.post().underwater = y < 105.f;
            const auto img = h.renderFrame(11.f + float(i) / 30.f, {});
            if (!img.valid() || h.lastNonFiniteSamples()) ++errors;
            if (!visual::writePng((out / (std::string(names[scene]) + "_" + std::to_string(i) + ".png")).string(), img)) ++errors;
        }
    }
    if (h.validationErrors() != baseline) ++errors;
    writeText(out / "timings.csv", report.str());
    std::cout << report.str() << "water audit errors=" << errors << '\n';
    return errors ? 1 : 0;
}

// Temporal auto-exposure adaptation check (issue #140). GPU-exercised: drives
// the production metering + exposure_adapt passes over many consecutive
// frames in a sealed dark room and validates the CPU debug readout
// (WorldRenderer::exposureReadout, copied from the frame slot's SSBO during
// recordExposure after the slot's fence was waited).
//
// Readout lag: recordExposure copies the state BEFORE the frame's adapt pass,
// so the readout observed after frame k is the GPU state after frame k-1 —
// every sampled sequence below therefore renders one throwaway "flush" frame
// before trusting the readout, and the observed sequence lags the rendered
// frames by exactly one step.
// ---------------------------------------------------------------------------
int runAdaptationCheck(VisualHarness &harness)
{
	std::vector<std::string> errors;
	const float kTime = 3.0f; // pinned animation time — fully static scene

	harness.beginScene(9001);
	harness.shader() = ShaderParameters{};
	harness.renderSettings() = RenderSettings{};
	harness.post() = PostProcessSettings{}; // auto exposure on (defaults)
	harness.shader().dayTime = 0.25f;		// irrelevant inside the sealed room
	updateAtmosphereFromDayTime(harness.shader());

	// cave_emissive positioning, no light sources: the meter sits far below
	// middle grey, so exposure must climb toward the max-EV clamp.
	const glm::vec3 anchor = findCaveAnchor(harness.terrain());
	harness.camera().setPosition(anchor + glm::vec3(-3.2f, 1.5f, -3.2f));
	harness.camera().setYawPitch(45.f, -12.f);
	harness.buildArea(harness.camera().getPosition(), 3);
	carveCaveRoom(harness.chunks(), anchor);
	harness.remeshEditedChunks();

	// One rendered frame = one adaptation step at the given dt; returns the
	// readout observed after the frame (the state after the PREVIOUS frame).
	const auto step = [&](float dt) {
		harness.renderer().setFrameDt(dt);
		harness.renderFrame(kTime, {});
		return harness.renderer().exposureReadout();
	};

	// Rising-edge re-seed (PostStack::recordExposure: seed on
	// useAuto && !m_lastAutoEnabled, and the adapt pass is skipped entirely
	// while auto is off): one manual frame re-arms the edge, the next auto
	// frame seeds the adaptation from the manual exposure.
	harness.post().autoExposureEnabled = false;
	step(1.f / 60.f); // discarded manual frame
	harness.post().autoExposureEnabled = true;
	step(1.f / 60.f); // seed frame — readout still pre-seed, flushed below

	if (harness.lastNonFiniteSamples() > 0)
		errors.push_back("non-finite HDR samples in the dark-room frames");

	// --- Monotonic approach, no overshoot (40 observed states, ~1.3 s) ---
	std::vector<float> adaptedLog;
	float targetExposure = 0.0f;
	for (int i = 0; i < 40; ++i)
	{
		// First sample flushes the seed frame's state out of the readout.
		const auto s = step(i == 0 ? 1.f / 60.f : 1.f / 30.f);
		adaptedLog.push_back(std::log2(std::max(s.adaptedExposure, 1e-6f)));
		targetExposure = s.targetExposure;
	}
	const float targetLog = std::log2(std::max(targetExposure, 1e-6f));
	for (size_t i = 1; i < adaptedLog.size(); ++i)
	{
		if (adaptedLog[i] < adaptedLog[i - 1] - 1e-5f)
		{
			errors.push_back("adapted exposure is not monotonic toward the target (sample " +
							 std::to_string(i) + ")");
			break;
		}
	}
	if (targetLog < adaptedLog.back() - 1e-3f)
		errors.push_back("adapted exposure overshot the target (adapted log2 " +
						 std::to_string(adaptedLog.back()) + " > target " +
						 std::to_string(targetLog) + ")");
	if (!(targetLog > adaptedLog.front()))
		errors.push_back("the dark room did not drive adaptation upward (target log2 " +
						 std::to_string(targetLog) + ")");

	// --- Frame-split independence: identical simulated time, two splits ---
	// Both runs re-seed from the same manual exposure via the off->on toggle,
	// then integrate the same simulated second: the per-step alpha
	// 1 - exp(-speed*dt) is the exact exponential integral, so 30x1/30 and
	// 60x1/60 must land on the same value modulo GPU fp32 rounding.
	const auto reseedAndRun = [&](int frames, float dt) {
		harness.post().autoExposureEnabled = false;
		step(1.f / 60.f); // re-arm the rising edge
		harness.post().autoExposureEnabled = true;
		step(1.f / 60.f); // seed frame: identical start state for both splits
		for (int i = 0; i < frames; ++i)
			step(dt);
		step(1.f / 60.f); // flush: readout now holds the state after the last frame
		return harness.renderer().exposureReadout().adaptedExposure;
	};
	const float end30 = reseedAndRun(30, 1.f / 30.f);
	const float end60 = reseedAndRun(60, 1.f / 60.f);
	if (std::abs(end30 - end60) / std::max(end30, end60) > 5e-3f)
		errors.push_back("frame-split independence violated: 30x1/30s ended at " +
						 std::to_string(end30) + ", 60x1/60s at " + std::to_string(end60));

	// --- Limit reporting: clampState must flag the max-EV clamp in the dark ---
	// With the default maxEv=+4 the room is not PROVABLY dark enough to hit
	// the clamp, so force a small clamp: the precondition check below asserts
	// metered <= -1 EV, which makes the raw target (-metered) >= +1 EV and
	// therefore guarantees the raw target rides the maxEv=+1 clamp.
	harness.post().autoExposureMaxEv = 1.0f;
	step(1.f / 60.f); // adapt pass runs with maxEv=+1; readout is one frame stale
	const auto limit = step(1.f / 60.f); // readout reflects the maxEv=+1 pass
	if (limit.meteredLogLum > -1.0f)
		errors.push_back("the sealed room is not dark (metered " +
						 std::to_string(limit.meteredLogLum) + " EV) — clamp check vacuous");
	if (limit.clampState != 2u)
		errors.push_back("dark scene must report the max-EV clamp (clampState " +
						 std::to_string(limit.clampState) + ", want 2)");
	harness.post().autoExposureMaxEv = 4.0f;

	for (const std::string &error : errors)
		std::cerr << "  FAIL auto-exposure-adaptation: " << error << "\n";
	if (errors.empty())
	{
		std::cout << "  PASS auto-exposure-adaptation\n";
		return 0;
	}
	return 1;
}

} // namespace

int main(int argc, char **argv)
{
	bool updateReferences = false;
	bool waterAudit = false;
	uint32_t auditHeight = 1080;
	bool strict = false;
	bool smoke = std::getenv("FT_VOX_VISUAL_SMOKE") != nullptr &&
				 std::string(std::getenv("FT_VOX_VISUAL_SMOKE")) == "1";
	std::vector<std::string> onlyScenes;
	fs::path refsDir = "tests/visual-references";
	fs::path outDir = "build/visual-qa";
	std::vector<std::string> positional;
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--water-audit")
			waterAudit = true;
		else if (arg == "--audit-1440")
			auditHeight = 1440;
		else if (arg == "--update-references")
			updateReferences = true;
		else if (arg == "--smoke")
			smoke = true;
		else if (arg == "--strict")
			strict = true;
		else if (arg == "--scene" && i + 1 < argc)
			onlyScenes.push_back(argv[++i]);
		else if (arg == "--refs" && i + 1 < argc)
			refsDir = argv[++i];
		else if (arg == "--out" && i + 1 < argc)
			outDir = argv[++i];
		else if (!arg.empty() && arg[0] == '-')
		{
			std::cerr << "unknown argument: " << arg << "\n";
			return 1;
		}
		else
			positional.push_back(arg);
	}
	// ctest passes <references-dir> <artifacts-dir> positionally.
	if (positional.size() > 0)
		refsDir = positional[0];
	if (positional.size() > 1)
		outDir = positional[1];
	// --strict overrides the FT_VOX_VISUAL_SMOKE environment default: strict
	// mode is the canonical-GPU gate (reference updates, dev verification);
	// ctest defaults to smoke so heterogeneous machines don't go false-red.
	if (strict)
		smoke = false;

	VisualHarness harness;
	if (!harness.initDevice(std::cout, waterAudit ? auditHeight * 16 / 9 : VisualHarness::kWidth, waterAudit ? auditHeight : VisualHarness::kHeight))
		return 77; // no Vulkan device/surface or golden contract: explicit skip

	// Validation baseline (review P1): ONLY the device/swapchain phase may
	// carry creation-time errors from injected overlays (RTSS — see
	// docs/vulkan-validation.md). Everything from WorldRenderer::init onward
	// — descriptors, pipelines, internal images, the offscreen target — must
	// be validation-clean, plus every scene render below.
	const long baselineValidation = harness.validationErrors();
	if (baselineValidation > 0)
		std::cout << "warning: " << baselineValidation
				  << " validation error(s) during device/swapchain init (see docs/vulkan-validation.md "
					 "if RTSS is loaded) — tolerated as baseline\n";

	try
	{
		harness.initRenderer(std::cout);
	}
	catch (const std::exception &e)
	{
		std::cerr << "FAIL: renderer init failed: " << e.what() << "\n";
		harness.shutdown();
		return 1;
	}
	if (harness.validationErrors() > baselineValidation)
	{
		std::cerr << "FAIL: " << harness.validationErrors() - baselineValidation
				  << " Vulkan validation error(s) during renderer init\n";
		harness.shutdown();
		return 1;
	}

	if (waterAudit) {
		int result = 1;
		try { result = runWaterAudit(harness, outDir); }
		catch (const std::exception &e) { std::cerr << "water audit failed: " << e.what() << "\n"; }
		harness.shutdown();
		return result;
	}

	if (smoke)
		std::cout << "smoke mode: tolerances widened for heterogeneous GPUs\n";

	int failures = 0;
	int updated = 0;
	int ranScenes = 0;
	std::vector<SceneSpec> scenes = buildSceneTable();
	for (SceneSpec &scene : scenes)
	{
		if (!onlyScenes.empty() &&
			std::find(onlyScenes.begin(), onlyScenes.end(), scene.name) == onlyScenes.end())
			continue;
		++ranScenes;

		const fs::path sceneOut = outDir / scene.name;
		std::cout << "[scene] " << scene.name << " (seed " << scene.seed << ", dayTime " << scene.dayTime
				  << ", time " << scene.time << ")\n";

		try
		{
			harness.beginScene(scene.seed);
			SceneRun run{harness, {}, false};
			harness.shader() = ShaderParameters{};
			harness.renderSettings() = RenderSettings{};
			harness.post() = PostProcessSettings{};
			// Existing golden references were captured with a FIXED manual
			// exposure — auto mode gets the dedicated auto_exposure_* scenes
			// (issue #140), which opt back in below via SceneSpec::autoExposure.
			harness.post().autoExposureEnabled = false;
			harness.post().underwater = scene.underwater;
			harness.shader().dayTime = scene.dayTime;
			updateAtmosphereFromDayTime(harness.shader());
			if (scene.spot)
				scene.spot(run);
			harness.buildArea(harness.camera().getPosition(), scene.areaRadiusChunks);
			if (scene.fixture)
				scene.fixture(run);
			if (run.worldEdited)
				harness.remeshEditedChunks();

			// Two identical renders must be bit-identical: pins every
			// animation/feed-forward input and catches nondeterminism before
			// it can pollute the reference comparison.
			//
			// Auto-exposure scenes (issue #140): every recorded frame advances
			// the adaptation SSBO (exposure_adapt.frag) and the composite
			// consumes it in the same frame, so back-to-back auto frames can
			// never be bit-identical while adapting. recordExposure re-seeds
			// the adaptation from the manual exposure on the auto-mode RISING
			// edge, and a seeded frame ignores the stale state entirely (its
			// output depends only on the push constants and the static
			// scene meter). So each compared frame is rendered immediately
			// after an off->on toggle: both carry the identical seed exposure
			// and the bit-identical contract stays intact. The off frames in
			// between run the manual composite and are discarded.
			const bool autoScene = scene.autoExposure;
			const auto renderComparedFrame = [&]() {
				if (autoScene)
				{
					harness.post().autoExposureEnabled = false;
					harness.renderFrame(scene.time, run.mobs); // discarded: re-arm the rising edge
					harness.post().autoExposureEnabled = true;
				}
				return harness.renderFrame(scene.time, run.mobs);
			};
			const RgbaImage first = renderComparedFrame();
			const RgbaImage actual = renderComparedFrame();
			std::vector<std::string> errors;
			need(actual.valid(), errors, "render produced an invalid image");
			if (first.valid() && actual.valid() &&
				!std::equal(first.pixels.begin(), first.pixels.end(), actual.pixels.begin()))
				errors.push_back("two identical frames differ (nondeterministic render)");
			// NaN/Inf must be caught pre-tonemap: the LDR composite already
			// quantizes non-finite HDR values into undefined bytes. The
			// harness scans the R16G16B16A16_SFLOAT scene target (fp16
			// exponent-all-ones bit pattern) after every render.
			if (harness.lastNonFiniteSamples() > 0)
				errors.push_back("HDR target contains " +
								 std::to_string(harness.lastNonFiniteSamples()) +
								 " non-finite (NaN/Inf) sample(s)");

			RgbaImage mobFree;
			if (scene.mobDeltaBase && errors.empty())
				mobFree = harness.renderFrame(scene.time, {});

			if (errors.empty() && scene.invariants)
				for (std::string &error : scene.invariants(actual, mobFree))
					errors.push_back(std::move(error));

			// Reference handling: normal runs never write references.
			const fs::path refPath = refsDir / (std::string(scene.name) + ".png");
			if (errors.empty())
			{
				const RgbaImage expected = visual::readPng(refPath.string());
				if (updateReferences)
				{
					// Update mode still produces review artifacts: the new
					// render, the OLD reference and the diff between them,
					// so the PR shows exactly what changed.
					const bool wroteActual = visual::writePng((sceneOut / "actual.png").string(), actual);
					if (!visual::writePng(refPath.string(), actual))
					{
						errors.push_back("failed to write reference " + refPath.string() +
										 (wroteActual ? "" : " (and actual.png artifact)"));
					}
					else
					{
						++updated;
						std::cout << "  [update] wrote " << refPath.string() << "\n";
						if (expected.valid())
						{
							const CompareThresholds contextThresholds = scene.thresholds;
							const ImageMetrics shift = visual::compareImages(actual, expected,
																			 contextThresholds.hotPixelThreshold);
							if (!wroteActual)
								errors.push_back("failed to write actual.png artifact");
							visual::writePng((sceneOut / "expected.png").string(), expected);
							visual::writePng((sceneOut / "diff.png").string(),
											 visual::makeDiffImage(actual, expected));
							writeText(sceneOut / "metrics.txt",
									  "reference UPDATE for " + std::string(scene.name) +
										  ": actual.png is the new golden, expected.png the previous "
										  "one; shift vs previous reference:\n" +
										  formatMetrics(shift, contextThresholds, true));
						}
					}
				}
				else if (!expected.valid())
				{
					errors.push_back("missing or corrupt reference " + refPath.string() +
									 " — regenerate explicitly with --update-references");
				}
				else
				{
					CompareThresholds thresholds = scene.thresholds;
					if (smoke)
					{
						thresholds.maxMeanAbsError *= 5.0;
						thresholds.maxRmsError *= 5.0;
						thresholds.maxHotPixelRatio = 0.10;
					}
					const ImageMetrics metrics =
						visual::compareImages(actual, expected, thresholds.hotPixelThreshold);
					const bool pass = metrics.comparable() && visual::withinTolerance(metrics, thresholds);
					if (!pass)
					{
						errors.push_back("reference mismatch: mean_abs=" + std::to_string(metrics.meanAbsError) +
										 " rms=" + std::to_string(metrics.rmsError) +
										 " hot=" + std::to_string(metrics.hotPixelRatio));
						visual::writePng((sceneOut / "actual.png").string(), actual);
						visual::writePng((sceneOut / "expected.png").string(), expected);
						visual::writePng((sceneOut / "diff.png").string(),
										 visual::makeDiffImage(actual, expected));
						writeText(sceneOut / "metrics.txt", formatMetrics(metrics, thresholds, false));
					}
				}
			}

			if (errors.empty())
			{
				std::cout << "  PASS " << scene.name << "\n";
			}
			else
			{
				++failures;
				if (actual.valid())
					visual::writePng((sceneOut / "actual.png").string(), actual);
				std::string report;
				for (const std::string &error : errors)
				{
					report += error + "\n";
					std::cerr << "  FAIL " << scene.name << ": " << error << "\n";
				}
				writeText(sceneOut / "errors.txt", report);
			}
		}
		catch (const std::exception &e)
		{
			++failures;
			std::cerr << "  FAIL " << scene.name << ": exception: " << e.what() << "\n";
		}
	}

	// "resize_check" and "auto_exposure_adaptation" are valid selections too —
	// only fail when some real scene name matched nothing.
	const bool resizeRequested =
		std::find(onlyScenes.begin(), onlyScenes.end(), "resize_check") != onlyScenes.end();
	const bool adaptationRequested =
		std::find(onlyScenes.begin(), onlyScenes.end(), "auto_exposure_adaptation") != onlyScenes.end();
	if (ranScenes == 0 && !onlyScenes.empty() && !resizeRequested && !adaptationRequested)
	{
		std::cerr << "FAIL: --scene";
		for (const std::string &name : onlyScenes)
			std::cerr << " " << name;
		std::cerr << " matched no scene (valid names: noon_terrain, cascade_transition, cave_emissive,"
					 " water_shore, sunset, midnight, mob_lighting, underwater, auto_exposure_noon,"
					 " auto_exposure_cave, auto_exposure_adaptation, resize_check)\n";
		++failures;
	}

	// Live shadow-map resize smoke (issue #137 review): render terrain+mobs
	// at 1024, resize to 2048 through the production deferred path, render
	// again — must stay deterministic and validation-clean. This is the
	// regression gate for the stale-sampler descriptor bug found during
	// development (the recreated sampler must reach the mob sets).
	if (onlyScenes.empty() || resizeRequested)
	{
		std::cout << "[resize-check] 1024 -> 2048 with terrain + mobs\n";
		try
		{
			const long before = harness.validationErrors();
			harness.beginScene(4217);
			harness.post() = PostProcessSettings{};
			// Manual exposure here too (issue #140): the adaptation would
			// advance between the two post-resize frames and trip the
			// bit-identical check — resize-check pins the shadow-map change,
			// not exposure.
			harness.post().autoExposureEnabled = false;
			harness.shader() = ShaderParameters{};
			harness.shader().dayTime = 0.30f;
			updateAtmosphereFromDayTime(harness.shader());
			harness.camera().setPosition(glm::vec3(8.f, 96.f, 8.f));
			harness.camera().setYawPitch(225.f, -25.f);
			harness.buildArea(harness.camera().getPosition(), 3);
			std::vector<entities::MobRenderState> mobs;
			mobs.push_back({entities::MobSpecies::Cow, glm::vec3(6.f, 90.f, 10.f), 0.f, 0.f, 0.f, 0.f, 0.f});
			const visual::RgbaImage beforeResize = harness.renderFrame(5.f, mobs);
			harness.post().shadowMapSize = 2048; // engine applies this deferred in the real loop
			harness.renderer().applyShadowMapSize(2048);
			const visual::RgbaImage after = harness.renderFrame(5.f, mobs);
			const visual::RgbaImage after2 = harness.renderFrame(5.f, mobs);
			if (!beforeResize.valid() || !after.valid())
			{
				std::cerr << "  FAIL resize-check: invalid render\n";
				++failures;
			}
			else if (!std::equal(after.pixels.begin(), after.pixels.end(), after2.pixels.begin()))
			{
				std::cerr << "  FAIL resize-check: nondeterministic after resize\n";
				++failures;
			}
			else if (beforeResize.pixels == after.pixels)
			{
				std::cerr << "  FAIL resize-check: 2048 render identical to 1024 (filter radius did not change)\n";
				++failures;
			}
			else if (harness.lastNonFiniteSamples() > 0)
			{
				std::cerr << "  FAIL resize-check: non-finite HDR samples after resize\n";
				++failures;
			}
			else if (harness.validationErrors() - before != 0)
			{
				std::cerr << "  FAIL resize-check: " << harness.validationErrors() - before
						  << " validation error(s) during resize rendering\n";
				++failures;
			}
			else
				std::cout << "  PASS resize-check\n";
			harness.post().shadowMapSize = 1024;
			harness.renderer().applyShadowMapSize(1024);
		}
		catch (const std::exception &e)
		{
			std::cerr << "  FAIL resize-check: exception: " << e.what() << "\n";
			++failures;
		}
	}
	// AO camera-motion stability (issue #138): cheap deterministic numeric
	// check on the noon_terrain setup — runs in both smoke and strict modes,
	// and honors a --scene filter only when it excludes noon_terrain.
	if (onlyScenes.empty() ||
		std::find(onlyScenes.begin(), onlyScenes.end(), "noon_terrain") != onlyScenes.end())
		failures += runAoMotionCheck(harness, scenes, outDir);

	// Temporal auto-exposure adaptation check (issue #140): GPU-exercised,
	// verified through the CPU debug readout. Runs with the full suite or
	// when requested by name (--scene auto_exposure_adaptation).
	if (onlyScenes.empty() || adaptationRequested)
	{
		std::cout << "[auto-exposure-adaptation] dark-room temporal adaptation\n";
		try
		{
			if (runAdaptationCheck(harness) != 0)
				++failures;
		}
		catch (const std::exception &e)
		{
			std::cerr << "  FAIL auto-exposure-adaptation: exception: " << e.what() << "\n";
			++failures;
		}
	}

	const long renderValidationErrors = harness.validationErrors() - baselineValidation;
	if (renderValidationErrors > 0)
	{
		std::cerr << "FAIL: " << renderValidationErrors << " Vulkan validation error(s) during rendering\n";
		++failures;
	}
	harness.shutdown();

	if (failures == 0)
	{
		std::cout << "ft_vox_visual_tests: OK (" << ranScenes << " scene(s) "
				  << (updateReferences ? std::to_string(updated) + " reference(s) updated"
									   : "within tolerance")
				  << ")\n";
		return 0;
	}
	std::cout << "ft_vox_visual_tests: FAILED (" << failures << " failure(s)); artifacts in "
			  << fs::absolute(outDir).string() << "\n";
	return 1;
}
