// Engine-level integration test for the persistent-world lifecycle
// (issue #180 review round 4, items 5-9): reloadWorld() returns a real
// status, an open persistent world closes cleanly on reload while the save
// identity survives untouched, a fresh session adopts the stored seed, and
// the test writes ONLY under its own temporary saves root - never the
// repository. Needs a GPU/window: skips with code 77 when the ENGINE
// BOOTSTRAP fails (headless CI); a failure inside the test body is a real
// FAIL, not a skip.
#include <Engine/Engine.hpp>
#include <World/WorldPersistence.hpp>
#include <Chunk/Chunk.hpp>
#include <utils.hpp>

#include <cmath>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

static int g_fails = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                       \
	do                                                                         \
	{                                                                          \
		++g_checks;                                                            \
		if (!(cond))                                                           \
		{                                                                      \
			std::cerr << "FAIL: " << msg << " (" << __LINE__ << ")\n";         \
			++g_fails;                                                         \
		}                                                                      \
	} while (0)

namespace
{
std::string uniqueSuffix()
{
	return std::to_string(
		std::chrono::steady_clock::now().time_since_epoch().count());
}

// RAII cleanup: even an exception must not leave artifacts behind.
struct TempDirGuard
{
	std::filesystem::path path;
	~TempDirGuard()
	{
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
	}
};
} // namespace

int main()
{
	constexpr const char *kWorldName = "engine-reload";

	// ---- GPU/window bootstrap: the ONLY skip path (issue #180 review
	// round 4, item 7). Everything after this is a real test.
	std::unique_ptr<Engine> engine;
	try
	{
		engine = std::make_unique<Engine>("ressources");
	}
	catch (const std::exception &e)
	{
		std::cout << "SKIP (engine unavailable in this environment): " << e.what() << std::endl;
		return 77;
	}

	try
	{
		// Unique temp saves root (issue #180 review round 4, item 5): the
		// test must never write repository/saves.
		const std::filesystem::path repoSaves = std::filesystem::path("saves");
		const bool repoSavesExisted = std::filesystem::exists(repoSaves);
		const std::filesystem::path root =
			std::filesystem::temp_directory_path() /
			("ft-vox-engine-reload-" + uniqueSuffix());
		TempDirGuard guard{root};

		const auto checkIdentity = [&](int expectedSeed, const char *what)
		{
			CHECK(WorldPersistence::worldExists(root, kWorldName), what);
			int stored = 0;
			std::string err;
			CHECK(WorldPersistence::peekStoredSeed(root, kWorldName, stored, err) &&
			          stored == expectedSeed,
			      (std::string(what) + " (stored seed)").c_str());
		};

		// Session 1: create the persistent world, then reload on top of it.
		{
			engine->setSavesRootForTests(root);
			engine->requestOpenWorld(kWorldName);
			engine->initializeNoiseGenerator(42);

			CHECK(engine->worldSeed() == 42, "world opened with the requested seed");
			checkIdentity(42, "save created");

			// Reload with an OPEN persistent world: the close must succeed
			// here (no stranded edits in this deterministic path) and the
			// reload proceeds transiently. The save identity must survive
			// untouched (issue #180 review round 4, item 8).
			CHECK(engine->reloadWorld(43), "persistent world closes and reload succeeds");
			CHECK(engine->worldSeed() == 43, "new transient seed active");
			checkIdentity(42, "save identity untouched by the reload");
		} // ~Engine: world was closed by the reload - no player-state write.

		// Session 2: reopen WITHOUT an explicit seed - the stored seed must
		// be adopted (never overwritten), then a second reload closes again.
		{
			engine->setSavesRootForTests(root);
			engine->requestOpenWorld(kWorldName);
			engine->initializeNoiseGenerator(0);
			CHECK(engine->worldSeed() == 42, "stored seed adopted when no explicit seed is given");
			CHECK(engine->reloadWorld(44), "second reload proceeds");
			CHECK(engine->worldSeed() == 44, "second transient seed active");
		}

		// The save is still intact and openable after both sessions.
		{
			WorldPersistence verify;
			const WorldPersistence::OpenInfo info =
				verify.openOrCreate(root, kWorldName, 42, 1);
			CHECK(info.ok && !info.created, "save reopens with seed 42 after both sessions");
			verify.shutdown();
		}

		// Filesystem isolation (issue #180 review round 4, item 13): the
		// repository saves root must be untouched by the test.
		if (repoSavesExisted)
		{
			std::cout << "[info] repository saves/ existed before the run; contents untouched"
			          << std::endl;
		}
		else
		{
			CHECK(!std::filesystem::exists(repoSaves),
			      "repository saves/ was never created by the test");
		}
		// guard's destructor removes the temp root; assert after scope? The
		// guard runs at function exit - verify explicitly now and let the
		// destructor be idempotent (remove_all of a missing path is a no-op).
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
			CHECK(!std::filesystem::exists(root), "temp saves root removed after the test");
		}
	}
	catch (const std::exception &e)
	{
		// A lifecycle bug must FAIL, not skip (issue #180 review round 4,
		// item 7).
		std::cerr << "FAIL: unexpected exception: " << e.what() << std::endl;
		return 1;
	}

	// ---- Restored-player bootstrap at distance (issue #180 review round 6,
	// item 4): the initial area must generate around the SAVED position, not
	// the default origin.
	{
		const std::filesystem::path farRoot =
			std::filesystem::temp_directory_path() /
			("ft-vox-engine-far-" + uniqueSuffix());
		TempDirGuard farGuard{farRoot};
		constexpr const char *kFarWorld = "far-player";
		// Feet position at chunk (100, -80), high enough to be clear air
		// after generation (terrain validation policy: exact restore).
		const double savedX = 100.0 * 16 + 8.5;
		const double savedZ = -80.0 * 16 + 4.5;
		const double savedY = 240.0;

		{
			WorldPersistence writer;
			CHECK(writer.openOrCreate(farRoot, kFarWorld, 42, 1).ok, "far world created");
			PlayerPersistState state;
			state.x = savedX;
			state.y = savedY;
			state.z = savedZ;
			state.yaw = 30.0f;
			state.pitch = -10.0f;
			state.flight = true;
			state.selectedBlock = 3;
			CHECK(writer.writePlayerState(state), "far player.state written");
			writer.shutdown();
		}

		// Single live Engine per process: the ImGui Vulkan backend keeps
		// process-global state, so two simultaneous Engine instances would
		// double-free backend device objects at shutdown.
		engine.reset();

		{
			Engine farEngine("ressources");
			farEngine.setSavesRootForTests(farRoot);
			farEngine.requestOpenWorld(kFarWorld);
			farEngine.initializeNoiseGenerator(0); // adopts the stored seed

			CHECK(farEngine.worldSeed() == 42, "far world adopts the stored seed");

			// The bootstrap must have generated the SAVED chunk, not only the
			// origin area: chunk (100, -80) present and generated.
			const ChunkManager *chunks = farEngine.chunksForTests();
			const Chunk *savedChunk = chunks ? chunks->getChunk(glm::ivec3(100, 0, -80)) : nullptr;
			CHECK(savedChunk != nullptr && savedChunk->getState() >= ChunkState::GENERATED,
			      "saved-position chunk bootstrapped (100, -80)");
			const Chunk *originChunk = chunks ? chunks->getChunk(glm::ivec3(0, 0, 0)) : nullptr;
			CHECK(originChunk == nullptr || originChunk->getState() < ChunkState::GENERATED,
			      "origin area NOT the bootstrap center for a far saved player");

			// Player restored at the saved spot (not snapped to any surface).
			const glm::dvec3 feet = farEngine.playerPositionForTests();
			CHECK(std::abs(feet.x - savedX) < 0.5 && std::abs(feet.z - savedZ) < 0.5 &&
			          std::abs(feet.y - savedY) < 1.5,
			      "player restored at the saved far position");
		}
	}

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed (" << g_checks << " run)\n";
		return 1;
	}
	std::cout << "PASS: engine reload lifecycle (" << g_checks << " checks)" << std::endl;
	return 0;
}
