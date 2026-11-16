// test_gameplay — CPU-only gameplay-command acceptance (issue #225, Epic
// #223 Phase B). Links ONLY ft_vox_core: no SDL, no Vulkan device, no ImGui,
// no camera. Proves the issue's acceptance list end-to-end:
//
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
// Style mirrors test_game_simulation: a CHECK macro, plain main().

#include <Sim/GameSimulation.hpp>
#include <Core/ThreadPool.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>

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
	std::printf("%d checks, %d failures\n", g_checks, g_fails);
	if (g_fails == 0)
		std::printf("PASS\n");
	return g_fails == 0 ? 0 : 1;
}
