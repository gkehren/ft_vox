#include <Engine/EngineMultiplayer.hpp>

#include "TestMain.hpp"

#include <stdexcept>

using namespace std::chrono_literals;

struct EngineJoinProbe
{
	using Clock = Engine::RemoteSession::Clock;

	explicit EngineJoinProbe(Engine &engine) : engine(engine) {}
	Engine &engine;

	GameUIFrame frame()
	{
		GameUIFrame state;
		engine.populateJoinUi(state);
		if (engine.m_remote)
			engine.populateRemoteUi(state);
		return state;
	}

	void pump()
	{
		engine.applySessionRequests();
		if (engine.m_remote)
		{
			if (const auto &trace = engine.m_remote->motionTrace; trace && trace->frames == 0)
			{
				CHECK(trace->sent == 0 && trace->failed == 0 && trace->lastInputSequence == 0 &&
					trace->start == trace->lastFlush && trace->cameraAt == Clock::time_point{},
					"a replacement trace starts with fresh counters and timestamps");
			}
			engine.pollRemote();
			engine.tickStreaming(1.0 / 120.0);
			if (engine.m_remote->motionTrace)
				engine.traceRemoteMotion(1.0 / 120.0);
		}
	}

	void until(const auto &done, const char *description)
	{
		const auto deadline = Clock::now() + 20s;
		while (!done())
		{
			if (Clock::now() >= deadline)
				throw std::runtime_error(description);
			pump();
			std::this_thread::sleep_for(4ms);
		}
	}

	void join(const char *endpoint, const char *profile, const char *seed)
	{
		frame().connectServer({endpoint, profile, seed});
		pump();
		until([&] {
			return !engine.switchingSession() && engine.m_remote &&
				engine.m_remote->endpoint == endpoint && engine.m_remote->namedProfile == profile &&
				engine.m_remote->ready();
		}, "remote projection did not become ready");
	}

	void checkRetiredProjection()
	{
		const auto &remote = *engine.m_remote;
		CHECK(!remote.scope && !remote.projection && !remote.hasCameraPose &&
		    !remote.transport.inventory() && remote.prediction.backlog() == 0,
		    "departure clears replica, inventory, camera hold and predicted inputs");
		CHECK(!engine.renderBridge && engine.chunkPool->acquiredCount() == 0 &&
		    engine.drawList.empty() && engine.shadowList.empty() &&
		    engine.mobStates.empty() && engine.m_itemStates.empty(),
		    "departure clears draw references, entity views and pooled render handles");
	}

	void run(const char *first, const char *second)
	{
		engine.requestOpenWorld("solo");
		engine.initializeNoiseGenerator(13);
		engine.stepDerivedProjectionForTests(1.0 / 60.0);
		auto *const solo = engine.sim.get();
		const auto soloFeet = solo->playerFeet();
		engine.selectHotbarSlot(5);
		engine.paused = true;
		const float soloTime = engine.shaderParams.dayTime;
		const auto handleCapacity = engine.chunkPool->capacity();
		CHECK(!engine.drawList.empty(), "solo world has live mesh references before the switch");
		frame().createJoinProfile("alice");
		frame().createJoinProfile("bob");
		frame().connectServer({"localhost:1", "alice", ""});
		pump();
		CHECK(engine.sim.get() == solo && !frame().joinError.empty(), "invalid join leaves solo running");

		join(first, "alice", "42");
		engine.tickDayCycle(1.0);
		const auto aliceId = engine.m_remote->welcome->player;
		const auto firstScope = engine.m_remote->scope;
		const auto firstEpoch = engine.renderBridge->worldGenerationId();
		CHECK(firstScope && engine.viewWorld() != solo->world() && engine.m_suspendedSolo &&
			engine.m_suspendedSolo->simulation.get() == solo, "only the remote world is projected");
		engine.selectHotbarSlot(4);
		until([&] { return engine.selectedHotbarSlot() == 4; }, "server did not apply hotbar selection");
		const auto aliceInventory = engine.m_remote->transport.inventory()->slots;
		const auto aliceFeet = net::udp::playerFeet(*engine.m_remote->localPlayer());
		CHECK(engine.chunkPool->acquiredCount() > 0, "remote projection owns render handles before departure");

		frame().connectServer({second, "bob", "43"});
		pump();
		CHECK(engine.switchingSession() && engine.m_remote->endpoint == first,
			"queued join waits for old transport departure");
		checkRetiredProjection();
		until([&] { return engine.m_remote->endpoint == second && engine.m_remote->ready(); },
			"second endpoint did not connect");
		CHECK(engine.worldSeed() == 43 && engine.m_remote->scope != firstScope &&
		    engine.renderBridge->worldGenerationId() > firstEpoch && engine.selectedHotbarSlot() == 0,
		    "second server starts a fresh scope, projection and inventory");
		const auto bobId = engine.m_remote->welcome->player;
		join(second, "alice", "43");
		CHECK(engine.m_remote->welcome->player != bobId, "switching profiles on one endpoint changes actors");

		join(first, "alice", "42");
		CHECK(engine.m_remote->welcome->player == aliceId && engine.m_remote->scope != firstScope &&
		    engine.selectedHotbarSlot() == 4 && engine.m_remote->transport.inventory()->slots == aliceInventory &&
		    glm::length(net::udp::playerFeet(*engine.m_remote->localPlayer()) - aliceFeet) < 1.0,
		    "returning to the first profile restores its actor, inventory and position");
		frame().leaveServer();
		pump();
		checkRetiredProjection();
		until([&] { return static_cast<bool>(frame().joinServer); }, "Disconnect did not complete");
		frame().joinServer();
		pump();
		until([&] { return engine.m_remote->ready(); }, "Reconnect did not restore the session");
		CHECK(engine.m_remote->welcome->player == aliceId && engine.selectedHotbarSlot() == 4,
			"Reconnect uses the same endpoint and profile");

		frame().connectServer({first, "alice", "43"});
		pump();
		until([&] {
			const auto current = frame();
			return current.multiplayerStatus->state == ui::MultiplayerState::Rejected && current.multiplayerStatus->detail &&
				std::string_view(current.multiplayerStatus->detail) == "World seed mismatch";
		}, "expected-seed mismatch did not reach Rejected");
		checkRetiredProjection();
		frame().returnToSolo();
		pump();
		engine.tickStreaming(1.0 / 120.0);
		CHECK(!engine.m_remote && engine.sim.get() == solo && engine.viewWorld() == solo->world() &&
		    engine.playerPositionForTests() == soloFeet && engine.selectedHotbarSlot() == 5 &&
		    engine.chunkPool->capacity() == handleCapacity && engine.paused &&
		    engine.shaderParams.dayTime == soloTime && !engine.drawList.empty(),
		    "return restores paused solo position, inventory, time and visible terrain");
		CHECK(engine.renderBridge && engine.renderBridge->chunkCount() > 0 &&
			engine.renderBridge->pendingGenJobs() == 0, "solo projection rebinds after workers drain");

		net::udp::Socket silentServer({0x7f000001, 0});
		frame().connectServer({client::formatEndpoint(silentServer.address()), "bob", ""});
		pump();
		until([&] {
			return frame().multiplayerStatus->state == ui::MultiplayerState::TimedOut;
		}, "unreachable server did not time out");
		CHECK(frame().joinServer && frame().returnToSolo, "timeout leaves both retry and solo available");
		frame().returnToSolo();
		pump();
		CHECK(engine.sim.get() == solo && !engine.m_remote, "timeout leaves the same solo world resumable");
	}
};

int runEngineJoinSmoke(Engine &engine, const char *first, const char *second)
{
	EngineJoinProbe{engine}.run(first, second);
	std::cout << "Native multiplayer join smoke: " << testmain::checks << " checks\n";
	return testmain::exitCode();
}
