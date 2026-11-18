// test_player_sessions — CPU-only multi-player session acceptance (issue
// #246, Epic #245 Phase A). Links ONLY ft_vox_core: no SDL, no Vulkan device,
// no ImGui, no camera. Proves the issue's acceptance list end-to-end:
//
//   * one shared world hosting N independent player sessions: distinct
//     positions, velocities, flight flags and inventories coexist, survive
//     stepping and cannot overwrite one another;
//   * PlayerId semantics: monotonic allocation, never reused, invalid id 0,
//     local-anchor removal refusal, population cap, deterministic id-sorted
//     iteration, reproducible across runs;
//   * removing one player never unloads/resets/destroys the shared world,
//     mobs, dropped items or the other players;
//   * invalid/stale handles cannot access another player's state — every
//     per-player API answers a safe refusal/no-op, including across world
//     epochs (initialize/reload keep sessions, reset inventories, never
//     rewind the id allocator);
//   * solo compatibility: the legacy single-player APIs are thin forwards to
//     the local player — a scripted run through legacy calls and one through
//     explicit local-player calls stay bit-identical (no two sources of
//     truth);
//   * A1 scope boundaries pinned for the follow-up issues: auto-pickup still
//     serves the LOCAL player only (#247 makes it actor-aware), and the solo
//     save format still captures the LOCAL player only (#259 adds
//     multi-player checkpoints);
//   * per-player fixed 120 Hz stepping, suspension without catch-up burst,
//     bounded catch-up — independently per session.
//
// Style mirrors test_game_simulation / test_gameplay: CHECK macro, plain
// main(), controlled residency (synchronous bootstrap, no async streaming)
// and a fixed 1/60 or 1/120 host clock for determinism.

#include <Sim/GameSimulation.hpp>
#include <Sim/PlayerId.hpp>
#include <Core/ThreadPool.hpp>
#include <Items/ItemRegistry.hpp>
#include <World/WorldCollisionView.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "TestMain.hpp"

namespace
{
using items::ItemId;
using items::ItemResult;
using items::ItemStack;

constexpr ItemStack S(ItemId id, uint16_t count)
{
	return {id, count};
}

constexpr ItemStack kEmpty{};

// --- PlayerId contract pins ---------------------------------------------------
static_assert(std::is_trivially_copyable_v<sim::PlayerId>, "PlayerId is a plain value");
static_assert(sizeof(sim::PlayerId) == 4, "PlayerId is a compact 4-byte handle");
static_assert(!sim::kInvalidPlayerId.valid(), "the invalid handle is never valid");
static_assert(sim::PlayerId{1} < sim::PlayerId{2}, "PlayerId orders by ordinal");

// --- Helpers -------------------------------------------------------------------

sim::InventorySnapshot inventoryOf(const sim::GameSimulation &sim, sim::PlayerId id)
{
	sim::InventorySnapshot out;
	CHECK(sim.inventorySnapshot(id, out), "per-player inventory snapshot fills for a live id");
	return out;
}

sim::PlayerSnapshot playerOf(const sim::GameSimulation &sim, sim::PlayerId id)
{
	sim::PlayerSnapshot out;
	CHECK(sim.playerSnapshot(id, out), "per-player player snapshot fills for a live id");
	return out;
}

bool emptyInventory(const sim::InventorySnapshot &s)
{
	if (s.selectedSlot != 0)
		return false;
	for (const ItemStack &slot : s.slots)
		if (slot != kEmpty)
			return false;
	return true;
}

// Full field-by-field snapshot equality (no memcmp: padding is not state).
bool samePlayer(const sim::PlayerSnapshot &a, const sim::PlayerSnapshot &b)
{
	return a.position == b.position && a.velocity == b.velocity && a.renderEye == b.renderEye &&
		   a.grounded == b.grounded && a.waitingForTerrain == b.waitingForTerrain &&
		   a.flight == b.flight && a.submerged.water == b.submerged.water &&
		   a.submerged.lava == b.submerged.lava && a.metrics.steps == b.metrics.steps &&
		   a.metrics.droppedSteps == b.metrics.droppedSteps &&
		   a.metrics.queries.cells == b.metrics.queries.cells &&
		   a.metrics.queries.iterations == b.metrics.queries.iterations &&
		   a.selectedBlock == b.selectedBlock && std::strcmp(a.status, b.status) == 0;
}

bool finite(const glm::dvec3 &v)
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// ItemEntity is plain state without value equality (the snapshot/event types
// carry it); compare the authoritative fields field by field.
bool sameItems(const std::vector<items::ItemEntity> &a, const std::vector<items::ItemEntity> &b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i)
	{
		if (a[i].id != b[i].id || !(a[i].stack == b[i].stack) ||
			a[i].body.position != b[i].body.position || a[i].body.velocity != b[i].body.velocity ||
			a[i].ageTicks != b[i].ageTicks || a[i].pickupDelay != b[i].pickupDelay)
			return false;
	}
	return true;
}

// Highest occupied cell of a column: solid terrain OR fluid (the
// test_game_simulation fixture helper).
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

// Deterministic solid-surface column search near (x, z): the pickup fixture
// needs a SOLID top (a fluid top would let the drop sink out of reach).
glm::ivec3 findSolidTopColumn(const VoxelWorld &world, int x, int z)
{
	WorldCollisionView view(world);
	for (int radius = 0; radius <= 8; ++radius)
		for (int dx = -radius; dx <= radius; ++dx)
			for (int dz = -radius; dz <= radius; ++dz)
			{
				if (std::max(std::abs(dx), std::abs(dz)) != radius)
					continue;
				const int cx = x + dx, cz = z + dz;
				const int top = topOccupiedY(world, cx, cz);
				if (top < 0)
					continue;
				TextureType type = AIR;
				const auto cell = view.sampleWithType({cx, top, cz}, &type);
				if (cell.available && cell.solid)
					return {cx, top, cz};
			}
	return {x, -1, z};
}

std::filesystem::path makeTempDir(const char *tag)
{
	const auto base = std::filesystem::temp_directory_path() /
					  ("ft_vox_sessions_" + std::string(tag) + "_" +
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

// Silence the grant/restore audit logs inside bulk fixtures (the logging
// itself is asserted by test_gameplay's grant-seam test). RAII restore.
struct CoutSilencer
{
	std::ostringstream sink;
	std::streambuf *old;
	CoutSilencer() : old(std::cout.rdbuf(sink.rdbuf())) {}
	~CoutSilencer() { std::cout.rdbuf(old); }
};

// ---------------------------------------------------------------------------
// 1. Session identity: local anchor, monotonic never-reused ids, invalid
//    handles, removal refusal, population cap, deterministic iteration.
// ---------------------------------------------------------------------------
void testSessionIdentity()
{
	sim::GameSimulation sim(nullptr); // session management needs no world/pool
	CHECK(sim.playerCount() == 1, "the local player exists from construction");
	const sim::PlayerId local = sim.localPlayerId();
	CHECK(local.valid(), "the local anchor is a valid handle");
	CHECK(sim.hasPlayer(local), "hasPlayer answers the anchor");
	CHECK(!sim.hasPlayer(sim::kInvalidPlayerId), "the invalid handle addresses nobody");
	CHECK(!sim.hasPlayer(sim::PlayerId{999}), "a never-allocated handle addresses nobody");

	const sim::PlayerId a = sim.createPlayer();
	const sim::PlayerId b = sim.createPlayer();
	const sim::PlayerId c = sim.createPlayer();
	CHECK(a.valid() && b.valid() && c.valid(), "createPlayer allocates valid handles");
	CHECK(local < a && a < b && b < c, "allocation is monotonic in call order");
	CHECK(sim.playerCount() == 4, "the population counts every session");

	std::vector<sim::PlayerId> ids;
	sim.playerIds(ids);
	CHECK((ids == std::vector<sim::PlayerId>{local, a, b, c}), "iteration is ascending id order");

	CHECK(sim.removePlayer(b), "a non-local session is removable");
	CHECK(!sim.hasPlayer(b), "the removed session is gone");
	CHECK(sim.playerCount() == 3, "removal shrinks the population");
	sim.playerIds(ids);
	CHECK((ids == std::vector<sim::PlayerId>{local, a, c}), "removal preserves id order");

	const sim::PlayerId d = sim.createPlayer();
	CHECK(c < d, "a retired id is NEVER reused (allocation stays monotonic)");
	CHECK(!sim.hasPlayer(b), "the retired id stays retired after new allocations");

	CHECK(!sim.removePlayer(local), "the local anchor cannot be removed");
	CHECK(!sim.removePlayer(b), "double removal is refused");
	CHECK(!sim.removePlayer(sim::kInvalidPlayerId), "invalid removal is refused");
	CHECK(!sim.removePlayer(sim::PlayerId{9999}), "unknown removal is refused");
	CHECK(sim.playerCount() == 4, "refused removals change nothing");

	// In-place fill reuses the caller's capacity (the snapshot pattern).
	const auto capacityBefore = ids.capacity();
	sim.playerIds(ids);
	CHECK(ids.capacity() >= capacityBefore, "playerIds keeps the caller's capacity");

	// Population cap: bounded, refusal allocates nothing, removal frees a slot.
	while (sim.playerCount() < sim::GameSimulation::kMaxPlayers)
		CHECK(sim.createPlayer().valid(), "creation succeeds below the cap");
	CHECK(sim.playerCount() == sim::GameSimulation::kMaxPlayers, "the population reaches the cap");
	CHECK(!sim.createPlayer().valid(), "creation at the cap answers the invalid handle");
	CHECK(sim.playerCount() == sim::GameSimulation::kMaxPlayers, "refused creation adds nothing");
	CHECK(sim.removePlayer(a), "removal at the cap frees a slot");
	const sim::PlayerId e = sim.createPlayer();
	CHECK(e.valid() && d < e, "the freed slot takes a FRESH id (never a retired one)");

	// Deterministic allocation across runs: identical call sequences produce
	// identical id sequences (command order is the allocation order).
	const auto sequence = [](std::vector<sim::PlayerId> &out) {
		sim::GameSimulation s(nullptr);
		out.push_back(s.localPlayerId());
		const auto x = s.createPlayer();
		const auto y = s.createPlayer();
		out.push_back(x);
		out.push_back(y);
		s.removePlayer(x);
		out.push_back(s.createPlayer());
		s.playerIds(out); // final population appended for order comparison
	};
	std::vector<sim::PlayerId> run1, run2;
	sequence(run1);
	sequence(run2);
	CHECK(run1 == run2, "id allocation/iteration reproducible across runs");
}

// ---------------------------------------------------------------------------
// 2. Two players coexist: distinct positions/velocities/flight/inventories,
//    independent stepping, no cross-overwrite (the core acceptance).
// ---------------------------------------------------------------------------
void testTwoPlayerIndependence(ThreadPool &pool)
{
	CoutSilencer silence;
	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 1337;
	const glm::dvec3 boot = sim.initialize(cfg, glm::dvec3(8.0, 100.0, 8.0));
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);

	const glm::dvec3 spawn = sim.playerFeet();
	const int px = static_cast<int>(std::floor(spawn.x));
	const int pz = static_cast<int>(std::floor(spawn.z));
	const int py = topOccupiedY(*sim.world(), px, pz) + 4; // guaranteed air gap
	CHECK(py > 0 && py < WORLD_HEIGHT - 4, "platform fixture height valid");
	for (int dx = -4; dx <= 4; ++dx)
		for (int dz = -1; dz <= 1; ++dz)
			sim.placeVoxel({px + dx + 0.5f, py + 0.5f, pz + dz + 0.5f}, STONE);
	sim.pumpWorld();

	const sim::PlayerId b = sim.createPlayer();
	CHECK(b.valid(), "second session created");
	const glm::dvec3 feetA(px - 3 + 0.5, py + 1.0 + physics::skin, pz + 0.5);
	const glm::dvec3 feetB(px + 3 + 0.5, py + 1.0 + physics::skin, pz + 0.5);
	sim.setPlayerPosition(feetA);    // legacy form -> local
	sim.setPlayerPosition(b, feetB); // per-player form
	sim::PlayerCommand cmd{};
	for (int i = 0; i < 12; ++i)
	{
		sim.advancePlayer(1.0 / 120.0, cmd, false);
		sim.advancePlayer(b, 1.0 / 120.0, cmd, false);
	}
	CHECK(sim.playerSnapshot().grounded && playerOf(sim, b).grounded, "both players settle grounded");

	// Distinct inventories, hotbar selections, creative selections, flight.
	CHECK(sim.grantItems(ItemId::Stone, 10).result == ItemResult::Ok, "local grant applied");
	CHECK(sim.grantItems(b, ItemId::Dirt, 20).result == ItemResult::Ok, "player B grant applied");
	CHECK(sim.selectHotbarSlot(1) == ItemResult::Ok, "local hotbar selection");
	CHECK(sim.selectHotbarSlot(b, 2) == ItemResult::Ok, "player B hotbar selection");
	sim.setSelectedBlock(b, DIRT);
	CHECK(sim.setPlayerFlight(b, true, feetB), "player B flight enabled");
	CHECK(!sim.playerFlight() && sim.playerFlight(b), "flight flags are per-player");

	const sim::PlayerSnapshot a0 = sim.playerSnapshot();
	const sim::PlayerSnapshot b0 = playerOf(sim, b);
	const sim::InventorySnapshot invA0 = inventoryOf(sim, sim.localPlayerId());
	const sim::InventorySnapshot invB0 = inventoryOf(sim, b);
	CHECK(a0.position != b0.position, "the players hold distinct positions");
	CHECK(invA0.slots[0] == S(ItemId::Stone, 10) && invA0.selectedSlot == 1,
		  "local inventory/selection independent");
	CHECK(invB0.slots[0] == S(ItemId::Dirt, 20) && invB0.selectedSlot == 2,
		  "player B inventory/selection independent");
	CHECK(a0.selectedBlock == STONE && b0.selectedBlock == DIRT, "creative selections independent");
	CHECK(sim.inventory(b) != nullptr && sim.inventory(b)->totalCount(ItemId::Dirt) == 20,
		  "per-player read-only inventory seam");

	// Walk the LOCAL player +X; flying B is host-driven and must not move.
	cmd.move = glm::dvec2(1, 0);
	for (int i = 0; i < 60; ++i)
		sim.advancePlayer(1.0 / 60.0, cmd, false);
	const sim::PlayerSnapshot a1 = sim.playerSnapshot();
	const sim::PlayerSnapshot b1 = playerOf(sim, b);
	CHECK(a1.position.x > a0.position.x + 1.0, "local player walked +X");
	CHECK(a1.velocity.x > 0.0, "local player carries walking velocity");
	CHECK(samePlayer(b0, b1), "stepping the local player leaves flying B bit-identical");
	CHECK(inventoryOf(sim, sim.localPlayerId()) == invA0, "stepping never touches the local inventory");
	CHECK(inventoryOf(sim, b) == invB0, "stepping never touches B's inventory");

	// Walk B -X; the local player is not stepped and must not move.
	CHECK(sim.setPlayerFlight(b, false, sim.playerFeet(b)), "B lands back on the platform");
	const sim::PlayerSnapshot a2 = sim.playerSnapshot();
	cmd.move = glm::dvec2(-1, 0);
	for (int i = 0; i < 60; ++i)
		sim.advancePlayer(b, 1.0 / 60.0, cmd, false);
	const sim::PlayerSnapshot b2 = playerOf(sim, b);
	CHECK(b2.position.x < b1.position.x - 1.0, "player B walked -X");
	CHECK(samePlayer(a2, sim.playerSnapshot()), "stepping B leaves the local player bit-identical");

	// Bulk advance steps EVERY walking player (ascending id order, one view).
	sim.advancePlayers(1.0 / 30.0, false);
	CHECK(sim.playerSnapshot().metrics.steps == 4, "bulk step: local 120 Hz (4 steps per 1/30 s)");
	CHECK(playerOf(sim, b).metrics.steps == 4, "bulk step: B 120 Hz (4 steps per 1/30 s)");

	// Per-player suspension: A frozen, B keeps stepping, no catch-up burst.
	const glm::dvec3 heldA = sim.playerFeet();
	const glm::dvec3 beforeB = sim.playerFeet(b);
	sim.advancePlayer(5.0, cmd, true); // suspend the local player
	CHECK(sim.playerFeet() == heldA, "suspended local player does not step");
	CHECK(sim.playerSnapshot().metrics.steps == 0, "suspended frame reports zero steps");
	sim.advancePlayer(b, 1.0 / 60.0, cmd, false);
	CHECK(sim.playerFeet(b) != beforeB, "B keeps stepping while A is suspended");
	sim.advancePlayer(1.0 / 120.0, cmd, false);
	CHECK(sim.playerSnapshot().metrics.steps == 1, "no catch-up burst after suspension");

	// Per-player surface spawn + saved-state round-trip, local untouched.
	const sim::PlayerSnapshot a3 = sim.playerSnapshot();
	const glm::dvec3 eyeB = sim.spawnPlayerOnSurface(b, spawn + glm::dvec3(2.0, 0.0, 2.0));
	CHECK(finite(eyeB) && eyeB.y > 0.0, "per-player surface spawn returns a usable eye");
	CHECK(!sim.playerFlight(b), "surface-spawned B walks");
	CHECK(samePlayer(a3, sim.playerSnapshot()), "spawning B leaves the local player untouched");
	const PlayerPersistState savedB = sim.capturePlayerState(b, 45.f, -20.f);
	CHECK(savedB.selectedBlock == static_cast<int32_t>(DIRT), "per-player capture carries B's selection");
	CHECK(savedB.yaw == 45.f && savedB.pitch == -20.f, "per-player capture carries the orientation");
	sim.setPlayerPosition(b, feetB);
	CHECK(sim.restorePlayerState(b, savedB), "per-player restore succeeds at the saved spot");
	CHECK(glm::distance(sim.playerFeet(b), glm::dvec3(savedB.x, savedB.y, savedB.z)) < 0.5,
		  "B restored at the saved position");
	CHECK(samePlayer(a3, sim.playerSnapshot()), "restoring B leaves the local player untouched");

	// Determinism: the whole two-player fixture replays bit-identically.
	const auto replay = [&]() {
		sim::GameSimulation s(&pool);
		const glm::dvec3 bt = s.initialize(cfg, glm::dvec3(8.0, 100.0, 8.0));
		s.bootstrapTerrain(bt);
		s.finalizePlayerSpawn(bt);
		const sim::PlayerId pb = s.createPlayer();
		s.setPlayerPosition(feetA);
		s.setPlayerPosition(pb, feetB);
		s.grantItems(ItemId::Stone, 10);
		s.grantItems(pb, ItemId::Dirt, 20);
		sim::PlayerCommand c{};
		c.move = glm::dvec2(1, 0);
		for (int i = 0; i < 120; ++i)
		{
			s.advancePlayer(1.0 / 60.0, c, false);
			s.advancePlayer(pb, 1.0 / 60.0, c, false);
		}
		return std::pair{s.playerSnapshot(), playerOf(s, pb)};
	};
	const auto r1 = replay();
	const auto r2 = replay();
	CHECK(samePlayer(r1.first, r2.first) && samePlayer(r1.second, r2.second),
		  "two-player stepping reproducible across runs");
}

// ---------------------------------------------------------------------------
// 3. Removal isolation: the shared world, mobs, items and the other player
//    survive a session removal untouched; stale APIs all refuse safely.
// ---------------------------------------------------------------------------
void testRemovalIsolation(ThreadPool &pool)
{
	CoutSilencer silence;
	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 1337;
	const glm::dvec3 boot = sim.initialize(cfg, glm::dvec3(8.0, 100.0, 8.0));
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);

	const sim::PlayerId b = sim.createPlayer();
	sim.grantItems(ItemId::Stone, 10);
	sim.grantItems(b, ItemId::Dirt, 20);
	sim.selectHotbarSlot(b, 3);

	// Populate the shared systems: mobs + one registry drop in the world.
	for (int i = 0; i < 30; ++i)
		sim.advanceMobs(1.0 / 60.0, sim.playerFeet(), 40.0, false);
	const glm::dvec3 spawn = sim.playerFeet();
	const glm::ivec3 column = findSolidTopColumn(*sim.world(), static_cast<int>(std::floor(spawn.x)) + 3,
												 static_cast<int>(std::floor(spawn.z)) + 3);
	CHECK(column.y >= 0, "solid fixture column found");
	const glm::ivec3 cell(column.x, column.y + 2, column.z);
	sim.placeVoxel({cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f}, STONE);
	sim.pumpWorld();
	CHECK(sim.breakBlock(cell) == sim::BreakResult::Ok, "fixture break spawns a shared drop");
	CHECK(sim.itemEntities().size() == 1, "one live drop before removal");
	// B stands at the drop spot — the item is spatially "B's", but it belongs
	// to the SHARED world population.
	sim.setPlayerPosition(b, glm::dvec3(cell.x + 0.5, column.y + 1.0, cell.z + 0.5));

	// Record the full shared + local state.
	VoxelWorld *worldBefore = sim.world();
	const sim::WorldSnapshot world0 = sim.worldSnapshot();
	const std::vector<items::ItemEntity> items0 = sim.itemEntities().entities();
	const size_t mobs0 = sim.mobs().mobs().size();
	const sim::PlayerSnapshot local0 = sim.playerSnapshot();
	const sim::InventorySnapshot inv0 = inventoryOf(sim, sim.localPlayerId());

	CHECK(sim.removePlayer(b), "B removed");
	CHECK(sim.world() == worldBefore, "removal never recreates/unloads the world");
	const sim::WorldSnapshot world1 = sim.worldSnapshot();
	CHECK(world1.chunkCount == world0.chunkCount && world1.editRevision == world0.editRevision &&
			  world1.worldGenerationId == world0.worldGenerationId,
		  "removal never resets world identity/revision/residency");
	CHECK(sameItems(sim.itemEntities().entities(), items0), "the shared item population survives removal");
	CHECK(sim.mobs().mobs().size() == mobs0, "the shared mob population survives removal");
	CHECK(samePlayer(local0, sim.playerSnapshot()), "the other player's body survives removal");
	CHECK(inventoryOf(sim, sim.localPlayerId()) == inv0, "the other player's inventory survives removal");

	// Every stale-B API refuses safely (never another player's state).
	sim::PlayerSnapshot staleOut = local0; // sentinel: must stay untouched
	CHECK(!sim.playerSnapshot(b, staleOut), "stale snapshot query refuses");
	CHECK(samePlayer(staleOut, local0), "refused snapshot leaves the out-value untouched");
	sim::InventorySnapshot staleInv = inv0;
	CHECK(!sim.inventorySnapshot(b, staleInv), "stale inventory query refuses");
	CHECK(staleInv == inv0, "refused inventory query leaves the out-value untouched");
	sim::PlayerCommand cmd{};
	CHECK(!sim.advancePlayer(b, 1.0 / 60.0, cmd, false), "stale advance refuses");
	CHECK(sim.selectHotbarSlot(b, 0) == ItemResult::InvalidPlayer, "stale selection refuses");
	CHECK(sim.moveStack(b, 0, 1) == ItemResult::InvalidPlayer, "stale move refuses");
	CHECK(sim.swapStacks(b, 0, 1) == ItemResult::InvalidPlayer, "stale swap refuses");
	CHECK(sim.splitStack(b, 0, 1) == ItemResult::InvalidPlayer, "stale split refuses");
	const items::AddResult grant = sim.grantItems(b, ItemId::Stone, 5);
	CHECK(grant.result == ItemResult::InvalidPlayer && grant.remainder == S(ItemId::Stone, 5),
		  "stale grant refuses and echoes the canonicalized request");
	CHECK(sim.inventory(b) == nullptr, "stale inventory seam answers nullptr");
	CHECK(!sim.hasPlayer(b), "stale handle addresses nobody");
	CHECK(sim.playerFeet(b) == glm::dvec3(0.0), "stale feet query answers the zero vector");
	CHECK(!sim.playerFlight(b), "stale flight query answers false");
	CHECK(!sim.setPlayerFlight(b, true, spawn), "stale flight toggle refuses");
	CHECK(sim.spawnPlayerOnSurface(b, spawn) == spawn, "stale spawn answers the fallback unchanged");
	const PlayerPersistState captured = sim.capturePlayerState(b, 10.f, 10.f);
	CHECK(captured.x == 0.0 && captured.y == 0.0 && captured.z == 0.0 && !captured.flight,
		  "stale capture answers a default state");
	CHECK(!sim.restorePlayerState(b, captured), "stale restore refuses");
	// Void stale commands are safe no-ops (no crash, no state change).
	sim.suspendPlayer(b);
	sim.clearPlayerInput(b);
	sim.setPlayerPosition(b, spawn);
	sim.restorePlayerFlight(b, true, spawn);
	sim.setSelectedBlock(b, DIRT);
	sim.advancePlayers(1.0 / 60.0, false); // bulk step skips the retired session
	CHECK(finite(sim.playerFeet()), "bulk step after removal keeps the local player usable");
	CHECK(inventoryOf(sim, sim.localPlayerId()) == inv0, "stale commands never touched the local inventory");
	CHECK(sameItems(sim.itemEntities().entities(), items0), "stale commands never touched the shared items");
}

// ---------------------------------------------------------------------------
// 4. Stale handles + world epochs: sessions survive initialize/reload, every
//    inventory resets, the allocator never rewinds, retired ids stay retired.
// ---------------------------------------------------------------------------
void testStaleHandlesAndEpochs(ThreadPool &pool)
{
	CoutSilencer silence;
	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 1337;
	const glm::dvec3 center(8.0, 100.0, 8.0);
	const glm::dvec3 boot = sim.initialize(cfg, center);
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);

	const sim::PlayerId local = sim.localPlayerId();
	const sim::PlayerId b = sim.createPlayer();
	const sim::PlayerId c = sim.createPlayer();
	sim.grantItems(ItemId::Stone, 5);
	sim.grantItems(b, ItemId::Dirt, 7);
	sim.grantItems(c, ItemId::Glass, 9);
	sim.setPlayerPosition(b, glm::dvec3(8.5, 100.0, 8.5));
	sim.setPlayerPosition(c, glm::dvec3(12.5, 100.0, 12.5));
	const glm::dvec3 feetLocal = sim.playerFeet();
	const glm::dvec3 feetC = sim.playerFeet(c);

	CHECK(sim.removePlayer(b), "B retired before the epoch change");
	const sim::PlayerId d = sim.createPlayer();
	CHECK(c < d, "post-retirement allocation stays monotonic");

	// Epoch 1: reload. Sessions survive; inventories reset; bodies carry over.
	const uint64_t gen0 = sim.worldGenerationId();
	CHECK(sim.reload(44), "transient reload succeeds");
	CHECK(sim.worldGenerationId() == gen0 + 1, "reload bumps the world generation");
	CHECK(sim.localPlayerId() == local, "the local anchor id is epoch-stable");
	CHECK(sim.hasPlayer(local) && sim.hasPlayer(c) && sim.hasPlayer(d), "sessions survive the reload");
	CHECK(!sim.hasPlayer(b), "a retired id stays retired across the epoch");
	CHECK(emptyInventory(inventoryOf(sim, local)), "reload resets the local inventory");
	CHECK(emptyInventory(inventoryOf(sim, c)), "reload resets every session's inventory");
	CHECK(emptyInventory(inventoryOf(sim, d)), "reload resets the newest session's inventory");
	CHECK(sim.playerFeet() == feetLocal, "local body carries over until the host respawns");
	CHECK(sim.playerFeet(b) == glm::dvec3(0.0), "retired B still addresses nobody after reload");
	CHECK(sim.playerFeet(c) == feetC, "C's body carries over the reload");
	const sim::PlayerId e = sim.createPlayer();
	CHECK(e.valid() && d < e, "the player-id allocator never rewinds on reload");

	// The host respawn step works after the reload (solo contract generalized).
	sim.bootstrapTerrain(center);
	const auto outcome = sim.finalizePlayerSpawn(center);
	CHECK(finite(outcome.eye), "local respawn usable after reload");

	// Epoch 2: initialize. Same continuity guarantees.
	sim::GameSimulation::Config cfg2;
	cfg2.seed = 45;
	sim.initialize(cfg2, center);
	CHECK(sim.seed() == 45, "initialize adopts the new seed");
	CHECK(sim.localPlayerId() == local && sim.hasPlayer(c) && sim.hasPlayer(d) && sim.hasPlayer(e),
		  "sessions survive initialize too");
	CHECK(!sim.hasPlayer(b), "retired ids never come back");
	CHECK(emptyInventory(inventoryOf(sim, local)) && emptyInventory(inventoryOf(sim, c)),
		  "initialize resets every inventory");
	const sim::PlayerId f = sim.createPlayer();
	CHECK(f.valid() && e < f, "the allocator never rewinds on initialize");
	CHECK(sim.selectHotbarSlot(b, 0) == ItemResult::InvalidPlayer, "stale commands still refuse after epochs");
	CHECK(sim.playerFeet(b) == glm::dvec3(0.0), "stale queries still refuse after epochs");
}

// ---------------------------------------------------------------------------
// 5. Solo compatibility: legacy APIs == explicit local-player APIs, bit for
//    bit (one source of truth); legacy commands work before initialize().
// ---------------------------------------------------------------------------
void runSoloScript(sim::GameSimulation &sim, bool explicitLocal)
{
	const sim::PlayerId id = sim.localPlayerId();
	sim::GameSimulation::Config cfg;
	cfg.seed = 1337;
	const glm::dvec3 boot = sim.initialize(cfg, glm::dvec3(8.0, 100.0, 8.0));
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);
	const glm::dvec3 spawn = sim.playerFeet();

	for (int i = 0; i < 300; ++i)
	{
		sim::PlayerCommand cmd{};
		const int leg = (i / 60) % 4;
		cmd.move = leg == 0 ? glm::dvec2(1, 0) : leg == 1 ? glm::dvec2(0, 1)
				 : leg == 2 ? glm::dvec2(-1, 0)
							: glm::dvec2(0, -1);
		cmd.sprint = (i % 120) >= 60;
		cmd.jumpPressed = (i == 30 || i == 170);
		if (explicitLocal)
			sim.advancePlayer(id, 1.0 / 60.0, cmd, false);
		else
			sim.advancePlayer(1.0 / 60.0, cmd, false);
		sim.advanceMobs(1.0 / 60.0, sim.playerFeet(), 40.0, false);
		sim.advanceItems(1.0 / 60.0, false);
		if (i == 100)
			sim.placeVoxel({static_cast<float>(spawn.x + 3.0), static_cast<float>(spawn.y + 4.0),
							static_cast<float>(spawn.z + 3.0)}, STONE);
		if (i == 150)
		{
			if (explicitLocal)
				sim.grantItems(id, ItemId::Stone, 20);
			else
				sim.grantItems(ItemId::Stone, 20);
		}
		if (i == 160)
		{
			if (explicitLocal)
				sim.selectHotbarSlot(id, 2);
			else
				sim.selectHotbarSlot(2);
		}
		if (i == 170)
		{
			if (explicitLocal)
				sim.moveStack(id, 0, 9);
			else
				sim.moveStack(0, 9);
		}
		if (i == 180)
		{
			if (explicitLocal)
				sim.setPlayerFlight(id, true, sim.playerFeet(id));
			else
				sim.setPlayerFlight(true, sim.playerFeet());
		}
		if (i == 220)
		{
			const glm::dvec3 target(spawn.x, spawn.y + 6.0, spawn.z);
			if (explicitLocal)
				sim.setPlayerPosition(id, target);
			else
				sim.setPlayerPosition(target);
		}
		if (i == 240)
		{
			if (explicitLocal)
				sim.restorePlayerFlight(id, false, sim.playerFeet(id));
			else
				sim.restorePlayerFlight(false, sim.playerFeet());
		}
		sim.pumpWorld();
	}
}

void testSoloCompatibility(ThreadPool &pool)
{
	CoutSilencer silence;
	// Legacy commands are total BEFORE initialize (the inventory exists from
	// construction — the historical contract, now the local session).
	{
		sim::GameSimulation sim(nullptr);
		CHECK(sim.grantItems(ItemId::Stone, 5).result == ItemResult::Ok, "pre-init grant works");
		CHECK(sim.selectHotbarSlot(3) == ItemResult::Ok, "pre-init selection works");
		CHECK(sim.playerSnapshot().position == glm::dvec3(0.0), "pre-init body at the origin");
		sim::PlayerCommand cmd{};
		sim.advancePlayer(1.0 / 60.0, cmd, false); // no world: safe no-step
		sim::InventorySnapshot inv;
		sim.inventorySnapshot(inv);
		CHECK(inv.slots[0] == S(ItemId::Stone, 5) && inv.selectedSlot == 3, "pre-init state intact");
	}

	sim::GameSimulation legacy(&pool);
	sim::GameSimulation explicitSim(&pool);
	runSoloScript(legacy, false);
	runSoloScript(explicitSim, true);

	const sim::PlayerId id = explicitSim.localPlayerId();
	const sim::PlayerSnapshot viaLegacy = legacy.playerSnapshot();
	sim::PlayerSnapshot viaExplicit;
	CHECK(explicitSim.playerSnapshot(id, viaExplicit), "explicit local snapshot fills");
	CHECK(samePlayer(viaLegacy, viaExplicit), "legacy and explicit-local player state bit-identical");

	sim::InventorySnapshot invLegacy, invExplicit;
	legacy.inventorySnapshot(invLegacy);
	CHECK(explicitSim.inventorySnapshot(id, invExplicit), "explicit local inventory fills");
	CHECK(invLegacy == invExplicit, "legacy and explicit-local inventory bit-identical");

	const sim::WorldSnapshot wLegacy = legacy.worldSnapshot();
	const sim::WorldSnapshot wExplicit = explicitSim.worldSnapshot();
	CHECK(wLegacy.seed == wExplicit.seed && wLegacy.editRevision == wExplicit.editRevision &&
			  wLegacy.chunkCount == wExplicit.chunkCount &&
			  wLegacy.worldGenerationId == wExplicit.worldGenerationId,
		  "legacy and explicit-local world identity identical");
	CHECK(sameItems(legacy.itemEntities().entities(), explicitSim.itemEntities().entities()),
		  "shared item population identical through both API paths");
	CHECK(legacy.mobs().mobs().size() == explicitSim.mobs().mobs().size(),
		  "shared mob population identical through both API paths");
	CHECK(legacy.playerFeet() == explicitSim.playerFeet(id), "feet accessors agree");
	CHECK(legacy.playerFlight() == explicitSim.playerFlight(id), "flight accessors agree");
	CHECK(legacy.selectedBlock() == explicitSim.playerSnapshot().selectedBlock, "selection accessors agree");
}

// ---------------------------------------------------------------------------
// 6. A1 pickup boundary (pinned for #247): advanceItems serves the LOCAL
//    player only — a non-local player standing on a drop does not pick it up.
// ---------------------------------------------------------------------------
void testPickupBoundaryA1(ThreadPool &pool)
{
	CoutSilencer silence;
	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 1337;
	const glm::dvec3 boot = sim.initialize(cfg, glm::dvec3(8.0, 100.0, 8.0));
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);

	const sim::PlayerId b = sim.createPlayer();
	const glm::dvec3 spawn = sim.playerFeet();
	const glm::ivec3 column = findSolidTopColumn(*sim.world(), static_cast<int>(std::floor(spawn.x)) + 3,
												 static_cast<int>(std::floor(spawn.z)) + 3);
	CHECK(column.y >= 0, "solid fixture column found");
	const glm::ivec3 cell(column.x, column.y + 2, column.z);
	sim.placeVoxel({cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f}, STONE);
	sim.pumpWorld();

	// B stands where the drop will land; the local player hovers far away.
	const glm::dvec3 dropSpot(cell.x + 0.5, column.y + 1.0, cell.z + 0.5);
	sim.setPlayerPosition(b, dropSpot);
	sim.setPlayerFlight(true, spawn + glm::dvec3(20.0, 10.0, 20.0));
	CHECK(sim.breakBlock(cell) == sim::BreakResult::Ok, "fixture break accepted");
	CHECK(sim.itemEntities().size() == 1, "one live drop");

	for (int i = 0; i < 90; ++i)
		sim.advanceItems(1.0 / 60.0, false);
	CHECK(sim.itemEntities().size() == 1, "A1: a non-local player on the drop does NOT pick it up");
	CHECK(emptyInventory(inventoryOf(sim, sim.localPlayerId())), "local inventory stays empty");
	CHECK(emptyInventory(inventoryOf(sim, b)), "B's inventory stays empty (pickup is local-only in A1)");

	// Bring the LOCAL player to the drop: pickup merges into the local
	// inventory hotbar-first (the unchanged #226 contract).
	sim.setPlayerPosition(dropSpot);
	for (int i = 0; i < 90; ++i)
		sim.advanceItems(1.0 / 60.0, false);
	CHECK(sim.itemEntities().size() == 0, "the local player picks the drop up");
	const sim::InventorySnapshot inv = inventoryOf(sim, sim.localPlayerId());
	CHECK(inv.slots[0] == S(ItemId::Cobblestone, 1), "pickup lands in the LOCAL inventory (Stone -> Cobblestone)");
	CHECK(emptyInventory(inventoryOf(sim, b)), "B's inventory untouched by the local pickup");
}

// ---------------------------------------------------------------------------
// 7. Per-player fixed rates: independent 120 Hz stepping + bounded catch-up.
// ---------------------------------------------------------------------------
void testPerPlayerRates(ThreadPool &pool)
{
	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 1337;
	const glm::dvec3 boot = sim.initialize(cfg, glm::dvec3(8.0, 100.0, 8.0));
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);
	const sim::PlayerId b = sim.createPlayer();
	sim.setPlayerPosition(b, sim.playerFeet() + glm::dvec3(2.0, 0.0, 2.0));

	static_assert(physics::PlayerController::fixedStep == 1.0 / sim::kPlayerTickHz,
				  "player controller pinned to the 120 Hz contract");
	sim::PlayerCommand cmd{};
	sim.advancePlayer(1.0 / 30.0, cmd, false);
	sim.advancePlayer(b, 1.0 / 30.0, cmd, false);
	CHECK(sim.playerSnapshot().metrics.steps == 4, "local: 4 steps per 1/30 s frame");
	CHECK(playerOf(sim, b).metrics.steps == 4, "B: 4 steps per 1/30 s frame");
	sim.advancePlayer(b, 1.0 / 240.0, cmd, false);
	CHECK(playerOf(sim, b).metrics.steps == 0, "B: sub-step frame accumulates");
	sim.advancePlayer(b, 1.0 / 240.0, cmd, false);
	CHECK(playerOf(sim, b).metrics.steps == 1, "B: accumulated sub-steps fire exactly once");

	// Delta spike: per-player bounded catch-up, independent counters.
	const uint64_t localDropped = sim.playerSnapshot().metrics.droppedSteps;
	sim.advancePlayer(b, 10.0, cmd, false);
	const sim::PlayerSnapshot spikeB = playerOf(sim, b);
	CHECK(spikeB.metrics.steps <= physics::PlayerController::maxSteps, "B catch-up stays bounded");
	CHECK(spikeB.metrics.droppedSteps > 0, "B drops excess steps after the spike");
	CHECK(sim.playerSnapshot().metrics.droppedSteps == localDropped, "B's spike never touches the local counter");
	CHECK(finite(sim.playerFeet(b)), "B position finite after the spike");
}

// ---------------------------------------------------------------------------
// 8. A1 persistence boundary (pinned for #259): the solo save format captures
//    the LOCAL player only — non-local sessions are not persisted yet.
// ---------------------------------------------------------------------------
void testPersistenceBoundaryA1(ThreadPool &pool)
{
	CoutSilencer silence;
	TempRoot root("persistence");
	const glm::dvec3 center(8.0, 100.0, 8.0);
	{
		sim::GameSimulation sim(&pool);
		sim::GameSimulation::Config cfg;
		cfg.seed = 42;
		cfg.worldName = "mp-a1";
		cfg.savesRoot = root.path;
		const glm::dvec3 boot = sim.initialize(cfg, center);
		CHECK(sim.isWorldOpen(), "persistent world opened");
		sim.bootstrapTerrain(boot);
		sim.finalizePlayerSpawn(boot);
		const sim::PlayerId b = sim.createPlayer();
		sim.grantItems(ItemId::Stone, 12);
		sim.grantItems(b, ItemId::Dirt, 30);
		sim.selectHotbarSlot(b, 4);
		CHECK(sim.savePlayerState(10.f, -5.f), "durable capture succeeds with two sessions live");
		sim.shutdown();
	}
	{
		sim::GameSimulation sim(&pool);
		sim::GameSimulation::Config cfg;
		cfg.seed = 0;
		cfg.worldName = "mp-a1";
		cfg.savesRoot = root.path;
		const glm::dvec3 boot = sim.initialize(cfg, center);
		sim.bootstrapTerrain(boot);
		const auto outcome = sim.finalizePlayerSpawn(boot);
		CHECK(outcome.restored, "the LOCAL player restores from the solo format");
		CHECK(sim.playerCount() == 1, "A1: non-local sessions are not persisted (multi-player checkpoints are #259)");
		const sim::InventorySnapshot inv = inventoryOf(sim, sim.localPlayerId());
		CHECK(inv.slots[0] == S(ItemId::Stone, 12), "the LOCAL inventory restores unchanged");
		sim.shutdown();
	}
}
} // namespace

int main()
{
	std::printf("test_player_sessions: CPU-only multi-player sessions (no GPU, no SDL, no camera)\n");
	ThreadPool pool(2);
	testSessionIdentity();
	testTwoPlayerIndependence(pool);
	testRemovalIsolation(pool);
	testStaleHandlesAndEpochs(pool);
	testSoloCompatibility(pool);
	testPickupBoundaryA1(pool);
	testPerPlayerRates(pool);
	testPersistenceBoundaryA1(pool);
	std::printf("%ld checks, %ld testmain::fails\n", testmain::checks, testmain::fails);
	if (testmain::fails == 0)
		std::printf("PASS\n");
	return testmain::exitCode();
}
