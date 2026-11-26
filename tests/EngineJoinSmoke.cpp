#include <Engine/EngineMultiplayer.hpp>
#include <Engine/GameUiSample.hpp>
#include <Hosting/RoleCredentials.hpp>
#include <Items/ItemRegistry.hpp>
#include <World/WorldCollisionView.hpp>

#include "TestMain.hpp"

#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <span>
#include <sstream>
#include <stdexcept>
#include <vector>

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
			// The closure runs two live windows side by side on a real
			// desktop; whichever one the OS foregrounds must not gate the
			// other's synthetic input. The session-lived window is held
			// "focused" for the routing policy while it is pumping.
			engine.windowFocused = true;
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
		// The closure spends minutes inside until() pumps; keep presenting
		// through the real swapchain so the client window stays live (an
		// unpumped WSI reads as frozen and its close/interaction exits the
		// phase prematurely). Throttled to display cadence and guarded:
		// before the join lands there is no remote projection to present.
		if (engine.m_remote && Clock::now() - m_lastLivePresent >= std::chrono::milliseconds(16))
		{
			m_lastLivePresent = Clock::now();
			try
			{
				presentClosureFrame();
			}
			catch (const std::exception &)
			{
			}
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
		if (engine.m_remote->projection)
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
		// The HUD derivation is eventually consistent: while the owner keeps
		// the shared projection churning through resyncs, the confirmed
		// snapshot can trail the authoritative player for a few frames. Poll
		// for convergence instead of sampling a single instant.
		until([&] {
			const auto hud = engine.playerHudView();
			if (!hud.hotbar.available)
				return false;
			for (int slot = 0; slot < items::kHotbarSize; ++slot)
			{
				const auto &shown = hud.hotbar.slots[size_t(slot)];
				if (shown.count != actor.slots[size_t(slot)].count ||
					gameui::style::has(shown.state, gameui::style::State::Selected) != (slot == actor.selected))
					return false;
			}
			return true;
		}, "HUD presents this client's authoritative selection and counts");
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

	void walk(glm::dvec2 direction, bool jump = false)
	{
		const auto before = witness().feet;
		until([&] {
			engine.sendRemoteInput({.move = direction, .swimUp = jump});
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

	bool mineableAim(const sim::VoxelTargetTrace &hit) const
	{
		const auto material = hit.hit ? materialAt(hit.block) : std::nullopt;
		return material && !items::dropForBlock(*material).isEmpty();
	}

	/// One acknowledged break through the ordinary remote action path.
	void breakRemoteAction(const char *what)
	{
		const auto hit = engine.remoteTarget();
		if (!mineableAim(hit))
			throw std::runtime_error(std::string(what) + ": no mineable target");
		const auto inventoryBefore = witness().slots;
		engine.sendRemoteVoxelAction(false);
		CHECK(engine.playerHudView().activity == "Action pending" && witness().slots == inventoryBefore,
			"submitted remote break exposes pending feedback without a predicted stack");
		until([&] { return !engine.m_remote->actionPending; }, what);
		const auto *feedback = engine.m_remote->feedback;
		if (!feedback || std::string_view(feedback) != "Action accepted")
			throw std::runtime_error(std::string(what) + ": owner break refused: " +
				(feedback ? feedback : "no action feedback"));
		until([&] { return materialAt(hit.block) == TextureType::AIR; },
			"owner break did not publish through the graphical client");
	}

	/// F02 (#316): the dedicated server has no item-grant admin seam, so the
	/// matrix mines its budget with ordinary break actions. The player swims
	/// to the bank like the single-block flow, then sweeps the bank floor
	/// one station at a time: each station breaks every intact mineable
	/// floor cell that rings the support under its feet and lies inside the
	/// shared 1.5-block auto-pickup radius of the standing player, and the
	/// miner walk-hops two cells along a serpentine row to the next one.
	/// The support cell itself is never broken, so the sweep needs no
	/// descent — the authoritative body parks on the bank floor and every
	/// broken cell stays within its pickup radius. The sweep stops once the
	/// confirmed totals hold `goal` units of one kind — the exact-1
	/// increments pin the capped pile plus its 1-unit remainder — while the
	/// bank topsoil always yields a second material for the swap row along
	/// the way.
	glm::ivec3 mineForMatrix(uint32_t goal, const std::function<void(const glm::ivec3 &)> &announce = {})
	{
		engine.camera.setYawPitch(0.f, -35.f);
		until([&] {
			if (mineableAim(engine.remoteTarget()))
				return true;
			engine.sendRemoteInput({.move = {1, 0}, .swimUp = true});
			return false;
		}, "matrix mining did not reach the bank topsoil");
		engine.sendRemoteInput({});
		until([&] {
			const auto &actor = *engine.m_remote->localPlayer();
			return (actor.flags & net::wire::PlayerState::grounded) &&
				std::hypot(actor.velocity[0], actor.velocity[2]) < 0.1;
		}, "matrix miner did not settle at the bank");
		glm::ivec3 firstCell{};
		bool done = false;
		std::vector<glm::ivec3> deadCells;
		const auto kindTotal = [&](items::ItemId item) {
			uint32_t total = 0;
			for (const auto &stack : engine.m_remote->transport.inventory()->slots)
				if (stack.item == item)
					total += stack.count;
			return total;
		};
		const auto totalUnits = [&] {
			uint32_t total = 0;
			for (const auto &stack : engine.m_remote->transport.inventory()->slots)
				total += stack.count;
			return total;
		};
		// Neighborhood shaft (PR #316 F02): the miner breaks the 3x3 cells
		// around its feet at each level — every one of them lies inside the
		// shared 1.5-block auto-pickup radius — takes the cell below last and
		// falls one level. ~8-14 levels assemble goal units above bedrock
		// whatever the spawn's column depth; the topsoil levels supply the
		// second material the swap row needs.
		const auto feetOf = [&] {
			const auto &f = engine.m_remote->localPlayer()->feet;
			return glm::ivec3(int(std::floor(f[0])), int(std::floor(f[1] + 0.05)),
				int(std::floor(f[2])));
		};
		const auto withinPickup = [&](const glm::ivec3 &cell) {
			const auto &f = engine.m_remote->localPlayer()->feet;
			const glm::dvec3 center = glm::dvec3{cell} + glm::dvec3{0.5, 0.5, 0.5};
			return glm::length(center - glm::dvec3{f[0], f[1], f[2]}) <= 1.45;
		};
		const auto skip = [&](const glm::ivec3 &, char) {
		};
		const auto mineCell = [&](const glm::ivec3 &target) {
			if (done)
				return;
			if (!withinPickup(target))
				return skip(target, 'w');
			if (std::find(deadCells.begin(), deadCells.end(), target) != deadCells.end())
				return skip(target, 'd');
			const auto material = materialAt(target);
			if (!material || *material == TextureType::AIR ||
				items::dropForBlock(*material).isEmpty())
				return skip(target, 'm');
			// Aim at the cell centre; the ray may first clip a neighbouring
			// cell, which is broken instead only while it stays inside the
			// shared pickup radius.
			const glm::dvec3 eye = glm::dvec3{engine.camera.getPosition()};
			const glm::dvec3 d = glm::dvec3{target} + glm::dvec3{0.5, 0.5, 0.5} - eye;
			const float horizontal = float(std::hypot(d.x, d.z));
			engine.camera.setYawPitch(float(glm::degrees(std::atan2(d.z, d.x))),
				float(glm::degrees(std::atan2(d.y, horizontal))));
			auto hit = engine.remoteTarget();
			const glm::ivec3 mine = (hit.hit && withinPickup(hit.block) &&
				(hit.block == target || mineableAim(hit))) ? hit.block : target;
			if (firstCell == glm::ivec3{})
				firstCell = mine;
			const auto inventoryBefore = witness().slots;
			engine.sendRemoteVoxelAction(false);
			CHECK(engine.playerHudView().activity == "Action pending" && witness().slots == inventoryBefore,
				"submitted remote break exposes pending feedback without a predicted stack");
			until([&] { return !engine.m_remote->actionPending; },
				"matrix break was not acknowledged");
			const auto *feedback = engine.m_remote->feedback;
			// The authoritative trace can disagree with the client view on
			// fluid/deco cells; a refused cell is written off and the rest
			// of the neighbourhood keeps the budget flowing.
			if (!feedback || std::string_view(feedback) != "Action accepted")
			{
				deadCells.push_back(target);
				return;
			}
			until([&] { return materialAt(mine) == TextureType::AIR; },
				"owner break did not publish through the graphical client");
			if (announce)
				announce(firstCell);
			// Resync, pickup and settle serialize before the next break. The
			// pickup wait is bounded and not fatal: a drop can fall through
			// a previously mined cell of the sweep and land outside the
			// shared pickup radius; the budget just keeps mining.
			until([&] { return engine.m_remote->ready(); },
				"player terrain did not resynchronize after a matrix break",
				std::chrono::seconds(90));
			{
				const auto before = totalUnits();
				const auto pickupDeadline = Clock::now() + std::chrono::seconds(8);
				while (totalUnits() <= before && Clock::now() < pickupDeadline)
				{
					pump();
					std::this_thread::sleep_for(4ms);
				}
			}
			until([&] {
				const auto &actor = *engine.m_remote->localPlayer();
				return (actor.flags & net::wire::PlayerState::grounded) &&
					std::hypot(actor.velocity[0], actor.velocity[2]) < 0.1;
			}, "matrix miner did not settle");
			uint32_t largest = 0;
			for (const auto &stack : engine.m_remote->transport.inventory()->slots)
			{
				if (stack.isEmpty())
					continue;
				const auto total = kindTotal(stack.item);
				if (total > largest)
					largest = total;
			}
			// Every accepted break adds exactly one unit, so the largest
			// kind crosses the goal without overshooting it and the matrix
			// always finds its capped pile plus 1-unit remainder. Lost drops
			// (see the pickup note above) only postpone the crossing.
			if (largest >= goal)
				done = true;
		};
		// Neighborhood sweep (PR #316 F02): the miner breaks the floor cells
		// ringing its support — every one of them lies inside the shared
		// 1.5-block auto-pickup radius — then walk-hops two cells along a
		// serpentine row. ~18 stations assemble the goal units above the
		// bank floor; the floor topsoil supplies the second material the
		// swap row needs.
		static constexpr glm::ivec3 ring[] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1},
			{1, 0, 1}, {1, 0, -1}, {-1, 0, 1}, {-1, 0, -1}};
		bool eastward = true;
		for (int station = 0; !done && station < 60; ++station)
		{
			const auto base = feetOf() + glm::ivec3{0, -1, 0};
			for (const auto &side : ring)
				mineCell(base + side);
			if (done)
				break;
			bool advanced = false;
			try
			{
				// Jump-walk: the sweep drifts down into its own mined
				// trenches, so hops clear the one-block steps a plain walk
				// would grind against.
				walk(eastward ? glm::dvec2{2, 0} : glm::dvec2{-2, 0}, true);
				advanced = true;
			}
			catch (const std::exception &)
			{
			}
			if (!advanced)
			{
				eastward = !eastward;
				try
				{
					walk({0, 2}, true);
					advanced = true;
				}
				catch (const std::exception &)
				{
				}
			}
			// A walk hop keeps the same floor layer: the support under the
			// new feet has not been broken yet (the sweep rings the support,
			// never breaks it), so the mining plane only shifts sideway.
		}
		if (!done)
		{
			std::string dump;
			for (const auto &stack : engine.m_remote->transport.inventory()->slots)
				if (!stack.isEmpty())
					dump += " " + std::to_string(int(stack.item)) + "x" + std::to_string(stack.count);
			throw std::runtime_error("matrix could not assemble its " +
				std::to_string(goal) + "-unit budget;confirmed kinds:" +
				(dump.empty() ? " none" : dump));
		}
		return firstCell;
	}

	/// F02 (#316): the full inventory operation matrix driven through the native
	/// screen on the two-client hosted server. Every op passes the one planner
	/// path to a single wire action, checks the authoritative totals and — for
	/// drops — the world items they create. Mined budget: one capped 64-unit
	/// pile plus a 1-unit remainder of the shaft's stone (65 = a partial merge
	/// needs a source/destination pair above the 64 cap) and topsoil junk for
	/// the swap.
	void hostedInventoryMatrix()
	{
		const auto workspace = engine.developerWorkspaceVisible();
		engine.setDeveloperWorkspaceVisible(false);
		focus();
		// Open/close through the same toggle the E shortcut calls: the
		// windowed closure's imgui event handoff can hold stale pending
		// focus the routing policy correctly keeps for the developer UI,
		// and F01's toggle routing is covered by the solo proof and the
		// routing unit tests.
		engine.setInventoryOpen(true);
		until([&] { return engine.inventoryPanelOpen(); },
			"hosted inventory matrix could not open the native screen",
			std::chrono::seconds(5));
		// A terrain resync transient left over from the sweep disables
		// submissions; the gestures only run once the view is ready.
		until([&] {
			const auto view = engine.playerInventoryView();
			return view.enabled() && view.snapshot.has_value();
		}, "hosted inventory did not become ready for native gestures");
		int width = 0, height = 0;
		SDL_GetWindowSize(engine.window, &width, &height);
		const auto layout = gameui::inventoryLayout({{0, 0, float(width), float(height)}},
			gameui::style::baseline(), engine.m_playerUi->playerUiScale());
		const auto fire = [&](glm::vec2 point, Uint8 button) {
			// pump() holds the window "focused" only while it pumps; the
			// operator shares this desktop, so a real focus change or a
			// minimization arriving between pumps must not disable the
			// synthetic gestures (the routing policy evaluates
			// InputContext per event batch).
			engine.windowFocused = true;
			engine.m_windowHidden = false;
			engine.m_windowMinimized = false;
			for (const auto type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP})
			{
				SDL_Event event{};
				event.type = type;
				event.button.windowID = SDL_GetWindowID(engine.window);
				event.button.button = button;
				event.button.x = point.x;
				event.button.y = point.y;
				SDL_PushEvent(&event);
			}
			engine.handleEvents();
		};
		const auto click = [&](int slot, Uint8 button) {
			const auto rect = layout.slot({slot});
			fire(glm::vec2(rect.rect) + gameui::size(rect) * 0.5f, button);
		};
		const auto slots = [&] { return engine.m_remote->transport.inventory()->slots; };
		const auto at = [&](int slot) { return slots()[size_t(slot)]; };
		const auto itemTotal = [&](items::ItemId item) {
			uint32_t total = 0;
			for (const auto &stack : slots())
				if (stack.item == item)
					total += stack.count;
			return total;
		};
		const auto pileOf = [&](items::ItemId item, uint32_t count) {
			const auto &current = slots();
			for (int s = 0; s < items::kInventorySize; ++s)
				if (current[size_t(s)].item == item && !current[size_t(s)].isEmpty() &&
					current[size_t(s)].count == count)
					return s;
			return -1;
		};
		const auto emptyMainSlot = [&]() -> int {
			const auto &current = slots();
			for (int s = items::kInventorySize - 1; s >= int(items::kHotbarSize); --s)
				if (current[size_t(s)].isEmpty())
					return s;
			throw std::runtime_error("matrix ran out of empty main slots");
		};
		const auto settle = [&](std::string_view feedback, const char *what) {
			until([&] { return !engine.m_remote->actionPending; }, what);
			const auto *published = engine.m_remote->feedback;
			if (!published || std::string_view(published) != feedback)
				throw std::runtime_error(std::string(what) + ": feedback '" +
					(published ? published : "none") + "' expected '" + std::string(feedback) + "'");
			CHECK(!engine.m_remote->actionPending && !engine.m_remote->pendingInventoryAction,
				"the acknowledged action clears its pending slots");
		};
		const auto worldStacks = [&] {
			std::vector<items::ItemStack> stacks;
			if (const auto &snapshot = engine.m_remote->transport.itemSnapshots().current)
				for (const auto &item : std::span(snapshot->items).first(snapshot->count))
					stacks.push_back(item.stack);
			return stacks;
		};

		// The mined budget is whatever the sweep yielded: exactly one stackable
		// kind at 65 units (a capped 64 pile plus a 1-unit remainder — a
		// partial merge needs a pair above the 64 cap) plus second-material
		// topsoil junk for the swap row.
		items::ItemId cob = items::ItemId::None;
		for (const auto &stack : slots())
		{
			if (stack.isEmpty())
				continue;
			if (itemTotal(stack.item) == 65)
				cob = stack.item;
		}
		if (cob == items::ItemId::None)
			throw std::runtime_error("matrix budget misses its 65-unit material pile");

		// M1 — whole move: the 1-unit remainder pile changes home entirely; a
		// second completed gesture fired while the first awaits the
		// authoritative update is refused by the single-action-in-flight
		// policy and never runs (duplicate input row).
		{
			const auto source = pileOf(cob, 1);
			if (source < 0)
				throw std::runtime_error("matrix budget misses the 1-unit cobblestone pile");
			const auto before = slots();
			const int dest = emptyMainSlot();
			click(source, SDL_BUTTON_LEFT);
			click(dest, SDL_BUTTON_LEFT);
			const auto held = engine.m_remote->pendingInventoryAction;
			if (!held)
				throw std::runtime_error("the whole move published no pending action (F04)");
			CHECK(!engine.submitRemoteInventoryAction({inventoryui::Command::Move,
				inventoryui::SlotId{source}, inventoryui::SlotId{dest}}),
				"a duplicate inventory action is refused while one action is in flight (F04)");
			const auto rival = dest - 1;
			if (rival < int(items::kHotbarSize) || !at(rival).isEmpty())
				throw std::runtime_error("matrix duplicate row has no adjacent empty main slot");
			const auto donor = pileOf(cob, 64);
			click(donor, SDL_BUTTON_LEFT);
			click(rival, SDL_BUTTON_LEFT);
			settle("Action accepted", "the whole move was not acknowledged");
			const auto &pending = engine.m_remote->pendingInventoryAction;
			const bool kept = pending && pending->command == held->command &&
				pending->from == held->from && pending->to == held->to;
			CHECK(kept && at(rival).isEmpty() && at(dest) == before[size_t(source)] &&
				at(source).isEmpty() && itemTotal(cob) == 65,
				"the duplicate runs no command and one transfer executes exactly once (F04)");
		}
		// M2 — split chain: the capped 64 pile halves five times into
		// 32/16/8/4/2/2 sub-piles (the moved half is the larger).
		{
			if (pileOf(cob, 64) < 0)
				throw std::runtime_error("matrix budget misses the capped 64 pile");
			for (const uint32_t size : {64u, 32u, 16u, 8u, 4u})
			{
				const auto source = pileOf(cob, size);
				const int dest = emptyMainSlot();
				click(source, SDL_BUTTON_RIGHT);
				click(dest, SDL_BUTTON_LEFT);
				settle("Action accepted", "the split chain was not acknowledged");
			}
			CHECK(itemTotal(cob) == 65, "the split chain conserves the stack budget");
		}
		// M3 — merge: 4 + 1 rebuilds a 5 pile (matching stacks combined).
		{
			const auto unit = pileOf(cob, 1); // M1's moved remainder pile
			const auto four = pileOf(cob, 4);
			if (unit < 0 || four < 0)
				throw std::runtime_error("matrix merge misses a donor pile");
			click(unit, SDL_BUTTON_LEFT);
			click(four, SDL_BUTTON_LEFT);
			settle("Action accepted", "the merge was not acknowledged");
			CHECK(pileOf(cob, 5) >= 0 && pileOf(cob, 1) < 0 && pileOf(cob, 4) < 0 &&
				itemTotal(cob) == 65, "matching stacks merge without loss remotely");
		}
		// M4 — odd split: 5 becomes 2 + 3 (the moved half is the larger).
		{
			const auto source = pileOf(cob, 5);
			const int dest = emptyMainSlot();
			click(source, SDL_BUTTON_RIGHT);
			click(dest, SDL_BUTTON_LEFT);
			settle("Action accepted", "the odd split was not acknowledged");
			CHECK(at(dest).count == 3 && pileOf(cob, 2) >= 0 && pileOf(cob, 5) < 0,
				"odd split transfers 3 and leaves 2 through the native remote path");
		}
		// M5 — partial merge: the destination reaches capacity, the remainder
		// stays at the source. 32 + 16 + 8 + 2 + 2 + 2 = 62 leaves room 2 while
		// the split's 3 pile can only partly fit.
		{
			const auto merge = [&](uint32_t donor, uint32_t catcher) {
				const auto from = pileOf(cob, donor);
				const auto into = pileOf(cob, catcher);
				if (from < 0 || into < 0)
					throw std::runtime_error("matrix partial build misses a donor pile");
				click(from, SDL_BUTTON_LEFT);
				click(into, SDL_BUTTON_LEFT);
				settle("Action accepted", "the partial build was not acknowledged");
			};
			merge(16, 32);
			merge(8, 48);
			merge(2, 56);
			merge(2, 58);
			merge(2, 60);
			const auto from = pileOf(cob, 3);
			const auto into = pileOf(cob, 62);
			if (from < 0 || into < 0)
				throw std::runtime_error("matrix partial merge misses a donor pile");
			click(from, SDL_BUTTON_LEFT);
			click(into, SDL_BUTTON_LEFT);
			settle("Action partially applied", "the partial merge was not acknowledged");
			const auto full = pileOf(cob, 64);
			CHECK(full >= 0 && at(full).count == 64 && pileOf(cob, 1) >= 0 && itemTotal(cob) == 65,
				"partial merge fills the destination to capacity and keeps the remainder");
		}
		// M6 — swap: the topsoil junk pile exchanges slots with a cobblestone pile.
		{
			const auto &current = slots();
			const auto junk = std::ranges::find_if(current, [&](const auto &stack) {
				return !stack.isEmpty() && stack.item != cob;
			});
			if (junk == current.end())
				throw std::runtime_error("matrix found no second material for the native swap");
			const auto junkSlot = int(junk - current.begin());
			if (junkSlot != 0)
				throw std::runtime_error("matrix expected the topsoil pile in the first slot");
			const auto cobble = pileOf(cob, 1);
			const auto before = slots();
			click(junkSlot, SDL_BUTTON_LEFT);
			click(cobble, SDL_BUTTON_LEFT);
			settle("Action accepted", "the swap was not acknowledged");
			const auto &after = slots();
			CHECK(after[size_t(junkSlot)] == before[size_t(cobble)] &&
				after[size_t(cobble)] == before[size_t(junkSlot)],
				"whole drop onto another item swaps through the native remote path");
		}
		// M7 — drop one: the selected hotbar slot loses exactly one unit and the
		// world gains one item entity; pending state populates before each ack
		// and clears with the result (F04).
		{
			CHECK(pileOf(cob, 1) == 0, "the swap seated the unit pile in the first hotbar slot");
			tap(SDLK_1);
			CHECK(engine.m_remote->pendingInventoryAction &&
				engine.m_remote->pendingInventoryAction->command == inventoryui::Command::Select,
				"hotbar selection populates pending slots before the ack (F04)");
			settle("Action accepted", "the hotbar selection was not acknowledged");
			const auto before = slots();
			const auto beforeTotal = itemTotal(cob);
			tap(SDLK_Q);
			CHECK(engine.m_remote->pendingInventoryAction &&
				engine.m_remote->pendingInventoryAction->command == inventoryui::Command::DropOne,
				"the drop populates pending slots before the ack (F04)");
			settle("Action accepted", "the one-unit drop was not acknowledged");
			const auto world = worldStacks();
			CHECK(itemTotal(cob) == beforeTotal - 1 && world.size() == 1 &&
				world.front().item == cob && world.front().count == 1,
				"drop one removes exactly one unit and creates one world item");
		}
		// The stationary player can re-pick the dropped unit once the manual
		// pickup delay expires; converge before the next row so the counts and
		// the checkpoint stay deterministic.
		until([&] { return worldStacks().empty(); }, "the one-unit drop did not return to the inventory");
		CHECK(itemTotal(cob) == 65, "the returned unit restores the budget");
		// M8 — drop stack: seat the whole 64 pile in a hotbar slot, select it
		// with its number key and drop the entire stack with the screen's
		// Drop control.
		{
			const auto capped = pileOf(cob, 64);
			if (capped < 0)
				throw std::runtime_error("matrix budget misses the capped 64 pile");
			const int seat = [ & ] {
				for (int slot = 0; slot < int(items::kHotbarSize); ++slot)
					if (at(slot).isEmpty())
						return slot;
				throw std::runtime_error("matrix drop stack found no empty hotbar slot");
			}();
			click(capped, SDL_BUTTON_LEFT);
			click(seat, SDL_BUTTON_LEFT);
			settle("Action accepted", "the drop-stack seating move was not acknowledged");
			CHECK(pileOf(cob, 64) == seat, "the capped pile was seated in an empty hotbar slot");
			tap(SDLK_1 + seat);
			settle("Action accepted", "the stack-drop selection was not acknowledged");
			const auto beforeTotal = itemTotal(cob);
			const auto dropButton = glm::vec2(layout.drop.rect) + gameui::size(layout.drop) * 0.5f;
			fire(dropButton, SDL_BUTTON_LEFT);
			settle("Action accepted", "the stack drop was not acknowledged");
			const auto world = worldStacks();
			const bool stacked = std::ranges::any_of(world, [&](const auto &stack) {
				return stack.item == cob && stack.count == 64;
			});
			CHECK(at(seat).isEmpty() && itemTotal(cob) == beforeTotal - 64 && stacked,
				"drop stack empties the selected stack and creates its world item");
		}
		// Converge again so the closure witness and the checkpoint agree.
		until([&] { return worldStacks().empty(); }, "the stack drop did not return to the inventory");
		CHECK(itemTotal(cob) == 65, "the matrix conserves the mined budget end to end");
		// M9 — rejected action: dropping from an empty selected slot leaves the
		// authoritative inventory unchanged.
		{
			tap(SDLK_1 + 2); // slot 2 stays empty: the two re-picks merged above
			settle("Action accepted", "the rejection selection was not acknowledged");
			const auto confirmed = slots();
			tap(SDLK_Q);
			until([&] { return !engine.m_remote->actionPending; }, "empty drop did not receive its result");
			const auto rejected = engine.playerInventoryView();
			CHECK(rejected.feedbackKind == gameui::style::Status::Error && rejected.enabled() &&
				rejected.snapshot && rejected.snapshot->slots == confirmed,
				"rejected remote drop preserves the confirmed snapshot");
		}
		engine.setInventoryOpen(false);
		until([&] { return !engine.inventoryPanelOpen(); },
			"remote inventory closes through the native shortcut", std::chrono::seconds(5));
		engine.setDeveloperWorkspaceVisible(workspace);
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
			// F02: drop conservation runs only on the owner, and shared
			// auto-pickup conserves totals across both private inventories
			// (1.5-block radius), so the guest moves beyond the mining
			// radius first; terrain may block a bearing, so the hops rotate.
			// The sweep heads away from the guest (the owner parks on the
			// bank at x>13), so a 5-unit separation keeps every mined cell
			// far outside the guest's shared pickup radius.
			if (!host)
			{
				const auto separation = [&] {
					const auto &sample = engine.m_remote->transport.playerSnapshots().current;
					if (!sample)
						return 0.0;
					glm::dvec3 mine{}, other{};
					for (const auto &p : std::span(sample->players).first(sample->count))
					{
						const auto position = glm::dvec3{p.feet[0], p.feet[1], p.feet[2]};
						(p.player == engine.m_remote->welcome->player ? mine : other) = position;
					}
					return glm::distance(mine, other);
				};
				for (int hop = 0; hop < 12 && separation() < 5.0; ++hop)
				{
					static constexpr glm::dvec2 dirs[] = {{-1, 0}, {0, 1}, {-1, 0}, {0, -1}};
					try
					{
						walk(dirs[hop % 4]);
					}
					catch (const std::exception &)
					{
					}
				}
				CHECK(separation() >= 5.0,
					"guest moved beyond the mining pickup radius");
				publish(round / "guest-separated.txt", "ok");
			}
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
			until([&] { return fs::exists(round / "guest-separated.txt"); },
				"guest did not move beyond the mining radius", std::chrono::seconds(120));
			// Announced right after the first break so the guest only waits
			// on world replication, not on the full budget assembly or the
			// local inventory operations.
			const auto cell = mineForMatrix(65, [&](const glm::ivec3 &c) {
				publish(edit, std::to_string(c.x) + " " + std::to_string(c.y) + " " + std::to_string(c.z));
			});
			hostedInventoryMatrix();
			std::cout << "HOSTED edit=break inventory_matrix=9ops cell=" << cell.x << ',' << cell.y << ',' << cell.z << std::endl;
		}
		else
		{
			until([&] { return fs::exists(edit); }, "owner did not publish the edited cell");
			glm::ivec3 cell{};
			if (!(std::ifstream(edit) >> cell.x >> cell.y >> cell.z))
				throw std::runtime_error("invalid hosted edit witness");
			until([&] { return materialAt(cell) == TextureType::AIR; }, "both clients must see the edited block after replication and restart");
			CHECK(true, "client observes the shared world edit");
			if (!host)
				CHECK(witness().slots[9].isEmpty(), "guest never receives the owner's private inventory move");
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
			until([&] { return fs::exists(round / "role.txt"); },
				"owner did not publish the grant", std::chrono::seconds(600));
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

	void until(const auto &done, const char *description,
		std::chrono::seconds budget = std::chrono::seconds(20))
	{
		const auto deadline = Clock::now() + budget;
		while (!done())
		{
			if (Clock::now() >= deadline)
				throw std::runtime_error(description);
			pump();
			std::this_thread::sleep_for(4ms);
		}
	}

	Clock::time_point m_lastLivePresent{};

	void join(const char *endpoint, const char *profile, const char *seed)
	{
		frame().connectServer({endpoint, profile, seed});
		pump();
		until([&] {
			const bool joined = !engine.switchingSession() && engine.m_remote &&
				engine.m_remote->endpoint == endpoint && engine.m_remote->namedProfile == profile;
			return joined && engine.m_remote->ready();
		}, "remote projection did not become ready");
		checkReadinessPresentation();
		const auto readyHud = engine.playerHudView();
		CHECK(readyHud.hotbar.available && readyHud.status.empty(),
			"the ready session presents the confirmed inventory and drops the session copy");
	}

	/// Readiness transition (issue #303 F07/F12). The join can complete inside a
	/// single pump, so instead of hoping to catch a transient unready frame the
	/// gate that ready() requires is dropped for one observation while the
	/// transport stays Connected — the exact state a slow terrain sync produces.
	void checkReadinessPresentation()
	{
		auto &remote = *engine.m_remote;
		const bool ready = remote.ready();
		remote.hasCameraPose = false;
		const bool gateObserved = !remote.ready();
		const auto synchronizing = engine.playerHudView();
		const auto synchronizingInventory = engine.playerInventoryView();
		remote.hasCameraPose = true;
		const auto restored = engine.playerHudView();
		CHECK(ready && gateObserved, "the camera-pose gate is what readiness exposes");
		CHECK(!synchronizing.hotbar.available && !synchronizing.aim &&
			synchronizing.statusKind == gameui::style::Status::Pending && !synchronizing.status.empty(),
			"an unready session presents pending synchronization, no bar and no reticle ("
				<< synchronizing.status << ')');
		CHECK(!synchronizingInventory.enabled() && !synchronizingInventory.snapshot &&
			synchronizingInventory.availability == gameui::InventoryAvailability::Resyncing,
			"synchronizing inventory disables input and withholds the previous private stacks");
		CHECK(restored.hotbar.available && restored.status.empty(),
			"restoring readiness returns the confirmed inventory and drops the session copy (status=["
				<< restored.status << "])");
		// The reticle additionally needs the host's cursor capture and window
		// focus, which the window manager owns: assert the invariant the engine is
		// responsible for here.
		CHECK(!restored.captured || restored.aim,
			"a ready session with a captured cursor aims at the world again");
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
		CHECK(hud.statusKind == gameui::style::Status::Pending ||
			hud.recovery == gameui::recoveryInstruction(engine.m_showDeveloperUi, engine.mouseCaptured),
			"an ended session publishes the reconnect steps reachable from this workspace/capture state");
	}

	/// One real SDL key event through the live router (issue #303 F12): the
	/// recovery scenario must reach the shortcut by input, not by calling the
	/// engine setter or the reconnect callback directly.
	void key(SDL_Keycode code, bool down, SDL_Keymod mods = SDL_KMOD_NONE, bool repeat = false)
	{
		SDL_Event event{};
		event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		event.key.windowID = SDL_GetWindowID(engine.window);
		event.key.key = code;
		event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
		event.key.repeat = repeat;
		event.key.mod = mods;
		SDL_PushEvent(&event);
		engine.handleEvents();
	}

	void tap(SDL_Keycode code, SDL_Keymod mods = SDL_KMOD_NONE)
	{
		key(code, true, mods);
		key(code, false, mods);
	}

	/// Real focus event: gameplay shortcuts sit behind the host's focus gate, so
	/// the scenario starts from the same state an interactive player has.
	void focus()
	{
		SDL_Event event{};
		event.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
		event.window.windowID = SDL_GetWindowID(engine.window);
		SDL_PushEvent(&event);
		engine.handleEvents();
	}

	void run(const char *first, const char *second)
	{
		engine.requestOpenWorld("solo");
		engine.initializeNoiseGenerator(13);
		engine.stepDerivedProjectionForTests(1.0 / 60.0);
		engine.enableGameUi(false, false);
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
		// Recovery through the real input path (issue #303 F03/F12). The blocked row
		// the player just read asks for the workspace chord and the mouse release;
		// both arrive here as SDL events and must reach their owner while the
		// session is blocked. The reconnect itself then invokes the same
		// GameUIFrame::joinServer callback the Developer > Multiplayer menu item
		// calls (UiShell::drawMainMenuBar), because driving the ImGui menu bar by
		// synthetic pointer positions is not reproducible here. What the input path
		// owns — the chord, the cursor release and the state the row describes — is
		// therefore validated end to end, and the callback is the documented seam.
		focus();
		engine.setDeveloperWorkspaceVisible(false);
		engine.setInventoryOpen(false);
		engine.setMouseCaptured(true);
		const bool captured = engine.mouseCaptured;
		const auto blockedHud = engine.playerHudView();
		CHECK(blockedHud.statusKind == gameui::style::Status::Error && !blockedHud.hotbar.available &&
			blockedHud.recovery == gameui::recoveryInstruction(false, captured),
			"the blocked HUD publishes the reconnect steps for the hidden workspace and the actual mouse state");
		CHECK(captured, "the recovery scenario starts from a captured mouse");
		tap(SDLK_E);
		presentClosureFrame();
		const auto disconnectedInventory = engine.playerInventoryView();
		CHECK(engine.inventoryPanelOpen() && !disconnectedInventory.enabled() && !disconnectedInventory.snapshot &&
			disconnectedInventory.availability == gameui::InventoryAvailability::Disconnected,
			"disconnected inventory shows recovery and clears the old private snapshot");
		tap(SDLK_Q);
		CHECK(!engine.m_remote->actionPending, "disconnected inventory submits no action");
		tap(SDLK_E);
		tap(SDLK_D, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
		CHECK(engine.developerWorkspaceVisible(),
			"Ctrl+Shift+D reaches the workspace while the session is blocked");
		tap(SDLK_C);
		CHECK(!engine.mouseCaptured, "C releases the captured mouse while the session is blocked");
		const auto reachableHud = engine.playerHudView();
		CHECK(reachableHud.recovery == gameui::recoveryInstruction(true, false),
			"the recovery line follows the state the player just reached");
		CHECK(static_cast<bool>(frame().joinServer),
			"the Multiplayer Reconnect action is reachable after the recovery steps");
		frame().joinServer();
		pump();
		until([&] { return engine.m_remote->ready(); }, "Reconnect did not restore the session");
		CHECK(engine.m_remote->welcome->player == aliceId && engine.selectedHotbarSlot() == 4 &&
			engine.m_remote->transport.inventory()->slots == aliceInventory,
			"recovery restores the same actor, selected slot and authoritative inventory");
		const auto reconnectedHud = engine.playerHudView();
		CHECK(reconnectedHud.hotbar.available && reconnectedHud.recovery.empty() &&
			reconnectedHud.status.empty(),
			"a reconnected session drops the recovery line and republishes the inventory");

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
