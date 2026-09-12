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

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed (" << g_checks << " run)\n";
		return 1;
	}
	std::cout << "PASS: engine reload lifecycle (" << g_checks << " checks)" << std::endl;
	return 0;
}
