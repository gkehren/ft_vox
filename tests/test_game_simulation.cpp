// test_game_simulation — CPU-only GameSimulation acceptance (issue #208,
// Epic #204 Phase D). Links ONLY ft_vox_core: no SDL, no Vulkan device, no
// ImGui, no render chunk handles, no Camera. Proves the issue's acceptance
// list end-to-end without a graphics runtime:
//
//   * headless fixture: fixed seed + scripted inputs/edits -> equivalent
//     logical player/mob/voxel output across repeated runs;
//   * physics edges: jump / coyote / swimming / flight toggle, unknown and
//     unloaded voxel collision, spawn recovery, dropped-step cap, frame
//     delta spikes, suspension without catch-up burst;
//   * player-aware placement rule shared with the graphical host (issue
//     #209): solid blocks cannot entomb the authoritative body;
//   * mobs: fixed-step wandering, group spawn/retire, no NaN, correct
//     previous/current interpolation fields, reproducible per-seed
//     decisions with controlled residency and clock;
//   * world reload + save/player restore without camera or renderer;
//     failures keep the old world alive;
//   * independent 120 Hz player / 60 Hz mob update rates with no frame
//     rendered; bounded read-only value snapshots.
//
// Style mirrors the other headless suites: a CHECK macro, plain main().

#include <Sim/GameSimulation.hpp>
#include <World/WorldCollisionView.hpp>
#include <World/WorldMobView.hpp>
#include <Core/ThreadPool.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static int g_fails = 0;
static int g_checks = 0;
#define CHECK(cond, msg)                                                       \
	do                                                                         \
	{                                                                          \
		++g_checks;                                                            \
		if (!(cond))                                                           \
		{                                                                      \
			std::cerr << "FAIL: " << msg << " (" << __LINE__ << ")\n";          \
			++g_fails;                                                         \
		}                                                                      \
	} while (0)

namespace
{
std::filesystem::path makeTempDir(const char *tag)
{
	const auto base = std::filesystem::temp_directory_path() /
					  ("ft_vox_sim_" + std::string(tag) + "_" +
					   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(base);
	return base;
}

struct TempRoot
{
	std::filesystem::path path;
	explicit TempRoot(const char *tag) : path(makeTempDir(tag)) {}
	~TempRoot()
	{
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
	}
};

void pumpUntilIdle(VoxelWorld &world)
{
	for (int i = 0; i < 30000 && world.asyncBusy(); ++i)
	{
		world.pump();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	world.pump();
}

// Highest occupied cell of a column: solid terrain OR fluid (water/lava).
// Fixtures build above this so their edits always target AIR (a place into
// water is a logical no-op refusal, not an edit).
int topOccupiedY(const VoxelWorld &world, int x, int z)
{
	WorldCollisionView view(world);
	for (int y = WORLD_HEIGHT - 1; y >= 0; --y)
	{
		const auto cell = view.sample({x, y, z});
		if (cell.available && (cell.solid || cell.medium != physics::Medium::Air))
			return y;
	}
	return -1;
}

bool finite(const glm::dvec3 &v)
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// ---------------------------------------------------------------------------
// 1. Headless scripted determinism: identical logical player/mob/voxel output
//    across repeated runs (controlled residency: synchronous bootstrap only,
//    no async streaming; controlled clock: fixed 1/60 host frames).
// ---------------------------------------------------------------------------
struct ScriptedRun
{
	glm::dvec3 playerPos{0.0}, playerVel{0.0};
	bool grounded{false};
	bool waitingForTerrain{false};
	uint64_t editRevision{0};
	size_t chunkCount{0};
	size_t mobCount{0};
	std::vector<sim::MobSnapshot> mobs;
	uint8_t editedVoxel{0};
	uint8_t deletedVoxel{0};
};

ScriptedRun scriptedRun(ThreadPool &pool)
{
	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 1337; // transient: no persistence in the determinism fixture
	const glm::dvec3 center(8.0, 100.0, 8.0);
	const glm::dvec3 boot = sim.initialize(cfg, center);
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);

	const glm::dvec3 spawn = sim.playerFeet();
	const glm::vec3 editA(static_cast<float>(spawn.x + 3.0), static_cast<float>(spawn.y + 4.0),
						  static_cast<float>(spawn.z + 3.0));
	const glm::vec3 editB(static_cast<float>(spawn.x - 3.0), static_cast<float>(spawn.y + 4.0),
						  static_cast<float>(spawn.z + 3.0));

	for (int i = 0; i < 600; ++i)
	{
		sim::PlayerCommand cmd{};
		const int leg = (i / 120) % 4;
		cmd.move = leg == 0 ? glm::dvec2(1, 0) : leg == 1 ? glm::dvec2(0, 1)
				 : leg == 2 ? glm::dvec2(-1, 0)
							: glm::dvec2(0, -1);
		cmd.sprint = (i % 240) >= 120;
		cmd.jumpPressed = (i == 30 || i == 310);
		sim.advancePlayer(1.0 / 60.0, cmd, false);
		sim.advanceMobs(1.0 / 60.0, sim.playerFeet(), 40.0, false);
		if (i == 100)
			sim.placeVoxel(editA, STONE);
		if (i == 200)
			sim.placeVoxel(editB, DIRT);
		if (i == 300)
			sim.deleteVoxel(editA);
		sim.pumpWorld();
	}

	ScriptedRun out;
	const auto player = sim.playerSnapshot();
	out.playerPos = player.position;
	out.playerVel = player.velocity;
	out.grounded = player.grounded;
	out.waitingForTerrain = player.waitingForTerrain;
	const auto world = sim.worldSnapshot();
	out.editRevision = world.editRevision;
	out.chunkCount = world.chunkCount;
	sim.mobSnapshots(out.mobs);
	out.mobCount = sim.mobs().mobs().size();
	WorldCollisionView view(*sim.world());
	TextureType type = AIR;
	view.sampleWithType(glm::ivec3(glm::floor(editB)), &type);
	out.editedVoxel = static_cast<uint8_t>(type);
	view.sampleWithType(glm::ivec3(glm::floor(editA)), &type);
	out.deletedVoxel = static_cast<uint8_t>(type);
	return out;
}

void testScriptedDeterminism(ThreadPool &pool)
{
	const ScriptedRun a = scriptedRun(pool);
	const ScriptedRun b = scriptedRun(pool);
	CHECK(a.playerPos == b.playerPos, "scripted player position identical across runs");
	CHECK(a.playerVel == b.playerVel, "scripted player velocity identical across runs");
	CHECK(a.grounded == b.grounded, "scripted grounded flag identical across runs");
	CHECK(a.waitingForTerrain == b.waitingForTerrain, "scripted terrain-wait flag identical");
	CHECK(a.editRevision == b.editRevision && a.editRevision > 1,
		  "scripted logical edit revision identical and advanced");
	CHECK(a.chunkCount == b.chunkCount, "scripted residency identical (controlled bootstrap)");
	CHECK(a.editedVoxel == static_cast<uint8_t>(DIRT), "scripted place edit is authoritative");
	CHECK(a.deletedVoxel != static_cast<uint8_t>(STONE), "scripted delete edit is authoritative");
	CHECK(a.mobCount == b.mobCount, "scripted mob population identical across runs");
	CHECK(a.mobs.size() == b.mobs.size(), "scripted mob snapshot count identical");
	bool identical = a.mobs.size() == b.mobs.size();
	for (size_t i = 0; identical && i < a.mobs.size(); ++i)
		identical = a.mobs[i].species == b.mobs[i].species && a.mobs[i].position == b.mobs[i].position &&
					a.mobs[i].yaw == b.mobs[i].yaw && a.mobs[i].gait == b.mobs[i].gait &&
					a.mobs[i].stride == b.mobs[i].stride;
	CHECK(identical, "scripted mob transforms/animation identical across runs");
}

// ---------------------------------------------------------------------------
// 2. Independent fixed update rates (no frame rendered): player 120 Hz,
//    mobs 60 Hz, bounded catch-up.
// ---------------------------------------------------------------------------
void testFixedRates(ThreadPool &pool)
{
	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 1337;
	const glm::dvec3 boot = sim.initialize(cfg, glm::dvec3(8.0, 100.0, 8.0));
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);

	CHECK(physics::PlayerController::fixedStep == 1.0 / sim::kPlayerTickHz,
		  "player controller pinned to the 120 Hz contract");
	CHECK(sim.mobSettings().fixedStep == 1.0 / sim::kMobTickHz,
		  "mob settings default to the 60 Hz contract");

	sim::PlayerCommand cmd{};
	sim.advancePlayer(1.0 / 30.0, cmd, false);
	CHECK(sim.playerSnapshot().metrics.steps == 4, "120 Hz player: 4 steps per 1/30 s frame");
	sim.advancePlayer(1.0 / 120.0, cmd, false);
	CHECK(sim.playerSnapshot().metrics.steps == 1, "120 Hz player: 1 step per 1/120 s frame");
	sim.advancePlayer(1.0 / 240.0, cmd, false);
	CHECK(sim.playerSnapshot().metrics.steps == 0, "sub-step frame accumulates (no step)");
	sim.advancePlayer(1.0 / 240.0, cmd, false);
	CHECK(sim.playerSnapshot().metrics.steps == 1, "accumulated sub-steps fire exactly once");

	// Mob catch-up cap: 1 s requests 60 steps, maxSteps bounds execution and
	// the excess is dropped (never an unbounded burst).
	const uint64_t droppedBefore = sim.mobs().droppedSteps();
	sim.advanceMobs(1.0, sim.playerFeet(), 40.0, false);
	CHECK(sim.mobs().droppedSteps() > droppedBefore, "60 Hz mobs drop steps beyond the cap");

	// The gameplay toggle is sim-owned: disabled mobs freeze even when the
	// host does not pass a suspension flag.
	sim.mobsEnabled() = false;
	const size_t popBefore = sim.mobs().mobs().size();
	const uint64_t droppedBeforeToggle = sim.mobs().droppedSteps();
	sim.advanceMobs(1.0, sim.playerFeet(), 40.0, false);
	CHECK(sim.mobs().mobs().size() == popBefore, "disabled mobs do not populate/retire");
	CHECK(sim.mobs().droppedSteps() == droppedBeforeToggle, "disabled mobs do not step");
	sim.mobsEnabled() = true;
	const uint64_t playerDroppedBefore = sim.playerSnapshot().metrics.droppedSteps;
	sim.advancePlayer(10.0, cmd, false);
	CHECK(sim.playerSnapshot().metrics.steps <= physics::PlayerController::maxSteps,
		  "player catch-up stays bounded (maxSteps)");
	CHECK(sim.playerSnapshot().metrics.droppedSteps > playerDroppedBefore,
		  "player drops excess steps after a delta spike");
	CHECK(finite(sim.playerFeet()), "player position finite after a 10 s delta spike");
}

// ---------------------------------------------------------------------------
// 3. Physics edge fixtures on real generated terrain + authoritative edits.
// ---------------------------------------------------------------------------
void testPlayerPhysicsEdges(ThreadPool &pool)
{
	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 1337;
	const glm::dvec3 boot = sim.initialize(cfg, glm::dvec3(8.0, 100.0, 8.0));
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);
	CHECK(!sim.playerFlight(), "surface spawn starts walking");

	const glm::dvec3 spawn = sim.playerFeet();
	const int px = static_cast<int>(std::floor(spawn.x));
	const int pz = static_cast<int>(std::floor(spawn.z));
	const int py = topOccupiedY(*sim.world(), px, pz) + 4; // guaranteed air gap
	CHECK(py > 0 && py < WORLD_HEIGHT - 4, "platform fixture height valid");

	// Build a 3x3 stone platform and settle the player on it.
	for (int dx = -1; dx <= 1; ++dx)
		for (int dz = -1; dz <= 1; ++dz)
			sim.placeVoxel({px + dx + 0.5f, py + 0.5f, pz + dz + 0.5f}, STONE);
	sim.pumpWorld();
	const glm::dvec3 platformFeet(px + 0.5, py + 1.0 + physics::skin, pz + 0.5);
	sim.setPlayerPosition(platformFeet);
	sim::PlayerCommand cmd{};
	for (int i = 0; i < 12; ++i)
		sim.advancePlayer(1.0 / 120.0, cmd, false);
	CHECK(sim.playerSnapshot().grounded, "player settles grounded on the platform");

	// Jump: buffered press produces the upward impulse.
	cmd.jumpPressed = true;
	sim.advancePlayer(1.0 / 120.0, cmd, false);
	CHECK(sim.playerSnapshot().velocity.y > 5.0, "jump impulse applied");
	cmd.jumpPressed = false;
	sim.advancePlayer(1.0 / 120.0, cmd, false);
	CHECK(!sim.playerSnapshot().grounded, "airborne after the jump");

	// Coyote: deleting the floor keeps the jump grace window for a short time.
	sim.setPlayerPosition(platformFeet);
	for (int i = 0; i < 12; ++i)
		sim.advancePlayer(1.0 / 120.0, cmd, false);
	CHECK(sim.playerSnapshot().grounded, "re-grounded before the coyote fixture");
	sim.deleteVoxel({px + 0.5f, py + 0.5f, pz + 0.5f});
	sim.pumpWorld();
	sim.advancePlayer(1.0 / 120.0, cmd, false); // support gone, coyote armed
	CHECK(!sim.playerSnapshot().grounded, "deleted floor revalidates grounded");
	cmd.jumpPressed = true;
	sim.advancePlayer(1.0 / 120.0, cmd, false);
	CHECK(sim.playerSnapshot().velocity.y > 5.0, "coyote jump within the grace window");
	cmd.jumpPressed = false;

	// Expired coyote: after the window, a press no longer jumps.
	sim.placeVoxel({px + 0.5f, py + 0.5f, pz + 0.5f}, STONE);
	sim.pumpWorld();
	sim.setPlayerPosition(platformFeet);
	for (int i = 0; i < 12; ++i)
		sim.advancePlayer(1.0 / 120.0, cmd, false);
	sim.deleteVoxel({px + 0.5f, py + 0.5f, pz + 0.5f});
	sim.pumpWorld();
	for (int i = 0; i < 30; ++i) // 0.25 s > coyoteTime (0.1 s)
		sim.advancePlayer(1.0 / 120.0, cmd, false);
	cmd.jumpPressed = true;
	sim.advancePlayer(1.0 / 120.0, cmd, false);
	CHECK(sim.playerSnapshot().velocity.y < 0.0, "expired coyote window refuses the jump");
	cmd.jumpPressed = false;

	// Swimming: a water column immerses the body; swimUp drives it upward.
	const int wx = px + 5;
	const int wy = topOccupiedY(*sim.world(), wx, pz) + 1;
	for (int dx = -1; dx <= 1; ++dx)
		for (int dz = -1; dz <= 1; ++dz)
		{
			sim.placeVoxel({wx + dx + 0.5f, wy + 0.5f, pz + dz + 0.5f}, STONE);
			sim.placeVoxel({wx + dx + 0.5f, wy + 1.5f, pz + dz + 0.5f}, WATER);
			sim.placeVoxel({wx + dx + 0.5f, wy + 2.5f, pz + dz + 0.5f}, WATER);
		}
	sim.pumpWorld();
	sim.setPlayerPosition({wx + 0.5, wy + 1.2, pz + 0.5});
	for (int i = 0; i < 24; ++i)
		sim.advancePlayer(1.0 / 120.0, cmd, false);
	CHECK(sim.playerSnapshot().submerged.water > 0.1, "body immersed in the water column");
	cmd.swimUp = true;
	for (int i = 0; i < 24; ++i)
		sim.advancePlayer(1.0 / 120.0, cmd, false);
	CHECK(sim.playerSnapshot().velocity.y > 0.0, "swimUp drives the body upward");
	cmd.swimUp = false;

	// Flight toggle: host-driven body, no gravity; walk refusal inside solid.
	sim.setPlayerFlight(true, sim.playerFeet());
	CHECK(sim.playerFlight(), "flight enabled");
	const glm::dvec3 flightPos = sim.playerFeet();
	sim.advancePlayer(0.1, cmd, false);
	CHECK(sim.playerFeet() == flightPos, "flight body is host-driven (no physics stepping)");
	CHECK(!sim.setPlayerFlight(false, {px + 1.5, py + 0.5, pz + 1.5}),
		  "walk refused inside solid terrain");
	CHECK(std::string(sim.playerStatus()).rfind("Walk unavailable", 0) == 0,
		  "refusal surfaces the historical status message");
	CHECK(sim.playerFlight(), "refused toggle keeps flight");
	CHECK(sim.setPlayerFlight(false, {px + 0.5, py + 3.5, pz + 0.5}),
		  "walk accepted in clear air");
	CHECK(!sim.playerFlight(), "flight disabled");

	// Unknown / unloaded terrain: the solver holds, never speculates.
	sim.setPlayerPosition({5000.5, 100.0, 5000.5});
	sim.advancePlayer(1.0 / 60.0, cmd, false);
	CHECK(sim.playerSnapshot().waitingForTerrain, "unknown terrain holds the body");
	CHECK(sim.playerFeet() == glm::dvec3(5000.5, 100.0, 5000.5),
		  "no speculative teleport into unloaded space");

	// Spawn recovery: embedding up to one block is corrected, not fatal.
	sim.placeVoxel({px + 0.5f, py + 0.5f, pz + 0.5f}, STONE);
	sim.pumpWorld();
	sim.setPlayerPosition({px + 0.5, py + 0.5, pz + 0.5}); // inside the stone
	sim.advancePlayer(1.0 / 120.0, cmd, false);
	CHECK(sim.playerFeet().y >= py + 1.0, "recover() lifts the embedded body out");
	CHECK(finite(sim.playerFeet()), "recovered position finite");

	// Suspension: no stepping while suspended, NO catch-up burst afterwards.
	sim.setPlayerPosition(platformFeet);
	sim.suspendPlayer();
	const glm::dvec3 heldPos = sim.playerFeet();
	sim.advancePlayer(5.0, cmd, true);
	CHECK(sim.playerFeet() == heldPos, "suspended player does not step");
	CHECK(sim.playerSnapshot().metrics.steps == 0, "suspended frame reports zero steps");
	sim.advancePlayer(1.0 / 120.0, cmd, false);
	CHECK(sim.playerSnapshot().metrics.steps == 1, "no catch-up burst after suspension");
}

// ---------------------------------------------------------------------------
// 3b. Player-aware placement rule (issue #209, Epic #204 Phase E): the
//     graphical host and headless hosts share the SAME validation through
//     the facade — a solid block cannot entomb the authoritative body.
// ---------------------------------------------------------------------------
void testPlacementRule(ThreadPool &pool)
{
	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 1337;
	const glm::dvec3 boot = sim.initialize(cfg, glm::dvec3(8.0, 100.0, 8.0));
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);
	CHECK(!sim.playerFlight(), "placement fixture starts walking");

	const glm::dvec3 feet = sim.playerFeet();
	const glm::ivec3 bodyCell(glm::floor(feet)); // cell containing the feet
	const glm::vec3 insideBody(bodyCell.x + 0.5f, bodyCell.y + 0.5f, bodyCell.z + 0.5f);
	// A guaranteed-air target away from the body (above the local terrain).
	const int px = bodyCell.x;
	const int pz = bodyCell.z;
	const int awayY = topOccupiedY(*sim.world(), px + 4, pz + 4) + 2;
	const glm::vec3 awayFromBody(px + 4.5f, awayY + 0.5f, pz + 4.5f);

	// Walking: a solid block overlapping the body is refused; the same
	// block away from the body is allowed.
	CHECK(!sim.canPlaceVoxel(insideBody, STONE), "solid placement inside the walking body refused");
	CHECK(sim.canPlaceVoxel(awayFromBody, STONE), "solid placement away from the body allowed");
	// Non-solid blocks never collide with the body.
	CHECK(sim.canPlaceVoxel(insideBody, WATER), "non-solid (water) placement inside the body allowed");
	CHECK(sim.canPlaceVoxel(insideBody, SHORT_GRASS), "non-solid (grass) placement inside the body allowed");

	// The rule is a pure query: it never edits the world.
	const uint64_t revisionBefore = sim.worldSnapshot().editRevision;
	(void)sim.canPlaceVoxel(insideBody, STONE);
	(void)sim.canPlaceVoxel(awayFromBody, STONE);
	CHECK(sim.worldSnapshot().editRevision == revisionBefore, "placement query does not edit the world");

	// Flight: the body no longer constrains placement.
	CHECK(sim.setPlayerFlight(true, feet), "flight enabled for the placement fixture");
	CHECK(sim.canPlaceVoxel(insideBody, STONE), "flying player may place a solid inside the body");

	// Accepted placements still publish through the render-free world seam.
	CHECK(sim.placeVoxel(awayFromBody, STONE).accepted, "accepted placement is authoritative");
	sim.pumpWorld();
	WorldCollisionView view(*sim.world());
	TextureType placed = AIR;
	view.sampleWithType(glm::ivec3(glm::floor(awayFromBody)), &placed);
	CHECK(placed == STONE, "accepted placement lands in the authoritative world");
}

// ---------------------------------------------------------------------------
// 4. Mob fixtures: real-terrain group spawn/retire, fixed-step wandering,
//    interpolation fields, no NaN, per-seed reproducibility with controlled
//    residency (streamed once, quiesced) and clock.
// ---------------------------------------------------------------------------
struct MobRun
{
	size_t natural{0};
	std::vector<sim::MobSnapshot> snapshots;
	std::vector<glm::dvec3> positions;
	bool allFinite{true};
	bool interpolationOk{true};
	bool retired{false};
	bool suspendedFrozen{true};
};

MobRun mobRun(ThreadPool &pool)
{
	MobRun out;
	// Find a meadow fixture column for seed 42 (same search as the historical
	// mobs profile): deterministic per seed.
	TerrainGenerator probe(42);
	glm::ivec2 center{};
	bool found = false;
	for (int radius = 0; radius <= 32 && !found; ++radius)
		for (int z = -radius; z <= radius && !found; ++z)
			for (int x = -radius; x <= radius && !found; ++x)
			{
				if (std::max(std::abs(x), std::abs(z)) != radius)
					continue;
				const auto biome = probe.getBiomeAt(x * 64, z * 64);
				if (biome == BIOME_PLAINS || biome == BIOME_FLOWER_MEADOW)
				{
					center = {x * 64, z * 64};
					found = true;
				}
			}
	if (!found)
	{
		out.allFinite = false;
		return out;
	}

	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 42;
	const glm::dvec3 obs(center.x, 100.0, center.y);
	const glm::dvec3 boot = sim.initialize(cfg, obs);
	sim.bootstrapTerrain(boot);

	// Controlled residency: stream the 112-block footprint once and quiesce
	// BEFORE the mob clock starts, so spawn decisions see a fixed world.
	WorldStreamSettings ws;
	ws.maxRenderDistance = 112;
	ws.minRenderDistance = 112;
	sim.world()->updateStreaming(glm::vec3(obs), glm::vec2(0.f, 1.f), ws);
	sim.world()->processChunkLoading(512);
	sim.world()->generatePendingVoxels(glm::vec3(obs), (2.f * 112.f) * (2.f * 112.f), 512);
	pumpUntilIdle(*sim.world());
	sim.finalizePlayerSpawn(boot);

	for (int i = 0; i < 300; ++i)
		sim.advanceMobs(1.0 / 60.0, obs, 112.0, false);
	out.natural = sim.mobs().mobs().size();

	// Interpolation fields: a full step leaves a ~ 0; a half frame must sit
	// exactly between previous and current (the #129 presentation contract).
	sim.advanceMobs(1.0 / 60.0, obs, 112.0, false);
	if (!sim.mobs().mobs().empty())
	{
		const entities::Mob &m = sim.mobs().mobs()[0];
		const glm::dvec3 previous = m.previous;
		const glm::dvec3 current = m.body.position;
		sim.advanceMobs(1.0 / 120.0, obs, 112.0, false);
		std::vector<sim::MobSnapshot> snaps;
		sim.mobSnapshots(snaps);
		if (!snaps.empty())
		{
			const glm::vec3 expected(glm::mix(previous, current, 0.5));
			out.interpolationOk = glm::distance(snaps[0].position, expected) < 1e-3f;
		}
	}

	sim.mobSnapshots(out.snapshots);
	for (const auto &m : sim.mobs().mobs())
	{
		out.positions.push_back(m.body.position);
		out.allFinite = out.allFinite && finite(m.body.position) && finite(m.body.velocity) &&
						std::isfinite(m.yaw) && std::isfinite(m.targetYaw) && std::isfinite(m.gait);
	}
	for (const auto &s : out.snapshots)
		out.allFinite = out.allFinite && std::isfinite(s.position.x) && std::isfinite(s.position.y) &&
						std::isfinite(s.position.z) && std::isfinite(s.yaw) && std::isfinite(s.gait) &&
						std::isfinite(s.look) && std::isfinite(s.flap) && std::isfinite(s.stride);

	// Suspension freezes the population and syncs previous/current.
	const size_t beforeSuspend = sim.mobs().mobs().size();
	sim.advanceMobs(1.0, obs, 112.0, true);
	out.suspendedFrozen = sim.mobs().mobs().size() == beforeSuspend;
	for (const auto &m : sim.mobs().mobs())
		out.suspendedFrozen = out.suspendedFrozen && m.previous == m.body.position;

	// Retire: moving the observer far away retires every mob and group.
	sim.advanceMobs(1.0 / 60.0, obs + glm::dvec3(500.0, 0.0, 0.0), 112.0, false);
	out.retired = sim.mobs().mobs().empty();
	return out;
}

void testMobSimulation(ThreadPool &pool)
{
	const MobRun a = mobRun(pool);
	CHECK(a.natural > 0, "natural mob groups spawn on generated meadow voxels");
	CHECK(a.allFinite, "mob simulation produces no NaN (bodies + snapshots)");
	CHECK(a.interpolationOk, "mob snapshot interpolates previous -> current at the accumulator");
	CHECK(a.snapshots.size() <= entities::kMaxMobCount, "mob snapshots bounded by the population cap");
	bool unassignedLight = true;
	for (const auto &s : a.snapshots)
		unassignedLight = unassignedLight && s.localSkylight < 0.0f;
	CHECK(unassignedLight, "headless mob snapshots leave entity light unassigned (derived)");
	CHECK(a.suspendedFrozen, "suspended mobs freeze and sync previous/current");
	CHECK(a.retired, "far observer retires the whole population");

	const MobRun b = mobRun(pool);
	CHECK(a.natural == b.natural, "per-seed mob population reproducible (controlled residency)");
	bool identical = a.snapshots.size() == b.snapshots.size();
	for (size_t i = 0; identical && i < a.snapshots.size(); ++i)
		identical = a.snapshots[i].species == b.snapshots[i].species &&
					a.snapshots[i].position == b.snapshots[i].position &&
					a.snapshots[i].yaw == b.snapshots[i].yaw && a.snapshots[i].gait == b.snapshots[i].gait;
	CHECK(identical, "per-seed mob decisions reproducible across runs");
}

// ---------------------------------------------------------------------------
// 5. World reload + save/player restore without camera or renderer; failures
//    keep the old world alive (issue #180 contracts through the facade).
// ---------------------------------------------------------------------------
void testReloadAndPersistence(ThreadPool &pool)
{
	TempRoot root("reload");
	const glm::dvec3 center(8.0, 100.0, 8.0);

	// Session 1: create, edit, capture the player, durable shutdown.
	glm::dvec3 savedFeet{0.0};
	{
		sim::GameSimulation sim(&pool);
		sim::GameSimulation::Config cfg;
		cfg.seed = 42;
		cfg.worldName = "acceptance";
		cfg.savesRoot = root.path;
		const glm::dvec3 boot = sim.initialize(cfg, center);
		CHECK(sim.seed() == 42 && sim.isWorldOpen(), "persistent world opened");
		sim.bootstrapTerrain(boot);
		sim.finalizePlayerSpawn(boot);

		const glm::dvec3 feet = sim.playerFeet();
		const int editY = topOccupiedY(*sim.world(), static_cast<int>(std::floor(feet.x)) + 2,
									static_cast<int>(std::floor(feet.z)) + 2) + 2;
		CHECK(sim.placeVoxel({static_cast<float>(feet.x + 2.0), static_cast<float>(editY),
							  static_cast<float>(feet.z + 2.0)}, STONE).accepted,
			  "headless edit accepted");
		sim.pumpWorld();
		CHECK(sim.worldSnapshot().editRevision > 1, "edit revision advances in the snapshot");
		CHECK(sim.worldSnapshot().persistenceOpen, "world snapshot reports persistence open");
		CHECK(sim.worldSnapshot().worldName == "acceptance", "world snapshot carries the save name");

		sim.setPlayerFlight(true, sim.playerFeet());
		sim.selectedBlock() = DIRT;
		savedFeet = sim.playerFeet();
		CHECK(sim.savePlayerState(30.f, -10.f), "player state captured + flushed");
		sim.shutdown();
		CHECK(!sim.isWorldOpen(), "shutdown closes persistence");
	}

	// The save is independently reopenable; player.state round-trips.
	{
		WorldPersistence verify;
		const auto info = verify.openOrCreate(root.path, "acceptance", 42,
											  TerrainGenerator::kGeneratorVersion);
		CHECK(info.ok && !info.created, "save reopens with the stored identity");
		PlayerPersistState state;
		CHECK(verify.readPlayerState(state), "player.state readable");
		CHECK(std::abs(state.x - savedFeet.x) < 1e-9 && std::abs(state.y - savedFeet.y) < 1e-9 &&
				  std::abs(state.z - savedFeet.z) < 1e-9,
			  "persisted feet match the authoritative position");
		CHECK(state.flight && state.selectedBlock == static_cast<int32_t>(DIRT) &&
				  state.yaw == 30.f && state.pitch == -10.f,
			  "persisted mode/selection/orientation round-trip");
		verify.shutdown();
	}

	// Session 2: reopen WITHOUT an explicit seed; restore around the saved
	// position — no camera, no renderer involved.
	{
		sim::GameSimulation sim(&pool);
		sim::GameSimulation::Config cfg;
		cfg.seed = 0;
		cfg.worldName = "acceptance";
		cfg.savesRoot = root.path;
		const glm::dvec3 boot = sim.initialize(cfg, center);
		CHECK(sim.seed() == 42, "stored seed adopted when no explicit seed is given");
		CHECK(std::abs(boot.x - savedFeet.x) < 1e-6 && std::abs(boot.z - savedFeet.z) < 1e-6,
			  "bootstrap centers on the saved position");
		sim.bootstrapTerrain(boot);
		const glm::ivec3 savedChunk = VoxelWorld::worldToChunkCoord(glm::vec3(savedFeet));
		CHECK(sim.world()->getChunk(savedChunk) != nullptr,
			  "saved-position chunk bootstrapped (not the origin)");
		const auto outcome = sim.finalizePlayerSpawn(boot);
		CHECK(outcome.restored, "saved player restored");
		CHECK(outcome.yaw == 30.f && outcome.pitch == -10.f, "restored orientation handed to the host");
		CHECK(sim.playerFlight() && sim.selectedBlock() == DIRT, "restored mode + selection");
		CHECK(glm::distance(sim.playerFeet(), savedFeet) < 0.5, "restored at the saved spot");
		sim.shutdown();
	}

	// Explicit seed contradicting the save: transient session, identity kept.
	{
		sim::GameSimulation sim(&pool);
		sim::GameSimulation::Config cfg;
		cfg.seed = 999;
		cfg.worldName = "acceptance";
		cfg.savesRoot = root.path;
		sim.initialize(cfg, center);
		CHECK(sim.seed() == 999, "contradicting seed runs transient");
		CHECK(!sim.isWorldOpen(), "contradicting seed never opens the save");
		CHECK(!sim.openWorldError().empty(), "open error surfaced for the host UI");
		sim.shutdown();
	}

	// Reload: transient recreation, generation counter advances.
	{
		sim::GameSimulation sim(&pool);
		sim::GameSimulation::Config cfg;
		cfg.seed = 42;
		const glm::dvec3 boot = sim.initialize(cfg, center);
		sim.bootstrapTerrain(boot);
		sim.finalizePlayerSpawn(boot);
		const uint64_t gen0 = sim.worldGenerationId();
		CHECK(sim.reload(43), "transient reload succeeds");
		CHECK(sim.seed() == 43, "reload adopts the new seed");
		CHECK(sim.worldGenerationId() == gen0 + 1, "reload bumps the generation counter");
		CHECK(!sim.isWorldOpen(), "reloaded world is transient");
		sim.bootstrapTerrain(glm::dvec3(0.0, 100.0, 0.0));
		const auto outcome = sim.finalizePlayerSpawn(glm::dvec3(0.0, 100.0, 0.0));
		CHECK(finite(outcome.eye) && outcome.eye.y > 0.0, "reload + spawn usable without a camera");
		sim.shutdown();
	}

	// Refused reload: stranded accepted edits keep the OLD world alive.
	{
		sim::GameSimulation sim(&pool);
		sim::GameSimulation::Config cfg;
		cfg.seed = 42;
		cfg.worldName = "stranded-reload";
		cfg.savesRoot = root.path;
		const glm::dvec3 boot = sim.initialize(cfg, center);
		sim.bootstrapTerrain(boot);
		WorldChunk *c = sim.world()->getChunk({0, 0, 0});
		CHECK(c != nullptr, "bootstrap record present");
		c->reset(c->getPosition(), WorldChunk::ResetMode::ForGeneration);
		CHECK(sim.placeVoxel({8.5f, 200.5f, 8.5f}, STONE).accepted, "stranded edit accepted");
		CHECK(sim.world()->hasPendingLogicalEdits(), "edit stranded outside authoritative state");
		const VoxelWorld *worldBefore = sim.world();
		CHECK(!sim.reload(44), "reload refused while edits are stranded");
		CHECK(sim.world() == worldBefore, "refused reload keeps the old world authoritative");
		CHECK(sim.seed() == 42, "refused reload keeps the old seed");
		CHECK(sim.isWorldOpen(), "refused reload keeps persistence alive");
		// Resolve the strand, then the reload succeeds.
		CHECK(c->prepareVoxelStorageForGeneration(), "stranded prepare");
		c->generateTerrain(*sim.terrainGenerator());
		sim.pumpWorld();
		CHECK(!sim.world()->hasPendingLogicalEdits(), "stranded edit landed");
		CHECK(sim.reload(44), "reload succeeds after resolution");
		CHECK(sim.seed() == 44, "resolved reload adopts the new seed");
		sim.shutdown();
	}
}

// ---------------------------------------------------------------------------
// 6. Snapshot contract: plain bounded values, identity + revision seams.
// ---------------------------------------------------------------------------
static_assert(std::is_trivially_copyable_v<sim::PlayerSnapshot>,
			  "player snapshot is a plain bounded value");
static_assert(std::is_trivially_copyable_v<sim::MobSnapshot>,
			  "mob snapshot is a plain bounded value");

void testSnapshotContract(ThreadPool &pool)
{
	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 1337;
	const glm::dvec3 boot = sim.initialize(cfg, glm::dvec3(8.0, 100.0, 8.0));
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);

	const auto world = sim.worldSnapshot();
	CHECK(world.seed == 1337, "world snapshot carries the seed");
	CHECK(world.generatorVersion == TerrainGenerator::kGeneratorVersion,
		  "world snapshot carries the generator version");
	CHECK(world.chunkCount == sim.world()->chunkCount(), "world snapshot mirrors residency");
	CHECK(!world.persistenceOpen && world.worldName.empty(), "transient snapshot identity");

	const auto player = sim.playerSnapshot();
	CHECK(player.position == sim.playerFeet(), "player snapshot mirrors authoritative feet");
	CHECK(player.renderEye == sim.playerRenderEye(), "player snapshot carries the interpolated eye");
	CHECK(player.selectedBlock == sim.selectedBlock(), "player snapshot carries the selection");

	// The CPU mob seam answers collision exactly like the collision view.
	WorldMobView mobView(*sim.world(), *sim.terrainGenerator());
	WorldCollisionView collisionView(*sim.world());
	const glm::ivec3 probe(static_cast<int>(std::floor(boot.x)), 100,
						   static_cast<int>(std::floor(boot.z)));
	const auto a = mobView.sample(probe);
	const auto b = collisionView.sample(probe);
	CHECK(a.available == b.available && a.solid == b.solid && a.medium == b.medium,
		  "WorldMobView collision delegates to WorldCollisionView");
}
} // namespace

int main()
{
	std::printf("test_game_simulation: CPU-only GameSimulation (no GPU, no SDL, no camera)\n");
	ThreadPool pool(2);
	testScriptedDeterminism(pool);
	testFixedRates(pool);
	testPlayerPhysicsEdges(pool);
	testPlacementRule(pool);
	testMobSimulation(pool);
	testReloadAndPersistence(pool);
	testSnapshotContract(pool);
	std::printf("%d checks, %d failures\n", g_checks, g_fails);
	if (g_fails == 0)
		std::printf("PASS\n");
	return g_fails == 0 ? 0 : 1;
}
