// test_actor_interactions — CPU-only actor-aware gameplay acceptance (issue
// #247, Epic #245 Phase A2). Links ONLY ft_vox_core: no SDL, no Vulkan device,
// no ImGui, no camera. Proves the issue's acceptance list end-to-end:
//
//   * two players contend for one block: at most one accepted break/drop and
//     one accepted place; no duplicated item generation, no double consume;
//   * two players attempt pickup of one entity: exactly one aggregate
//     conserved count, reproducible winner / tie-break (ascending PlayerId),
//     partial merges offered to the next eligible player in the same tick;
//   * separate inventories and hotbar selections; per-player quantity
//     conserved across drop/placement failures and item-ID exhaustion;
//   * reject out-of-range, invalid-actor, overlapping-player and
//     unknown-terrain interactions WITHOUT side effects;
//   * actor-aware targeting derives the ray origin from the actor's
//     authoritative eye and clamps reach to kInteractionReach (never a
//     client-supplied origin / distance);
//   * deferred edits and the solo compatibility forwards stay green: legacy
//     break/place/drop == actor-aware(local) bit for bit (one source of truth).
//
// Style mirrors test_gameplay / test_player_sessions: CHECK macro, plain
// main(), controlled residency (synchronous bootstrap, no async streaming)
// and a fixed host clock for determinism. System-level pickup fixtures use a
// flat synthetic world + explicit PickupCandidate arrays (precise control of
// the priority order); facade-level fixtures use real player sessions.

#include <Sim/GameSimulation.hpp>
#include <Sim/PlayerId.hpp>
#include <Core/ThreadPool.hpp>
#include <Items/ItemEntitySystem.hpp>
#include <Items/ItemRegistry.hpp>
#include <World/WorldCollisionView.hpp>
#include <World/WorldSave.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "TestMain.hpp"

namespace
{
using items::ItemId;
using items::ItemResult;
using items::ItemStack;

constexpr ItemStack S(ItemId id, uint16_t count) { return {id, count}; }
constexpr ItemStack kEmpty{};

// --- Helpers -------------------------------------------------------------------

sim::InventorySnapshot inventoryOf(const sim::GameSimulation &sim, sim::PlayerId id)
{
	sim::InventorySnapshot out;
	CHECK(sim.inventorySnapshot(id, out), "per-player inventory snapshot fills for a live id");
	return out;
}

uint64_t inventoryTotal(const sim::InventorySnapshot &s)
{
	uint64_t total = 0;
	for (const ItemStack &slot : s.slots)
		total += slot.count;
	return total;
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

// Highest occupied cell of a column (solid OR fluid). Caller owns the view.
int topOccupiedY(const WorldCollisionView &view, int x, int z)
{
	for (int y = WORLD_HEIGHT - 1; y >= 0; --y)
	{
		const auto cell = view.sample({x, y, z});
		if (cell.available && (cell.solid || cell.medium != physics::Medium::Air))
			return y;
	}
	return -1;
}

// First breakable SOLID cell of a column, scanning from the sky down.
bool findBreakableCell(const VoxelWorld &world, int x, int z, glm::ivec3 &out, TextureType &type)
{
	WorldCollisionView view(world);
	for (int y = WORLD_HEIGHT - 1; y >= 1; --y)
	{
		TextureType t = AIR;
		const physics::Cell c = view.sampleWithType({x, y, z}, &t);
		if (!c.available)
			return false;
		if (c.solid && items::isBreakableBlock(t))
		{
			out = {x, y, z};
			type = t;
			return true;
		}
	}
	return false;
}

// A breakable surface cell with four AIR cells of headroom.
bool findLandCell(const VoxelWorld &world, int x0, int z0, glm::ivec3 &out, TextureType &type)
{
	for (int dx = 0; dx < 8; ++dx)
		for (int dz = 0; dz < 8; ++dz)
		{
			glm::ivec3 candidate{};
			TextureType t = AIR;
			if (!findBreakableCell(world, x0 + dx, z0 + dz, candidate, t))
				continue;
			WorldCollisionView view(world);
			bool airAbove = true;
			for (int dy = 1; dy <= 4; ++dy)
			{
				TextureType above = AIR;
				const physics::Cell c = view.sampleWithType({candidate.x, candidate.y + dy, candidate.z}, &above);
				if (!c.available || above != AIR)
				{
					airAbove = false;
					break;
				}
			}
			if (airAbove)
			{
				out = candidate;
				type = t;
				return true;
			}
		}
	return false;
}

// Bootstrap a world and build a flat STONE platform with guaranteed air above
// (the testTwoPlayerIndependence fixture): deterministic player placement.
// Returns the platform origin cell (px, py, pz); the platform top is py + 1.
glm::ivec3 setupPlatform(sim::GameSimulation &sim, int seed)
{
	sim::GameSimulation::Config cfg;
	cfg.seed = seed;
	const glm::dvec3 boot = sim.initialize(cfg, glm::dvec3(8.0, 100.0, 8.0));
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);
	const glm::dvec3 spawn = sim.playerFeet();
	const int px = static_cast<int>(std::floor(spawn.x));
	const int pz = static_cast<int>(std::floor(spawn.z));
	int py;
	{
		WorldCollisionView view(*sim.world());
		py = topOccupiedY(view, px, pz) + 4; // guaranteed air gap
	}
	for (int dx = -6; dx <= 6; ++dx)
		for (int dz = -2; dz <= 2; ++dz)
			sim.placeVoxel({px + dx + 0.5f, py + 0.5f, pz + dz + 0.5f}, STONE);
	sim.pumpWorld();
	return {px, py, pz};
}

glm::dvec3 platformFeet(const glm::ivec3 &p, int dx, int dz)
{
	return {p.x + dx + 0.5, p.y + 1.0 + physics::skin, p.z + dz + 0.5};
}

std::filesystem::path makeTempDir(const char *tag)
{
	const auto base = std::filesystem::temp_directory_path() /
					  ("ft_vox_actor_" + std::string(tag) + "_" +
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

struct CoutSilencer
{
	std::ostringstream sink;
	std::streambuf *old;
	CoutSilencer() : old(std::cout.rdbuf(sink.rdbuf())) {}
	~CoutSilencer() { std::cout.rdbuf(old); }
};

// Flat synthetic world for system-level pickup fixtures: solid below y = 64.
class FlatWorld : public physics::VoxelCollisionWorld
{
  public:
	physics::Cell sample(glm::ivec3 p) const override
	{
		if (p.y < 64)
			return {true, true, physics::Medium::Air};
		return {true, false, physics::Medium::Air};
	}
};

// Fill an inventory so it has EXACTLY `room` free capacity for `item`: slot 0
// holds (maxStack - room) of `item`, every other slot a full stack of a
// DIFFERENT item. addStack(item) then merges only `room` (hotbar-first).
void fillAllButRoom(items::Inventory &inv, ItemId item, uint16_t room)
{
	std::array<ItemStack, items::kInventorySize> slots{};
	slots[0] = S(item, static_cast<uint16_t>(items::maxStackOf(item) - room));
	for (int i = 1; i < items::kInventorySize; ++i)
		slots[i] = S(ItemId::Dirt, items::maxStackOf(ItemId::Dirt));
	inv.restore(slots, 0);
}

// MSVC-proof PickupCandidate construction: member assignment, NO aggregate
// braces. MSVC misparses a braced-init-list of aggregates used to initialize a
// std::vector (even with explicit element types), so candidates are built with
// this maker and pushed into a default-constructed vector (issue #247 review).
items::PickupCandidate pickupCandidate(glm::dvec3 feet, items::Inventory *inventory)
{
	items::PickupCandidate c;
	c.feet = feet;
	c.inventory = inventory;
	return c;
}

// ---------------------------------------------------------------------------
// 1. Break contention: two players break one block — at most one accepted
//    break, exactly one drop, no duplicated item generation.
// ---------------------------------------------------------------------------
void testBreakContention(ThreadPool &pool)
{
	CoutSilencer silence;
	sim::GameSimulation sim(&pool);
	const glm::ivec3 p = setupPlatform(sim, 1337);
	const sim::PlayerId a = sim.localPlayerId();
	const sim::PlayerId b = sim.createPlayer();
	sim.setPlayerPosition(a, platformFeet(p, -3, 0));
	sim.setPlayerPosition(b, platformFeet(p, 3, 0));

	// A target block sitting on the platform, in clear air.
	const glm::ivec3 cell(p.x, p.y + 1, p.z);
	sim.placeVoxel({cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f}, STONE);
	sim.pumpWorld();

	const uint64_t rev0 = sim.worldSnapshot().editRevision;
	CHECK(sim.breakBlock(a, cell) == sim::BreakResult::Ok, "first break accepted (player A)");
	CHECK(sim.breakBlock(b, cell) == sim::BreakResult::WorldRejected,
		  "second break of the same cell refused (already AIR)");
	CHECK(sim.worldSnapshot().editRevision == rev0 + 1, "exactly one committed edit");
	std::vector<sim::ItemEntitySnapshot> snaps;
	sim.itemSnapshots(snaps);
	CHECK(snaps.size() == 1 && snaps[0].stack == S(ItemId::Cobblestone, 1),
		  "one accepted break -> exactly one registry drop (no duplication)");

	// Re-breaking after publication stays a no-op for BOTH players.
	sim.pumpWorld();
	CHECK(sim.breakBlock(a, cell) == sim::BreakResult::WorldRejected, "A re-break refused");
	CHECK(sim.breakBlock(b, cell) == sim::BreakResult::WorldRejected, "B re-break refused");
	sim.itemSnapshots(snaps);
	CHECK(snaps.size() == 1, "no second drop ever spawned");
	sim.shutdown();
}

// ---------------------------------------------------------------------------
// 2. Place contention: two players place one cell — first Ok consumes 1,
//    second WorldRejected consumes nothing.
// ---------------------------------------------------------------------------
void testPlaceContention(ThreadPool &pool)
{
	CoutSilencer silence;
	sim::GameSimulation sim(&pool);
	const glm::ivec3 p = setupPlatform(sim, 1337);
	const sim::PlayerId a = sim.localPlayerId();
	const sim::PlayerId b = sim.createPlayer();
	sim.setPlayerPosition(a, platformFeet(p, -3, 0));
	sim.setPlayerPosition(b, platformFeet(p, 3, 0));
	CHECK(sim.grantItems(a, ItemId::Stone, 5).result == ItemResult::Ok, "A granted placeable items");
	CHECK(sim.grantItems(b, ItemId::Stone, 5).result == ItemResult::Ok, "B granted placeable items");
	sim.selectHotbarSlot(a, 0);
	sim.selectHotbarSlot(b, 0);

	const glm::ivec3 cell(p.x, p.y + 1, p.z); // clear air above the platform
	const uint64_t rev0 = sim.worldSnapshot().editRevision;
	CHECK(sim.placeSelectedBlock(a, cell) == sim::PlaceResult::Ok, "first place accepted (A)");
	CHECK(sim.placeSelectedBlock(b, cell) == sim::PlaceResult::WorldRejected,
		  "second place of the occupied cell refused (B)");
	CHECK(inventoryOf(sim, a).slots[0] == S(ItemId::Stone, 4), "A consumed exactly 1");
	CHECK(inventoryOf(sim, b).slots[0] == S(ItemId::Stone, 5), "refused place consumed nothing from B");
	CHECK(sim.worldSnapshot().editRevision == rev0 + 1, "exactly one committed place edit");
	{
		WorldCollisionView view(*sim.world());
		TextureType placed = AIR;
		view.sampleWithType(cell, &placed);
		CHECK(placed == STONE, "the cell holds exactly the one placed block");
	}
	sim.shutdown();
}

// ---------------------------------------------------------------------------
// 3. System-level shared pickup: priority order, partial merge, full skip,
//    out-of-range, conservation. Explicit PickupCandidate arrays give precise
//    control of the deterministic order (the facade supplies ascending id).
// ---------------------------------------------------------------------------
void testSharedPickupSystem()
{
	FlatWorld world;
	const glm::dvec3 spot(0.5, 64.125, 0.5); // resting on the y=64 ground
	const glm::dvec3 feet(0.5, 64.0, 0.5);	 // within pickup radius (1.5)
	const glm::dvec3 farFeet(100.0, 64.0, 100.0); // NB: not "far" — a Windows macro

	const auto run = [&](const std::vector<items::PickupCandidate> &candidates,
						 const ItemStack &drop) {
		items::ItemEntitySystem sys;
		sys.spawn(drop, spot, glm::dvec3(0.0), 0); // delay 0: eligible immediately
		sys.update(1.0 / 60.0, world, candidates, false);
		return sys;
	};

	// (a) Priority: the FIRST in-range candidate wins the whole stack.
	{
		items::Inventory invA, invB;
		std::vector<items::PickupCandidate> candidates;
		candidates.push_back(pickupCandidate(feet, &invA));
		candidates.push_back(pickupCandidate(feet, &invB));
		const auto sys = run(candidates, S(ItemId::Stone, 10));
		CHECK(invA.totalCount(ItemId::Stone) == 10, "first candidate (A) wins the whole stack");
		CHECK(invB.totalCount(ItemId::Stone) == 0, "second candidate (B) gets nothing");
		CHECK(sys.entities().empty(), "fully claimed entity removed");
	}
	// (b) Reversed order reverses the winner — order IS the priority.
	{
		items::Inventory invA, invB;
		std::vector<items::PickupCandidate> candidates;
		candidates.push_back(pickupCandidate(feet, &invB));
		candidates.push_back(pickupCandidate(feet, &invA));
		const auto sys = run(candidates, S(ItemId::Stone, 10));
		CHECK(invB.totalCount(ItemId::Stone) == 10, "reversed order: B wins");
		CHECK(invA.totalCount(ItemId::Stone) == 0, "reversed order: A gets nothing");
		CHECK(sys.entities().empty(), "entity removed");
	}
	// (c) Partial merge: A has room for only 4, B empty -> A 4 + B 6 = 10.
	{
		items::Inventory invA, invB;
		fillAllButRoom(invA, ItemId::Stone, 4);
		std::vector<items::PickupCandidate> candidates;
		candidates.push_back(pickupCandidate(feet, &invA));
		candidates.push_back(pickupCandidate(feet, &invB));
		const auto sys = run(candidates, S(ItemId::Stone, 10));
		CHECK(invA.totalCount(ItemId::Stone) == items::maxStackOf(ItemId::Stone),
			  "A topped up to a full stack (took 4)");
		CHECK(invB.totalCount(ItemId::Stone) == 6, "partial remainder offered to B in the same tick");
		CHECK(sys.entities().empty(), "aggregate fully claimed -> entity removed");
		const uint16_t tookA = static_cast<uint16_t>(invA.totalCount(ItemId::Stone) -
													 (items::maxStackOf(ItemId::Stone) - 4));
		CHECK(tookA + invB.totalCount(ItemId::Stone) == 10, "aggregate count conserved (A take + B take == 10)");
	}
	// (d) Full first candidate skips to the next: A full, B empty -> B all.
	{
		items::Inventory invA, invB;
		fillAllButRoom(invA, ItemId::Stone, 0); // no room for Stone
		std::vector<items::PickupCandidate> candidates;
		candidates.push_back(pickupCandidate(feet, &invA));
		candidates.push_back(pickupCandidate(feet, &invB));
		const auto sys = run(candidates, S(ItemId::Stone, 10));
		CHECK(invA.totalCount(ItemId::Stone) == items::maxStackOf(ItemId::Stone),
			  "full A took nothing");
		CHECK(invB.totalCount(ItemId::Stone) == 10, "full A skipped: B takes the whole stack");
		CHECK(sys.entities().empty(), "entity removed");
	}
	// (e) Both full: entity untouched (conservation, no loss).
	{
		items::Inventory invA, invB;
		fillAllButRoom(invA, ItemId::Stone, 0);
		fillAllButRoom(invB, ItemId::Stone, 0);
		std::vector<items::PickupCandidate> candidates;
		candidates.push_back(pickupCandidate(feet, &invA));
		candidates.push_back(pickupCandidate(feet, &invB));
		const auto sys = run(candidates, S(ItemId::Stone, 10));
		CHECK(sys.entities().size() == 1 && sys.entities()[0].stack == S(ItemId::Stone, 10),
			  "both full: entity stays with its full stack");
	}
	// (f) Out of range: a far candidate never picks up.
	{
		items::Inventory invA;
		std::vector<items::PickupCandidate> candidates;
		candidates.push_back(pickupCandidate(farFeet, &invA));
		const auto sys = run(candidates, S(ItemId::Stone, 10));
		CHECK(invA.totalCount(ItemId::Stone) == 0, "out-of-range candidate picks nothing");
		CHECK(sys.entities().size() == 1, "entity stays");
	}
	// (g) Null inventory candidate is skipped defensively.
	{
		items::Inventory invB;
		std::vector<items::PickupCandidate> candidates;
		candidates.push_back(pickupCandidate(feet, nullptr));
		candidates.push_back(pickupCandidate(feet, &invB));
		const auto sys = run(candidates, S(ItemId::Stone, 10));
		CHECK(invB.totalCount(ItemId::Stone) == 10, "null candidate skipped; B takes the stack");
		CHECK(sys.entities().empty(), "entity removed");
	}
	// (h) Legacy single-player update is bit-identical to a one-candidate batch.
	{
		items::Inventory invLegacy, invBatch;
		items::ItemEntitySystem legacy;
		legacy.spawn(S(ItemId::Stone, 7), spot, glm::dvec3(0.0), 0);
		legacy.update(1.0 / 60.0, world, feet, invLegacy, false);
		items::ItemEntitySystem batch;
		batch.spawn(S(ItemId::Stone, 7), spot, glm::dvec3(0.0), 0);
		std::vector<items::PickupCandidate> batchCandidates;
		batchCandidates.push_back(pickupCandidate(feet, &invBatch));
		batch.update(1.0 / 60.0, world, batchCandidates, false);
		CHECK(invLegacy.slots() == invBatch.slots(), "legacy single-player pickup == one-candidate batch");
		CHECK(legacy.entities().size() == batch.entities().size(), "same population outcome");
	}
}

// ---------------------------------------------------------------------------
// 4. Facade-level shared pickup: two real sessions contend for one settled
//    drop — ascending PlayerId wins, reproducible across runs, conserved.
// ---------------------------------------------------------------------------
void testSharedPickupFacade(ThreadPool &pool)
{
	const auto fixture = [&](ThreadPool &p) {
		CoutSilencer silence;
		sim::GameSimulation sim(&p);
		const glm::ivec3 plat = setupPlatform(sim, 1337);
		const sim::PlayerId a = sim.localPlayerId();
		const sim::PlayerId b = sim.createPlayer();
		const glm::dvec3 farAway(8.0, 200.0, 8.0);
		sim.setPlayerFlight(a, true, farAway);
		sim.setPlayerFlight(b, true, farAway);

		// Break a block and let the drop SETTLE while both players are far.
		const glm::ivec3 cell(plat.x, plat.y + 1, plat.z);
		sim.placeVoxel({cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f}, STONE);
		sim.pumpWorld();
		CHECK(sim.breakBlock(a, cell) == sim::BreakResult::Ok, "fixture break accepted");
		for (int i = 0; i < 120; ++i)
			sim.advanceItems(1.0 / 60.0, false);
		std::vector<sim::ItemEntitySnapshot> snaps;
		sim.itemSnapshots(snaps);
		CHECK(snaps.size() == 1, "one settled drop before contention");
		const glm::dvec3 dropPos = snaps[0].current;

		// Teleport BOTH players within pickup radius of the settled drop.
		// advanceItems does not step player physics, so the feet stay put.
		sim.setPlayerPosition(a, dropPos + glm::dvec3(0.2, 0.0, 0.0));
		sim.setPlayerPosition(b, dropPos + glm::dvec3(-0.2, 0.0, 0.0));
		for (int i = 0; i < 5; ++i)
			sim.advanceItems(1.0 / 60.0, false);

		const sim::InventorySnapshot invA = inventoryOf(sim, a);
		const sim::InventorySnapshot invB = inventoryOf(sim, b);
		sim.itemSnapshots(snaps);
		const uint64_t live = snaps.empty() ? 0 : snaps[0].stack.count;
		const uint64_t total = inventoryTotal(invA) + inventoryTotal(invB) + live;
		CHECK(total == 1, "aggregate conserved: exactly one Cobblestone across players + world");
		CHECK(invA.slots[0] == S(ItemId::Cobblestone, 1), "ascending PlayerId: local (A) wins the drop");
		CHECK(emptyInventory(invB), "higher-id B gets nothing when A can hold it all");
		CHECK(snaps.empty(), "fully claimed drop removed");
		sim.shutdown();
		return std::pair{invA, invB};
	};
	const auto r1 = fixture(pool);
	const auto r2 = fixture(pool);
	CHECK(r1.first == r2.first && r1.second == r2.second, "shared pickup winner reproducible across runs");
}

// ---------------------------------------------------------------------------
// 5. Invalid actor: every per-player command/query refuses WITHOUT side
//    effects (no world edit, no inventory change, no item spawn).
// ---------------------------------------------------------------------------
void testInvalidActorRejections(ThreadPool &pool)
{
	CoutSilencer silence;
	sim::GameSimulation sim(&pool);
	const glm::ivec3 p = setupPlatform(sim, 1337);
	const sim::PlayerId local = sim.localPlayerId();
	const sim::PlayerId ghost{999}; // never allocated
	const sim::PlayerId retired = sim.createPlayer();
	CHECK(sim.removePlayer(retired), "fixture: retire a session");
	CHECK(sim.grantItems(local, ItemId::Stone, 5).result == ItemResult::Ok, "local has items");

	const glm::ivec3 cell(p.x, p.y + 1, p.z);
	sim.placeVoxel({cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f}, STONE);
	sim.pumpWorld();
	const sim::WorldSnapshot w0 = sim.worldSnapshot();
	const sim::InventorySnapshot inv0 = inventoryOf(sim, local);
	const size_t items0 = sim.itemEntities().size();

	const sim::PlayerId badIds[] = {ghost, retired, sim::kInvalidPlayerId};
	for (const sim::PlayerId bad : badIds)
	{
		CHECK(sim.breakBlock(bad, cell) == sim::BreakResult::InvalidPlayer, "invalid-actor break refused");
		CHECK(sim.placeSelectedBlock(bad, cell) == sim::PlaceResult::InvalidPlayer, "invalid-actor place refused");
		CHECK(sim.dropFromSlot(bad, 0, 1, {0, 1, 0}) == ItemResult::InvalidPlayer, "invalid-actor drop refused");
		CHECK(sim.selectHotbarSlot(bad, 0) == ItemResult::InvalidPlayer, "invalid-actor selection refused");
		CHECK(sim.moveStack(bad, 0, 1) == ItemResult::InvalidPlayer, "invalid-actor move refused");
		CHECK(sim.swapStacks(bad, 0, 1) == ItemResult::InvalidPlayer, "invalid-actor swap refused");
		CHECK(sim.splitStack(bad, 0, 1) == ItemResult::InvalidPlayer, "invalid-actor split refused");
		const items::AddResult grant = sim.grantItems(bad, ItemId::Stone, 3);
		CHECK(grant.result == ItemResult::InvalidPlayer && grant.remainder == S(ItemId::Stone, 3),
			  "invalid-actor grant refuses and echoes the request");
		CHECK(!sim.canPlaceVoxel(bad, {cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f}, STONE),
			  "invalid-actor placement query refuses");
		CHECK(!sim.raycastVoxel(bad, {0, -1, 0}, 8.f).hit, "invalid-actor targeting answers no-hit");
	}

	// NOTHING changed: world identity/revision, local inventory, item population.
	const sim::WorldSnapshot w1 = sim.worldSnapshot();
	CHECK(w1.editRevision == w0.editRevision && w1.chunkCount == w0.chunkCount,
		  "invalid-actor commands never edited the world");
	CHECK(inventoryOf(sim, local) == inv0, "invalid-actor commands never touched the local inventory");
	CHECK(sim.itemEntities().size() == items0, "invalid-actor commands never spawned items");
	{
		WorldCollisionView view(*sim.world());
		TextureType still = AIR;
		view.sampleWithType(cell, &still);
		CHECK(still == STONE, "the target block is intact (no invalid-actor break)");
	}
	sim.shutdown();
}

// ---------------------------------------------------------------------------
// 6. Overlapping-player protection: a solid placement overlapping ANY walking
//    body is refused (deterministic); flying bodies are exempt; non-solid ok.
// ---------------------------------------------------------------------------
void testOverlapProtection(ThreadPool &pool)
{
	CoutSilencer silence;
	sim::GameSimulation sim(&pool);
	const glm::ivec3 p = setupPlatform(sim, 1337);
	const sim::PlayerId a = sim.localPlayerId();
	const sim::PlayerId b = sim.createPlayer();
	sim.setPlayerPosition(a, platformFeet(p, -3, 0));
	sim.setPlayerPosition(b, platformFeet(p, 3, 0)); // B walking at dx=+3
	CHECK(sim.grantItems(a, ItemId::Stone, 10).result == ItemResult::Ok, "A granted items");
	sim.selectHotbarSlot(a, 0);

	// B's body cell (floor of B feet): a solid here overlaps walking B.
	const glm::dvec3 feetB = sim.playerFeet(b);
	const glm::ivec3 bCell(static_cast<int>(std::floor(feetB.x)),
						   static_cast<int>(std::floor(feetB.y)),
						   static_cast<int>(std::floor(feetB.z)));
	const glm::vec3 bCenter(bCell.x + 0.5f, bCell.y + 0.5f, bCell.z + 0.5f);

	// A (walking, far from B) places into B's body -> PlayerOverlap, no consume.
	CHECK(!sim.canPlaceVoxel(bCenter, STONE), "all-body query refuses a solid in walking B");
	CHECK(sim.placeSelectedBlock(a, bCell) == sim::PlaceResult::PlayerOverlap,
		  "A cannot entomb walking B");
	CHECK(inventoryOf(sim, a).slots[0] == S(ItemId::Stone, 10), "refused overlap consumed nothing");

	// A placing into its OWN walking body is also refused.
	const glm::dvec3 feetA = sim.playerFeet(a);
	const glm::ivec3 aCell(static_cast<int>(std::floor(feetA.x)),
						   static_cast<int>(std::floor(feetA.y)),
						   static_cast<int>(std::floor(feetA.z)));
	CHECK(sim.placeSelectedBlock(a, aCell) == sim::PlaceResult::PlayerOverlap,
		  "A cannot entomb itself while walking");

	// Non-solid blocks never collide with any body.
	CHECK(sim.canPlaceVoxel(bCenter, WATER), "non-solid placement ignores bodies");

	// B flies -> B exempt; A may now place into B's (former) body cell.
	CHECK(sim.setPlayerFlight(b, true, feetB), "B takes flight");
	CHECK(sim.canPlaceVoxel(bCenter, STONE), "flying B is exempt from the overlap rule");
	CHECK(sim.placeSelectedBlock(a, bCell) == sim::PlaceResult::Ok, "A places into flying B's cell");
	CHECK(inventoryOf(sim, a).slots[0] == S(ItemId::Stone, 9), "successful place consumed 1");

	// A flying may place into its OWN body, but a walking B still blocks.
	// Clear the block we just placed in B's cell FIRST so B can land cleanly.
	sim.deleteVoxel(bCenter);
	sim.pumpWorld();
	CHECK(sim.setPlayerFlight(a, true, platformFeet(p, -3, 0)), "A takes flight");
	CHECK(sim.setPlayerFlight(b, false, feetB), "B lands again (walking)");
	const glm::ivec3 aBodyCell(static_cast<int>(std::floor(sim.playerFeet(a).x)),
							   static_cast<int>(std::floor(sim.playerFeet(a).y)),
							   static_cast<int>(std::floor(sim.playerFeet(a).z)));
	CHECK(sim.placeSelectedBlock(a, aBodyCell) == sim::PlaceResult::Ok,
		  "flying A may place into its own body (creative rule)");
	CHECK(sim.placeSelectedBlock(a, bCell) == sim::PlaceResult::PlayerOverlap,
		  "flying A still cannot entomb walking B");
	sim.shutdown();
}

// ---------------------------------------------------------------------------
// 7. Unknown terrain + out-of-range/bounds rejections without side effects.
// ---------------------------------------------------------------------------
void testUnknownTerrainAndBounds(ThreadPool &pool)
{
	CoutSilencer silence;
	sim::GameSimulation sim(&pool);
	const glm::ivec3 p = setupPlatform(sim, 1337);
	const sim::PlayerId a = sim.localPlayerId();
	sim.setPlayerPosition(a, platformFeet(p, 0, 0));
	CHECK(sim.grantItems(a, ItemId::Stone, 5).result == ItemResult::Ok, "grant items");
	sim.selectHotbarSlot(a, 0);
	const sim::InventorySnapshot inv0 = inventoryOf(sim, a);

	// Unknown terrain: a far unloaded cell (within the world coordinate
	// envelope, well outside the bootstrapped region).
	CHECK(sim.breakBlock(a, {1000, 100, 1000}) == sim::BreakResult::UnknownTerrain,
		  "unloaded break answers UnknownTerrain");
	CHECK(sim.placeSelectedBlock(a, {1000, 100, 1000}) == sim::PlaceResult::UnknownTerrain,
		  "unloaded place answers UnknownTerrain");
	CHECK(inventoryOf(sim, a) == inv0, "unknown-terrain place consumed nothing");

	// Out-of-bounds cell (above the world): WorldRejected, no consume.
	CHECK(sim.breakBlock(a, {p.x, WORLD_HEIGHT + 5, p.z}) == sim::BreakResult::WorldRejected,
		  "out-of-bounds break refused");
	CHECK(sim.placeSelectedBlock(a, {p.x, WORLD_HEIGHT + 5, p.z}) == sim::PlaceResult::WorldRejected,
		  "out-of-bounds place refused");
	CHECK(inventoryOf(sim, a) == inv0, "out-of-bounds place consumed nothing");

	// Out-of-range slot: InvalidSlot, no consume / no entity.
	CHECK(sim.dropFromSlot(a, 99, 1, {0, 1, 0}) == ItemResult::InvalidSlot, "invalid drop slot refused");
	CHECK(sim.dropFromSlot(a, -1, 1, {0, 1, 0}) == ItemResult::InvalidSlot, "negative drop slot refused");
	CHECK(sim.selectHotbarSlot(a, items::kHotbarSize) == ItemResult::InvalidSlot, "invalid hotbar refused");
	CHECK(inventoryOf(sim, a) == inv0, "invalid-slot commands consumed nothing");
	CHECK(sim.itemEntities().size() == 0, "invalid-slot commands spawned nothing");
	sim.shutdown();
}

// ---------------------------------------------------------------------------
// 8. Actor-aware targeting: origin derived from the actor eye, reach clamped
//    to kInteractionReach; legacy raw-origin parity for the local actor.
// ---------------------------------------------------------------------------
void testActorTargeting(ThreadPool &pool)
{
	CoutSilencer silence;
	sim::GameSimulation sim(&pool);
	const glm::ivec3 p = setupPlatform(sim, 1337);
	const sim::PlayerId a = sim.localPlayerId();
	const glm::dvec3 feet = platformFeet(p, 0, 0);
	sim.setPlayerPosition(a, feet);
	const glm::vec3 eye(sim.playerEye(a));
	const glm::vec3 forward(1.f, 0.f, 0.f);
	const int eyeY = static_cast<int>(std::floor(eye.y));

	// Clear a horizontal corridor at eye level along +X (out to beyond the
	// reach) so natural terrain / vegetation cannot create stray hits: the
	// targeting fixture is fully controlled. The ray has no Y component, so
	// it stays in the eyeY row for its whole march.
	for (int dx = 1; dx <= 10; ++dx)
		sim.deleteVoxel({p.x + dx + 0.5f, eyeY + 0.5f, p.z + 0.5f});
	sim.pumpWorld();
	CHECK(!sim.raycastVoxel(a, forward, sim::GameSimulation::kInteractionReach).hit,
		  "baseline: the cleared corridor has no target within reach");

	// A block WITHIN reach along +X from the eye is hit.
	const glm::ivec3 nearCell(p.x + 3, eyeY, p.z);
	sim.placeVoxel({nearCell.x + 0.5f, nearCell.y + 0.5f, nearCell.z + 0.5f}, STONE);
	sim.pumpWorld();
	const sim::RaycastHit hit = sim.raycastVoxel(a, forward, sim::GameSimulation::kInteractionReach);
	CHECK(hit.hit && hit.block == glm::vec3(nearCell), "actor-aware ray hits the in-reach block from the actor eye");

	// Legacy raw-origin form with origin == eye and dist <= reach is bit-identical.
	const sim::RaycastHit legacy = sim.raycastVoxel(eye, forward, sim::GameSimulation::kInteractionReach);
	CHECK(legacy.hit == hit.hit && legacy.block == hit.block && legacy.adjacent == hit.adjacent,
		  "legacy(eye, dir, reach) == actor-aware(local, dir, reach)");

	// A block BEYOND reach is never hit, even with a huge requested distance.
	sim.deleteVoxel({nearCell.x + 0.5f, nearCell.y + 0.5f, nearCell.z + 0.5f});
	const glm::ivec3 farCell(p.x + 12, eyeY, p.z);
	sim.placeVoxel({farCell.x + 0.5f, farCell.y + 0.5f, farCell.z + 0.5f}, STONE);
	sim.pumpWorld();
	CHECK(!sim.raycastVoxel(a, forward, 100.f).hit,
		  "reach clamped: a block beyond kInteractionReach is not targetable");
	CHECK(!sim.raycastVoxel(a, forward, sim::GameSimulation::kInteractionReach).hit,
		  "at exactly the reach the far block is still out of range");

	// A shorter requested distance is honored (subset of the reach).
	const glm::ivec3 midCell(p.x + 3, eyeY, p.z);
	sim.placeVoxel({midCell.x + 0.5f, midCell.y + 0.5f, midCell.z + 0.5f}, STONE);
	sim.pumpWorld();
	CHECK(sim.raycastVoxel(a, forward, 8.f).hit, "in-reach block hit at full reach");
	CHECK(!sim.raycastVoxel(a, forward, 1.f).hit, "a shorter requested distance excludes the block");

	// Invalid actor / degenerate direction answer a safe no-hit.
	CHECK(!sim.raycastVoxel(sim::PlayerId{999}, forward, 8.f).hit, "invalid actor no-hit");
	CHECK(!sim.raycastVoxel(a, glm::vec3(0.f), 8.f).hit, "degenerate direction no-hit");
	sim.shutdown();
}

// ---------------------------------------------------------------------------
// 9. Solo compatibility: legacy break/place/drop == actor-aware(local) bit for
//    bit (one source of truth, no parallel solo state).
// ---------------------------------------------------------------------------
void testSoloParity(ThreadPool &pool)
{
	const auto script = [&](sim::GameSimulation &sim, bool explicitLocal) {
		CoutSilencer silence;
		const sim::PlayerId id = sim.localPlayerId();
		const glm::ivec3 p = setupPlatform(sim, 4242);
		sim.setPlayerPosition(platformFeet(p, 0, 0));
		const auto grant = [&](ItemId item, uint16_t n) {
			return explicitLocal ? sim.grantItems(id, item, n) : sim.grantItems(item, n);
		};
		const auto select = [&](int s) {
			return explicitLocal ? sim.selectHotbarSlot(id, s) : sim.selectHotbarSlot(s);
		};
		const auto brk = [&](const glm::ivec3 &c) {
			return explicitLocal ? sim.breakBlock(id, c) : sim.breakBlock(c);
		};
		const auto place = [&](const glm::ivec3 &c) {
			return explicitLocal ? sim.placeSelectedBlock(id, c) : sim.placeSelectedBlock(c);
		};
		const auto drop = [&](int s, uint16_t n) {
			return explicitLocal ? sim.dropFromSlot(id, s, n, glm::dvec3(0, 1, 0))
								 : sim.dropFromSlot(s, n, glm::dvec3(0, 1, 0));
		};
		grant(ItemId::Stone, 8);
		select(0);
		const glm::ivec3 target(p.x + 1, p.y + 1, p.z);
		CHECK(brk(target) == sim::BreakResult::WorldRejected, "parity: air break refused");
		CHECK(place(target) == sim::PlaceResult::Ok, "parity: place accepted");
		CHECK(place(target) == sim::PlaceResult::WorldRejected, "parity: re-place refused");
		CHECK(drop(0, 2) == ItemResult::Ok, "parity: drop accepted");
		CHECK(drop(0, 0) == ItemResult::Ok, "parity: zero drop no-op");
		for (int i = 0; i < 30; ++i)
			sim.advanceItems(1.0 / 60.0, false);
		sim.pumpWorld();
	};

	sim::GameSimulation legacy(&pool), explicitSim(&pool);
	script(legacy, false);
	script(explicitSim, true);

	CHECK(legacy.worldSnapshot().editRevision == explicitSim.worldSnapshot().editRevision,
		  "parity: identical committed edit revision");
	sim::InventorySnapshot invL, invE;
	legacy.inventorySnapshot(invL);
	explicitSim.inventorySnapshot(explicitSim.localPlayerId(), invE);
	CHECK(invL == invE, "parity: identical local inventory");
	CHECK(legacy.itemEntities().entities().size() == explicitSim.itemEntities().entities().size(),
		  "parity: identical item population size");
	CHECK(legacy.itemIdHighWater() == explicitSim.itemIdHighWater(), "parity: identical id high-water");
	legacy.shutdown();
	explicitSim.shutdown();
}

// ---------------------------------------------------------------------------
// 10. Actor-aware deferred break: one accepted logical break == one drop, the
//     effective state is AIR immediately, publication adds no second event.
// ---------------------------------------------------------------------------
void testActorDeferredEdit(ThreadPool &pool)
{
	CoutSilencer silence;
	sim::GameSimulation sim(&pool);
	const glm::ivec3 p = setupPlatform(sim, 1337);
	const sim::PlayerId a = sim.localPlayerId();
	const sim::PlayerId b = sim.createPlayer();
	sim.setPlayerFlight(a, true, glm::dvec3(8.0, 200.0, 8.0)); // far: no auto-pickup
	sim.setPlayerFlight(b, true, glm::dvec3(8.0, 200.0, 8.0));

	const glm::ivec3 cell(p.x, p.y + 1, p.z);
	sim.placeVoxel({cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f}, STONE);
	sim.pumpWorld();
	WorldChunk *chunk = sim.world()->getChunk({cell.x >> 4, 0, cell.z >> 4});
	CHECK(chunk != nullptr, "deferred fixture: target chunk resident");
	chunk->setInTransit(true);
	CHECK(sim.breakBlock(b, cell) == sim::BreakResult::Ok, "actor-aware deferred break accepted (B)");
	CHECK(sim.world()->hasPendingLogicalEdits(), "deletion deferred while in transit");
	{
		WorldCollisionView view(*sim.world());
		TextureType effective = STONE;
		view.sampleWithType(cell, &effective);
		CHECK(effective == AIR, "effective state is AIR immediately after acceptance");
	}
	CHECK(sim.breakBlock(a, cell) == sim::BreakResult::WorldRejected,
		  "a second actor cannot break the already-deleted cell");
	for (int i = 0; i < 60; ++i)
		sim.advanceItems(1.0 / 60.0, false);
	std::vector<sim::ItemEntitySnapshot> snaps;
	sim.itemSnapshots(snaps);
	CHECK(snaps.size() == 1, "one accepted break -> exactly one drop");
	chunk->setInTransit(false);
	sim.pumpWorld();
	CHECK(!sim.world()->hasPendingLogicalEdits(), "publication drained the queue");
	sim.itemSnapshots(snaps);
	CHECK(snaps.size() == 1, "delayed publication spawned no second drop");
	sim.shutdown();
}

// ---------------------------------------------------------------------------
// 11. Per-player id-exhaustion conservation: a refused drop/break consumes
//     nothing from ANY session; separate inventories stay independent.
// ---------------------------------------------------------------------------
void testIdExhaustionPerPlayer(ThreadPool &pool)
{
	CoutSilencer silence;
	TempRoot root("actor-id-limit");
	namespace fs = std::filesystem;
	fs::create_directories(root.path / "limit");
	worldsave::ItemsPersistState seedState;
	seedState.nextEntityId = items::kMaxNextEntityId; // fully exhausted
	CHECK(worldsave::writeFileAtomic(root.path / "limit" / "items.state",
									 worldsave::encodeItemsState(seedState)),
		  "pre-seed an exhausted allocator");

	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 42;
	cfg.worldName = "limit";
	cfg.savesRoot = root.path;
	const glm::dvec3 boot = sim.initialize(cfg, glm::dvec3(8.0, 100.0, 8.0));
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);
	CHECK(sim.itemEntities().idsExhausted(), "allocator restored exhausted");

	const sim::PlayerId a = sim.localPlayerId();
	const sim::PlayerId b = sim.createPlayer();
	CHECK(sim.grantItems(a, ItemId::Stone, 4).result == ItemResult::Ok, "A granted");
	CHECK(sim.grantItems(b, ItemId::Dirt, 6).result == ItemResult::Ok, "B granted");
	const sim::InventorySnapshot invA0 = inventoryOf(sim, a);
	const sim::InventorySnapshot invB0 = inventoryOf(sim, b);

	// Exhausted manual drops refuse for BOTH players without consuming.
	CHECK(sim.dropFromSlot(a, 0, 1, {0, 1, 0}) == ItemResult::IdExhausted, "A exhausted drop refused");
	CHECK(sim.dropFromSlot(b, 0, 1, {0, 1, 0}) == ItemResult::IdExhausted, "B exhausted drop refused");
	CHECK(inventoryOf(sim, a) == invA0, "A inventory untouched by refused drop");
	CHECK(inventoryOf(sim, b) == invB0, "B inventory untouched by refused drop");

	// Exhausted break refuses before the world edit (block stays).
	glm::ivec3 cell{};
	TextureType type = AIR;
	CHECK(findLandCell(*sim.world(), 8, 8, cell, type), "id-limit fixture cell");
	CHECK(sim.breakBlock(a, cell) == sim::BreakResult::IdExhausted, "A exhausted break refused");
	CHECK(sim.breakBlock(b, cell) == sim::BreakResult::IdExhausted, "B exhausted break refused");
	{
		WorldCollisionView view(*sim.world());
		TextureType still = AIR;
		view.sampleWithType(cell, &still);
		CHECK(still == type, "refused breaks left the block intact (conservation)");
	}
	CHECK(sim.itemEntities().size() == 0, "no items spawned at exhaustion");
	sim.shutdown();
}

// ---------------------------------------------------------------------------
// 12. Coordinate validation (issue #247 review P2): extreme integer cells and
//     non-finite float queries are rejected BEFORE any float->int conversion
//     (no UB), with existing result codes and no side effects.
// ---------------------------------------------------------------------------
void testCoordinateValidation(ThreadPool &pool)
{
	CoutSilencer silence;
	sim::GameSimulation sim(&pool);
	const glm::ivec3 p = setupPlatform(sim, 1337);
	const sim::PlayerId a = sim.localPlayerId();
	sim.setPlayerPosition(a, platformFeet(p, 0, 0));
	CHECK(sim.grantItems(a, ItemId::Stone, 10).result == ItemResult::Ok, "grant placeable items");
	sim.selectHotbarSlot(a, 0);
	const sim::InventorySnapshot inv0 = inventoryOf(sim, a);
	const sim::WorldSnapshot w0 = sim.worldSnapshot();
	const size_t items0 = sim.itemEntities().size();

	// Extreme integer cells (near the INT32 bounds): rejected with integer
	// comparisons before a float position / AABB is ever constructed.
	const glm::ivec3 extremeCells[] = {
		glm::ivec3(INT32_MAX, 64, 0), glm::ivec3(INT32_MIN, 64, 0),
		glm::ivec3(0, 64, INT32_MAX), glm::ivec3(0, 64, INT32_MIN),
		glm::ivec3(INT32_MAX, INT32_MAX, INT32_MAX), glm::ivec3(INT32_MIN, INT32_MIN, INT32_MIN),
		glm::ivec3(0, INT32_MAX, 0), glm::ivec3(0, INT32_MIN, 0),
	};
	for (const glm::ivec3 &c : extremeCells)
	{
		CHECK(sim.placeSelectedBlock(a, c) == sim::PlaceResult::WorldRejected,
			  "extreme cell place refused without float conversion");
		CHECK(sim.breakBlock(a, c) == sim::BreakResult::WorldRejected,
			  "extreme cell break refused without float conversion");
	}

	// World boundary coordinates: in-envelope edges are unloaded (defined
	// UnknownTerrain); out-of-envelope Y is WorldRejected. No UB either way.
	CHECK(sim.placeSelectedBlock(a, {SHRT_MAX - 1, 64, 0}) == sim::PlaceResult::UnknownTerrain,
		  "x upper boundary (in envelope) unloaded");
	CHECK(sim.breakBlock(a, {SHRT_MIN, 64, 0}) == sim::BreakResult::UnknownTerrain,
		  "x lower boundary (in envelope) unloaded");
	CHECK(sim.placeSelectedBlock(a, {p.x, WORLD_HEIGHT, p.z}) == sim::PlaceResult::WorldRejected,
		  "y at WORLD_HEIGHT refused");
	CHECK(sim.placeSelectedBlock(a, {p.x, -1, p.z}) == sim::PlaceResult::WorldRejected,
		  "negative y refused");
	CHECK(sim.breakBlock(a, {p.x, WORLD_HEIGHT, p.z}) == sim::BreakResult::WorldRejected,
		  "break above the world refused");
	CHECK(sim.breakBlock(a, {p.x, 0, p.z}) != sim::BreakResult::Ok,
		  "y=0 boundary break is a defined refusal (bedrock)");

	// Non-finite float placement queries: safe refusal for solid blocks.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	CHECK(!sim.canPlaceVoxel({nan, 64.f, 0.f}, STONE), "NaN x refused");
	CHECK(!sim.canPlaceVoxel({0.f, nan, 0.f}, STONE), "NaN y refused");
	CHECK(!sim.canPlaceVoxel({0.f, 64.f, nan}, STONE), "NaN z refused");
	CHECK(!sim.canPlaceVoxel({inf, 64.f, 0.f}, STONE), "+Inf x refused");
	CHECK(!sim.canPlaceVoxel({-inf, 64.f, 0.f}, STONE), "-Inf x refused");
	CHECK(!sim.canPlaceVoxel({0.f, inf, 0.f}, STONE), "+Inf y refused");
	CHECK(!sim.canPlaceVoxel({0.f, -inf, 0.f}, STONE), "-Inf y refused");
	CHECK(!sim.canPlaceVoxel({1e9f, 64.f, 0.f}, STONE), "out-of-envelope float refused");
	CHECK(!sim.canPlaceVoxel({0.f, -1.f, 0.f}, STONE), "negative y float refused");
	CHECK(!sim.canPlaceVoxel({0.f, static_cast<float>(WORLD_HEIGHT), 0.f}, STONE),
		  "y at WORLD_HEIGHT float refused");
	// Non-solid blocks short-circuit before any coordinate work (historical).
	CHECK(sim.canPlaceVoxel({nan, nan, nan}, WATER), "non-solid ignores coordinates");

	// Invalid actor combined with invalid coordinates: identity checked first.
	const sim::PlayerId bad{999};
	CHECK(sim.placeSelectedBlock(bad, {INT32_MAX, 64, 0}) == sim::PlaceResult::InvalidPlayer,
		  "invalid actor + extreme cell place");
	CHECK(sim.breakBlock(bad, {INT32_MIN, 64, 0}) == sim::BreakResult::InvalidPlayer,
		  "invalid actor + extreme cell break");
	CHECK(!sim.canPlaceVoxel(bad, {nan, 64.f, 0.f}, STONE), "invalid actor + NaN query");

	// NOTHING changed: no consumption, no world edit, no item spawn.
	CHECK(inventoryOf(sim, a) == inv0, "coordinate rejections consumed nothing");
	CHECK(sim.worldSnapshot().editRevision == w0.editRevision, "coordinate rejections never edited the world");
	CHECK(sim.itemEntities().size() == items0, "coordinate rejections spawned nothing");
	sim.shutdown();
}

// ---------------------------------------------------------------------------
// 13. Unpositioned sessions (issue #247 review P2): a created-but-not-placed
//     session (default-origin body) is excluded from shared pickup and
//     player-body placement checks until activated; activation persists across
//     world epochs; removal leaves no stale candidate; solo is unchanged.
// ---------------------------------------------------------------------------
void testUnpositionedSessions(ThreadPool &pool)
{
	CoutSilencer silence;
	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 1337;
	const glm::dvec3 boot = sim.initialize(cfg, glm::dvec3(8.0, 100.0, 8.0));
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);
	const sim::PlayerId local = sim.localPlayerId();
	CHECK(sim.playerPositioned(local), "the local anchor is positioned from construction (solo)");
	CHECK(sim.grantItems(local, ItemId::Stone, 10).result == ItemResult::Ok, "grant local items");
	sim.selectHotbarSlot(local, 0);

	// Create B WITHOUT positioning it: default body at the origin, inactive.
	const sim::PlayerId b = sim.createPlayer();
	CHECK(!sim.playerPositioned(b), "a new non-local session starts unpositioned");
	CHECK(sim.playerFeet(b) == glm::dvec3(0.0), "unpositioned B body sits at the origin");
	sim.setPlayerFlight(local, true, glm::dvec3(8.0, 200.0, 8.0)); // local far from origin

	// Build a breakable block in an air pocket AT the origin cell so the drop
	// lands within pickup radius of B's default body.
	const auto makeOriginDrop = [&]() {
		sim.deleteVoxel({0.5f, 0.5f, 0.5f}); // clear bedrock at (0,0,0)
		sim.placeVoxel({0.5f, 0.5f, 0.5f}, STONE);
		sim.pumpWorld();
		CHECK(sim.breakBlock(local, glm::ivec3(0, 0, 0)) == sim::BreakResult::Ok, "origin drop fixture break");
		for (int i = 0; i < 60; ++i) // settle + pickup delay elapses
			sim.advanceItems(1.0 / 60.0, false);
	};

	// Phase 1: inactive B must NOT vacuum-collect the origin drop.
	makeOriginDrop();
	std::vector<sim::ItemEntitySnapshot> snaps;
	sim.itemSnapshots(snaps);
	CHECK(snaps.size() == 1, "one origin drop before activation");
	CHECK(glm::distance(snaps[0].current, glm::dvec3(0.0)) <= sim.itemSettings().pickupRadius,
		  "fixture: the drop is within B's default pickup radius");
	CHECK(emptyInventory(inventoryOf(sim, b)), "unpositioned B collects nothing");

	// Phase 2: inactive B must NOT block placement at the origin cell.
	CHECK(sim.canPlaceVoxel({0.5f, 0.5f, 0.5f}, STONE), "inactive B does not block placement");
	CHECK(sim.placeSelectedBlock(local, glm::ivec3(0, 0, 0)) == sim::PlaceResult::Ok,
		  "placement into the origin cell succeeds despite inactive B");
	CHECK(inventoryOf(sim, local).slots[0] == S(ItemId::Stone, 9), "the place consumed 1 from local");

	// Phase 3: position B at the origin -> activated.
	sim.setPlayerPosition(b, glm::dvec3(0.5, 0.0, 0.5));
	CHECK(sim.playerPositioned(b), "setPlayerPosition activates the session");

	// Phase 4: active walking B now blocks placement into its body cell.
	sim.deleteVoxel({0.5f, 0.5f, 0.5f}); // clear the placed block
	sim.pumpWorld();
	CHECK(!sim.canPlaceVoxel({0.5f, 0.5f, 0.5f}, STONE), "active B blocks placement into its body");
	CHECK(sim.placeSelectedBlock(local, glm::ivec3(0, 0, 0)) == sim::PlaceResult::PlayerOverlap,
		  "placement into active B's body is refused");
	CHECK(inventoryOf(sim, local).slots[0] == S(ItemId::Stone, 9), "refused overlap consumed nothing");

	// Phase 5: active B picks the lingering origin drop up normally (the same
	// drop B ignored while unpositioned in Phase 1 — no second drop created).
	for (int i = 0; i < 30; ++i)
		sim.advanceItems(1.0 / 60.0, false);
	sim.itemSnapshots(snaps);
	CHECK(snaps.empty(), "activated B collects the origin drop");
	CHECK(inventoryOf(sim, b).slots[0] == S(ItemId::Cobblestone, 1), "pickup lands in B's inventory");

	// Phase 6: removing B leaves no stale pickup candidate.
	CHECK(sim.removePlayer(b), "B removed");
	CHECK(!sim.playerPositioned(b), "a removed session is not positioned");
	makeOriginDrop();
	sim.itemSnapshots(snaps);
	CHECK(snaps.size() == 1, "no stale candidate collects after removal (local is far)");

	// Phase 7: activation semantics persist across world epochs.
	const sim::PlayerId c = sim.createPlayer();
	CHECK(!sim.playerPositioned(c), "C starts unpositioned");
	sim.setPlayerPosition(c, glm::dvec3(8.5, 100.0, 8.5));
	CHECK(sim.playerPositioned(c), "C positioned");
	CHECK(sim.reload(44), "transient reload");
	CHECK(sim.playerPositioned(local), "local stays positioned across reload");
	CHECK(sim.playerPositioned(c), "a positioned session stays positioned across reload (body carries over)");
	const sim::PlayerId d = sim.createPlayer();
	CHECK(!sim.playerPositioned(d), "a fresh session after reload is unpositioned");
	sim::GameSimulation::Config cfg2;
	cfg2.seed = 45;
	sim.initialize(cfg2, glm::dvec3(8.0, 100.0, 8.0));
	CHECK(sim.playerPositioned(local) && sim.playerPositioned(c), "positioned survives initialize");
	CHECK(!sim.playerPositioned(d), "unpositioned survives initialize");
	sim.shutdown();
}

} // namespace

int main()
{
	std::printf("test_actor_interactions: CPU-only actor-aware gameplay (no GPU, no SDL, no camera)\n");
	ThreadPool pool(2);
	testBreakContention(pool);
	testPlaceContention(pool);
	testSharedPickupSystem();
	testSharedPickupFacade(pool);
	testInvalidActorRejections(pool);
	testOverlapProtection(pool);
	testUnknownTerrainAndBounds(pool);
	testActorTargeting(pool);
	testSoloParity(pool);
	testActorDeferredEdit(pool);
	testIdExhaustionPerPlayer(pool);
	testCoordinateValidation(pool);
	testUnpositionedSessions(pool);
	std::printf("%ld checks, %ld testmain::fails\n", testmain::checks, testmain::fails);
	if (testmain::fails == 0)
		std::printf("PASS\n");
	return testmain::exitCode();
}
