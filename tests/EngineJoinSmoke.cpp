#include <Engine/EngineMultiplayer.hpp>
#include <Engine/GameUiSample.hpp>
#include <Hosting/RoleCredentials.hpp>
#include <Items/ItemRegistry.hpp>
#include <World/MultiplayerSave.hpp>
#include <World/WorldCheckpoint.hpp>
#include <World/WorldCollisionView.hpp>

#include "TestMain.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <functional>
#include <iomanip>
#include <limits>
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
			// F09 (#316): the scenario never overrides the host's focus
			// bookkeeping. When the operator's desktop owns the focus, a
			// controlled SDL focus event re-asserts it through the real
			// input path instead of writing the flag behind SDL's back.
			if (!engine.windowFocused)
				assertWindowFocus();
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
		// phase prematurely). Throttled to display cadence, skipped while the
		// window cannot show anything (minimized/hidden) — the game loop's
		// own frame-skip policy for an unusable swapchain: F09 removed the
		// silent catch, so any unexpected rendering error now surfaces.
		if (engine.m_remote && !engine.m_windowHidden && !engine.m_windowMinimized &&
			Clock::now() - m_lastLivePresent >= std::chrono::milliseconds(16))
		{
			m_lastLivePresent = Clock::now();
			presentClosureFrame();
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

	bool presentClosureFrame()
	{
		// Render through the same swapchain, mesh upload and ImGui paths as
		// the game while the rendezvous drives the panel callbacks. A frame
		// the WSI cannot deliver (no viewport yet, or a failed acquire or
		// present — minimized, occluded, desk-op out-of-date) is a skip, not
		// an error: the game loop requests the swapchain recreation and
		// continues; F09 removed the silent catch so unexpected rendering
		// errors propagate.
		int width = 0, height = 0;
		SDL_GetWindowSizeInPixels(engine.window, &width, &height);
		if (width <= 0 || height <= 0)
			return false;
		engine.recreateSwapchainIfNeeded(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
		uint32_t image = 0;
		if (!engine.frameCtx->beginFrame(*engine.swapchain, image))
		{
			engine.requestSwapchainRecreate();
			return false;
		}
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
		{
			engine.requestSwapchainRecreate();
			return false;
		}
		++engine.frameNumber;
		return true;
	}

	ActorWitness witness() const
	{
		// F07 (#316): transport.inventory() returns the optional BY VALUE.
		// Binding a reference into the returned temporary (and range-for over
		// its slots) reads destroyed storage — the 0xC0000005 this round
		// chases. Every access takes an owned copy and reports liveness
		// failures with their own diagnostic.
		const auto &remote = *engine.m_remote;
		if (!remote.welcome)
			throw std::runtime_error("witness: the session has no welcome identity yet");
		const auto inventory = remote.transport.inventory();
		if (!inventory)
			throw std::runtime_error("witness: the authoritative inventory is unavailable (session resync or departure)");
		const auto *actor = remote.localPlayer();
		if (!actor)
			throw std::runtime_error("witness: the authoritative player is missing from the player snapshot");
		return {remote.welcome->worldId, remote.welcome->player.value,
			net::udp::playerFeet(*actor), inventory->selected, inventory->slots};
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
			const auto *actor = engine.m_remote ? engine.m_remote->localPlayer() : nullptr;
			return actor && std::hypot(actor->velocity[0], actor->velocity[2]) < 0.1;
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
	/// Returns the authoritative block that was mined.
	glm::ivec3 breakRemoteAction(const char *what)
	{
		const auto hit = engine.remoteTarget();
		if (!mineableAim(hit))
			throw std::runtime_error(std::string(what) + ": no mineable target");
		const auto mined = hit.block;
		const auto inventoryBefore = witness().slots;
		engine.sendRemoteVoxelAction(false);
		CHECK(engine.playerHudView().activity == "Action pending" && witness().slots == inventoryBefore,
			"submitted remote break exposes pending feedback without a predicted stack");
		until([&] { return !engine.m_remote->actionPending; }, what);
		const auto *feedback = engine.m_remote->feedback;
		if (!feedback || std::string_view(feedback) != "Action accepted")
			throw std::runtime_error(std::string(what) + ": owner break refused: " +
				(feedback ? feedback : "no action feedback"));
		until([&] { return materialAt(mined) == TextureType::AIR; },
			"owner break did not publish through the graphical client");
		return mined;
	}

	// -------------------------------------------------------------------------
	// Deterministic inventory fixture (PR #316 F09). The dedicated server has no
	// item-grant seam, so the known inventories are prepared through the
	// ordinary persistence path: a graceful stop commits the player roster, the
	// committed players.state is rewritten with the fixture stacks using the
	// worldsave encoders (the exact records the restart validator reads) and
	// the restart restores them as ordinary saved players. No debug messages,
	// no server-authority bypass, no mined budget.
	static constexpr char kFixtureWorld[] = "Closure loopback";
	static constexpr items::ItemId kMaterialA = items::ItemId::Cobblestone;
	static constexpr items::ItemId kMaterialB = items::ItemId::OakPlanks;
	static constexpr items::ItemId kGuestItem = items::ItemId::Dirt;

	static constexpr items::ItemStack stackOf(items::ItemId item, uint16_t count)
	{
		return {item, count};
	}

	struct FixtureInventory
	{
		std::array<items::ItemStack, items::kInventorySize> slots{};
		int selected{};
	};

	FixtureInventory ownerFixture() const
	{
		FixtureInventory fixture;
		fixture.slots[0] = stackOf(kMaterialA, 64); // merge + split row
		fixture.slots[1] = stackOf(kMaterialA, 40); // partial-merge donor
		fixture.slots[2] = stackOf(kMaterialA, 5);  // whole move + odd split
		fixture.slots[3] = stackOf(kMaterialB, 8);  // swap + drop stack
		fixture.selected = 3;
		return fixture;
	}

	FixtureInventory guestFixture() const
	{
		FixtureInventory fixture;
		fixture.slots[12] = stackOf(kGuestItem, 8); // distinct private state
		fixture.selected = 6;
		return fixture;
	}

	void rewriteCommittedRoster(const fs::path &worldDir, uint32_t ownerActorValue) const
	{
		// In checkpoint mode the restore source is the ACTIVE committed
		// generation, not the loose baseline files: rewrite the roster there
		// and keep the commit manifest's roster checksum consistent.
		const auto checkpointsDir = worldDir / "checkpoints";
		const auto manifestPath = checkpointsDir / "manifest";
		worldsave::CheckpointManifest manifest;
		const auto commitPoint = readFileBytes(manifestPath);
		CHECK(commitPoint && worldsave::decodeCheckpointManifest(commitPoint->data(),
			commitPoint->size(), manifest) == worldsave::SaveStatus::Ok &&
			manifest.committedGeneration > 0,
			"the fixture world exposes a valid committed checkpoint manifest");
		const auto path = checkpointsDir /
			("generation-" + std::to_string(manifest.committedGeneration)) / worldsave::kRosterFile;
		worldsave::PlayerRoster roster;
		CHECK(rosterExists(path) && worldsave::readPlayerRoster(path, roster) == worldsave::SaveStatus::Ok &&
			roster.players.size() == 2,
			"the committed fixture world exposes the roster of both joined actors");
		const auto ownerActor = sim::PlayerId{ownerActorValue};
		const auto owner = ownerFixture();
		const auto guest = guestFixture();
		bool ownerWritten = false, guestWritten = false;
		for (auto &player : roster.players)
		{
			const bool isOwner = player.id.value == ownerActor.value;
			const auto &source = isOwner ? owner.slots : guest.slots;
			for (size_t slot = 0; slot < source.size(); ++slot)
			{
				const auto &stack = source[slot];
				player.inventory.slots[slot] = {static_cast<uint16_t>(stack.item), stack.count};
			}
			player.inventory.selectedSlot = static_cast<uint8_t>(
				(isOwner ? owner : guest).selected);
			ownerWritten |= isOwner;
			guestWritten |= !isOwner;
		}
		CHECK(ownerWritten && guestWritten,
			"the fixture rewrite binds a distinct private inventory to each actor");
		const auto encoded = worldsave::encodePlayerRoster(roster);
		auto pending = path;
		pending += ".fixture";
		{
			std::ofstream out(pending, std::ios::binary);
			out.write(reinterpret_cast<const char *>(encoded.data()),
				static_cast<std::streamsize>(encoded.size()));
			out.close();
			if (!out)
				throw std::runtime_error("could not stage the fixture roster");
		}
		fs::rename(pending, path);

		manifest.rosterChecksum = worldsave::fnv1a64(encoded.data(), encoded.size());
		const auto manifestPatch = worldsave::encodeCheckpointManifest(manifest);
		auto manifestPending = manifestPath;
		manifestPending += ".fixture";
		{
			std::ofstream out(manifestPending, std::ios::binary);
			out.write(reinterpret_cast<const char *>(manifestPatch.data()),
				static_cast<std::streamsize>(manifestPatch.size()));
			out.close();
			if (!out)
				throw std::runtime_error("could not stage the fixture manifest");
		}
		fs::rename(manifestPending, manifestPath);
	}

	static std::optional<std::vector<uint8_t>> readFileBytes(const fs::path &path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return std::nullopt;
		std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
			std::istreambuf_iterator<char>());
		if (!in)
			return std::nullopt;
		return bytes;
	}

	static bool rosterExists(const fs::path &path)
	{
		std::error_code error;
		return fs::is_regular_file(path, error);
	}

	/// F07 (#316): every confirmed-inventory access takes an owned snapshot
	/// out of the by-value optional; a missing snapshot is a terminal failure
	/// with its own diagnostic (polls read the optional directly instead of
	/// dereferencing it).
	std::array<items::ItemStack, items::kInventorySize> confirmedSlots() const
	{
		const auto inventory = engine.m_remote->transport.inventory();
		if (!inventory)
			throw std::runtime_error("the authoritative inventory is unavailable (resync or departure)");
		return inventory->slots;
	}

	/// F09: the full deterministic inventory operation matrix, driven through
	/// the native screen on the two-client hosted server. The fixture put
	/// known piles in known slots, so every row asserts exact source and
	/// destination contents instead of scanning mined piles: whole move (+
	/// duplicate input), odd split, merge, partial merge, swap, drop one,
	/// drop stack, rejected drop, with the F04 pending feedback, identified
	/// world entities, pickup restoration and item conservation verified
	/// along the way.
	void hostedInventoryMatrix()
	{
		const auto workspace = engine.developerWorkspaceVisible();
		engine.setDeveloperWorkspaceVisible(false);
		focus();
		engine.setInventoryOpen(true);
		until([&] { return engine.inventoryPanelOpen(); },
			"hosted inventory matrix could not open the native screen",
			std::chrono::seconds(5));
		// A terrain resync transient disables submissions; the gestures only
		// run once the view is ready AND the native screen rebuilt its layout
		// during a rendered frame (the slot geometry is published by the paint
		// step, so clicking before the first paint can hit nothing).
		until([&] {
			const auto view = engine.playerInventoryView();
			return view.enabled() && view.snapshot.has_value();
		}, "hosted inventory did not become ready for native gestures",
			std::chrono::seconds(20));
		until([&] {
			const auto &built = engine.m_playerUi->m_inventoryLayout;
			const auto ready = [&](int slot) {
				const auto rect = built.slot({slot});
				return rect.rect.z - rect.rect.x > 0.5f && rect.rect.w - rect.rect.y > 0.5f;
			};
			return ready(2) && ready(10);
		}, "the native inventory did not publish its slot geometry",
			std::chrono::seconds(5));

		int width = 0, height = 0;
		SDL_GetWindowSize(engine.window, &width, &height);
		const auto layout = gameui::inventoryLayout({{0, 0, float(width), float(height)}},
			gameui::style::baseline(), engine.m_playerUi->playerUiScale());
		const auto fire = [&](glm::vec2 point, Uint8 button) {
			// F09 (#316): the harness never overrides the host's focus or
			// window state — the gesture re-asserts focus through a real SDL
			// focus event, and a hidden or minimized window is the failure
			// rather than a condition to paper over.
			if (!engine.windowFocused)
				assertWindowFocus();
			if (engine.m_windowHidden || engine.m_windowMinimized)
				throw std::runtime_error("the inventory gesture ran into a hidden or minimized window");
			// Mirror the solo proof's real-input pattern: warp the OS pointer
			// and deliver a motion event before the down, so the screen sees
			// the hover state a human click carries.
			SDL_WarpMouseInWindow(engine.window, point.x, point.y);
			SDL_Event motionEvent{};
			motionEvent.type = SDL_EVENT_MOUSE_MOTION;
			motionEvent.motion.windowID = SDL_GetWindowID(engine.window);
			motionEvent.motion.x = point.x;
			motionEvent.motion.y = point.y;
			SDL_PushEvent(&motionEvent);
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
			// The screen resolves gestures during its update/paint step, so
			// the event batch advances a rendered frame exactly like the
			// solo proof's present().
			presentClosureFrame();
		};
		const auto click = [&](int slot, Uint8 button) {
			const auto rect = layout.slot({slot});
			fire(glm::vec2(rect.rect) + gameui::size(rect) * 0.5f, button);
		};
		const auto slots = [&] { return confirmedSlots(); };
		const auto at = [&](int slot) { return slots()[size_t(slot)]; };
		// Tolerant reads for polls: a resync missing the confirmed inventory
		// keeps polling instead of crashing into destroyed storage (F07).
		const auto totalOf = [&](items::ItemId item) -> std::optional<uint32_t> {
			const auto inventory = engine.m_remote->transport.inventory();
			if (!inventory)
				return std::nullopt;
			uint32_t total = 0;
			for (const auto &stack : inventory->slots)
				if (stack.item == item)
					total += stack.count;
			return total;
		};
		const auto itemTotal = [&](items::ItemId item) { return *totalOf(item); };
		const auto settle = [&](std::string_view feedback, const char *what) {
			until([&] { return !engine.m_remote->actionPending; }, what);
			const auto *published = engine.m_remote->feedback;
			if (!published || std::string_view(published) != feedback)
				throw std::runtime_error(std::string(what) + ": feedback '" +
					(published ? published : "none") + "' expected '" + std::string(feedback) + "'");
			CHECK(!engine.m_remote->actionPending && !engine.m_remote->pendingInventoryAction,
				"the acknowledged action clears its pending slots");
		};
		// F09 §4: world entities are identified by their entity id against a
		// baseline snapshot, and consummation waits on both channels — the
		// confirmed inventory change AND the entity leaving the world —
		// independently.
		const auto itemIdSet = [&] {
			std::vector<uint64_t> ids;
			const auto &snapshot = engine.m_remote->transport.itemSnapshots().current;
			if (snapshot)
				for (const auto &item : std::span(snapshot->items).first(snapshot->count))
					ids.push_back(item.id);
			std::ranges::sort(ids);
			return ids;
		};
		const auto newWorldItems = [&](const std::vector<uint64_t> &beforeIds) {
			std::vector<std::pair<uint64_t, items::ItemStack>> spawned;
			const auto &snapshot = engine.m_remote->transport.itemSnapshots().current;
			if (snapshot)
				for (const auto &item : std::span(snapshot->items).first(snapshot->count))
					if (!std::ranges::binary_search(beforeIds, item.id))
						spawned.emplace_back(item.id, item.stack);
			return spawned;
		};
		// F09 §4: an action acknowledgment and the corresponding world item
		// snapshot do not arrive simultaneously — the identified entity is
		// awaited on its own channel before its contents are asserted.
		const auto awaitWorldItems = [&](const std::vector<uint64_t> &beforeIds, const char *what) {
			std::vector<std::pair<uint64_t, items::ItemStack>> spawned;
			until([&] {
				spawned = newWorldItems(beforeIds);
				return !spawned.empty();
			}, what, std::chrono::seconds(15));
			return spawned;
		};
		const auto itemsGone = [&](const std::vector<std::pair<uint64_t, items::ItemStack>> &tracked) {
			const auto &snapshot = engine.m_remote->transport.itemSnapshots().current;
			if (!snapshot)
				return false;
			for (const auto &[trackedId, trackedStack] : tracked)
				for (const auto &item : std::span(snapshot->items).first(snapshot->count))
					if (item.id == trackedId)
						return false;
			return true;
		};
		const auto waitPickupRestored = [&](const std::vector<std::pair<uint64_t, items::ItemStack>> &dropped,
			items::ItemId item, uint32_t during, const char *what) {
			uint32_t total = 0;
			for (const auto &entry : dropped)
				total += entry.second.count;
			// Steering state: obstacles (including the excavated edit cell)
			// block the straight walk, so stagnation rotates the bearing and
			// hops, like the disjoint mining separation hop pattern.
			glm::dvec2 lastFeet{std::numeric_limits<float>::max(),
				std::numeric_limits<float>::max()};
			int stagnant = 0;
			int attempts = 0;
			try
			{
				until([&] {
					if (const auto confirmed = totalOf(item);
						itemsGone(dropped) && confirmed && *confirmed == during + total)
					{
						engine.sendRemoteInput({});
						return true;
					}
					// Steer the actor toward the tracked drop: a thrown stack
					// can land just outside the 1.5-block shared pickup radius,
					// and auto-pickup needs the feet inside it. The wire entity
					// motion is a /64 fixed-point of the world position.
					const auto &snapshot = engine.m_remote->transport.itemSnapshots().current;
					glm::dvec2 steer{};
					if (snapshot)
					{
						const auto *actor = engine.m_remote->localPlayer();
						glm::dvec3 feet{};
						if (actor)
							feet = {actor->feet[0], actor->feet[1], actor->feet[2]};
						double best = std::numeric_limits<double>::max();
						glm::dvec3 itemPosition{};
						for (const auto &itemEntity : std::span(snapshot->items).first(snapshot->count))
						{
							if (std::ranges::find(dropped, itemEntity.id, &std::pair<uint64_t, items::ItemStack>::first) == dropped.end())
								continue;
							const glm::dvec3 position{double(itemEntity.motion.position[0]) / 64.0,
								double(itemEntity.motion.position[1]) / 64.0,
								double(itemEntity.motion.position[2]) / 64.0};
							if (actor)
							{
								if (const double distance = glm::distance(feet, position); distance < best)
								{
									best = distance;
									itemPosition = position;
								}
							}
							else if (!std::isfinite(best))
							{
								itemPosition = position;
							}
						}
						const glm::dvec2 delta{itemPosition.x - feet.x, itemPosition.z - feet.z};
						if (glm::length(delta) > 1e-6)
							steer = delta / glm::length(delta);
						// Stagnation: hop and rotate the bearing.
						const glm::dvec2 feetFlat{float(feet.x), float(feet.z)};
						if (glm::distance(feetFlat, lastFeet) < 0.15)
						{
							if (++stagnant >= 40)
							{
								const bool left = attempts % 2 == 0;
								const double turn = left ? 0.8 : -0.8;
								steer = {steer.x * std::cos(turn) - steer.y * std::sin(turn),
									steer.x * std::sin(turn) + steer.y * std::cos(turn)};
								stagnant = 0;
								++attempts;
							}
						}
						else
						{
							stagnant = 0;
							lastFeet = feetFlat;
						}
					}
					if (glm::length(steer) > 1e-6)
						engine.sendRemoteInput({.move = steer, .jumpPressed = stagnant > 20 || attempts > 8});
					return false;
				}, what, std::chrono::seconds(60));
			}
			catch (const std::exception &)
			{
				engine.sendRemoteInput({});
				const auto &snapshot = engine.m_remote->transport.itemSnapshots().current;
				std::cout << "pickup-restore diagnostics: worldCount="
					<< (snapshot ? snapshot->count : 0xffffffffu);
				if (snapshot)
					for (const auto &itemEntity : std::span(snapshot->items).first(snapshot->count))
						std::cout << " {id=" << itemEntity.id << ",item="
							<< static_cast<uint32_t>(itemEntity.stack.item)
							<< ",count=" << itemEntity.stack.count
							<< ",pos=" << itemEntity.motion.position[0] << ','
							<< itemEntity.motion.position[1] << ',' << itemEntity.motion.position[2] << "}";
				const auto *actor = engine.m_remote->localPlayer();
				if (actor)
					std::cout << " ownerFeet=" << actor->feet[0] << ',' << actor->feet[1] << ','
						<< actor->feet[2];
				const auto confirmed = totalOf(item);
				std::cout << " confirmed=" << (confirmed ? std::to_string(*confirmed) : "none")
					<< " want " << during + total << std::endl;
				std::cout.flush();
				throw;
			}
		};
		const auto kindTotal = [&](const char *what) {
			uint32_t counted = 0;
			for (const auto &stack : slots())
			{
				if (stack.item == kMaterialA)
					counted += stack.count;
				else if (stack.item == kMaterialB)
					counted += stack.count;
				else if (!stack.isEmpty())
					throw std::runtime_error(std::string(what) + ": unexpected material entered the inventory");
			}
			CHECK(counted == 117, what);
		};

		// The fixture is asserted verbatim before the first row: anything else
		// is a fixture bug, not a matrix result.
		const auto fixture = ownerFixture();
		const auto restored = slots();
		const int restoredSelected = witness().selected;
		if (restored != fixture.slots || restoredSelected != fixture.selected)
		{
			std::ostringstream diagnostic;
			diagnostic << "the fixture restart restores the owner's exact known piles and selection;"
				<< " restored selected=" << restoredSelected << " wanted " << fixture.selected;
			for (size_t slot = 0; slot < fixture.slots.size(); ++slot)
				diagnostic << " [" << slot << "] got {"
					<< static_cast<uint32_t>(restored[slot].item) << ',' << restored[slot].count
					<< "} want {" << static_cast<uint32_t>(fixture.slots[slot].item) << ','
					<< fixture.slots[slot].count << '}';
			throw std::runtime_error(diagnostic.str());
		}

		// M1 — whole move (F08: pending state validated BEFORE the ack,
		// confirmed state AFTER): a repeated submission is refused by the
		// single-action-in-flight policy, the pending metadata stays exactly
		// the armed one, and exactly one transfer executes.
		{
			constexpr int source = 2, dest = 10, rival = 11;
			const auto before = slots();
			click(source, SDL_BUTTON_LEFT);
			click(dest, SDL_BUTTON_LEFT);
			const auto held = engine.m_remote->pendingInventoryAction;
			if (!held)
			{
				const auto view = engine.playerInventoryView();
				const auto srcRect = layout.slot({source});
				const auto srcHit = layout.hitSlot(glm::vec2(srcRect.rect) + gameui::size(srcRect) * 0.5f);
				const auto *playerUi = engine.m_playerUi.get();
				std::ostringstream diagnostic;
				diagnostic << "the whole move published no pending action (F04): snapshot="
					<< view.snapshot.has_value() << " availability=" << int(view.availability)
					<< " actionPending=" << view.actionPending
					<< " pendingSlots=" << view.pendingSlots.has_value()
					<< " captured=" << engine.mouseCaptured
					<< " feedback='" << view.feedback << "'"
					<< " source-hit-slot=" << srcHit.index
					<< " screensRev=" << playerUi->screens().revision()
					<< " pressArmed=" << playerUi->hasArmedPress();
				for (const auto &log : engine.m_hostSnapshot.logs)
					diagnostic << " | " << log;
				throw std::runtime_error(diagnostic.str());
			}
			CHECK(!engine.submitRemoteInventoryAction({inventoryui::Command::Move,
				inventoryui::SlotId{source}, inventoryui::SlotId{dest}}),
				"a duplicate inventory action is refused while one action is in flight (F04)");
			const auto &pending = engine.m_remote->pendingInventoryAction;
			CHECK(pending && pending->command == held->command && pending->from == held->from &&
				pending->to == held->to,
				"the refused duplicate left the pending action metadata unchanged");
			const auto confirmedDuringPending = slots();
			if (confirmedDuringPending != before)
				throw std::runtime_error("the duplicate submission mutated the confirmed inventory before the ack");
			settle("Action accepted", "the whole move was not acknowledged");
			CHECK(!engine.m_remote->pendingInventoryAction && !engine.m_remote->actionPending,
				"the acknowledged move clears its pending metadata");
			CHECK(at(source).isEmpty() && at(dest).item == kMaterialA && at(dest).count == 5 &&
				at(dest) == before[size_t(source)] && at(rival) == before[size_t(rival)],
				"the whole move transfers the entire stack exactly once and leaves the rival slot untouched");
		}
		// M2 — odd split (exact): 5 becomes 2 + 3; the moved half is the larger.
		{
			constexpr int source = 10, dest = 12;
			const auto before = slots();
			click(source, SDL_BUTTON_RIGHT);
			click(dest, SDL_BUTTON_LEFT);
			settle("Action accepted", "the odd split was not acknowledged");
			CHECK(before[size_t(dest)].isEmpty() && at(source).item == kMaterialA &&
				at(source).count == 2 && at(dest).item == kMaterialA && at(dest).count == 3,
				"the odd split transfers 3 and leaves 2");
		}
		// M3 — merge (exact): an even half split rebuilds the capped pile.
		{
			constexpr int pile = 0, scratch = 13;
			click(pile, SDL_BUTTON_RIGHT);
			click(scratch, SDL_BUTTON_LEFT);
			settle("Action accepted", "the merge setup split was not acknowledged");
			CHECK(at(pile).item == kMaterialA && at(pile).count == 32 &&
				at(scratch).item == kMaterialA && at(scratch).count == 32,
				"the capped pile splits into two exact halves");
			click(scratch, SDL_BUTTON_LEFT);
			click(pile, SDL_BUTTON_LEFT);
			settle("Action accepted", "the merge was not acknowledged");
			CHECK(at(pile).item == kMaterialA && at(pile).count == 64 && at(scratch).isEmpty(),
				"matching stacks merge without loss remotely");
		}
		// M4 — partial merge (exact): moving 40 into a pile with 32 room fills
		// the destination to capacity and keeps the remainder at the source.
		{
			constexpr int capped = 0, donor = 1, scratch = 14;
			click(capped, SDL_BUTTON_RIGHT);
			click(scratch, SDL_BUTTON_LEFT);
			settle("Action accepted", "the partial setup split was not acknowledged");
			CHECK(at(capped).item == kMaterialA && at(capped).count == 32 &&
				at(scratch).item == kMaterialA && at(scratch).count == 32,
				"the partial merge halves its capped pile first");
			click(donor, SDL_BUTTON_LEFT);
			click(scratch, SDL_BUTTON_LEFT);
			settle("Action partially applied", "the partial merge was not acknowledged");
			CHECK(at(scratch).item == kMaterialA && at(scratch).count == 64 &&
				at(donor).item == kMaterialA && at(donor).count == 8,
				"the partial merge fills the destination to capacity and keeps the remainder");
		}
		// M5 — swap (exact): different stacks exchange slots in both directions.
		{
			constexpr int a = 1, b = 3;
			const auto before = slots();
			CHECK(before[size_t(a)].item == kMaterialA && before[size_t(b)].item == kMaterialB,
				"the swap row starts from the fixture's two distinct piles");
			click(a, SDL_BUTTON_LEFT);
			click(b, SDL_BUTTON_LEFT);
			settle("Action accepted", "the swap was not acknowledged");
			CHECK(at(a) == before[size_t(b)] && at(b) == before[size_t(a)],
				"whole drop onto another item swaps through the native remote path");
		}
		// M6 — drop one (F08): the select and the drop populate the pending
		// slots before their acks; the inventory confirmation, the identified
		// world entity and the pickup restoration are each waited on their own
		// channel. The actor looks nearly straight down so the drop lands
		// inside the shared auto-pickup radius.
		{
			constexpr int seat = 7;
			engine.camera.setYawPitch(0.f, -85.f);
			presentClosureFrame();
			click(3, SDL_BUTTON_LEFT);
			click(seat, SDL_BUTTON_LEFT);
			settle("Action accepted", "the drop-one seating move was not acknowledged");
			tap(SDLK_1 + seat);
			CHECK(engine.m_remote->pendingInventoryAction &&
				engine.m_remote->pendingInventoryAction->command == inventoryui::Command::Select,
				"hotbar selection populates pending slots before the ack (F04)");
			settle("Action accepted", "the drop-one selection was not acknowledged");
			const auto entitiesBefore = itemIdSet();
			const uint32_t beforeTotal = itemTotal(kMaterialA);
			tap(SDLK_Q);
			CHECK(engine.m_remote->pendingInventoryAction &&
				engine.m_remote->pendingInventoryAction->command == inventoryui::Command::DropOne,
				"the drop populates pending slots before the ack (F04)");
			settle("Action accepted", "the one-unit drop was not acknowledged");
			until([&] { return totalOf(kMaterialA) == beforeTotal - 1; },
				"drop one did not confirm its inventory loss");
			const auto dropped = awaitWorldItems(entitiesBefore, "drop one did not publish its world item");
			CHECK(dropped.size() == 1 && dropped.front().second.item == kMaterialA &&
				dropped.front().second.count == 1,
				"drop one removes exactly one unit and creates one identified world item");
			waitPickupRestored(dropped, kMaterialA, *totalOf(kMaterialA),
				"the one-unit drop was not collected back");
		}
		// M7 — drop stack: the seated B pile is selected by its number key and
		// leaves as one identified stack through the screen's Drop control.
		{
			constexpr int seat = 6;
			click(1, SDL_BUTTON_LEFT);
			click(seat, SDL_BUTTON_LEFT);
			settle("Action accepted", "the drop-stack seating move was not acknowledged");
			CHECK(at(seat).item == kMaterialB && at(seat).count == 8 && at(1).isEmpty(),
				"the whole B pile was seated in an empty hotbar slot");
			tap(SDLK_1 + seat);
			settle("Action accepted", "the stack-drop selection was not acknowledged");
			const auto entitiesBefore = itemIdSet();
			const auto beforeB = itemTotal(kMaterialB);
			const auto dropButton = glm::vec2(layout.drop.rect) + gameui::size(layout.drop) * 0.5f;
			fire(dropButton, SDL_BUTTON_LEFT);
			settle("Action accepted", "the stack drop was not acknowledged");
			until([&] { return totalOf(kMaterialB) == beforeB - 8; },
				"the stack drop did not confirm its inventory loss");
			const auto dropped = awaitWorldItems(entitiesBefore, "the stack drop did not publish its world item");
			CHECK(dropped.size() == 1 && dropped.front().second.item == kMaterialB &&
				dropped.front().second.count == 8,
				"the stack drop empties the selected stack and creates its world item");
			waitPickupRestored(dropped, kMaterialB, *totalOf(kMaterialB),
				"the stack drop was not collected back");
		}
		// M9 — rejected action (F08): the empty hotbar slot is discovered
		// dynamically, verified empty before the drop, and the complete
		// confirmed inventory is proven untouched after the rejection. The
		// fixture (pre-matrix) hotbar selection is restored for the closure's
		// end-state contract and the shutdown phase.
		{
			const auto current = slots();
			const int fixtureSelected = ownerFixture().selected;
			int emptySlot = -1;
			for (int slot = 0; slot < int(items::kHotbarSize); ++slot)
				if (current[size_t(slot)].isEmpty())
				{
					emptySlot = slot;
					break;
				}
			if (emptySlot < 0)
				throw std::runtime_error("the rejection row found no empty hotbar slot");
			tap(SDLK_1 + emptySlot);
			settle("Action accepted", "the rejection selection was not acknowledged");
			const auto confirmed = slots();
			CHECK(confirmed[size_t(emptySlot)].isEmpty(),
				"the rejected drop runs from a verified empty hotbar slot");
			tap(SDLK_Q);
			until([&] { return !engine.m_remote->actionPending; }, "empty drop did not receive its result");
			const auto rejected = engine.playerInventoryView();
			CHECK(rejected.feedbackKind == gameui::style::Status::Error && rejected.enabled() &&
				rejected.snapshot && rejected.snapshot->slots == confirmed,
				"rejected remote drop preserves the complete confirmed snapshot");
			tap(SDLK_1 + fixtureSelected);
			settle("Action accepted", "the fixture hotbar selection was not restored");
			CHECK(engine.selectedHotbarSlot() == fixtureSelected,
				"the matrix restores the fixture hotbar selection");
		}
		kindTotal("the matrix conserves the fixture budget end to end");
		engine.setInventoryOpen(false);
		until([&] { return !engine.inventoryPanelOpen(); },
			"remote inventory closes through the native shortcut", std::chrono::seconds(5));
		engine.setDeveloperWorkspaceVisible(workspace);
		std::cout << "HOSTED inventory matrix=10ops budget=117 conserved" << std::endl;
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
		// The owner actor recorded at the baseline join — the fixture rebind
		// reuses it (the identity registry owns the mapping, not the test).
		uint32_t fixtureOwnerActor = 0;
		if (host)
		{
			engine.m_hostExecutable = server;
			engine.m_hostSavesRoot = engine.m_savesRoot / "dedicated";
			hosting::Options options;
			options.world = kFixtureWorld;
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
			fixtureOwnerActor = engine.m_remote->welcome->player.value;
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
					if (!sample || !engine.m_remote->welcome)
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

		// F09 (round 2): matrix phases are explicit rendezvous. The owner
		// keeps one ordinary shared-terrain break, then establishes the
		// fixture (graceful stop -> committed roster rewrite -> restart on
		// the same world), runs the deterministic matrix and publishes the
		// completion marker. The guest rides the restart and verifies its
		// private-state isolation across the whole matrix.
		const auto edit = evidence / "first" / "edit.txt";
		if (host && !restoring)
		{
			// The spawn column can stand in water: seek a mineable bank cell
			// with the same bounded swim the single-block flow used, then
			// settle before the break.
			engine.camera.setYawPitch(0.f, -60.f);
			until([&] {
				if (mineableAim(engine.remoteTarget()))
					return true;
				engine.sendRemoteInput({.move = {1, 0}, .swimUp = true});
				return false;
			}, "hosted closure could not reach a mineable surface", std::chrono::seconds(60));
			engine.sendRemoteInput({});
			until([&] {
				const auto *actor = engine.m_remote->localPlayer();
				return actor && (actor->flags & net::wire::PlayerState::grounded) &&
					std::hypot(actor->velocity[0], actor->velocity[2]) < 0.1;
			}, "the shared-edit miner did not settle at the bank");
			const auto cell = breakRemoteAction("fixed shared terrain edit");
			publish(edit, std::to_string(cell.x) + " " + std::to_string(cell.y) + " " + std::to_string(cell.z));

			// The fixture rewrite assumes a clean committed item population:
			// the mined drop must be consumed (server-side pickup) before the
			// stop, so items.state commits empty and the restart cannot
			// re-deliver it into the fixture inventory. The client's world
			// entity snapshot is interest-limited, so the authoritative
			// confirmed inventory gaining a stack is the reliable signal.
			until([&] {
				const auto confirmed = confirmedSlots();
				return std::any_of(confirmed.begin(), confirmed.end(),
					[](const items::ItemStack &stack) { return !stack.isEmpty(); });
			}, "the mined drop did not return to the owner's confirmed inventory before the fixture stop",
				std::chrono::seconds(60));

			frame().stopHostedServer();
			until([&] { return engine.m_hostSnapshot.state == hosting::State::Stopped; },
				"the fixture baseline stop did not reach Stopped", std::chrono::seconds(60));
			CHECK(engine.m_hostSnapshot.checkpoint > checkpoint,
				"the fixture stop commits its baseline checkpoint");
			until([&] {
				const auto ui = frame();
				return ui.multiplayerStatus && ui.joinServer && engine.m_remote && !engine.m_remote->scope;
			}, "the fixture stop did not publish the departure to the owner", std::chrono::seconds(30));
			checkRetiredProjection();
			frame().returnToSolo();
			pump();
			CHECK(!engine.m_remote, "the owner returns to solo between the fixture stop and its restart");

			rewriteCommittedRoster(engine.m_hostSavesRoot / kFixtureWorld, fixtureOwnerActor);
			hosting::Options options;
			options.world = kFixtureWorld;
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
				return state.state == hosting::State::Ready && engine.m_remote && engine.m_remote->ready();
			}, "the fixture server did not become ready and auto-join", std::chrono::seconds(90));
			CHECK(engine.m_remote->welcome && engine.m_remote->welcome->player.value == fixtureOwnerActor &&
				engine.m_remote->namedProfile == profile,
				"the fixture restart resumes the owner actor with its exact identity");
			const auto endpoint = engine.m_hostSnapshot.address + ":" +
				std::to_string(engine.m_hostSnapshot.port);
			publish(round / "fixture-endpoint.txt", endpoint);
			until([&] { return seesPeer(); }, "the guest did not rejoin the fixture world",
				std::chrono::seconds(120));
			hostedInventoryMatrix();
			publish(round / "matrix-done.txt", "done");
		}
		else if (!host && !restoring)
		{
			until([&] {
				const auto ui = frame();
				return ui.multiplayerStatus && ui.joinServer && engine.m_remote && !engine.m_remote->scope;
			}, "the fixture stop did not publish the departure to the guest", std::chrono::seconds(120));
			checkRetiredProjection();
			frame().returnToSolo();
			pump();
			until([&] { return fs::exists(round / "fixture-endpoint.txt"); },
				"the owner did not publish the fixture endpoint", std::chrono::seconds(120));
			std::string fixtureEndpoint;
			std::ifstream(round / "fixture-endpoint.txt") >> fixtureEndpoint;
			join(fixtureEndpoint.c_str(), profile, "42");
			until([&] { return fs::exists(edit); }, "owner did not publish the edited cell",
				std::chrono::seconds(30));
			glm::ivec3 cell{};
			if (!(std::ifstream(edit) >> cell.x >> cell.y >> cell.z))
				throw std::runtime_error("invalid hosted edit witness");
			until([&] { return materialAt(cell) == TextureType::AIR; },
				"the guest must see the edited block after the fixture restart",
				std::chrono::seconds(30));
			CHECK(true, "guest observes the shared world edit after the fixture restart");
			// F09 §5: the guest records its authoritative inventory right
			// after the rejoin, keeps it under continuous observation while
			// the owner runs the matrix, and re-verifies it afterwards — one
			// early snapshot alone is not isolation evidence.
			const auto before = confirmedSlots();
			const auto expected = guestFixture();
			CHECK(before == expected.slots && witness().selected == expected.selected,
				"the fixture restart restores the guest's distinct private inventory");
			until([&] {
				const auto inventory = engine.m_remote->transport.inventory();
				if (inventory && inventory->slots != before)
					throw std::runtime_error("the guest's private inventory changed while the owner ran the matrix");
				return fs::exists(round / "matrix-done.txt");
			}, "the owner's inventory matrix did not complete for the isolated guest",
				std::chrono::seconds(300));
			CHECK(confirmedSlots() == before && !engine.m_remote->pendingInventoryAction,
				"the guest's private inventory is still unchanged after the matrix");
		}
		else
		{
			// Restoring run: the persisted edit is only observed; the exact
			// inventory and selection restoration is checkWitness below.
			until([&] { return fs::exists(edit); }, "the first run did not publish the edited cell");
			glm::ivec3 cell{};
			if (!(std::ifstream(edit) >> cell.x >> cell.y >> cell.z))
				throw std::runtime_error("invalid hosted edit witness");
			until([&] { return materialAt(cell) == TextureType::AIR; },
				"both clients must see the edited block after replication and restart");
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
			const auto owner = witness();
			if (guestWorld != owner.world || guestId == 0 || guestId == owner.player ||
				guestSlot != 6 || owner.selected != 3)
			{
				std::ostringstream diagnostic;
				diagnostic << "host and guest keep independent identities and hotbars:"
					<< " guestWorld=" << guestWorld << " guestId=" << guestId
					<< " guestSlot=" << guestSlot << " ownerWorld=" << owner.world
					<< " ownerPlayer=" << owner.player << " ownerSelected=" << owner.selected;
				throw std::runtime_error(diagnostic.str());
			}
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

	/// F09: a controlled SDL focus event re-asserts focus through the real
	/// input path — never a direct write behind SDL's back.
	void assertWindowFocus()
	{
		focus();
		if (!engine.windowFocused)
			throw std::runtime_error("the harness window could not take keyboard focus");
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
		const auto aliceInventory = confirmedSlots();
		const auto *aliceActor = engine.m_remote->localPlayer();
		if (!aliceActor)
			throw std::runtime_error("the first profile lost its authoritative player before the switch");
		const auto aliceFeet = net::udp::playerFeet(*aliceActor);
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
		const auto *returnedActor = engine.m_remote->localPlayer();
		if (!returnedActor)
			throw std::runtime_error("the returning profile is missing from the authoritative snapshot");
		CHECK(engine.m_remote->welcome->player == aliceId && engine.m_remote->scope != firstScope &&
		    engine.selectedHotbarSlot() == 4 && confirmedSlots() == aliceInventory &&
		    glm::length(net::udp::playerFeet(*returnedActor) - aliceFeet) < 1.0,
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
			confirmedSlots() == aliceInventory,
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
