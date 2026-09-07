/// Deterministic visual-regression harness for the Vulkan graphics pipeline
/// (issue #142).
///
/// Renders a fixed set of scenes offscreen through the PRODUCTION
/// WorldRenderer pass graph (Shadow → Opaque → Water → Sky → Post) with
/// pinned world seed, camera, dayTime and animation time, then compares the
/// readback against committed tolerant references in
/// tests/visual-references/ (never bit-exact across GPU vendors).
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
#include <filesystem>
#include <fstream>
#include <functional>
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
	noon.spot = [](SceneRun &run) {
		VisualHarness &h = run.harness;
		const glm::ivec2 col = findLandColumn(h.terrain(), 70, 100, 8);
		const float ground = float(h.terrain().getTerrainSample(col.x, col.y).postErosionHeight);
		h.camera().setPosition(glm::vec3(col.x + 10.f, ground + 14.f, col.y + 10.f));
		h.camera().setYawPitch(225.f, -28.f); // look back across the anchor column
		h.shader().fogStart = 60.f;
		h.shader().fogEnd = 135.f;
	};
	noon.invariants = [](const RgbaImage &actual, const RgbaImage &) {
		std::vector<std::string> errors;
		const RegionStats sky = rowStats(actual, 0, actual.height * 15 / 100);
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
		// Thick rock so the carved room stays sealed; center the room inside
		// one chunk (local 8, 38, 8) so light BFS never crosses a border.
		const glm::ivec2 col = findLandColumn(h.terrain(), 85, 130, 8);
		const int baseX = (col.x / CHUNK_SIZE) * CHUNK_SIZE;
		const int baseZ = (col.y / CHUNK_SIZE) * CHUNK_SIZE;
		run.anchor = glm::vec3(float(baseX + 8), 38.f, float(baseZ + 8));
		h.camera().setPosition(run.anchor + glm::vec3(-3.2f, 1.5f, -3.2f));
		h.camera().setYawPitch(45.f, -12.f); // look across the lava pool
	};
	cave.fixture = [](SceneRun &run) {
		ChunkManager &chunks = run.harness.chunks();
		const int cx = int(run.anchor.x), cy = int(run.anchor.y), cz = int(run.anchor.z);
		// Squashed-sphere cavity (dy weighted 1.2): floor ends up around
		// y = 34 near the center, stepping up towards the rim.
		for (int dz = -6; dz <= 6; ++dz)
			for (int dx = -6; dx <= 6; ++dx)
				for (int dy = -5; dy <= 4; ++dy)
				{
					const float room = float(dx * dx + dz * dz) + float(dy * dy) * 1.44f;
					if (room <= 30.f)
						chunks.deleteVoxel(glm::vec3(float(cx + dx), float(cy + dy), float(cz + dz)));
				}
		// Lava pool embedded in the center floor (top face exposed by the
		// y=34 cavity), magma ring on the first carved step around it.
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

} // namespace

int main(int argc, char **argv)
{
	bool updateReferences = false;
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
		if (arg == "--update-references")
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
	if (!harness.initDevice(std::cout))
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
			const RgbaImage first = harness.renderFrame(scene.time, run.mobs);
			const RgbaImage actual = harness.renderFrame(scene.time, run.mobs);
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

	if (ranScenes == 0)
	{
		std::cerr << "FAIL: --scene";
		for (const std::string &name : onlyScenes)
			std::cerr << " " << name;
		std::cerr << " matched no scene (valid names: noon_terrain, cascade_transition, cave_emissive,"
					 " water_shore, sunset, midnight, mob_lighting, underwater)\n";
		++failures;
	}

	// Live shadow-map resize smoke (issue #137 review): render terrain+mobs
	// at 1024, resize to 2048 through the production deferred path, render
	// again — must stay deterministic and validation-clean. This is the
	// regression gate for the stale-sampler descriptor bug found during
	// development (the recreated sampler must reach the mob sets).
	if (onlyScenes.empty() || std::find(onlyScenes.begin(), onlyScenes.end(), "resize_check") != onlyScenes.end())
	{
		std::cout << "[resize-check] 1024 -> 2048 with terrain + mobs\n";
		try
		{
			const long before = harness.validationErrors();
			harness.beginScene(4217);
			harness.post() = PostProcessSettings{};
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
	std::cout << "ft_vox_visual_tests: FAILED (" << failures << " scene failure(s)); artifacts in "
			  << fs::absolute(outDir).string() << "\n";
	return 1;
}
