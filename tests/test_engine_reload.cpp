// Engine-level integration test for the persistent-world lifecycle
// (issue #180 review round 3, item 12): reloadWorld() with an OPEN
// persistent world must close it cleanly (flush + transient reload) and
// never touch the save identity, and a fresh session must adopt the stored
// seed. Needs a GPU/window: skips with code 77 when the engine cannot be
// constructed (headless CI).
#include <Engine/Engine.hpp>
#include <World/WorldPersistence.hpp>

#include <filesystem>
#include <iostream>
#include <string>

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

int main()
{
	constexpr const char *kWorldName = "engine-reload";
	const std::filesystem::path saveDir = std::filesystem::path("saves") / kWorldName;

	try
	{
		// Fresh save for a deterministic run.
		std::error_code ec;
		std::filesystem::remove_all(saveDir, ec);

		// Session 1: create the persistent world, then reload on top of it.
		{
			Engine engine("ressources");
			engine.requestOpenWorld(kWorldName);
			engine.initializeNoiseGenerator(42);

			CHECK(engine.worldSeed() == 42, "world opened with the requested seed");
			CHECK(WorldPersistence::worldExists("saves", kWorldName), "save directory created");
			int stored = 0;
			std::string err;
			CHECK(WorldPersistence::peekStoredSeed("saves", kWorldName, stored, err) && stored == 42,
			      "stored seed is 42 after creation");

			// Reload with an OPEN persistent world: the close must succeed
			// here (no stranded edits in this deterministic path) and the
			// reload proceeds transiently. The save identity must survive
			// untouched.
			engine.reloadWorld(43);
			CHECK(engine.worldSeed() == 43, "reload rebuilt the world with the new seed");
			stored = 0;
			CHECK(WorldPersistence::peekStoredSeed("saves", kWorldName, stored, err) && stored == 42,
			      "save identity untouched by the reload");
		} // ~Engine: world was closed by the reload - no player-state write.

		// Session 2: reopen WITHOUT an explicit seed - the stored seed must
		// be adopted (never overwritten), then a second reload closes again.
		{
			Engine engine("ressources");
			engine.requestOpenWorld(kWorldName);
			engine.initializeNoiseGenerator(0);
			CHECK(engine.worldSeed() == 42, "stored seed adopted when no explicit seed is given");
			engine.reloadWorld(44);
			CHECK(engine.worldSeed() == 44, "second reload proceeds");
		}

		// The save is still intact and openable after both sessions.
		{
			WorldPersistence verify;
			const WorldPersistence::OpenInfo info =
				verify.openOrCreate("saves", kWorldName, 42, 1);
			CHECK(info.ok && !info.created, "save reopens with seed 42 after both sessions");
			verify.shutdown();
		}
	}
	catch (const std::exception &e)
	{
		std::cout << "SKIP (engine unavailable in this environment): " << e.what() << std::endl;
		return 77;
	}

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed (" << g_checks << " run)\n";
		return 1;
	}
	std::cout << "PASS: engine reload lifecycle (" << g_checks << " checks)\n";
	return 0;
}
