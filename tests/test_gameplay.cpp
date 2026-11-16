// test_gameplay — CPU-only gameplay acceptance (issues #225/#226/#227, Epic
// #223 Phases B/C/D). Links ONLY ft_vox_core: no SDL, no Vulkan device, no
// ImGui, no camera. Proves the issues' acceptance lists end-to-end:
//
// Phase B (#225):
//   * command result matrix through the facade (invalid slots, empty
//     stacks, full inventory, mismatches, partial moves/splits/grants);
//   * grantItems test/admin seam: partial semantics + EXPLICIT logging;
//   * grant -> move -> split -> swap sequences deterministic across runs
//     (two simulations step-identical, seeded replay reproduces the state);
//   * InventorySnapshot boundedness + in-place fill (no per-frame alloc);
//   * initialize/reload reset the inventory; a REFUSED reload keeps it;
//     reopening a save restores the player but NOT the inventory (#228);
//   * suspension never replays buffered commands (there is no buffer:
//     commands apply exactly once, at call time);
//   * selection duality: the gameplay hotbar selection and the creative
//     selectedBlock() seam never synchronize; player.state is untouched.
//
// Phase C (#226):
//   * break -> drop -> pickup -> inventory end-to-end; pickup delay honored;
//     partial pickup with a nearly-full inventory; full inventory leaves
//     entities untouched;
//   * breakBlock result matrix (bedrock, fluids, unloaded/unknown, AIR,
//     double-break, no-world); dropFromSlot matrix (bounds, clamping,
//     degenerate direction, deterministic launch);
//   * age despawn + bounded population cap with deterministic oldest-first
//     overflow (age ties break on the smallest id); ids monotonic, never
//     reused within a session, high-water exposed;
//   * two fixed-seed facade runs produce identical per-frame populations /
//     trajectories, event streams and inventory;
//   * 60 Hz rate independent of host dt (bit-identical), dropped-step cap,
//     delta spikes, suspension freeze without replay burst;
//   * id-sorted bounded snapshots + in-place fill; bounded spawn/remove
//     event stream with observable cap drops;
//   * initialize/reload reset the population + id allocator; refused reload
//     keeps them.
//
// Phase D (#227):
//   * raycast parity fixtures vs the verbatim historical Engine march:
//     hit / miss / adjacent-fallback / unknown-terrain / max-distance;
//   * placeSelectedBlock result matrix (EmptySlot, PlayerOverlap,
//     UnknownTerrain, WorldRejected, Ok; NotPlaceable is future vocabulary —
//     every v1 item is placeable) + consume-on-success-only + edit-result
//     exposure for the bridge publication path;
//   * the full walking loop: break -> drop -> pickup -> select -> place
//     consumes, with the world edit verified at every stage.
//
// Style mirrors test_game_simulation: a CHECK macro, plain main().

#include <Sim/GameSimulation.hpp>
#include <Core/ThreadPool.hpp>
#include <Items/ItemEntitySystem.hpp>
#include <Items/ItemRegistry.hpp>
#include <World/WorldCollisionView.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
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
using items::ItemId;
using items::ItemResult;
using items::ItemStack;

constexpr ItemStack S(ItemId id, uint16_t count)
{
	return {id, count};
}

constexpr ItemStack kEmpty{};

// --- Snapshot contract: bounded plain value, in-place fill -------------------
static_assert(std::is_trivially_copyable_v<sim::InventorySnapshot>,
			  "InventorySnapshot is a plain bounded value");
static_assert(sizeof(sim::InventorySnapshot) == items::kInventorySize * 4 + 4,
			  "InventorySnapshot is exactly 36 stacks + the selected slot");
static_assert(std::is_trivially_copyable_v<sim::ItemEntitySnapshot>,
			  "ItemEntitySnapshot is a plain bounded value");
static_assert(std::is_trivially_copyable_v<items::ItemEntityEvent>,
			  "ItemEntityEvent is a plain bounded value");
static_assert(std::is_trivially_copyable_v<items::ItemEntity>,
			  "ItemEntity is plain value state (no pointers)");
static_assert(std::is_same_v<sim::ItemEntitySnapshot, items::ItemEntitySnapshot>,
			  "sim::ItemEntitySnapshot re-exports the items system snapshot");

sim::InventorySnapshot snap(const sim::GameSimulation &sim)
{
	sim::InventorySnapshot out;
	sim.inventorySnapshot(out);
	return out;
}

bool sameSnapshot(const sim::InventorySnapshot &a, const sim::InventorySnapshot &b)
{
	return a == b; // defaulted value equality (the ItemStack pattern)
}

bool emptyInventory(const sim::GameSimulation &sim)
{
	const auto s = snap(sim);
	if (s.selectedSlot != 0)
		return false;
	for (const ItemStack &slot : s.slots)
		if (slot != kEmpty)
			return false;
	return true;
}

std::filesystem::path makeTempDir(const char *tag)
{
	const auto base = std::filesystem::temp_directory_path() /
					  ("ft_vox_gameplay_" + std::string(tag) + "_" +
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

// Silence the grant audit log inside bulk sequence tests (the logging itself
// is asserted explicitly in testGrantSeam). RAII: always restores the buffer.
struct CoutSilencer
{
	std::ostringstream sink;
	std::streambuf *old;
	CoutSilencer() : old(std::cout.rdbuf(sink.rdbuf())) {}
	~CoutSilencer() { std::cout.rdbuf(old); }
};

// ---------------------------------------------------------------------------
// 1. Command result matrix through the facade. Inventory commands are
//    world-independent: this whole section runs BEFORE any initialize().
// ---------------------------------------------------------------------------
void testCommandMatrix()
{
	sim::GameSimulation sim(nullptr); // no world: commands still apply
	CHECK(emptyInventory(sim), "fresh simulation owns an empty inventory, selection 0");

	// Selection bounds.
	CHECK(sim.selectHotbarSlot(-1) == ItemResult::InvalidSlot, "negative hotbar slot rejected");
	CHECK(sim.selectHotbarSlot(items::kHotbarSize) == ItemResult::InvalidSlot,
		  "hotbar slot == kHotbarSize rejected");
	CHECK(sim.selectHotbarSlot(items::kInventorySize) == ItemResult::InvalidSlot,
		  "main-inventory slot not selectable");
	CHECK(snap(sim).selectedSlot == 0, "rejected selections leave the selection unchanged");
	CHECK(sim.selectHotbarSlot(8) == ItemResult::Ok, "last hotbar slot selectable");
	CHECK(sim.selectHotbarSlot(0) == ItemResult::Ok, "first hotbar slot selectable");

	// Slot bounds on every mutation command.
	CHECK(sim.moveStack(-1, 0) == ItemResult::InvalidSlot, "move from negative slot rejected");
	CHECK(sim.moveStack(0, items::kInventorySize) == ItemResult::InvalidSlot, "move to out-of-range rejected");
	CHECK(sim.swapStacks(items::kInventorySize, 0) == ItemResult::InvalidSlot, "swap out-of-range rejected");
	CHECK(sim.splitStack(0, -1) == ItemResult::InvalidSlot, "split to negative slot rejected");

	// Empty sources / no-ops.
	CHECK(sim.moveStack(0, 1) == ItemResult::EmptyStack, "move from empty slot is EmptyStack");
	CHECK(sim.splitStack(0, 1) == ItemResult::EmptyStack, "split from empty slot is EmptyStack");
	CHECK(sim.swapStacks(0, 1) == ItemResult::Ok, "swap of two empty slots is an Ok no-op");
	CHECK(sim.moveStack(5, 5) == ItemResult::Ok, "move onto itself is an Ok no-op");
	CHECK(sim.splitStack(5, 5) == ItemResult::Ok, "split onto itself is an Ok no-op");

	// Grant -> move -> merge overflow -> Full / ItemMismatch / swap / split.
	CHECK(sim.grantItems(ItemId::Stone, 10).result == ItemResult::Ok, "grant 10 Stone is Ok");
	CHECK(snap(sim).slots[0] == S(ItemId::Stone, 10), "grant lands hotbar-first in slot 0");
	CHECK(sim.moveStack(0, 20) == ItemResult::Ok, "move into empty main slot is Ok");
	CHECK(snap(sim).slots[0] == kEmpty && snap(sim).slots[20] == S(ItemId::Stone, 10),
		  "move transfers the whole stack");
	CHECK(sim.moveStack(0, 20) == ItemResult::EmptyStack, "drained source answers EmptyStack");

	const items::AddResult overflow = sim.grantItems(ItemId::Stone, 64);
	CHECK(overflow.result == ItemResult::Ok && overflow.remainder == kEmpty,
		  "grant merges into the existing stack first, the overflow fills an empty slot");
	CHECK(snap(sim).slots[20] == S(ItemId::Stone, 64) && snap(sim).slots[0] == S(ItemId::Stone, 10),
		  "merge-first proven: the main stack absorbed 54, the remainder took slot 0");
	CHECK(sim.moveStack(0, 20) == ItemResult::Full, "move into a full stack is Full");
	CHECK(snap(sim).slots[0] == S(ItemId::Stone, 10), "Full moves nothing");

	CHECK(sim.grantItems(ItemId::Dirt, 5).result == ItemResult::Ok, "grant Dirt is Ok");
	CHECK(sim.moveStack(0, 1) == ItemResult::ItemMismatch, "move onto a different item is ItemMismatch");
	CHECK(sim.splitStack(0, 1) == ItemResult::ItemMismatch, "split onto a different item is ItemMismatch");
	CHECK(sim.swapStacks(0, 1) == ItemResult::Ok, "swap different items is Ok");
	CHECK(snap(sim).slots[0] == S(ItemId::Dirt, 5) && snap(sim).slots[1] == S(ItemId::Stone, 10),
		  "swap exchanges contents");

	CHECK(sim.splitStack(1, 2) == ItemResult::Ok, "even split is Ok");
	CHECK(snap(sim).slots[1] == S(ItemId::Stone, 5) && snap(sim).slots[2] == S(ItemId::Stone, 5),
		  "even split: 10 -> 5 + 5");
	CHECK(sim.splitStack(2, 3) == ItemResult::Ok, "odd split is Ok");
	CHECK(snap(sim).slots[2] == S(ItemId::Stone, 2) && snap(sim).slots[3] == S(ItemId::Stone, 3),
		  "odd split: 5 -> source keeps 2, destination takes the larger half 3");
	CHECK(sim.splitStack(1, 20) == ItemResult::Full, "split into a full stack is Full");

	// Partial split fixture: source {Stone,5}, destination {Stone,62} (the
	// split chain parks exact counts without a same-item merge interfering).
	sim::GameSimulation partial(nullptr);
	CHECK(partial.grantItems(ItemId::Stone, 64).result == ItemResult::Ok, "fixture: full stack");
	CHECK(partial.grantItems(ItemId::Stone, 62).result == ItemResult::Ok, "fixture: 62 stack");
	CHECK(partial.splitStack(0, 2) == ItemResult::Ok, "fixture split 64");
	CHECK(partial.splitStack(0, 3) == ItemResult::Ok, "fixture split 32");
	CHECK(partial.splitStack(0, 4) == ItemResult::Ok, "fixture split 16");
	CHECK(partial.splitStack(0, 5) == ItemResult::Ok, "fixture split 8");
	CHECK(partial.grantItems(ItemId::Stone, 1).result == ItemResult::Ok, "fixture: top up to 5");
	CHECK(snap(partial).slots[0] == S(ItemId::Stone, 5) && snap(partial).slots[1] == S(ItemId::Stone, 62),
		  "partial-split fixture parked");
	CHECK(partial.splitStack(0, 1) == ItemResult::Partial, "split with limited room is Partial");
	CHECK(snap(partial).slots[0] == S(ItemId::Stone, 3) && snap(partial).slots[1] == S(ItemId::Stone, 64),
		  "Partial split moves only what fits (half 3, room 2)");

	// Invalid / empty grants never store anything.
	const ItemStack invalid{static_cast<ItemId>(0xFFFF), 5};
	const items::AddResult bad = sim.grantItems(static_cast<ItemId>(0xFFFF), 5);
	CHECK(bad.result == ItemResult::InvalidItem && bad.remainder == invalid,
		  "unknown-item grant is InvalidItem (input echoed verbatim)");
	const items::AddResult zero = sim.grantItems(ItemId::Stone, 0);
	CHECK(zero.result == ItemResult::Ok && zero.remainder == kEmpty, "zero-count grant is an Ok no-op");
}

// ---------------------------------------------------------------------------
// 2. grantItems: bulk partial semantics + the EXPLICIT admin-seam log.
// ---------------------------------------------------------------------------
void testGrantSeam()
{
	{
		sim::GameSimulation sim(nullptr);
		CoutSilencer silence;
		const items::AddResult exact = sim.grantItems(ItemId::Stone, 36 * 64);
		CHECK(exact.result == ItemResult::Ok && exact.remainder == kEmpty,
			  "bulk grant distributes exactly 36 full stacks");
		bool allFull = true;
		for (const ItemStack &slot : snap(sim).slots)
			allFull = allFull && slot == S(ItemId::Stone, 64);
		CHECK(allFull, "every slot holds maxStack after the exact bulk grant");
		const items::AddResult full = sim.grantItems(ItemId::Stone, 1);
		CHECK(full.result == ItemResult::Full && full.remainder == S(ItemId::Stone, 1),
			  "full inventory rejects further grants");
	}
	{
		sim::GameSimulation sim(nullptr);
		CoutSilencer silence;
		const items::AddResult over = sim.grantItems(ItemId::Stone, 36 * 64 + 1);
		CHECK(over.result == ItemResult::Partial && over.remainder == S(ItemId::Stone, 1),
			  "bulk grant + 1 returns the unfit remainder");
		const items::AddResult full = sim.grantItems(ItemId::Dirt, 65535);
		CHECK(full.result == ItemResult::Full && full.remainder == S(ItemId::Dirt, 65535),
			  "uint16_t-max grant on a full inventory is Full without wrapping");
	}
	{
		sim::GameSimulation sim(nullptr);
		CoutSilencer silence;
		const items::AddResult max = sim.grantItems(ItemId::Dirt, 65535);
		CHECK(max.result == ItemResult::Partial && max.remainder == S(ItemId::Dirt, 65535 - 36 * 64),
			  "uint16_t-max grant distributes 2304 items without wrapping");
	}

	// The admin seam is EXPLICITLY logged (epic #223 §3): capture std::cout.
	{
		sim::GameSimulation sim(nullptr);
		std::ostringstream capture;
		std::streambuf *old = std::cout.rdbuf(capture.rdbuf());
		const items::AddResult ok = sim.grantItems(ItemId::Stone, 10);
		const items::AddResult partial = sim.grantItems(ItemId::Stone, 36 * 64);
		const items::AddResult bad = sim.grantItems(static_cast<ItemId>(0xFFFF), 5);
		const items::AddResult collapsed = sim.grantItems(ItemId::None, 5);
		std::cout.rdbuf(old);
		CHECK(ok.result == ItemResult::Ok, "logged grant applied");
		const std::string log = capture.str();
		CHECK(log.find("[items] grant 10x Stone (admin seam): Ok") != std::string::npos,
			  "grant log carries the tag, count, registry name, seam marker and result");
		CHECK(log.find("Partial, remainder") != std::string::npos,
			  "partial grant log carries the remainder");
		CHECK(log.find("item#65535") != std::string::npos && log.find("InvalidItem") != std::string::npos,
			  "unknown-item grant log carries the ordinal and rejection code");
		CHECK(log.find("grant 5x item#0 (admin seam): Ok, applied 0") != std::string::npos,
			  "None-sentinel grant log notes the collapse (applied 0, not the requested 5)");
		CHECK(log.find("applied 2294") != std::string::npos,
			  "partial grant log records the applied amount (2304 requested - 10 remainder)");
		CHECK(partial.result == ItemResult::Partial && bad.result == ItemResult::InvalidItem &&
				  collapsed.result == ItemResult::Ok,
			  "logged grants keep the partial/rejection/collapse semantics");
		bool allStoneFull = true;
		for (const ItemStack &slot : snap(sim).slots)
			allStoneFull = allStoneFull && slot == S(ItemId::Stone, 64);
		CHECK(allStoneFull, "the collapsed None grant stored nothing (inventory unchanged by it)");
	}
}

// ---------------------------------------------------------------------------
// 3. Deterministic grant -> move -> split -> swap sequences: two simulations
//    stay step-identical; a seeded replay reproduces the exact final state.
// ---------------------------------------------------------------------------
struct Lcg
{
	uint64_t state;
	uint32_t next()
	{
		state = state * 6364136223846793005ULL + 1442695040888963407ULL;
		return static_cast<uint32_t>(state >> 32);
	}
};

struct SequenceStep
{
	uint32_t op;
	int slotA, slotB;
	ItemId item;
	uint16_t count;
	int hotbar;
};

SequenceStep drawStep(Lcg &rng)
{
	SequenceStep s;
	s.op = rng.next() % 5;
	s.slotA = static_cast<int>(rng.next() % items::kInventorySize);
	s.slotB = static_cast<int>(rng.next() % items::kInventorySize);
	s.item = static_cast<ItemId>(1 + rng.next() % items::kItemCount);
	s.count = static_cast<uint16_t>(rng.next() % 200);
	s.hotbar = static_cast<int>(rng.next() % (items::kHotbarSize + 2));
	return s;
}

ItemResult applyStep(sim::GameSimulation &sim, const SequenceStep &s, items::AddResult *grantOut)
{
	switch (s.op)
	{
	case 0:
		*grantOut = sim.grantItems(s.item, s.count);
		return grantOut->result;
	case 1:
		return sim.moveStack(s.slotA, s.slotB);
	case 2:
		return sim.swapStacks(s.slotA, s.slotB);
	case 3:
		return sim.splitStack(s.slotA, s.slotB);
	default:
		return sim.selectHotbarSlot(s.hotbar);
	}
}

void testDeterministicSequences()
{
	CoutSilencer silence; // bulk grants: the log is asserted in testGrantSeam
	constexpr int kSteps = 2000;
	sim::GameSimulation a(nullptr);
	sim::GameSimulation b(nullptr);
	Lcg rng{0x225E223ULL};

	bool identical = true;
	for (int step = 0; step < kSteps; ++step)
	{
		const SequenceStep s = drawStep(rng);
		items::AddResult ga{}, gb{};
		const ItemResult ra = applyStep(a, s, &ga);
		const ItemResult rb = applyStep(b, s, &gb);
		identical = identical && ra == rb && ga.remainder == gb.remainder &&
					sameSnapshot(snap(a), snap(b));
	}
	CHECK(identical, "2000-step command sequence: two simulations stay step-identical (results + snapshots)");

	sim::GameSimulation c(nullptr);
	Lcg replay{0x225E223ULL};
	for (int step = 0; step < kSteps; ++step)
	{
		items::AddResult gc{};
		applyStep(c, drawStep(replay), &gc);
	}
	CHECK(sameSnapshot(snap(a), snap(c)), "seeded replay reproduces the exact final inventory state");
}

// ---------------------------------------------------------------------------
// 4. Snapshot boundedness + in-place fill + value semantics.
// ---------------------------------------------------------------------------
void testSnapshotContract()
{
	sim::GameSimulation sim(nullptr);
	CoutSilencer silence;
	sim.grantItems(ItemId::Stone, 10);
	sim.selectHotbarSlot(3);

	sim::InventorySnapshot out;
	sim.inventorySnapshot(out); // fill the SAME object repeatedly (no alloc)
	CHECK(out.slots[0] == S(ItemId::Stone, 10) && out.selectedSlot == 3,
		  "in-place fill mirrors the authoritative inventory");
	bool mirrorsSeam = true;
	for (int i = 0; i < items::kInventorySize; ++i)
		mirrorsSeam = mirrorsSeam && out.slots[static_cast<std::size_t>(i)] == sim.inventory().stack(i);
	CHECK(mirrorsSeam && out.selectedSlot == sim.inventory().selectedSlot(),
		  "snapshot matches the read-only inventory() seam slot by slot");

	sim.grantItems(ItemId::Dirt, 7);
	sim.inventorySnapshot(out); // refill in place
	CHECK(out.slots[1] == S(ItemId::Dirt, 7), "refill reflects later commands");

	out.slots[0] = S(ItemId::GoldOre, 64); // the snapshot is a VALUE copy
	out.selectedSlot = 8;
	CHECK(sim.inventory().stack(0) == S(ItemId::Stone, 10) && sim.inventory().selectedSlot() == 3,
		  "mutating the snapshot never touches the simulation");

	// Defaulted value equality (future digest/replication comparisons, epic §6).
	sim::InventorySnapshot x, y;
	sim.inventorySnapshot(x);
	sim.inventorySnapshot(y);
	CHECK(x == y, "identical in-place fills compare equal");
	y.selectedSlot = 0;
	CHECK(!(x == y), "equality detects a selection difference");
	y = x;
	y.slots[5] = S(ItemId::Glass, 1);
	CHECK(!(x == y), "equality detects a slot difference");
}

// ---------------------------------------------------------------------------
// 5. Lifecycle: initialize/reload reset the inventory; a REFUSED reload keeps
//    it; reopening a save restores the player but NOT the inventory (#228).
// ---------------------------------------------------------------------------
void testLifecycleResets(ThreadPool &pool)
{
	const glm::dvec3 center(8.0, 100.0, 8.0);

	// initialize resets (transient session, no terrain needed).
	{
		sim::GameSimulation sim(&pool);
		sim::GameSimulation::Config cfg;
		cfg.seed = 42;
		sim.initialize(cfg, center);
		CoutSilencer silence;
		sim.grantItems(ItemId::Stone, 10);
		sim.selectHotbarSlot(4);
		CHECK(!emptyInventory(sim), "inventory populated before re-initialize");
		sim.initialize(cfg, center);
		CHECK(emptyInventory(sim), "initialize resets the inventory to empty + selection 0");
		sim.shutdown();
	}

	// Successful reload resets.
	{
		sim::GameSimulation sim(&pool);
		sim::GameSimulation::Config cfg;
		cfg.seed = 42;
		sim.initialize(cfg, center);
		CoutSilencer silence;
		sim.grantItems(ItemId::Dirt, 30);
		sim.selectHotbarSlot(7);
		CHECK(sim.reload(43), "transient reload succeeds");
		CHECK(emptyInventory(sim), "successful reload resets the inventory");
		sim.shutdown();
	}

	// Refused reload (stranded accepted edits): the OLD inventory stays.
	{
		TempRoot root("stranded");
		sim::GameSimulation sim(&pool);
		sim::GameSimulation::Config cfg;
		cfg.seed = 42;
		cfg.worldName = "stranded-inv";
		cfg.savesRoot = root.path;
		const glm::dvec3 boot = sim.initialize(cfg, center);
		sim.bootstrapTerrain(boot);
		WorldChunk *c = sim.world()->getChunk({0, 0, 0});
		CHECK(c != nullptr, "bootstrap record present");
		c->reset(c->getPosition(), WorldChunk::ResetMode::ForGeneration);
		CHECK(sim.placeVoxel({8.5f, 200.5f, 8.5f}, STONE).accepted, "stranded edit accepted");
		CHECK(sim.world()->hasPendingLogicalEdits(), "edit stranded outside authoritative state");
		{
			CoutSilencer silence;
			sim.grantItems(ItemId::Stone, 10);
		}
		sim.selectHotbarSlot(4);
		const auto before = snap(sim);
		CHECK(!sim.reload(44), "reload refused while edits are stranded");
		CHECK(sameSnapshot(snap(sim), before), "refused reload keeps the old inventory authoritative");
		// Resolve the strand: the reload now succeeds AND resets.
		CHECK(c->prepareVoxelStorageForGeneration(), "stranded prepare");
		c->generateTerrain(*sim.terrainGenerator());
		sim.pumpWorld();
		CHECK(sim.reload(44), "reload succeeds after resolution");
		CHECK(emptyInventory(sim), "resolved reload resets the inventory");
		sim.shutdown();
	}

	// Save/reopen: player.state restores, the inventory stays EMPTY until the
	// #228 restore hook lands (old saves load unchanged, epic #223 §4).
	{
		TempRoot root("reopen");
		{
			sim::GameSimulation sim(&pool);
			sim::GameSimulation::Config cfg;
			cfg.seed = 42;
			cfg.worldName = "inv-reopen";
			cfg.savesRoot = root.path;
			const glm::dvec3 boot = sim.initialize(cfg, center);
			sim.bootstrapTerrain(boot);
			sim.finalizePlayerSpawn(boot);
			CoutSilencer silence;
			sim.grantItems(ItemId::Stone, 64);
			sim.selectHotbarSlot(2);
			sim.selectedBlock() = DIRT;
			CHECK(sim.savePlayerState(15.f, -5.f), "player state saved with items present");
			sim.shutdown();
		}
		{
			sim::GameSimulation sim(&pool);
			sim::GameSimulation::Config cfg;
			cfg.seed = 0;
			cfg.worldName = "inv-reopen";
			cfg.savesRoot = root.path;
			const glm::dvec3 boot = sim.initialize(cfg, center);
			sim.bootstrapTerrain(boot);
			const auto outcome = sim.finalizePlayerSpawn(boot);
			CHECK(outcome.restored, "saved player restored on reopen");
			CHECK(sim.selectedBlock() == DIRT, "creative selection persistence untouched");
			CHECK(emptyInventory(sim), "reopen starts with an EMPTY inventory (restore hook is #228)");
			sim.shutdown();
		}
	}
}

// ---------------------------------------------------------------------------
// 6. Suspension: commands apply exactly once at call time; no buffered
//    replay across suspend/resume transitions.
// ---------------------------------------------------------------------------
void testSuspensionNoReplay(ThreadPool &pool)
{
	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 42;
	const glm::dvec3 boot = sim.initialize(cfg, glm::dvec3(8.0, 100.0, 8.0));
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);
	sim.setPlayerFlight(true, sim.playerFeet()); // host-driven body: stable fixture

	{
		CoutSilencer silence;
		sim.grantItems(ItemId::Stone, 10);
	}
	sim.selectHotbarSlot(2);
	const auto before = snap(sim);

	const glm::dvec3 observer = sim.playerFeet();
	sim.advancePlayer(1.0 / 60.0, sim::PlayerCommand{}, true);
	sim.advanceMobs(1.0 / 60.0, observer, 64.0, true);
	CHECK(sameSnapshot(snap(sim), before), "suspended advances never touch the inventory");

	// A command issued WHILE suspended applies immediately, exactly once.
	sim.suspendPlayer();
	{
		CoutSilencer silence;
		CHECK(sim.grantItems(ItemId::Dirt, 5).result == ItemResult::Ok,
			  "command applies at call time while suspended");
	}
	const auto during = snap(sim);
	CHECK(during.slots[1] == S(ItemId::Dirt, 5), "suspended-time command applied exactly once");
	sim.advancePlayer(1.0 / 60.0, sim::PlayerCommand{}, true);
	sim.advanceMobs(1.0 / 60.0, observer, 64.0, true);
	CHECK(sameSnapshot(snap(sim), during), "no replay while suspended");

	// Resume: no buffered command burst (there is no buffer).
	sim.advancePlayer(1.0 / 60.0, sim::PlayerCommand{}, false);
	sim.advanceMobs(1.0 / 60.0, observer, 64.0, false);
	CHECK(sameSnapshot(snap(sim), during), "resume never replays commands");
	sim.shutdown();
}

// ---------------------------------------------------------------------------
// 7. Selection duality: gameplay hotbar vs creative/editor selectedBlock().
// ---------------------------------------------------------------------------
void testSelectionDuality()
{
	sim::GameSimulation sim(nullptr);
	CHECK(sim.selectedBlock() == STONE, "default creative selection unchanged");
	CHECK(sim.selectHotbarSlot(5) == ItemResult::Ok, "gameplay hotbar selection set");
	CHECK(sim.selectedBlock() == STONE, "hotbar selection never touches the creative seam");
	CHECK(sim.playerSnapshot().selectedBlock == STONE,
		  "player snapshot still carries the creative selection");

	sim.selectedBlock() = DIRT;
	CHECK(snap(sim).selectedSlot == 5, "creative selection never touches the hotbar");
	const auto state = sim.capturePlayerState(10.f, -5.f);
	CHECK(state.selectedBlock == static_cast<int32_t>(DIRT),
		  "player.state capture unchanged (inventory persistence is #228)");
	CHECK(sim.playerSnapshot().selectedBlock == DIRT, "creative seam stays authoritative for the host");
}

// ===========================================================================
// Phase C (issue #226): dropped item entities + break/drop/pickup loop
// ===========================================================================

// Flat synthetic world for system-level fixtures (the test_core_contracts
// pattern): solid below y = 64, air above. No terrain inference, no GPU.
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

// Far outside any pickup radius: system fixtures never auto-pickup.
constexpr glm::dvec3 kFarAway{1e9, 1e9, 1e9};

// First breakable SOLID cell of a column, scanning from the sky down (skips
// fluids/bedrock). Deterministic per seed; the view is scoped so its shared
// lock never overlaps a later edit lock.
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

// A breakable surface cell with four AIR cells of headroom (skips fluid
// columns and overhangs): the placement fixtures need guaranteed-AIR targets.
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

// ---------------------------------------------------------------------------
// 8. breakBlock result matrix: bedrock, fluids, unloaded/unknown, AIR,
//    double-break, no-world; Ok spawns the registry drop.
// ---------------------------------------------------------------------------
void testBreakResultMatrix(ThreadPool &pool)
{
	{
		sim::GameSimulation none(nullptr);
		CHECK(none.breakBlock({0, 64, 0}) == sim::BreakResult::UnknownTerrain,
			  "break without a world answers UnknownTerrain");
	}

	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 42;
	const glm::dvec3 center(8.0, 100.0, 8.0);
	const glm::dvec3 boot = sim.initialize(cfg, center);
	sim.bootstrapTerrain(boot);

	CHECK(sim.breakBlock({1000, 100, 1000}) == sim::BreakResult::UnknownTerrain,
		  "unloaded cell answers UnknownTerrain (never inferred)");

	const int bx = static_cast<int>(std::floor(boot.x));
	const int bz = static_cast<int>(std::floor(boot.z));
	glm::ivec3 cell{};
	TextureType type = AIR;
	CHECK(findLandCell(*sim.world(), bx, bz, cell, type), "land cell with AIR headroom found");

	CHECK(sim.breakBlock({cell.x, cell.y + 1, cell.z}) == sim::BreakResult::WorldRejected,
		  "AIR cell answers WorldRejected (nothing to break)");
	CHECK(sim.breakBlock({cell.x, WORLD_HEIGHT + 5, cell.z}) == sim::BreakResult::WorldRejected,
		  "out-of-bounds cell answers WorldRejected");

	// Bedrock / fluids via the creative seam (deterministic regardless of
	// terrain): placed in AIR cells ABOVE the column's top solid, then
	// refused by hand.
	const int py = cell.y + 2;
	CHECK(sim.placeVoxel({cell.x + 0.5f, py + 0.5f, cell.z + 0.5f}, BEDROCK).accepted, "bedrock placed");
	CHECK(sim.breakBlock({cell.x, py, cell.z}) == sim::BreakResult::Unbreakable, "BEDROCK refused by hand");
	CHECK(sim.placeVoxel({cell.x + 0.5f, py + 1.5f, cell.z + 0.5f}, WATER).accepted, "water placed");
	CHECK(sim.breakBlock({cell.x, py + 1, cell.z}) == sim::BreakResult::Unbreakable, "WATER refused by hand");
	CHECK(sim.placeVoxel({cell.x + 0.5f, py + 2.5f, cell.z + 0.5f}, LAVA).accepted, "lava placed");
	CHECK(sim.breakBlock({cell.x, py + 2, cell.z}) == sim::BreakResult::Unbreakable, "LAVA refused by hand");

	// Ok break: world edit + registry drop at the cell center.
	CHECK(sim.breakBlock(cell) == sim::BreakResult::Ok, "breakable surface block breaks");
	std::vector<sim::ItemEntitySnapshot> snaps;
	sim.itemSnapshots(snaps);
	const items::ItemStack expectedDrop = items::dropForBlock(type);
	CHECK(snaps.size() == 1 && snaps[0].stack == expectedDrop,
		  "Ok break spawns exactly the registry drop");
	CHECK(sim.breakBlock(cell) == sim::BreakResult::WorldRejected,
		  "double break answers WorldRejected (already AIR)");
	sim.shutdown();
}

// ---------------------------------------------------------------------------
// 9. End-to-end: break -> drop -> pickup delay -> hotbar-first inventory.
// ---------------------------------------------------------------------------
void testBreakDropPickupEndToEnd(ThreadPool &pool)
{
	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 42;
	const glm::dvec3 center(8.0, 100.0, 8.0);
	const glm::dvec3 boot = sim.initialize(cfg, center);
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);

	const int bx = static_cast<int>(std::floor(sim.playerFeet().x)) + 2;
	const int bz = static_cast<int>(std::floor(sim.playerFeet().z)) + 2;
	glm::ivec3 cell{};
	TextureType type = AIR;
	CHECK(findBreakableCell(*sim.world(), bx, bz, cell, type), "target cell found");
	const items::ItemStack expectedDrop = items::dropForBlock(type);

	// Hover the authoritative body right above the target (flight is
	// host-driven: a stable pickup fixture).
	sim.setPlayerFlight(true, {cell.x + 0.5, cell.y + 1.0, cell.z + 0.5});
	CHECK(sim.breakBlock(cell) == sim::BreakResult::Ok, "break accepted");

	std::vector<sim::ItemEntitySnapshot> snaps;
	sim.itemSnapshots(snaps);
	CHECK(snaps.size() == 1 && snaps[0].id == 1 && snaps[0].stack == expectedDrop,
		  "drop entity spawned with id 1 and the registry drop");
	std::vector<items::ItemEntityEvent> events;
	sim.takeItemEvents(events);
	CHECK(events.size() == 1 && events[0].kind == items::ItemEventKind::Spawned &&
			  events[0].id == 1 && events[0].stack == expectedDrop,
		  "Spawned event carries id + stack");

	// Pickup delay honored: 5 ticks < the 10-tick default delay.
	for (int i = 0; i < 5; ++i)
		sim.advanceItems(1.0 / 60.0, false);
	sim.itemSnapshots(snaps);
	CHECK(snaps.size() == 1 && sim.inventory().totalCount(expectedDrop.item) == 0,
		  "pickup delay honored: no pickup before the delay elapses");

	// 5 more ticks: delay elapsed, proximity satisfied -> hotbar-first merge.
	for (int i = 0; i < 5; ++i)
		sim.advanceItems(1.0 / 60.0, false);
	sim.itemSnapshots(snaps);
	CHECK(snaps.empty(), "fully picked-up entity removed");
	CHECK(sim.inventory().totalCount(expectedDrop.item) == expectedDrop.count,
		  "picked-up items land in the inventory");
	CHECK(sim.inventory().stack(0) == expectedDrop, "pickup is hotbar-first (slot 0)");
	events.clear(); // takeItemEvents APPENDS (buffer-reuse drain)
	sim.takeItemEvents(events);
	CHECK(events.size() == 1 && events[0].kind == items::ItemEventKind::Removed &&
			  events[0].reason == items::ItemRemoveReason::PickedUp && events[0].stack.isEmpty(),
		  "Removed{PickedUp} event with empty stack (items moved to the inventory)");
	CHECK(sim.itemIdHighWater() == 1, "id high-water exposed for #228");
	sim.shutdown();
}

// ---------------------------------------------------------------------------
// 10. Partial pickup (nearly-full inventory) + full inventory leaves entities.
// ---------------------------------------------------------------------------
void testPartialAndFullPickup(ThreadPool &pool)
{
	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 42;
	const glm::dvec3 center(8.0, 100.0, 8.0);
	const glm::dvec3 boot = sim.initialize(cfg, center);
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);

	const int bx = static_cast<int>(std::floor(sim.playerFeet().x)) + 2;
	const int bz = static_cast<int>(std::floor(sim.playerFeet().z)) + 2;
	glm::ivec3 cell{};
	TextureType type = AIR;
	CHECK(findBreakableCell(*sim.world(), bx, bz, cell, type), "surface cell found");
	sim.setPlayerFlight(true, {cell.x + 0.5, cell.y + 1.0, cell.z + 0.5});

	CoutSilencer silence;
	// 35 full Stone stacks + slot 35 {Dirt,64}; drop 60 Dirt (slot keeps 4),
	// then the admin seam refills slot 35 to 60: exactly 4 room, no empty slot.
	CHECK(sim.grantItems(ItemId::Stone, 35 * 64).result == ItemResult::Ok, "fill 35 slots");
	CHECK(sim.grantItems(ItemId::Dirt, 64).result == ItemResult::Ok, "fill slot 35");
	CHECK(sim.dropFromSlot(35, 60, {0.0, -1.0, 0.0}) == ItemResult::Ok, "drop 60 Dirt");
	CHECK(sim.inventory().stack(35) == S(ItemId::Dirt, 4), "drop removed 60 from slot 35");
	CHECK(sim.grantItems(ItemId::Dirt, 56).result == ItemResult::Ok, "refill slot 35 to 60 (admin seam)");

	std::vector<sim::ItemEntitySnapshot> snaps;
	sim.itemSnapshots(snaps);
	CHECK(snaps.size() == 1 && snaps[0].stack == S(ItemId::Dirt, 60), "drop entity holds 60 Dirt");

	// Manual drops carry the longer 40-tick delay.
	for (int i = 0; i < 39; ++i)
		sim.advanceItems(1.0 / 60.0, false);
	sim.itemSnapshots(snaps);
	CHECK(snaps.size() == 1 && snaps[0].stack == S(ItemId::Dirt, 60),
		  "player-drop delay honored (39 < 40 ticks)");

	sim.advanceItems(1.0 / 60.0, false); // tick 40: pickup with room for 4
	sim.itemSnapshots(snaps);
	CHECK(snaps.size() == 1 && snaps[0].stack == S(ItemId::Dirt, 56),
		  "partial pickup leaves the remainder in the world");
	CHECK(sim.inventory().stack(35) == S(ItemId::Dirt, 64), "the 4 fitting items merged hotbar-first");

	// No room left: further ticks never touch the entity.
	for (int i = 0; i < 60; ++i)
		sim.advanceItems(1.0 / 60.0, false);
	sim.itemSnapshots(snaps);
	CHECK(snaps.size() == 1 && snaps[0].stack == S(ItemId::Dirt, 56),
		  "full inventory leaves the entity untouched");
	std::vector<items::ItemEntityEvent> events;
	sim.takeItemEvents(events);
	bool noRemoval = true;
	for (const auto &e : events)
		noRemoval = noRemoval && e.kind != items::ItemEventKind::Removed;
	CHECK(noRemoval, "no removal events while the inventory is full");

	// A fresh break drop also stays in the world against the full inventory.
	CHECK(sim.breakBlock(cell) == sim::BreakResult::Ok, "break with full inventory still drops");
	for (int i = 0; i < 120; ++i)
		sim.advanceItems(1.0 / 60.0, false);
	sim.itemSnapshots(snaps);
	CHECK(snaps.size() == 2, "both entities survive against the full inventory");
	sim.shutdown();
}

// ---------------------------------------------------------------------------
// 11. Age despawn + population cap overflow (system-level, bounded fixtures).
// ---------------------------------------------------------------------------
void testDespawnAndCap()
{
	FlatWorld world;
	items::Inventory inv;

	// Default pins (epic #223 §2).
	const items::ItemEntitySettings defaults;
	CHECK(defaults.fixedStep == 1.0 / sim::kItemTickHz, "item default step is the 60 Hz contract");
	CHECK(defaults.despawnAgeTicks == 18000, "default despawn age is 5 min of sim time");
	CHECK(defaults.pickupDelayTicks == 10 && defaults.playerDropDelayTicks == 40,
		  "default pickup delays pinned");
	CHECK(items::ItemEntitySettings::capacity == 1024, "population cap pinned at 1024");

	// Age despawn (tuned for a bounded fixture).
	{
		items::ItemEntitySystem sys;
		sys.settings.despawnAgeTicks = 120;
		sys.spawn(S(ItemId::Stone, 1), {0.5, 100.0, 0.5}, {}, 100000);
		for (int i = 0; i < 119; ++i)
			sys.update(1.0 / 60.0, world, kFarAway, inv, false);
		CHECK(sys.size() == 1, "entity alive before the despawn age");
		sys.update(1.0 / 60.0, world, kFarAway, inv, false);
		CHECK(sys.size() == 0, "age despawn at exactly despawnAgeTicks");
		const auto events = sys.takeEvents();
		bool aged = false;
		for (const auto &e : events)
			aged = aged || (e.kind == items::ItemEventKind::Removed &&
							e.reason == items::ItemRemoveReason::Aged && e.stack == S(ItemId::Stone, 1));
		CHECK(aged, "Removed{Aged} event carries the lost stack");
	}

	// Cap overflow: oldest-first, deterministic; ids never reused.
	{
		items::ItemEntitySystem sys;
		sys.spawn(S(ItemId::Stone, 1), {0.5, 100.0, 0.5}, {}, 100000); // id 1
		sys.update(1.0 / 60.0, world, kFarAway, inv, false);           // id 1 age 1
		for (int i = 0; i < 1023; ++i)
			sys.spawn(S(ItemId::Stone, 1), {i + 0.5, 100.0, 0.5}, {}, 100000);
		CHECK(sys.size() == 1024 && sys.idHighWater() == 1024, "population capped at 1024");
		sys.spawn(S(ItemId::Stone, 1), {0.5, 100.0, 0.5}, {}, 100000); // id 1025
		CHECK(sys.size() == 1024 && sys.idHighWater() == 1025, "overflow keeps the cap, ids monotonic");
		CHECK(sys.entities().front().id == 2, "oldest entity (id 1, age 1) despawned first");
		bool overflow = false;
		for (const auto &e : sys.takeEvents())
			overflow = overflow || (e.kind == items::ItemEventKind::Removed &&
									e.reason == items::ItemRemoveReason::Overflow && e.id == 1);
		CHECK(overflow, "Removed{Overflow} event for the despawned oldest");
		const auto next = sys.spawn(S(ItemId::Stone, 1), {0.5, 100.0, 0.5}, {}, 100000);
		CHECK(next == 1026, "ids never reused after despawn");
	}

	// Age ties break on the smallest id (all spawned in the same tick).
	{
		items::ItemEntitySystem sys;
		for (int i = 0; i < 1024; ++i)
			sys.spawn(S(ItemId::Stone, 1), {i + 0.5, 100.0, 0.5}, {}, 100000);
		sys.spawn(S(ItemId::Stone, 1), {0.5, 100.0, 0.5}, {}, 100000);
		CHECK(sys.entities().front().id == 2, "age tie despawns the smallest id");
	}
}

// ---------------------------------------------------------------------------
// 12. 60 Hz rate independence, dropped-step cap, delta spikes, suspension.
// ---------------------------------------------------------------------------
void testItemFixedRateAndSuspension()
{
	FlatWorld world;
	items::Inventory inv;

	// Same sim second under different host dt patterns -> bit-identical state.
	auto runOneSecond = [&](double dt, int frames) {
		items::ItemEntitySystem sys;
		sys.spawn(S(ItemId::Stone, 1), {0.5, 100.5, 0.5}, {1.0, 0.0, 0.0}, 100000);
		for (int i = 0; i < frames; ++i)
			sys.update(dt, world, kFarAway, inv, false);
		return sys.entities().front().body.position;
	};
	const glm::dvec3 a = runOneSecond(1.0 / 60.0, 60);
	const glm::dvec3 b = runOneSecond(1.0 / 30.0, 30);
	const glm::dvec3 c = runOneSecond(1.0 / 120.0, 120);
	CHECK(a == b && b == c, "60 Hz stepping independent of host dt (bit-identical trajectories)");

	// Delta spike: capped catch-up + dropped-step counter, no burst afterwards.
	// The exact dropped count sits on an fmod boundary (the MobSystem
	// pattern), so pin the contract — at most maxSteps, ~all excess counted,
	// at most the residual + one new tick on the next frame — not FP digits.
	{
		items::ItemEntitySystem sys;
		sys.spawn(S(ItemId::Stone, 1), {0.5, 100.5, 0.5}, {}, 100000);
		sys.update(10.0, world, kFarAway, inv, false); // 600 ticks owed
		CHECK(sys.entities().front().ageTicks == sys.settings.maxSteps,
			  "delta spike steps at most maxSteps");
		CHECK(sys.queryStats().cells > 0, "collision query diagnostics exposed (MobSystem pattern parity)");
		CHECK(sys.droppedSteps() >= 591 && sys.droppedSteps() <= 592,
			  "excess ticks dropped and counted (600 owed - 8 stepped)");
		sys.update(1.0 / 60.0, world, kFarAway, inv, false);
		const uint32_t age = sys.entities().front().ageTicks;
		CHECK(age == sys.settings.maxSteps + 1 || age == sys.settings.maxSteps + 2,
			  "no catch-up burst after the spike (residual + one new tick at most)");
	}

	// Suspension: freeze + previous/current sync, accumulator cleared.
	{
		items::ItemEntitySystem sys;
		sys.spawn(S(ItemId::Stone, 1), {0.5, 100.5, 0.5}, {1.0, 0.0, 0.0}, 100000);
		sys.update(1.0 / 60.0, world, kFarAway, inv, false);
		const uint32_t ageBefore = sys.entities().front().ageTicks;
		sys.update(1.0, world, kFarAway, inv, true);
		const auto &e = sys.entities().front();
		CHECK(e.ageTicks == ageBefore, "suspension freezes the population");
		CHECK(e.previous == e.body.position, "suspension syncs previous/current");
		CHECK(sys.interpolationAlpha() == 0.0, "suspension clears the accumulator");
		sys.update(10.0, world, kFarAway, inv, false);
		CHECK(sys.entities().front().ageTicks == ageBefore + sys.settings.maxSteps,
			  "resume after suspension never replays buffered time");
	}
}

// ---------------------------------------------------------------------------
// 13. Fixed-seed determinism: two identical facade runs produce identical
//     entity populations/trajectories, events and inventory.
// ---------------------------------------------------------------------------
struct ItemRunResult
{
	std::vector<std::vector<sim::ItemEntitySnapshot>> frames;
	std::vector<items::ItemEntityEvent> events;
	sim::InventorySnapshot finalInv{};
	uint64_t highWater{0};
};

ItemRunResult itemRun(ThreadPool &pool)
{
	ItemRunResult result;
	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 42;
	const glm::dvec3 center(8.0, 100.0, 8.0);
	const glm::dvec3 boot = sim.initialize(cfg, center);
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);
	CoutSilencer silence;
	sim.grantItems(ItemId::Stone, 128);

	const int bx = static_cast<int>(std::floor(sim.playerFeet().x));
	const int bz = static_cast<int>(std::floor(sim.playerFeet().z));
	glm::ivec3 c1{}, c2{};
	TextureType t1 = AIR, t2 = AIR;
	CHECK(findBreakableCell(*sim.world(), bx + 2, bz + 2, c1, t1), "determinism run: first break cell found");
	CHECK(findBreakableCell(*sim.world(), bx + 3, bz + 2, c2, t2), "determinism run: second break cell found");
	sim.setPlayerFlight(true, {c1.x + 0.5, c1.y + 1.0, c1.z + 0.5});
	sim.breakBlock(c1);
	sim.breakBlock(c2);
	sim.dropFromSlot(0, 10, {1.0, 0.0, 0.0});

	std::vector<sim::ItemEntitySnapshot> snaps;
	for (int f = 0; f < 180; ++f)
	{
		const double dt = (f % 7 == 0) ? 1.0 / 30.0 : ((f % 13 == 0) ? 0.2 : 1.0 / 60.0);
		sim.advanceItems(dt, false);
		sim.itemSnapshots(snaps);
		result.frames.push_back(snaps);
	}
	sim.takeItemEvents(result.events);
	sim.inventorySnapshot(result.finalInv);
	result.highWater = sim.itemIdHighWater();
	sim.shutdown();
	return result;
}

void testItemDeterminism(ThreadPool &pool)
{
	const ItemRunResult a = itemRun(pool);
	const ItemRunResult b = itemRun(pool);
	CHECK(a.frames == b.frames, "two fixed-seed runs: identical per-frame entity populations/trajectories");
	CHECK(a.events == b.events, "two fixed-seed runs: identical spawn/remove event streams");
	CHECK(a.finalInv == b.finalInv, "two fixed-seed runs: identical final inventory");
	CHECK(a.highWater == b.highWater && a.highWater >= 3, "two fixed-seed runs: identical id high-water");
}

// ---------------------------------------------------------------------------
// 14. dropFromSlot matrix + snapshot/event stream contracts.
// ---------------------------------------------------------------------------
void testDropMatrixAndStreams()
{
	{
		sim::GameSimulation sim(nullptr);
		CoutSilencer silence;
		CHECK(sim.dropFromSlot(-1, 1, {0, 1, 0}) == ItemResult::InvalidSlot, "drop from negative slot rejected");
		CHECK(sim.dropFromSlot(items::kInventorySize, 1, {0, 1, 0}) == ItemResult::InvalidSlot,
			  "drop from out-of-range slot rejected");
		CHECK(sim.dropFromSlot(0, 1, {0, 1, 0}) == ItemResult::EmptyStack, "drop from empty slot is EmptyStack");
		sim.grantItems(ItemId::Stone, 10);
		CHECK(sim.dropFromSlot(0, 0, {0, 1, 0}) == ItemResult::Ok, "count 0 drop is an Ok no-op");
		CHECK(sim.itemEntities().size() == 0, "count 0 spawns nothing");
		CHECK(sim.dropFromSlot(0, 99, {0, 1, 0}) == ItemResult::Ok, "drop clamps to the available count");
		CHECK(sim.inventory().stack(0) == kEmpty, "clamped drop drained the slot");
		std::vector<sim::ItemEntitySnapshot> snaps;
		sim.itemSnapshots(snaps);
		CHECK(snaps.size() == 1 && snaps[0].stack == S(ItemId::Stone, 10), "drop entity holds the removed stack");
		const auto &e0 = sim.itemEntities().entities()[0];
		CHECK(e0.pickupDelay == sim.itemSettings().playerDropDelayTicks, "manual drop carries the longer delay");
		CHECK(e0.body.velocity.y > 0.0, "manual drop launch carries the +Y bias");
		// Deterministic launch: an identical drop repeats the exact velocity.
		const glm::dvec3 v0 = e0.body.velocity;
		sim.grantItems(ItemId::Stone, 10);
		sim.dropFromSlot(0, 10, {0, 1, 0});
		CHECK(sim.itemEntities().entities()[1].body.velocity == v0,
			  "identical drops launch with identical deterministic velocity");

		// Degenerate direction: the EXACT +Y fallback (velocity AND origin) —
		// a {0,1,0} input cannot detect a missing guard, {0,0,0} can.
		const glm::dvec3 fallbackVel(0.0, sim.itemSettings().dropSpeed + 1.0, 0.0);
		const glm::dvec3 fallbackOrigin = sim.playerEye() + glm::dvec3(0.0, 0.3, 0.0);
		sim.grantItems(ItemId::Stone, 3);
		CHECK(sim.dropFromSlot(0, 3, {0.0, 0.0, 0.0}) == ItemResult::Ok, "zero-direction drop is Ok");
		CHECK(sim.itemEntities().entities().back().body.velocity == fallbackVel,
			  "zero direction falls back to the exact +Y launch velocity");
		CHECK(sim.itemEntities().entities().back().body.position == fallbackOrigin,
			  "zero direction spawns at eye + 0.3 along +Y");
		// Non-finite presentation input (inf / NaN) takes the same guarded
		// fallback instead of propagating NaN into origin/velocity.
		const double inf = std::numeric_limits<double>::infinity();
		const double nan = std::numeric_limits<double>::quiet_NaN();
		sim.grantItems(ItemId::Stone, 3);
		CHECK(sim.dropFromSlot(0, 3, {inf, 0.0, 0.0}) == ItemResult::Ok, "infinite-direction drop is Ok");
		CHECK(sim.itemEntities().entities().back().body.velocity == fallbackVel,
			  "infinite direction falls back to +Y (no NaN propagation)");
		sim.grantItems(ItemId::Stone, 3);
		CHECK(sim.dropFromSlot(0, 3, {nan, 0.0, 0.0}) == ItemResult::Ok, "NaN-direction drop is Ok");
		CHECK(sim.itemEntities().entities().back().body.velocity == fallbackVel,
			  "NaN direction falls back to +Y");
	}

	// Snapshots: id-sorted, bounded, in-place fill; events: drain order + cap.
	{
		items::ItemEntitySystem sys;
		sys.spawn(S(ItemId::Stone, 1), {0.5, 100.0, 0.5}, {}, 100000);
		sys.spawn(S(ItemId::Dirt, 2), {1.5, 100.0, 0.5}, {}, 100000);
		sys.spawn(S(ItemId::Glass, 3), {2.5, 100.0, 0.5}, {}, 100000);
		std::vector<sim::ItemEntitySnapshot> snaps;
		sys.snapshots(snaps);
		CHECK(snaps.size() == 3 && snaps[0].id == 1 && snaps[1].id == 2 && snaps[2].id == 3,
			  "snapshots are id-sorted");
		const auto capacity = snaps.capacity();
		sys.snapshots(snaps); // in-place refill keeps the buffer
		CHECK(snaps.capacity() >= capacity && snaps.size() == 3, "in-place refill reuses the buffer");
		auto events = sys.takeEvents();
		CHECK(events.size() == 3 && events[0].id == 1 && events[2].id == 3,
			  "events drain in spawn order");
		CHECK(sys.takeEvents().empty(), "drain empties the stream");

		// Bounded stream: a consumer that never drains drops the OLDEST,
		// counted (the WorldEvent contract).
		items::ItemEntitySystem flood;
		for (int i = 0; i < 5000; ++i)
			flood.spawn(S(ItemId::Stone, 1), {i + 0.5, 100.0, 0.5}, {}, 100000);
		CHECK(flood.droppedEventCount() > 0, "event stream cap drops are observable");
		std::vector<items::ItemEntityEvent> buffered;
		flood.takeEvents(buffered);
		CHECK(buffered.size() <= 4096, "event stream stays bounded");
	}
}

// ---------------------------------------------------------------------------
// 15. Lifecycle: initialize/reload reset the population + id allocator; a
//     refused reload keeps them; suspension at the facade level.
// ---------------------------------------------------------------------------
void testItemLifecycleResets(ThreadPool &pool)
{
	const glm::dvec3 center(8.0, 100.0, 8.0);

	{
		sim::GameSimulation sim(&pool);
		sim::GameSimulation::Config cfg;
		cfg.seed = 42;
		sim.initialize(cfg, center);
		CoutSilencer silence;
		sim.grantItems(ItemId::Stone, 20);
		sim.dropFromSlot(0, 5, {0, 1, 0});
		CHECK(sim.itemEntities().size() == 1 && sim.itemIdHighWater() == 1, "population before re-initialize");
		sim.initialize(cfg, center);
		CHECK(sim.itemEntities().size() == 0 && sim.itemIdHighWater() == 0,
			  "initialize resets the population and id allocator");
		std::vector<items::ItemEntityEvent> cleared;
		sim.takeItemEvents(cleared);
		CHECK(cleared.empty(), "initialize clears the event stream");
		sim.shutdown();
	}

	{
		sim::GameSimulation sim(&pool);
		sim::GameSimulation::Config cfg;
		cfg.seed = 42;
		sim.initialize(cfg, center);
		CoutSilencer silence;
		sim.grantItems(ItemId::Stone, 20);
		sim.dropFromSlot(0, 5, {0, 1, 0});
		CHECK(sim.reload(43), "reload succeeds");
		CHECK(sim.itemEntities().size() == 0 && sim.itemIdHighWater() == 0,
			  "successful reload resets the dropped-item session");
		sim.shutdown();
	}

	// Refused reload (stranded edits): population + ids stay authoritative.
	{
		TempRoot root("stranded-items");
		sim::GameSimulation sim(&pool);
		sim::GameSimulation::Config cfg;
		cfg.seed = 42;
		cfg.worldName = "stranded-items";
		cfg.savesRoot = root.path;
		const glm::dvec3 boot = sim.initialize(cfg, center);
		sim.bootstrapTerrain(boot);
		WorldChunk *c = sim.world()->getChunk({0, 0, 0});
		CHECK(c != nullptr, "bootstrap record present");
		c->reset(c->getPosition(), WorldChunk::ResetMode::ForGeneration);
		CHECK(sim.placeVoxel({8.5f, 200.5f, 8.5f}, STONE).accepted, "stranded edit accepted");
		{
			CoutSilencer silence;
			sim.grantItems(ItemId::Stone, 20);
		}
		sim.dropFromSlot(0, 5, {0, 1, 0});
		std::vector<sim::ItemEntitySnapshot> before;
		sim.itemSnapshots(before);
		CHECK(!sim.reload(44), "reload refused while edits are stranded");
		std::vector<sim::ItemEntitySnapshot> after;
		sim.itemSnapshots(after);
		CHECK(before == after && sim.itemIdHighWater() == 1,
			  "refused reload keeps the dropped-item population + ids");
		CHECK(c->prepareVoxelStorageForGeneration(), "stranded prepare");
		c->generateTerrain(*sim.terrainGenerator());
		sim.pumpWorld();
		CHECK(sim.reload(44), "reload succeeds after resolution");
		CHECK(sim.itemEntities().size() == 0 && sim.itemIdHighWater() == 0,
			  "resolved reload resets the dropped-item session");
		sim.shutdown();
	}

	// Facade suspension: advanceItems(dt, true) freezes + syncs, no replay.
	{
		sim::GameSimulation sim(&pool);
		sim::GameSimulation::Config cfg;
		cfg.seed = 42;
		const glm::dvec3 boot = sim.initialize(cfg, center);
		sim.bootstrapTerrain(boot);
		sim.finalizePlayerSpawn(boot);
		sim.setPlayerFlight(true, sim.playerFeet());
		{
			CoutSilencer silence;
			sim.grantItems(ItemId::Stone, 20);
		}
		sim.dropFromSlot(0, 5, {0, 1, 0});
		sim.advanceItems(1.0 / 60.0, false);
		std::vector<sim::ItemEntitySnapshot> before;
		sim.itemSnapshots(before);
		sim.advanceItems(1.0, true);
		std::vector<sim::ItemEntitySnapshot> frozen;
		sim.itemSnapshots(frozen);
		CHECK(frozen.size() == before.size(), "suspension keeps the population");
		bool synced = true;
		for (const auto &s : frozen)
			synced = synced && s.previous == s.current;
		CHECK(synced && sim.itemInterpolationAlpha() == 0.0,
			  "suspension syncs previous/current and clears the accumulator");
		sim.shutdown();
	}
}

// ===========================================================================
// Phase D (issue #227): targeting + place-from-hotbar + the walking loop
// ===========================================================================

static_assert(std::is_trivially_copyable_v<sim::RaycastHit>,
			  "RaycastHit is a plain value");

// The HISTORICAL Engine::raycastVoxel march (pre-#227), verbatim except for
// the active-cell seam: the render bridge forwarded to
// VoxelWorld::isVoxelActive, so querying the world directly answers
// identically by construction. The parity fixtures compare the sim query
// against this reference bit for bit.
bool historicalRaycast(const VoxelWorld &world, glm::vec3 origin, glm::vec3 front, float maxDist,
					   glm::vec3 &outBlock, glm::vec3 &outPrevious)
{
	const glm::vec3 dir = glm::normalize(front);
	const float step = 0.05f;
	glm::vec3 prev = origin;
	for (float t = 0.f; t <= maxDist; t += step)
	{
		const glm::vec3 p = origin + dir * t;
		const glm::vec3 block(std::floor(p.x), std::floor(p.y), std::floor(p.z));
		if (world.isVoxelActive(block))
		{
			outBlock = block;
			outPrevious = glm::vec3(std::floor(prev.x), std::floor(prev.y), std::floor(prev.z));
			if (outPrevious == outBlock)
				outPrevious = block - glm::vec3(dir.x > 0 ? 1.f : (dir.x < 0 ? -1.f : 0.f),
												dir.y > 0 ? 1.f : (dir.y < 0 ? -1.f : 0.f),
												dir.z > 0 ? 1.f : (dir.z < 0 ? -1.f : 0.f));
			return true;
		}
		prev = p;
	}
	return false;
}

void checkRaycastParity(const sim::GameSimulation &sim, const VoxelWorld &world, glm::vec3 origin,
						glm::vec3 front, float maxDist, const char *msg)
{
	glm::vec3 refBlock{}, refPrev{};
	const bool refHit = historicalRaycast(world, origin, front, maxDist, refBlock, refPrev);
	const sim::RaycastHit hit = sim.raycastVoxel(origin, front, maxDist);
	CHECK(hit.hit == refHit, msg);
	if (refHit)
	{
		CHECK(hit.block == refBlock, "raycast parity: hit block identical");
		CHECK(hit.adjacent == refPrev, "raycast parity: adjacent cell identical");
	}
}

// ---------------------------------------------------------------------------
// 16. Raycast parity fixtures: hit / miss / adjacent-fallback / unknown /
//     max-distance, identical to the historical Engine behavior.
// ---------------------------------------------------------------------------
void testRaycastParity(ThreadPool &pool)
{
	{
		sim::GameSimulation none(nullptr);
		const sim::RaycastHit miss = none.raycastVoxel({0, 100, 0}, {0, -1, 0}, 10.f);
		CHECK(!miss.hit, "raycast without a world answers miss");
	}

	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 42;
	const glm::dvec3 center(8.0, 100.0, 8.0);
	const glm::dvec3 boot = sim.initialize(cfg, center);
	sim.bootstrapTerrain(boot);
	const VoxelWorld &world = *sim.world();

	const int bx = static_cast<int>(std::floor(boot.x));
	const int bz = static_cast<int>(std::floor(boot.z));
	glm::ivec3 cell{};
	TextureType type = AIR;
	CHECK(findLandCell(world, bx, bz, cell, type), "parity fixture: land cell found");
	const glm::vec3 above(cell.x + 0.5f, cell.y + 3.5f, cell.z + 0.5f);

	// Hit straight down; adjacent is the cell above the hit.
	checkRaycastParity(sim, world, above, {0, -1, 0}, 10.f, "parity: downward hit");
	const sim::RaycastHit down = sim.raycastVoxel(above, {0, -1, 0}, 10.f);
	CHECK(down.hit && down.block == glm::vec3(cell) &&
			  down.adjacent == glm::vec3(cell.x, cell.y + 1, cell.z),
		  "downward ray hits the surface with the cell above as adjacent");

	// Miss into the sky.
	checkRaycastParity(sim, world, above, {0, 1, 0}, 50.f, "parity: sky miss");

	// Adjacent fallback: the ray STARTS inside the hit block (prev == hit).
	const glm::vec3 embedded(cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f);
	checkRaycastParity(sim, world, embedded, {0, -1, 0}, 10.f, "parity: embedded-origin fallback");
	const sim::RaycastHit emb = sim.raycastVoxel(embedded, {0, -1, 0}, 10.f);
	CHECK(emb.hit && emb.block == glm::vec3(cell) &&
			  emb.adjacent == glm::vec3(cell.x, cell.y + 1, cell.z),
		  "embedded origin falls back to hit minus direction signs");

	// Diagonal (unnormalized front: both sides normalize identically).
	checkRaycastParity(sim, world, {cell.x + 0.5f, cell.y + 2.5f, cell.z + 0.5f},
					   {0.3f, -1.f, 0.2f}, 10.f, "parity: diagonal ray");
	// Horizontal toward neighboring terrain.
	checkRaycastParity(sim, world, {cell.x + 3.5f, cell.y + 0.5f, cell.z + 0.5f},
					   {-1, 0, 0}, 10.f, "parity: horizontal ray");

	// Max-distance boundary: the surface is first sampled at t = 0.1.
	const glm::vec3 nearAbove(cell.x + 0.5f, cell.y + 1.05f, cell.z + 0.5f);
	checkRaycastParity(sim, world, nearAbove, {0, -1, 0}, 0.04f, "parity: just short of maxDist");
	checkRaycastParity(sim, world, nearAbove, {0, -1, 0}, 0.1f, "parity: exactly at maxDist");
	CHECK(!sim.raycastVoxel(nearAbove, {0, -1, 0}, 0.04f).hit, "maxDist excludes the hit");
	CHECK(sim.raycastVoxel(nearAbove, {0, -1, 0}, 0.1f).hit, "maxDist includes the hit");

	// Unknown terrain: start FAR outside the bootstrapped region so every
	// marched cell is unloaded — the march passes through, never hits.
	checkRaycastParity(sim, world, {bx + 200.5f, 100.f, bz + 0.5f}, {1, 0, 0}, 100.f,
					   "parity: ray through unloaded region misses");
	CHECK(!sim.raycastVoxel({bx + 200.5f, 100.f, bz + 0.5f}, {1, 0, 0}, 100.f).hit,
		  "unknown cells are never hit");
	sim.shutdown();
}

// ---------------------------------------------------------------------------
// 17. placeSelectedBlock result matrix + consume-on-success-only.
// ---------------------------------------------------------------------------
void testPlaceSelectedBlockMatrix(ThreadPool &pool)
{
	{
		sim::GameSimulation none(nullptr);
		CHECK(none.placeSelectedBlock({0, 64, 0}) == sim::PlaceResult::EmptySlot,
			  "empty hotbar answers EmptySlot");
	}

	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 42;
	const glm::dvec3 center(8.0, 100.0, 8.0);
	const glm::dvec3 boot = sim.initialize(cfg, center);
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot); // WALKING mode: the player-aware rule applies
	CHECK(!sim.playerFlight(), "fixture player is walking");

	glm::ivec3 cell{};
	TextureType type = AIR;
	CHECK(findLandCell(*sim.world(), static_cast<int>(std::floor(boot.x)),
					   static_cast<int>(std::floor(boot.z)), cell, type),
		  "place fixture: land cell found");

	CoutSilencer silence;
	CHECK(sim.grantItems(ItemId::Stone, 10).result == ItemResult::Ok, "grant placeable items");

	// UnknownTerrain: unloaded target (items present, slot selected).
	CHECK(sim.placeSelectedBlock({1000, 100, 1000}) == sim::PlaceResult::UnknownTerrain,
		  "unloaded target answers UnknownTerrain");
	CHECK(sim.inventory().stack(0).count == 10, "UnknownTerrain consumes nothing");

	// PlayerOverlap: a solid placement overlapping the walking body.
	const glm::dvec3 feet = sim.playerFeet();
	const glm::ivec3 bodyCell(static_cast<int>(std::floor(feet.x)),
							  static_cast<int>(std::floor(feet.y)),
							  static_cast<int>(std::floor(feet.z)));
	CHECK(sim.placeSelectedBlock(bodyCell) == sim::PlaceResult::PlayerOverlap,
		  "walking body overlap answers PlayerOverlap");
	CHECK(sim.inventory().stack(0).count == 10, "PlayerOverlap consumes nothing");

	// WorldRejected: occupied target (the surface cell itself).
	CHECK(sim.placeSelectedBlock(cell) == sim::PlaceResult::WorldRejected,
		  "occupied target answers WorldRejected");
	CHECK(sim.inventory().stack(0).count == 10, "WorldRejected consumes nothing");
	CHECK(sim.placeSelectedBlock({cell.x, WORLD_HEIGHT + 2, cell.z}) == sim::PlaceResult::WorldRejected,
		  "out-of-bounds target answers WorldRejected");

	// Ok: the AIR cell above the surface; the edit result moves out for the
	// bridge publication path; exactly 1 item consumed.
	const glm::ivec3 target(cell.x, cell.y + 1, cell.z);
	WorldEditResult edit;
	CHECK(sim.placeSelectedBlock(target, &edit) == sim::PlaceResult::Ok, "air target places Ok");
	CHECK(edit.accepted && edit.editId > 0 && !edit.geometricMembers.empty(),
		  "accepted edit result exposed for bridge publication");
	CHECK(sim.inventory().stack(0) == S(ItemId::Stone, 9), "consume exactly 1 on success");
	{
		WorldCollisionView view(*sim.world());
		TextureType placed = AIR;
		view.sampleWithType(target, &placed);
		CHECK(placed == STONE, "world cell holds the placed block");
	}
	CHECK(sim.placeSelectedBlock(target) == sim::PlaceResult::WorldRejected,
		  "re-placing the occupied cell is refused");
	CHECK(sim.inventory().stack(0).count == 9, "refused placement consumes nothing");

	// EmptySlot after draining the stack.
	CHECK(sim.dropFromSlot(0, 9, {0, 1, 0}) == ItemResult::Ok, "drain the stack");
	CHECK(sim.placeSelectedBlock({cell.x, cell.y + 2, cell.z}) == sim::PlaceResult::EmptySlot,
		  "drained hotbar answers EmptySlot");
	// NotPlaceable is future vocabulary: every v1 catalog item is placeable,
	// so no public API path can store a non-placeable item (pinned by the
	// registry static_asserts in ItemRegistry.cpp).

	// Flight: the creative overlap rule allows placement inside the body.
	// Hover over the fixture land cell (findLandCell guarantees four AIR
	// cells of headroom): the body at feet level +1 overlaps cells +1/+2;
	// +2 is still AIR (the earlier Ok placement took +1).
	CHECK(sim.grantItems(ItemId::Stone, 1).result == ItemResult::Ok, "grant one more");
	sim.setPlayerFlight(true, {cell.x + 0.5, cell.y + 1.0, cell.z + 0.5});
	const glm::ivec3 flightBodyCell(cell.x, cell.y + 2, cell.z);
	CHECK(sim.placeSelectedBlock(flightBodyCell) == sim::PlaceResult::Ok,
		  "flight placement ignores body overlap (the #209 creative rule)");
	CHECK(sim.inventory().stack(0).isEmpty(), "flight placement still consumes the item");
	sim.shutdown();
}

// ---------------------------------------------------------------------------
// 18. The full walking loop: break -> drop -> pickup -> select -> place
//     consumes, with the world edit verified at every stage.
// ---------------------------------------------------------------------------
void testWalkingLoopEndToEnd(ThreadPool &pool)
{
	sim::GameSimulation sim(&pool);
	sim::GameSimulation::Config cfg;
	cfg.seed = 42;
	const glm::dvec3 center(8.0, 100.0, 8.0);
	const glm::dvec3 boot = sim.initialize(cfg, center);
	sim.bootstrapTerrain(boot);
	sim.finalizePlayerSpawn(boot);
	CHECK(!sim.playerFlight(), "loop fixture player is walking");

	const int bx = static_cast<int>(std::floor(sim.playerFeet().x)) + 1;
	const int bz = static_cast<int>(std::floor(sim.playerFeet().z));
	glm::ivec3 cell{};
	TextureType type = AIR;
	CHECK(findBreakableCell(*sim.world(), bx, bz, cell, type), "loop: breakable cell found");
	const items::ItemStack drop = items::dropForBlock(type);

	// Break: the registry drop spawns as an entity.
	CHECK(sim.breakBlock(cell) == sim::BreakResult::Ok, "loop: break accepted");
	std::vector<sim::ItemEntitySnapshot> snaps;
	sim.itemSnapshots(snaps);
	CHECK(snaps.size() == 1 && snaps[0].stack == drop, "loop: drop entity spawned");

	// Stand over the hole so the drop settles within pickup radius; the
	// walking controller is reset at the safe feet (host reposition seam).
	sim.setPlayerPosition({cell.x + 0.5, cell.y + 1.0, cell.z + 0.5});
	for (int i = 0; i < 300 && sim.inventory().totalCount(drop.item) == 0; ++i)
		sim.advanceItems(1.0 / 60.0, false);
	CHECK(sim.inventory().totalCount(drop.item) == drop.count, "loop: drop auto-picked-up");
	sim.itemSnapshots(snaps);
	CHECK(snaps.empty(), "loop: picked-up entity removed");

	// Select the hotbar slot holding the drop and place it back into the
	// broken cell (below the feet: no body overlap).
	CHECK(sim.selectHotbarSlot(0) == ItemResult::Ok, "loop: select slot 0");
	CHECK(sim.inventory().selectedStack().item == drop.item, "loop: drop is hotbar-first");
	const uint16_t before = sim.inventory().selectedStack().count;
	CHECK(sim.placeSelectedBlock(cell) == sim::PlaceResult::Ok, "loop: place accepted");
	CHECK(sim.inventory().selectedStack().count == before - 1, "loop: place consumes exactly 1");
	{
		WorldCollisionView view(*sim.world());
		TextureType placed = AIR;
		view.sampleWithType(cell, &placed);
		CHECK(placed == *items::placeableBlockOf(drop.item), "loop: world cell holds the placed block");
	}
	sim.shutdown();
}
} // namespace

int main()
{
	std::printf("test_gameplay: CPU-only gameplay commands (no GPU, no SDL, no camera)\n");
	ThreadPool pool(2);
	testCommandMatrix();
	testGrantSeam();
	testDeterministicSequences();
	testSnapshotContract();
	testLifecycleResets(pool);
	testSuspensionNoReplay(pool);
	testSelectionDuality();
	testBreakResultMatrix(pool);
	testBreakDropPickupEndToEnd(pool);
	testPartialAndFullPickup(pool);
	testDespawnAndCap();
	testItemFixedRateAndSuspension();
	testItemDeterminism(pool);
	testDropMatrixAndStreams();
	testItemLifecycleResets(pool);
	testRaycastParity(pool);
	testPlaceSelectedBlockMatrix(pool);
	testWalkingLoopEndToEnd(pool);
	std::printf("%d checks, %d failures\n", g_checks, g_fails);
	if (g_fails == 0)
		std::printf("PASS\n");
	return g_fails == 0 ? 0 : 1;
}
