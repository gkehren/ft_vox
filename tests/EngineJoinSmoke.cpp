#include <Engine/EngineMultiplayer.hpp>
#include <Engine/GameUiSample.hpp>
#include <Hosting/RoleCredentials.hpp>
#include <Items/ItemRegistry.hpp>
#include <World/WorldCollisionView.hpp>

#include "TestMain.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace
{
namespace fs = std::filesystem;
using localcontrol::Operation;
using localcontrol::Result;
using localcontrol::Role;

// Single writers publish complete rendezvous files in a fresh evidence root.
void publish(const fs::path &path, const std::string &value)
{
	auto pending = path;
	pending += ".tmp";
	std::ofstream out(pending);
	out << value << '\n';
	out.close();
	if (!out)
		throw std::runtime_error("could not write hosted closure evidence");
	fs::rename(pending, path);
}

struct ActorWitness
{
	uint64_t world{};
	uint32_t player{};
	glm::dvec3 feet{};
	int selected{};
	std::array<items::ItemStack, items::kInventorySize> slots{};
};
}

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
		engine.tickHosting();
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

	void runHosting(const std::filesystem::path &server)
	{
		engine.m_hostExecutable = server;
		engine.m_hostSavesRoot = engine.m_savesRoot / "dedicated";
		engine.initializeNoiseGenerator(13);
		frame().createJoinProfile("host-choice");
		frame().createJoinProfile("join-draft");
		frame().createJoinProfile("saved-choice");
		frame().selectJoinProfile("saved-choice");
		client::JoinDraft joinDraft;
		joinDraft.setProfile("join-draft");
		hosting::Options options;
		options.world = "Hosted test";
		options.port = 0;
		options.playerSlots = 2;
		options.interestRadius = 1;
		frame().startHostedServer(options, true, "missing-profile");
		CHECK(!engine.m_hostServer.snapshot().active() && !engine.m_hostError.empty(),
			"auto-join refuses a missing Host profile before launching a child");
		CHECK(engine.m_joinPreferences.selectedProfile == "saved-choice" && joinDraft.request().profile == "join-draft",
			"the Join draft differs from the saved selection");
		frame().startHostedServer(options, true, "HOST-CHOICE");
		frame().selectJoinProfile(joinDraft.profile.data());
		until([&] {
			return engine.m_hostSnapshot.state == hosting::State::Ready &&
				engine.m_hostSnapshot.status && engine.m_hostSnapshot.status->transport.count == 1 &&
				engine.m_hostSnapshot.status->transport.roster().front().state == monitoring::SessionState::Connected &&
				engine.m_hostSnapshot.administration &&
				engine.m_remote && engine.m_remote->ready();
		}, "hosted server did not auto-join");
		auto state = engine.m_hostSnapshot;
		CHECK(state.address == "127.0.0.1" && state.port > 0 &&
			engine.m_remote->endpoint == state.address + ":" + std::to_string(state.port),
			"auto-join uses the ready loopback endpoint and assigned port");
		CHECK(engine.m_remote->namedProfile == "host-choice" && engine.m_suspendedSolo,
			"auto-join uses the canonical Host choice captured at Start and suspends solo play");
		CHECK(engine.m_remote->welcome && state.status->transport.roster().front().id == engine.m_remote->welcome->player &&
			state.status->transport.roster().front().state == monitoring::SessionState::Connected &&
			state.status->transport.capacity == 2 && state.status->checkpoint == state.checkpoint,
			"the graphical host receives authoritative identity, capacity, phase and checkpoint on its monitoring pipe");
		CHECK(localcontrol::readControlKey(engine.m_savesRoot / ".client-profiles" / "named" /
			"host-choice" / "owner-control.key") &&
			std::filesystem::is_regular_file(engine.m_hostSavesRoot / ".ft_vox..server-roles" / options.world),
			"the graphical host creates a private owner credential and publishes role authority before joining");
		frame().joinHostedServer("saved-choice");
		CHECK(engine.m_joinRequest && engine.m_joinRequest->profile == "saved-choice",
			"manual Join hosted server uses its explicit Host choice");
		engine.m_joinRequest.reset();

		using localcontrol::Operation;
		using localcontrol::Result;
		using localcontrol::Role;
		CHECK(state.administration->role == Role::Owner, "the frame gets server-verified owner permission");
		std::string profileError;
		const auto guestProfile = engine.loadNamedProfile("saved-choice", profileError);
		if (!guestProfile)
			throw std::runtime_error(profileError);
		net::udp::ClientTransport guest(net::udp::parseAddress(state.address, state.port), guestProfile->key);
		guest.connect(Clock::now());
		until([&] {
			guest.poll(Clock::now());
			return engine.m_hostSnapshot.status && engine.m_hostSnapshot.status->registeredProfiles == 2 &&
				guest.status() == net::udp::ClientStatus::Connected && guest.welcome();
		}, "guest profile was not admitted");
		const auto command = [&](Operation operation) {
			const auto previous = engine.m_hostSnapshot.administration->sequence;
			const auto error = frame().administerHostedServer(state.launch, operation, guest.welcome()->player,
				operation == Operation::GrantAdmin ? "saved-choice" : "");
			if (!error.empty())
				throw std::runtime_error(error);
			until([&] {
				guest.poll(Clock::now());
				return engine.m_hostSnapshot.administration && engine.m_hostSnapshot.administration->sequence > previous;
			}, "administration callback received no reply");
			CHECK(engine.m_hostSnapshot.adminResult == Result::Applied, "the server accepts the owner panel action");
		};
		command(Operation::GrantAdmin);
		CHECK(engine.m_hostSnapshot.administration->players.back().localAdmin &&
			engine.m_hostSnapshot.administration->audit.back().operation == Operation::GrantAdmin,
			"the panel grant binds the saved local profile and publishes the audit");
		command(Operation::Ban);
		CHECK(engine.m_hostSnapshot.administration->players.back().banned, "the panel ban changes server admission");

		engine.enableGameUi();
		SDL_Event focus{};
		focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
		focus.window.windowID = SDL_GetWindowID(engine.window);
		if (!SDL_PushEvent(&focus))
			throw std::runtime_error(SDL_GetError());
		SDL_Event escape{};
		escape.type = SDL_EVENT_KEY_DOWN;
		escape.key.windowID = SDL_GetWindowID(engine.window);
		escape.key.key = SDLK_ESCAPE;
		escape.key.scancode = SDL_SCANCODE_ESCAPE;
		if (!SDL_PushEvent(&escape))
			throw std::runtime_error(SDL_GetError());
		escape.type = SDL_EVENT_KEY_UP;
		if (!SDL_PushEvent(&escape))
			throw std::runtime_error(SDL_GetError());
		engine.handleEvents();
		CHECK(engine.m_playerUi->screens().top() == gameui::Screen::Pause && !engine.nativePause() &&
			!engine.m_movementAllowed && !engine.mouseCaptured, "connected menu blocks local control without solo suspension");
		const auto openAt = engine.m_remote->players.tick;
		until([&] {
			engine.processInput(1.0 / 120.0);
			return engine.m_remote->players.tick > openAt;
		}, "connected simulation stopped advancing while the menu was open");
		CHECK(engine.m_remote->ready(), "server snapshots continue under the native menu");

		engine.running = true;
		SDL_Event close{};
		close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
		close.window.windowID = SDL_GetWindowID(engine.window);
		if (!SDL_PushEvent(&close))
			throw std::runtime_error(SDL_GetError());
		engine.handleEvents();
		CHECK(engine.running && engine.m_exitAfterHostedStop, "game exit waits for its hosted server");
		CHECK(engine.m_playerUi->screens().top() == gameui::Screen::Hud && !engine.suppressDeveloperUi(),
			"host shutdown restores Developer UI for stop progress and recovery");
		until([&] { return !engine.running || engine.m_hostSnapshot.state == hosting::State::Failed; },
			"game did not reach a terminal hosted shutdown");
		CHECK(!engine.running && engine.m_hostSnapshot.state == hosting::State::Stopped && !engine.m_hostSnapshot.status &&
			engine.m_hostSnapshot.checkpoint > state.checkpoint,
			"game exits after the final hosted checkpoint was committed");
	}

	void presentClosureFrame()
	{
		// Render through the same swapchain, mesh upload and ImGui paths as
		// the game while the rendezvous drives the panel callbacks.
		int width = 0, height = 0;
		SDL_GetWindowSizeInPixels(engine.window, &width, &height);
		if (width <= 0 || height <= 0)
			throw std::runtime_error("native smoke window has no Vulkan viewport");
		engine.recreateSwapchainIfNeeded(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
		uint32_t image = 0;
		if (!engine.frameCtx->beginFrame(*engine.swapchain, image))
			throw std::runtime_error("native smoke could not acquire a swapchain image");
		const auto frameIndex = engine.frameCtx->frameIndex();
		engine.updateRemoteCamera();
		engine.tickStreaming(1.0 / 60.0);
		engine.updateRemoteEntities();
		engine.updateHighlight();
		engine.resourceRetire.beginFrame(engine.frameNumber);
		engine.worldRenderer->arenas().beginFrame(engine.frameNumber);
		engine.stagingRing.beginFrame(frameIndex);
		engine.imgui->beginFrame();
		engine.drawUi();
		engine.imgui->endFrame();
		const auto seconds = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
		engine.updateFrameUniforms(frameIndex, width, height, seconds);
		engine.recordFrame(frameIndex, image, engine.uploadBudgetThisFrame);
		if (!engine.frameCtx->submitAndPresent(*engine.swapchain, image))
			throw std::runtime_error("native smoke could not present the frame");
		++engine.frameNumber;
	}

	ActorWitness witness() const
	{
		const auto &remote = *engine.m_remote;
		const auto &inventory = *remote.transport.inventory();
		return {remote.welcome->worldId, remote.welcome->player.value,
			net::udp::playerFeet(*remote.localPlayer()), inventory.selected, inventory.slots};
	}

	void checkWitness(const fs::path &first, const fs::path &current, bool restoring)
	{
		const auto actor = witness();
		const auto hud = engine.playerHudView();
		bool matches = hud.hotbar.available;
		for (int slot = 0; slot < items::kHotbarSize; ++slot)
		{
			const auto &shown = hud.hotbar.slots[size_t(slot)];
			matches &= shown.count == actor.slots[size_t(slot)].count &&
				gameui::style::has(shown.state, gameui::style::State::Selected) == (slot == actor.selected);
		}
		CHECK(matches, "HUD presents this client's authoritative selection and counts");
		std::ostringstream status;
		status << std::setprecision(17) << actor.world << ' ' << actor.player << ' ' << actor.selected
			<< ' ' << actor.feet.x << ' ' << actor.feet.y << ' ' << actor.feet.z;
		for (const auto &stack : actor.slots)
			status << ' ' << static_cast<unsigned>(stack.item) << ' ' << stack.count;
		publish(current, status.str());
		if (restoring)
		{
			std::ifstream prior(first);
			ActorWitness expected;
			prior >> expected.world >> expected.player >> expected.selected >> expected.feet.x >> expected.feet.y >> expected.feet.z;
			for (auto &stack : expected.slots)
			{
				unsigned item = 0;
				prior >> item >> stack.count;
				stack.item = static_cast<items::ItemId>(item);
			}
			CHECK(prior && actor.world == expected.world && actor.player == expected.player &&
				actor.selected == expected.selected && actor.slots == expected.slots &&
				glm::length(actor.feet - expected.feet) < 0.75, "fresh graphical process restores actor, pose and private inventory");
		}
		const auto metrics = frame().networkStats;
		if (!metrics.ping || !metrics.snapshot || !metrics.traffic || !metrics.prediction)
			throw std::runtime_error("live panel diagnostics expired before evidence capture");
		std::cout << "HOSTED actor=" << actor.player << " world=" << actor.world << " slot=" << actor.selected
			<< " feet=" << actor.feet.x << ',' << actor.feet.y << ',' << actor.feet.z
			<< " rtt_ms=" << metrics.ping->roundTrip.count() * 1000.0
			<< " snapshot_ms=" << metrics.snapshot->age.count() * 1000.0
			<< " rx_kib_s=" << metrics.traffic->receiveKiB << " tx_kib_s=" << metrics.traffic->sendKiB
			<< " pending=" << metrics.prediction->pendingInputs << " corrections=" << metrics.prediction->corrections
			<< " hard_resets=" << metrics.prediction->hardResets << std::endl;
	}

	bool seesPeer() const
	{
		const auto &remote = *engine.m_remote;
		const auto &sample = remote.transport.playerSnapshots().current;
		return remote.ready() && sample && sample->count == 2;
	}

	void walk(glm::dvec2 direction)
	{
		const auto before = witness().feet;
		until([&] {
			engine.sendRemoteInput({.move = direction});
			const auto delta = witness().feet - before;
			return glm::dot(glm::dvec2(delta.x, delta.z), direction) > 1.25;
		}, "player did not walk in the authoritative snapshot");
		engine.sendRemoteInput({});
		until([&] {
			const auto &velocity = engine.m_remote->localPlayer()->velocity;
			return std::hypot(velocity[0], velocity[2]) < 0.1;
		}, "player did not settle after walking");
		CHECK(true, "player moved and settled in authoritative snapshots");
	}

	std::optional<TextureType> materialAt(glm::ivec3 target) const
	{
		TextureType material{};
		const WorldCollisionView view(*engine.viewWorld());
		return view.sampleWithType(target, &material).available ? std::optional{material} : std::nullopt;
	}

	glm::ivec3 mineNearSpawn()
	{
		engine.camera.setYawPitch(0.f, -89.f);
		const auto mineable = [&](const auto &hit) {
			const auto material = hit.hit ? materialAt(hit.block) : std::nullopt;
			return material && !items::dropForBlock(*material).isEmpty();
		};
		// Seed 42's spawn is in a shallow channel. Swim toward the bank
		// for an unobstructed shot at the ground. Water is unbreakable.
		until([&] {
			if (mineable(engine.remoteTarget()))
				return true;
			engine.sendRemoteInput({.move = {1, 0}, .swimUp = true});
			return false;
		}, "owner did not reach mineable ground");
		engine.sendRemoteInput({});
		until([&] {
			const auto &actor = *engine.m_remote->localPlayer();
			return (actor.flags & net::wire::PlayerState::grounded) && std::hypot(actor.velocity[0], actor.velocity[2]) < 0.1;
		}, "owner did not settle near the bank");
		const auto hit = engine.remoteTarget();
		if (!mineable(hit))
			throw std::runtime_error("no mineable block after settling");
		const auto inventoryBefore = witness().slots;
		engine.sendRemoteVoxelAction(false);
		CHECK(engine.playerHudView().activity == "Action pending" && witness().slots == inventoryBefore,
			"submitted remote break exposes pending feedback without a predicted stack");
		until([&] { return !engine.m_remote->actionPending; }, "owner break was not acknowledged");
		const auto *feedback = engine.m_remote->feedback;
		if (!feedback || std::string_view(feedback) != "Action accepted")
			throw std::runtime_error(std::string("owner break refused: ") + (feedback ? feedback : "no action feedback"));
		until([&] { return materialAt(hit.block) == TextureType::AIR; }, "owner break did not publish through the graphical client");
		until([&] {
			return witness().slots != inventoryBefore &&
				(engine.m_remote->localPlayer()->flags & net::wire::PlayerState::grounded);
		}, "owner did not pick up the mined drop and settle");
		return hit.block;
	}

	void runHostedClosure(const fs::path &server, const fs::path &evidence, bool restoring)
	{
		const bool host = !server.empty();
		const auto round = evidence / (restoring ? "restart" : "first");
		const auto actorFile = host ? "owner-actor.txt" : "guest-actor.txt";
		const char *profile = host ? "closure-owner" : "closure-guest";
		engine.initializeNoiseGenerator(host ? 13 : 19);
		engine.enableGameUi(false, false);
		auto *const solo = engine.sim.get();
		engine.gameUi->showHostPanel();
		if (!restoring)
			frame().createJoinProfile(profile);

		uint64_t checkpoint = 0;
		if (host)
		{
			engine.m_hostExecutable = server;
			engine.m_hostSavesRoot = engine.m_savesRoot / "dedicated";
			hosting::Options options;
			options.world = "Closure loopback";
			options.port = 0;
			options.playerSlots = 2;
			options.interestRadius = 1;
			options.workers = 1;
			options.streaming = hosting::StreamingPreset::Low;
			frame().startHostedServer(options, true, profile);
			until([&] {
				const auto &state = engine.m_hostSnapshot;
				if (state.state == hosting::State::Failed)
					throw std::runtime_error(state.message);
				return state.state == hosting::State::Ready && state.administration &&
					engine.m_remote && engine.m_remote->ready();
			}, "host did not become ready and auto-join");
			const auto &state = engine.m_hostSnapshot;
			checkpoint = state.checkpoint;
			const auto &admin = *state.administration;
			const auto owner = std::ranges::find_if(admin.players, [](const auto &p) { return p.role == Role::Owner; });
			CHECK(admin.role == Role::Owner && owner != admin.players.end() &&
				owner->id == engine.m_remote->welcome->player && engine.m_remote->namedProfile == profile,
				"verified durable owner matches the automatically joined host before guest connection");
			if (restoring)
			{
				uint64_t priorCheckpoint = 0;
				std::ifstream(evidence / "first" / "checkpoint.txt") >> priorCheckpoint;
				CHECK(priorCheckpoint && checkpoint == priorCheckpoint, "restart opens the checkpoint confirmed by the UI");
			}
			const auto endpoint = state.address + ":" + std::to_string(state.port);
			std::cout << "HOSTED ready endpoint=" << endpoint << " checkpoint=" << checkpoint << " role=Owner" << std::endl;
			publish(round / "endpoint.txt", endpoint);
		}
		else
		{
			until([&] { return fs::exists(round / "endpoint.txt"); }, "graphical host did not publish its endpoint");
			std::string endpoint;
			std::ifstream(round / "endpoint.txt") >> endpoint;
			join(endpoint.c_str(), profile, "42");
		}
		until([&] { return seesPeer(); }, "second player did not appear in the authoritative snapshot");
		CHECK(engine.m_remote->namedProfile == profile && engine.m_suspendedSolo,
			"each graphical instance uses its own profile and suspends solo");
		if (!restoring)
		{
			const int slot = host ? 3 : 6;
			engine.selectHotbarSlot(slot);
			until([&] { return engine.selectedHotbarSlot() == slot; }, "private hotbar selection was not acknowledged");
			walk(host ? glm::dvec2{1, 0} : glm::dvec2{-1, 0});
		}
		until([&] {
			const auto ui = frame();
			return ui.networkStats.ping && ui.networkStats.snapshot && ui.networkStats.traffic && ui.networkStats.prediction;
		}, "client panel did not publish its own ping, snapshot, traffic and prediction");
		for (int i = 0; i < 3; ++i)
			presentClosureFrame();

		const auto edit = evidence / "first" / "edit.txt";
		if (host && !restoring)
		{
			const auto cell = mineNearSpawn();
			publish(edit, std::to_string(cell.x) + " " + std::to_string(cell.y) + " " + std::to_string(cell.z));
			std::cout << "HOSTED edit=break pickup=1 cell=" << cell.x << ',' << cell.y << ',' << cell.z << std::endl;
		}
		else
		{
			until([&] { return fs::exists(edit); }, "owner did not publish the edited cell");
			glm::ivec3 cell{};
			if (!(std::ifstream(edit) >> cell.x >> cell.y >> cell.z))
				throw std::runtime_error("invalid hosted edit witness");
			until([&] { return materialAt(cell) == TextureType::AIR; }, "both clients must see the edited block after replication and restart");
			CHECK(true, "client observes the shared world edit");
		}
		checkWitness(evidence / "first" / actorFile, round / actorFile, restoring);

		if (host)
		{
			until([&] { return fs::exists(round / "guest-actor.txt"); }, "guest did not record its private state");
			uint64_t guestWorld = 0;
			uint32_t guestId = 0;
			int guestSlot = 0;
			std::ifstream(round / "guest-actor.txt") >> guestWorld >> guestId >> guestSlot;
			CHECK(guestWorld == witness().world && guestId && guestId != witness().player &&
				guestSlot == 6 && witness().selected == 3, "host and guest keep independent identities and hotbars");
			const auto submit = [&](Operation operation, sim::PlayerId target = {}) {
				const auto &state = engine.m_hostSnapshot;
				const auto error = frame().administerHostedServer(state.launch, operation, target, "");
				if (!error.empty())
					throw std::runtime_error(error);
			};
			const auto &admin = *engine.m_hostSnapshot.administration;
			if (!restoring)
			{
				submit(Operation::GrantAdmin, {guestId});
				until([&] {
					const auto &state = engine.m_hostSnapshot;
					return state.adminOperation == Operation::GrantAdmin && state.adminResult == Result::Applied;
				}, "owner grant did not reach the simulation");
			}
			else
				CHECK(std::ranges::any_of(admin.players, [&](const auto &p) {
					return p.id.value == guestId && p.role == Role::Admin && !p.localAdmin;
				}), "delegated role survives a graphical restart without an active local credential");
			until([&] {
				const auto &status = engine.m_hostSnapshot.status;
				return status && status->transport.count == 2;
			}, "host did not publish both admitted sessions");
			const auto monitor = engine.m_hostSnapshot.status;
			CHECK(monitor && monitor->transport.count == 2 && monitor->ticks > 0 && monitor->transport.receivedBytes > 0,
				"the host sees real capacity, roster, ticks and UDP traffic");
			if (monitor)
				std::cout << "HOSTED roster=" << monitor->transport.count << " capacity=" << monitor->transport.capacity
					<< " ticks=" << monitor->ticks << " rx_bytes=" << monitor->transport.receivedBytes << std::endl;
			publish(round / "role.txt", "granted");
			until([&] { return fs::exists(round / "guest-verified.txt"); }, "guest did not verify its available controls");
			submit(Operation::Stop);
			until([&] { return !engine.m_hostSnapshot.active(); }, "owner stop did not reach a terminal state");
			const auto &stopped = engine.m_hostSnapshot;
			CHECK(stopped.state == hosting::State::Stopped && stopped.checkpoint > checkpoint && !stopped.status,
				"owner stop waits for a committed checkpoint and retires the roster");
			for (const auto &line : stopped.logs)
				if (line.starts_with("SERVER status=stopped "))
					std::cout << line << '\n';
			publish(round / "checkpoint.txt", std::to_string(stopped.checkpoint));
		}
		else
		{
			until([&] { return fs::exists(round / "role.txt"); }, "owner did not publish the grant");
			const auto ui = frame();
			const auto error = ui.administerHostedServer(1, Operation::Stop, {}, "");
			CHECK(ui.hostedServer && !ui.hostedServer->active() && !ui.hostedServer->administration && !error.empty(),
				"forging a graphical callback cannot give a remote delegate local stop access");
			publish(round / "guest-verified.txt", "no local controls");
		}
		until([&] {
			const auto ui = frame();
			return ui.multiplayerStatus && ui.joinServer && !engine.m_remote->scope;
		}, "client never reported server departure and offered retry");
		checkRetiredProjection();
		frame().returnToSolo();
		pump();
		CHECK(!engine.m_remote && engine.sim.get() == solo && engine.viewWorld() == solo->world(),
			"client returns to the existing solo world after hosted shutdown");
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
		// Readiness transition (issue #303 F07): until the session is confirmed the
		// HUD shows pending synchronization and neither a bar nor a reticle, so no
		// stale or invented inventory is ever presented. One check per join keeps
		// the assertion count independent of how many frames the join takes.
		bool sampled = false, pendingOk = true;
		until([&] {
			const bool joined = !engine.switchingSession() && engine.m_remote &&
				engine.m_remote->endpoint == endpoint && engine.m_remote->namedProfile == profile;
			const bool ready = joined && engine.m_remote->ready();
			if (joined && !ready && !sampled)
			{
				const auto hud = engine.playerHudView();
				pendingOk = !hud.hotbar.available && !hud.aim && !hud.status.empty();
				sampled = true;
			}
			return ready;
		}, "remote projection did not become ready");
		CHECK(pendingOk,
			"an unready remote session presents synchronization instead of an inventory");
		const auto readyHud = engine.playerHudView();
		CHECK(readyHud.hotbar.available && readyHud.status.empty(),
			"the ready session presents the confirmed inventory and drops the session copy");
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
		const auto hud = engine.playerHudView();
		CHECK(!hud.hotbar.available && !hud.aim && !hud.status.empty(),
			"leaving or switching replaces the old HUD with session feedback");
		CHECK(!hud.recovery.empty() || hud.statusKind == gameui::style::Status::Pending,
			"a session that ended keeps an actionable recovery line on the HUD");
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
		const auto pendingHud = engine.playerHudView();
		CHECK(pendingHud.activity == "Action pending" &&
			gameui::style::has(pendingHud.hotbar.slots[0].state, gameui::style::State::Selected),
			"HUD keeps the confirmed selection while the requested slot is pending");
		until([&] { return engine.selectedHotbarSlot() == 4; }, "server did not apply hotbar selection");
		CHECK(gameui::style::has(engine.playerHudView().hotbar.slots[4].state, gameui::style::State::Selected),
			"HUD highlights the successor only when the server publishes it");
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

int runEngineHostingSmoke(Engine &engine, const std::filesystem::path &server)
{
	EngineJoinProbe{engine}.runHosting(server);
	std::cout << "Native hosting smoke: " << testmain::checks << " checks\n";
	return testmain::exitCode();
}

int runEngineHostedClosure(Engine &engine, const fs::path &server, const fs::path &evidence, bool restoring)
{
	EngineJoinProbe{engine}.runHostedClosure(server, evidence, restoring);
	std::cout << "Native hosted closure: " << testmain::checks << " checks\n";
	return testmain::exitCode();
}
