#pragma once

// =============================================================================
// HeadlessRunner — the graphical-free host of sim::GameSimulation (issue
// #210, Epic #204 Phase F). Proves the CPU simulation is reusable by a
// process with NO SDL3, Vulkan, ImGui, camera, renderer or GPU resource pool:
// this host owns only a ThreadPool (optional) and the simulation facade, and
// drives it with plain values exactly like the graphical Engine host does —
// commands in, snapshots out.
//
// Headless frame orchestration (fixed dt = 1/60, mirroring Engine::run's
// simulation -> streaming -> mobs order without any presentation step):
//   1. advancePlayer(dt, command, suspended=false)
//   2. interest source update (player feet, or the pinned scripted probe)
//   3. world.updateStreaming(interest, +Z forward, plain settings)
//   4. world.processChunkLoading(fixed per-frame budget)
//   5. generation: async dispatch + bounded per-frame quiesce (workers > 0),
//      or deterministic synchronous generation (workers == 0)
//   6. world.pump() + event drain (headless consumer discards the stream)
//   7. advanceMobs(dt, interest, interestRadius, suspended=false)
//   8. sim.pumpWorld()
//
// Residency is controlled by the EXPLICIT headless interest source (a plain
// point + radius in blocks), never a Camera or GPU render distance. Budgets
// are fixed per-frame counts — never wall-clock derived — so the workload is
// reproducible. With workers > 0 every dispatched generation job is
// published before the next simulation step (bounded by the run timeout),
// which keeps the authoritative state at frame boundaries deterministic
// while still exercising the real async worker/publication path.
//
// Lifecycle: initialize -> bootstrapTerrain -> finalizePlayerSpawn -> frames
// -> quiesce -> digest -> optional savePlayerState + refusable closeWorld ->
// shutdown (destructor safety net). Every failure path returns a distinct
// non-zero exit code; teardown never stalls indefinitely (run timeout).
// =============================================================================

#include <cstdint>
#include <filesystem>
#include <string>

#include <glm/glm.hpp>

#include <Headless/SimDigest.hpp>

namespace headless
{

struct RunnerOptions
{
	int seed{42};
	uint64_t ticks{600};
	std::filesystem::path scriptPath; // empty: idle run
	std::string worldName;			  // empty: transient session (no persistence)
	std::filesystem::path savesRoot{"saves"};
	bool fixedInterest{false};
	glm::dvec3 interest{0.0};
	int interestRadius{96}; // streaming radius in blocks (plain value)
	int workers{0};			// 0 = synchronous generation (no ThreadPool)
	bool hasDigestArea{false};
	DigestArea digestArea{};
	std::string expectedDigest; // --expect-digest (lowercase hex, empty = off)
	double timeoutSec{120.0};
	bool json{false};
};

/// Distinct process exit statuses (documented in docs/headless-runner.md).
enum class ExitCode : int
{
	Ok = 0,
	Usage = 2,
	Script = 3,
	WorldIo = 4,
	Assertion = 5,
	Timeout = 6
};

struct RunOutcome
{
	ExitCode code{ExitCode::Ok};
	std::string message; // error detail when code != Ok

	// Machine-readable summary fields (plain values only).
	int seed{0};
	uint32_t generatorVersion{0};
	uint64_t ticksRun{0};
	int workers{0};
	int interestRadius{0};
	size_t chunkCount{0};
	uint64_t committedEdits{0};
	uint64_t refusedEdits{0};	// player-aware canPlaceVoxel refusals
	uint64_t rejectedEdits{0};	// world-level rejections (e.g. outside residency)
	uint64_t editRevision{0};
	size_t mobCount{0};
	glm::dvec3 playerPos{0.0};
	DigestArea area{};
	std::string digest;
	std::string saveStatus{"none"}; // none | ok | failed | skipped
};

/// Run one headless session end-to-end. Never throws for expected failure
/// modes (script/world/assertion/timeout); the outcome carries the exit code.
RunOutcome runHeadless(const RunnerOptions &options);

/// Fixed host frame delta (seconds). The simulation's internal fixed steps
/// (120 Hz player / 60 Hz mobs) are unchanged — this is only the host cadence.
inline constexpr double kFrameDt = 1.0 / 60.0;

/// Fixed per-frame streaming budgets (counts, never wall-clock derived).
inline constexpr int kLoadPerFrame = 8;
inline constexpr int kGenPerFrame = 8;

} // namespace headless
