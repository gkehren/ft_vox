// HeadlessRunner — graphical-free host of sim::GameSimulation (issue #210,
// Epic #204 Phase F). See the header for the frame orchestration and the
// determinism contract. No SDL / Vulkan / ImGui / Camera / renderer type is
// reachable from this translation unit (guarded by tests/check_headless_deps.py).
#include "Headless/HeadlessRunner.hpp"

#include <Headless/SimScript.hpp>

#include <Core/ThreadPool.hpp>
#include <Items/ItemRegistry.hpp>
#include <Sim/GameSimulation.hpp>
#include <World/WorldCollisionView.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

namespace headless
{
namespace
{

/// The historical fallback bootstrap center (the graphical host passes its
/// initial camera position; headless fixtures use the same plain value as
/// test_game_simulation).
const glm::dvec3 kFallbackCenter{8.0, 100.0, 8.0};

std::string cellToString(const glm::ivec3 &cell)
{
	return "(" + std::to_string(cell.x) + ", " + std::to_string(cell.y) + ", " + std::to_string(cell.z) + ")";
}

/// AIR lives AFTER COUNT in the enum (it is not a texture), so the display
/// table must never be indexed with it.
std::string blockName(TextureType type)
{
	if (type == AIR)
		return "Air";
	const int ordinal = static_cast<int>(type);
	if (ordinal < 0 || ordinal >= static_cast<int>(COUNT))
		return "Block(" + std::to_string(ordinal) + ")";
	return std::string(textureTypeString[ordinal]);
}

/// Gameplay result vocabularies as diagnostic strings (issue #230).
const char *breakResultName(sim::BreakResult result)
{
	switch (result)
	{
	case sim::BreakResult::Ok: return "Ok";
	case sim::BreakResult::Unbreakable: return "Unbreakable";
	case sim::BreakResult::UnknownTerrain: return "UnknownTerrain";
	case sim::BreakResult::WorldRejected: return "WorldRejected";
	case sim::BreakResult::IdExhausted: return "IdExhausted";
	case sim::BreakResult::InvalidPlayer: return "InvalidPlayer";
	}
	return "?";
}

const char *placeResultName(sim::PlaceResult result)
{
	switch (result)
	{
	case sim::PlaceResult::Ok: return "Ok";
	case sim::PlaceResult::EmptySlot: return "EmptySlot";
	case sim::PlaceResult::NotPlaceable: return "NotPlaceable";
	case sim::PlaceResult::PlayerOverlap: return "PlayerOverlap";
	case sim::PlaceResult::UnknownTerrain: return "UnknownTerrain";
	case sim::PlaceResult::WorldRejected: return "WorldRejected";
	case sim::PlaceResult::InvalidPlayer: return "InvalidPlayer";
	}
	return "?";
}

std::string stackToString(const items::ItemStack &stack)
{
	if (stack.isEmpty())
		return "empty";
	return std::string(items::itemName(stack.item)) + " x" + std::to_string(stack.count);
}

} // namespace

RunOutcome runHeadless(const RunnerOptions &options)
{
	RunOutcome out;
	out.workers = options.workers;
	out.interestRadius = options.interestRadius;

	const auto startTime = std::chrono::steady_clock::now();
	const auto elapsedSec = [&]() {
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
	};

	// --- Script (pure parse; fail before any world state exists) ------------
	SimScript script;
	if (!options.scriptPath.empty())
	{
		std::string error;
		if (!parseSimScript(options.scriptPath, script, error))
		{
			out.code = ExitCode::Script;
			out.message = error;
			return out;
		}
		if (static_cast<uint64_t>(script.totalWaitFrames) > options.ticks)
		{
			out.code = ExitCode::Script;
			out.message = "script waits " + std::to_string(script.totalWaitFrames) +
						  " frames but --ticks is " + std::to_string(options.ticks);
			return out;
		}
	}

	// --- Simulation host ownership: pool (optional) outlives the facade -----
	std::unique_ptr<ThreadPool> pool;
	if (options.workers > 0)
		pool = std::make_unique<ThreadPool>(static_cast<size_t>(options.workers));

	sim::GameSimulation sim(pool.get());
	sim::GameSimulation::Config config;
	config.seed = options.seed;
	config.worldName = options.worldName;
	config.savesRoot = options.savesRoot;

	const glm::dvec3 bootCenter = sim.initialize(config, kFallbackCenter);
	out.seed = sim.seed();
	out.generatorVersion = sim.worldSnapshot().generatorVersion;

	// A requested persistent world MUST open: unlike the graphical host (which
	// continues transient), the runner reports the failure as an error exit.
	if (!options.worldName.empty() && !sim.isWorldOpen())
	{
		out.code = ExitCode::WorldIo;
		out.message = "failed to open world '" + options.worldName + "': " +
					  (sim.openWorldError().empty() ? std::string("unknown error") : sim.openWorldError());
		return out;
	}

	sim.bootstrapTerrain(bootCenter);
	sim.finalizePlayerSpawn(bootCenter);

	VoxelWorld *world = sim.world();
	out.area = options.hasDigestArea ? options.digestArea : defaultDigestArea(bootCenter);

	// Explicit headless interest source: plain point + radius, never a Camera.
	bool interestPinned = options.fixedInterest;
	glm::dvec3 interest = options.fixedInterest ? options.interest : sim.playerFeet();

	WorldStreamSettings streamSettings;
	streamSettings.maxRenderDistance = options.interestRadius;
	streamSettings.minRenderDistance = std::max(32, options.interestRadius / 2);
	streamSettings.streamFrontBias = 0.30f;
	const float lodThreshold = static_cast<float>(streamSettings.minRenderDistance) * 2.f;
	const float lodThresholdSq = lodThreshold * lodThreshold;

	// --- Failure bookkeeping -------------------------------------------------
	bool failed = false;
	const auto fail = [&](ExitCode code, const std::string &message) {
		if (!failed)
		{
			failed = true;
			out.code = code;
			out.message = message;
		}
	};
	const auto timedOut = [&]() {
		if (elapsedSec() > options.timeoutSec)
		{
			fail(ExitCode::Timeout,
				 "run exceeded the " + std::to_string(options.timeoutSec) + " s timeout");
			return true;
		}
		return false;
	};

	// Bounded quiesce: publish every dispatched generation job before the next
	// simulation step (deterministic frame boundary). Never spins forever —
	// the run timeout bounds it, and a stuck worker surfaces as exit 6.
	const auto quiesce = [&]() {
		if (!pool)
			return true;
		while (world->asyncBusy())
		{
			world->pump();
			if (timedOut())
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		world->pump();
		return true;
	};

	// --- Frame loop -----------------------------------------------------------
	sim::PlayerCommand cmd{};
	uint64_t framesRemaining = options.ticks;

	const auto stepOneFrame = [&]() {
		// Mirrors Engine::run's simulation -> streaming -> mobs order, minus
		// every presentation step (no camera, day cycle, meshing or GPU).
		sim.advancePlayer(kFrameDt, cmd, false);
		if (!interestPinned)
			interest = sim.playerFeet();
		const glm::vec3 center(interest);
		world->updateStreaming(center, glm::vec2(0.f, 1.f), streamSettings);
		world->processChunkLoading(kLoadPerFrame);
		if (pool)
		{
			world->generatePendingVoxels(center, lodThresholdSq, kGenPerFrame);
			if (!quiesce())
				return false;
		}
		else
		{
			world->generatePendingVoxelsSync(center, kGenPerFrame);
		}
		world->pump();
		// Headless consumer: drain and discard the change stream (bounded;
		// nothing derived is attached). Keeps droppedEventCount at zero.
		world->takeEvents();
		sim.advanceMobs(kFrameDt, interest, static_cast<double>(options.interestRadius), false);
		// Dropped-item tick rule (issue #230): gravity/pickup/despawn advance
		// at the same host cadence as mobs; zero items early-outs inside.
		sim.advanceItems(kFrameDt, false);
		sim.pumpWorld();
		return true;
	};

	const auto runFrames = [&](uint64_t frames) {
		for (uint64_t i = 0; i < frames && framesRemaining > 0; ++i)
		{
			if (timedOut())
				return false;
			if (!stepOneFrame())
				return false;
			cmd.jumpPressed = false; // one-frame press, consumed
			++out.ticksRun;
			--framesRemaining;
		}
		return true;
	};

	const auto editCenter = [](const glm::ivec3 &cell) {
		return glm::vec3(static_cast<float>(cell.x) + 0.5f, static_cast<float>(cell.y) + 0.5f,
						 static_cast<float>(cell.z) + 0.5f);
	};

	std::vector<std::string> expectedDigests;
	for (const ScriptAction &action : script.actions)
	{
		if (failed)
			break;
		switch (action.op)
		{
		case ScriptOp::Wait:
			runFrames(static_cast<uint64_t>(action.frames));
			break;
		case ScriptOp::Move:
			cmd.move = action.move;
			break;
		case ScriptOp::Sprint:
			cmd.sprint = action.enabled;
			break;
		case ScriptOp::Jump:
			cmd.jumpPressed = true;
			break;
		case ScriptOp::Swim:
			cmd.swimUp = action.swimDirection > 0;
			cmd.swimDown = action.swimDirection < 0;
			break;
		case ScriptOp::Flight:
			if (!sim.setPlayerFlight(action.enabled, sim.playerFeet()))
				std::cerr << "[headless] line " << action.line << ": flight toggle refused ("
						  << sim.playerStatus() << ")\n";
			break;
		case ScriptOp::Teleport:
			sim.setPlayerPosition(action.position);
			break;
		case ScriptOp::Place:
		{
			// Authorized local edit, gated by the SAME player-aware placement
			// rule the graphical host uses (issue #209): a refusal is counted
			// and logged, never a silent world write inside the body.
			const glm::vec3 pos = editCenter(action.cell);
			if (!sim.canPlaceVoxel(pos, action.block))
			{
				++out.refusedEdits;
				std::cerr << "[headless] line " << action.line << ": placement refused at "
						  << cellToString(action.cell) << " (player-aware rule)\n";
				break;
			}
			const WorldEditResult result = sim.placeVoxel(pos, action.block);
			if (result.accepted)
				++out.committedEdits;
			else
			{
				// World-level rejection (e.g. target outside residency / not
				// editable): surface it so a script author gets a diagnostic
				// instead of a silent edits=0 (issue #210 review).
				++out.rejectedEdits;
				std::cerr << "[headless] line " << action.line << ": place rejected by world at "
						  << cellToString(action.cell) << " (unloaded/in-transit target?)\n";
			}
			break;
		}
		case ScriptOp::Delete:
		{
			const WorldEditResult result = sim.deleteVoxel(editCenter(action.cell));
			if (result.accepted)
				++out.committedEdits;
			else
			{
				++out.rejectedEdits;
				std::cerr << "[headless] line " << action.line << ": delete rejected by world at "
						  << cellToString(action.cell) << " (unloaded/in-transit target?)\n";
			}
			break;
		}
		case ScriptOp::Give:
		{
			// The logged grantItems admin seam (epic #223 §3): the sim itself
			// logs every grant; a remainder that did not fit is diagnosed so
			// a script author sees WHY an expect inventory later fails.
			const items::AddResult result = sim.grantItems(action.item, static_cast<uint16_t>(action.count));
			if (!result.remainder.isEmpty())
				std::cerr << "[headless] line " << action.line << ": grant remainder "
						  << stackToString(result.remainder) << " did not fit\n";
			break;
		}
		case ScriptOp::Select:
			if (sim.selectHotbarSlot(action.slot) != items::ItemResult::Ok)
				std::cerr << "[headless] line " << action.line << ": hotbar selection refused (slot "
						  << action.slot << ")\n";
			break;
		case ScriptOp::Drop:
		{
			// Fixed deterministic direction (+Z): the headless host has no
			// camera, so the presentation input is a plain constant.
			const items::ItemResult result =
				sim.dropFromSlot(sim.inventory().selectedSlot(), static_cast<uint16_t>(action.count),
								 glm::dvec3(0.0, 0.0, 1.0));
			if (result != items::ItemResult::Ok)
				std::cerr << "[headless] line " << action.line << ": drop refused (selected slot "
						  << sim.inventory().selectedSlot() << ", count " << action.count << ")\n";
			break;
		}
		case ScriptOp::Break:
		{
			// Gameplay breakBlock (issue #226), distinct from creative delete:
			// an accepted break is a committed edit AND spawns the registry drop.
			const sim::BreakResult result = sim.breakBlock(action.cell);
			if (result == sim::BreakResult::Ok)
				++out.committedEdits;
			else
			{
				++out.rejectedEdits;
				std::cerr << "[headless] line " << action.line << ": break refused at "
						  << cellToString(action.cell) << " (" << breakResultName(result) << ")\n";
			}
			break;
		}
		case ScriptOp::PlaceSelected:
		{
			// Gameplay place-from-hotbar (issue #227), distinct from creative
			// place: consumes exactly 1 item on success only.
			const sim::PlaceResult result = sim.placeSelectedBlock(action.cell);
			if (result == sim::PlaceResult::Ok)
				++out.committedEdits;
			else if (result == sim::PlaceResult::PlayerOverlap)
			{
				++out.refusedEdits;
				std::cerr << "[headless] line " << action.line << ": placeSelected refused at "
						  << cellToString(action.cell) << " (player-aware rule)\n";
			}
			else
			{
				++out.rejectedEdits;
				std::cerr << "[headless] line " << action.line << ": placeSelected refused at "
						  << cellToString(action.cell) << " (" << placeResultName(result) << ")\n";
			}
			break;
		}
		case ScriptOp::Interest:
			interest = action.position;
			interestPinned = true;
			break;
		case ScriptOp::InterestFollow:
			interestPinned = false;
			break;
		case ScriptOp::Save:
			if (!sim.isWorldOpen())
			{
				fail(ExitCode::WorldIo,
					 "script line " + std::to_string(action.line) + ": save requires --world");
				break;
			}
			if (!sim.savePlayerState(0.f, 0.f))
				fail(ExitCode::WorldIo,
					 "script line " + std::to_string(action.line) + ": player-state save/flush failed");
			break;
		case ScriptOp::ExpectVoxel:
		{
			sim.pumpWorld();
			WorldCollisionView view(*world);
			TextureType type = AIR;
			const physics::Cell cell = view.sampleWithType(action.cell, &type);
			const bool matches = action.expectedVoxel == kExpectUnknown
									 ? !cell.available
									 : cell.available && static_cast<int>(type) == action.expectedVoxel;
			if (!matches)
			{
				const std::string actual = !cell.available ? std::string("unknown") : blockName(type);
				const std::string expected = action.expectedVoxel == kExpectUnknown
												 ? std::string("unknown")
												 : blockName(static_cast<TextureType>(action.expectedVoxel));
				fail(ExitCode::Assertion,
					 "script line " + std::to_string(action.line) + ": expected voxel " +
						 cellToString(action.cell) + " = " + expected + ", got " + actual);
			}
			break;
		}
		case ScriptOp::ExpectInventory:
		{
			sim::InventorySnapshot inventory;
			sim.inventorySnapshot(inventory);
			const items::ItemStack &actual = inventory.slots[static_cast<size_t>(action.slot)];
			const bool matches = action.expectEmpty
									 ? actual.isEmpty()
									 : actual.item == action.item && actual.count == action.count;
			if (!matches)
			{
				const std::string expected = action.expectEmpty
												 ? std::string("empty")
												 : std::string(items::itemName(action.item)) + " x" +
													   std::to_string(action.count);
				fail(ExitCode::Assertion,
					 "script line " + std::to_string(action.line) + ": expected inventory slot " +
						 std::to_string(action.slot) + " = " + expected + ", got " + stackToString(actual));
			}
			break;
		}
		case ScriptOp::ExpectItems:
		{
			const size_t live = sim.itemEntities().entities().size();
			if (live != static_cast<size_t>(action.expectedItems))
				fail(ExitCode::Assertion,
					 "script line " + std::to_string(action.line) + ": expected " +
						 std::to_string(action.expectedItems) + " live item entities, got " +
						 std::to_string(live));
			break;
		}
		case ScriptOp::ExpectDigest:
			expectedDigests.push_back(action.expectedDigest);
			break;
		}
	}

	// Idle-fill the remaining tick budget (no script, or script shorter than
	// --ticks): the deterministic end condition of every run.
	if (!failed)
		runFrames(framesRemaining);

	// --- Final quiesce + digest ------------------------------------------------
	if (!failed && !quiesce())
		failed = true;
	if (!failed)
		sim.pumpWorld();

	const sim::WorldSnapshot worldSnapshot = sim.worldSnapshot();
	out.chunkCount = worldSnapshot.chunkCount;
	out.editRevision = worldSnapshot.editRevision;
	out.mobCount = sim.mobs().mobs().size();
	out.itemCount = sim.itemEntities().entities().size();
	for (const items::ItemStack &stack : sim.inventory().slots())
		out.inventoryTotal += stack.count;
	out.itemIdHighWater = sim.itemIdHighWater();
	out.playerPos = sim.playerFeet();
	if (sim.isInitialized())
		out.digest = computeStateDigest(sim, out.area, out.ticksRun, out.committedEdits).hex();

	if (!failed)
	{
		if (!options.expectedDigest.empty() && out.digest != options.expectedDigest)
			fail(ExitCode::Assertion,
				 "--expect-digest mismatch: expected " + options.expectedDigest + ", got " + out.digest);
		for (const std::string &expected : expectedDigests)
			if (out.digest != expected)
				fail(ExitCode::Assertion,
					 "script expect digest mismatch: expected " + expected + ", got " + out.digest);
	}

	// --- Save on exit + teardown ------------------------------------------------
	// The explicit durable path: capture the player (orientation is
	// presentation; the runner pins yaw/pitch to 0), then the REFUSABLE close.
	// A refused/failed close keeps persistence open for the unconditional
	// shutdown safety net, and is reported as a WorldIo error exit.
	if (sim.isWorldOpen())
	{
		if (failed)
		{
			// "skipped" = the EXPLICIT savePlayerState + close was skipped
			// because the run errored. It does NOT mean nothing was persisted:
			// sim.shutdown() below still force-closes (resolves stranded edits
			// + flushes accepted chunk edits, issue #180). Only the explicit
			// player.state capture is skipped. See docs/headless-runner.md §3.
			out.saveStatus = "skipped";
		}
		else
		{
			bool ok = sim.savePlayerState(0.f, 0.f);
			ok = world->closeWorld() && ok;
			out.saveStatus = ok ? "ok" : "failed";
			if (!ok)
				fail(ExitCode::WorldIo, "save-on-exit failed (player state capture / flush / close)");
		}
	}
	sim.shutdown();

	return out;
}

} // namespace headless
