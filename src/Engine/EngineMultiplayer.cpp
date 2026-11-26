#include "EngineMultiplayer.hpp"

#include <Engine/ExclusiveFile.hpp>
#include <Engine/GameUiSample.hpp>
#include <Engine/Profiler.hpp>
#include <Items/ItemRegistry.hpp>
#include <World/WorldCollisionView.hpp>

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <type_traits>

namespace
{
namespace wire = net::wire;
using net::udp::ClientStatus;
using Clock = net::udp::ClientTransport::Clock;
using namespace std::chrono_literals;
using net::udp::playerFeet;

constexpr glm::dvec3 kEyeOffset(0.0, physics::PlayerSettings{}.eyeHeight, 0.0);

glm::vec3 position(const wire::EntityMotion &motion)
{
	return glm::vec3(motion.position[0], motion.position[1], motion.position[2]) /
		float(wire::kEntityPositionScale);
}

template<class State, size_t N, class Id>
const State *findState(const std::array<State, N> &states, uint8_t count, Id id)
{
	const auto active = std::span(states).first(count);
	const auto found = std::ranges::find_if(active, [&](const State &state) {
		if constexpr (std::is_same_v<State, wire::PlayerState>)
			return state.player == id;
		else
			return state.id == id;
	});
	return found == active.end() ? nullptr : &*found;
}

std::array<float, 2> cameraAim(const Camera &camera)
{
	// Camera yaw advances from +X towards +Z. Wire yaw advances in the
	// opposite direction from +Z.
	float turns = std::fmod(90.f - camera.getYaw(), 360.f) / 360.f;
	if (turns < 0.f)
		turns += 1.f;
	return {turns < 1.f ? turns : 0.f, camera.getPitch() / 360.f};
}

const char *actionFeedback(wire::ActionCode code)
{
	using enum wire::ActionCode;
	switch (code)
	{
	case Accepted: return "Action accepted";
	case Partial: return "Action partially applied";
	case InvalidTarget: return "Rejected: invalid target";
	case OutOfReach: return "Rejected: out of reach";
	case TerrainUnavailable: return "Rejected: terrain unavailable";
	case StaleRevision: return "Rejected: terrain changed, try again";
	case InventoryRejected: return "Inventory action rejected. Check the stack and available space.";
	case Occupied: return "Rejected: space occupied";
	case RateLimited: return "Rejected: too many actions";
	case Forbidden: return "Rejected: action forbidden";
	case Unavailable: return "Rejected: action unavailable";
	}
	return "Action unavailable";
}
}

Engine::RemoteSession::RemoteSession(net::udp::Address server, std::optional<uint32_t> expected,
	const net::wire::PlayerKey &key, std::string profile)
	: transport(server, key), address(server), endpoint(client::formatEndpoint(server)),
	  namedProfile(std::move(profile)), expectedSeed(expected)
{
}

void Engine::RemoteSession::startMotionTrace(uint64_t &nextFile) noexcept
{
	const char *configured = std::getenv("FT_VOX_MOTION_TRACE");
	if (!configured || !*configured)
		return;

	try
	{
		const std::filesystem::path base(configured);
		while (nextFile != 0)
		{
			auto path = base;
			if (nextFile > 1)
			{
				path = base.parent_path() / base.stem();
				path += ".session-" + std::to_string(nextFile);
				path += base.extension();
			}
			const auto result = client::reserveFile(path);
			if (result == client::FileReservation::Failed)
				break;
			++nextFile;
			if (result == client::FileReservation::Exists)
				continue;

			auto &trace = motionTrace.emplace();
			trace.file.open(path, std::ios::app);
			if (!trace.file)
				break;
			trace.start = trace.lastFlush = Clock::now();
			trace.file.imbue(std::locale::classic());
			trace.file << std::fixed << std::setprecision(6)
				<< "time_ms,frame_ms,acquire_ms,present_ms,projection,focused,mouse_captured,ready,vsync,"
				   "move_x,move_z,input_flags,inputs_sent,inputs_failed,first_input_tick,last_input_tick,last_input_seq,"
				   "snapshot_tick,ack_seq,rtt_ms,snapshot_age_ms,backlog,corrections,last_correction,"
				   "hard_resets,reset_reason,blocked_camera_sweeps,waiting_terrain,player_flags,motion_reset,"
				   "server_x,server_y,server_z,server_vx,server_vy,server_vz,predicted_x,predicted_y,predicted_z,"
				   "camera_valid,camera_time_ms,camera_x,camera_y,camera_z,yaw,pitch\n";
			if (!trace.file)
				break;
			std::cout << "[multiplayer] Motion trace: " << path.string() << '\n';
			return;
		}
	}
	catch (const std::exception &) {}
	motionTrace.reset();
	std::cerr << "[multiplayer] Could not open motion trace: " << configured << '\n';
}

double Engine::RemoteSession::SnapshotClock::alpha(uint64_t previousTick, Clock::time_point now) const
{
	const double interval = std::clamp(double(tick - previousTick) / wire::kTickRate, 0.001, 0.2);
	return std::clamp(std::chrono::duration<double>(now - arrived).count() / interval, 0.0, 1.0);
}

bool Engine::RemoteSession::SnapshotClock::fresh(Clock::time_point now) const
{
	return arrived != Clock::time_point{} && now - arrived < 2s;
}

const wire::PlayerState *Engine::RemoteSession::localPlayer() const
{
	const auto &snapshot = transport.playerSnapshots().current;
	return welcome && snapshot ? findState(snapshot->players, snapshot->count, welcome->player) : nullptr;
}

bool Engine::RemoteSession::ready() const
{
	const auto *player = localPlayer();
	return projection && player && hasCameraPose && !prediction.waitingForTerrain() &&
		players.fresh(Clock::now()) && transport.inventory() &&
		projection->observedVersion(glm::ivec3(glm::floor(playerFeet(*player)))).has_value();
}

bool Engine::RemoteSession::canReconnect() const
{
	const auto status = transport.status();
	return status == ClientStatus::Disconnected || status == ClientStatus::Rejected ||
		status == ClientStatus::TimedOut;
}

ui::MultiplayerStatus Engine::RemoteSession::connectionStatus(Clock::time_point now) const
{
	const auto status = transport.status();
	const bool connected = status == ClientStatus::Connected;
	const bool timingDegraded = connected && firstPlayerAt != Clock::time_point{} &&
		(!inputTiming.hasFreshClock(now) || transport.inputAcknowledgmentStalled(now));
	auto result = ui::multiplayerStatus(status, transport.rejection(), seedMismatch,
		connected && ready(), timingDegraded);
	if (result.state == ui::MultiplayerState::Disconnected && transport.serverShutDown())
		result.detail = "The server shut down. Reconnect when it is running again.";
	return result;
}

const char *Engine::RemoteSession::actionStatus(Clock::time_point now) const
{
	return actionPending ? "Action pending" : now < feedbackUntil ? feedback : nullptr;
}

void Engine::RemoteSession::disconnect() noexcept
{
	try
	{
		const auto now = Clock::now();
		transport.disconnect(now);
		transport.poll(now);
	}
	catch (const std::exception &error)
	{
		std::cerr << "[multiplayer] Could not send departure: " << error.what() << '\n';
	}
}

bool Engine::RemoteSession::submit(wire::Action action)
{
	if (actionPending)
		return false;
	const auto now = Clock::now();
	const bool synchronized = ready();
	if (synchronized && transport.sendAction(action, now))
	{
		actionPending = true;
		feedback = nullptr;
		return true;
	}
	feedback = synchronized ? "Could not send action, try again" : "Waiting for synchronized state";
	actionRejected = true;
	feedbackUntil = now + 3s;
	return false;
}

bool Engine::RemoteSession::submitSlots(wire::ActionKind operation, int slot, int other)
{
	return slot >= 0 && slot < items::kInventorySize && other >= 0 && other < items::kInventorySize &&
		submit({.operation = operation, .slot = uint8_t(slot), .otherSlot = uint8_t(other)});
}

void Engine::connectRemote(net::udp::Address address, std::optional<uint32_t> expectedSeed,
						   const net::wire::PlayerKey &key)
{
	if (sim || m_remote)
		throw std::logic_error("remote initialization requires a fresh host");
	startRemote(std::make_unique<RemoteSession>(address, expectedSeed, key));
}

void Engine::pollRemote()
{
	auto &remote = *m_remote;
	auto &transport = remote.transport;
	const auto now = Clock::now();
	const auto before = transport.status();
	const bool switching = switchingSession();
	if (remote.leaveRequested || (switching && before != ClientStatus::Connecting))
		transport.disconnect(now);
	if (!switching && remote.joinRequested && remote.canReconnect())
	{
		remote.seedMismatch = false;
		transport.connect(now);
	}
	remote.leaveRequested = remote.joinRequested = false;
	transport.poll(now);
	// A switch during Hello/Welcome waits for the scope (or join timeout).
	// This lets us release a profile reservation before trying it again.
	if (switching && transport.status() == ClientStatus::Connected)
		transport.disconnect(now);

	auto next = transport.status() == ClientStatus::Connected ? transport.scope() : std::nullopt;
	const auto welcome = next ? transport.welcome() : std::nullopt;
	if (welcome && remote.expectedSeed && welcome->seed != *remote.expectedSeed)
	{
		std::cerr << "[multiplayer] World seed mismatch. Expected " << *remote.expectedSeed
			<< ", received " << welcome->seed << '\n';
		remote.seedMismatch = true;
		transport.disconnect(now);
		next.reset();
	}
	// Receiving heartbeats without the player stream cannot leave a frozen
	// session on screen indefinitely. The transport retires and rejoins.
	const auto &history = transport.playerSnapshots();
	if (next && remote.scope == next && remote.players.arrived != Clock::time_point{} &&
		history.current && now - remote.players.arrived >= 3s &&
		history.current->tick == remote.players.tick)
	{
		transport.requestWorldResync(now);
		next.reset();
	}
	if (next != remote.scope)
	{
		const bool keepInventoryOpen = inventoryPanelOpen() && !switching;
		retireWorldProjection();
		remote.projection.reset();
		remote.scope = next;
		remote.welcome = next ? welcome : std::nullopt;
		remote.players = remote.items = remote.mobs = {};
		remote.inputTiming.reset();
		remote.prediction = {};
		remote.remotePlayers = {};
		remote.terrain = {};
		remote.hasCameraPose = false;
		camera.setPosition({0.f, 100.f, 0.f});
		remote.lastTraffic = transport.traffic();
		remote.trafficAt = now;
		remote.firstPlayerAt = {};
		remote.roundTripAt.reset();
		remote.trafficRates.reset();
		remote.jumpQueuedAt = {};
		remote.actionPending = false;
		remote.actionRejected = false;
		remote.pendingInventoryAction.reset();
		remote.feedback = nullptr;
		if (next)
		{
			remote.projection = std::make_unique<net::udp::ClientWorld>();
			renderBridge = std::make_unique<RenderWorldBridge>(&remote.projection->world(),
				threadPool.get(), chunkPool.get(), ++m_remoteRenderEpoch);
			std::cout << "[multiplayer] Joined player=" << welcome->player.value
				<< " seed=" << welcome->seed << " projection=" << m_remoteRenderEpoch << std::endl;
		}
		if (keepInventoryOpen)
			setInventoryOpen(true);
	}
	if (next)
	{
		if (const auto sample = transport.takeRoundTripSample(); sample && remote.inputTiming.observeRoundTrip(*sample))
			remote.roundTripAt = now;
		if (const auto *player = remote.localPlayer())
		{
			const net::udp::ClientWorld::CollisionView collision(*remote.projection, transport.chunks(), &remote.terrain);
			if (remote.players.arrived == Clock::time_point{} || history.current->tick != remote.players.tick)
			{
				const auto receivedAt = *transport.playerSnapshotReceivedAt();
				remote.inputTiming.observeSnapshot(history.current->tick, receivedAt);
				remote.players.observe(history, receivedAt);
				if (remote.firstPlayerAt == Clock::time_point{})
					remote.firstPlayerAt = receivedAt;
				remote.prediction.observe(*history.current, *player, receivedAt, collision);
				remote.remotePlayers.observe(*history.current, receivedAt);
			}
			else
				remote.prediction.checkTerrain(collision);
			remote.prediction.expire(now);
		}
		if (now - remote.trafficAt >= 1s)
		{
			const auto traffic = transport.traffic();
			const double elapsed = std::chrono::duration<double>(now - remote.trafficAt).count();
			remote.trafficRates = ui::NetworkStats::Traffic{
				double(traffic.receivedBytes - remote.lastTraffic.receivedBytes) / (1024.0 * elapsed),
				double(traffic.sentBytes - remote.lastTraffic.sentBytes) / (1024.0 * elapsed)};
			remote.lastTraffic = traffic;
			remote.trafficAt = now;
		}
	}
	const auto status = remote.connectionStatus(Clock::now());
	const char *message = status.detail ? status.detail : status.label;
	if (remote.lastConnectionStatus != message)
	{
		remote.lastConnectionStatus = message;
		std::cout << "[multiplayer] " << status.label;
		if (status.detail)
			std::cout << " - " << status.detail;
		std::cout << std::endl;
	}
	if (!next)
		return;
	if (const auto result = transport.takeActionResult())
	{
		remote.actionPending = false;
		remote.pendingInventoryAction.reset();
		remote.actionRejected = result->code != wire::ActionCode::Accepted &&
			result->code != wire::ActionCode::Partial;
		remote.feedback = actionFeedback(result->code);
		remote.feedbackUntil = now + 3s;
	}

	remote.items.observe(transport.itemSnapshots(), now);
	remote.mobs.observe(transport.mobSnapshots(), now);
}

void Engine::sendRemoteInput(const physics::PlayerInput &input)
{
	auto &remote = *m_remote;
	if (!remote.ready())
		return;
	const auto now = RemoteSession::Clock::now();
	const net::udp::ClientWorld::CollisionView collision(*remote.projection, remote.transport.chunks(), &remote.terrain);
	if (input.jumpPressed && now - remote.jumpQueuedAt >= 200ms)
		jumpPressed = false;
	wire::Input sample;
	const auto move = input.move * (0.999999 / std::max(glm::length(input.move), 1.0));
	sample.moveX = float(move.x);
	sample.moveZ = float(move.y);
	sample.flags = (input.sprint ? wire::Input::sprint : 0) |
		(input.swimUp ? wire::Input::swimUp : 0) | (input.swimDown ? wire::Input::swimDown : 0) |
		(input.jumpPressed && jumpPressed ? wire::Input::jump : 0);
	if (remote.motionTrace)
		remote.motionTrace->controls = sample;
	if (remote.inputTiming.send(now, sample, [&](const wire::Input &inputSample) {
		const auto sequence = remote.transport.sendInput(inputSample, now);
		if (remote.motionTrace)
		{
			auto &trace = *remote.motionTrace;
			if (sequence)
			{
				if (trace.sent++ == 0)
					trace.firstInputTick = inputSample.intendedTick;
				trace.lastInputTick = inputSample.intendedTick;
				trace.lastInputSequence = sequence;
			}
			else
				++trace.failed;
		}
		if (sequence)
			remote.prediction.record(sequence, inputSample, now, collision);
		return sequence != 0;
	}))
		jumpPressed = false;
}

void Engine::updateRemoteCamera()
{
	auto &remote = *m_remote;
	const net::udp::ClientWorld::CollisionView collision(*remote.projection, remote.transport.chunks(), &remote.terrain);
	remote.prediction.checkTerrain(collision);
	const auto now = Clock::now();
	const auto body = remote.prediction.present(now, collision);
	if (remote.motionTrace)
		remote.motionTrace->cameraAt = now;
	remote.hasCameraPose = body.has_value();
	if (body)
		camera.setPosition(glm::vec3(*body + kEyeOffset));
}

void Engine::traceRemoteMotion(double frameDt)
{
	auto &remote = *m_remote;
	auto &trace = *remote.motionTrace;
	const auto now = Clock::now();
	if (!trace.file)
	{
		std::cerr << "[multiplayer] Could not write motion trace\n";
		remote.motionTrace.reset();
		return;
	}
	if (trace.frames == 12000 || now - trace.start >= 60s)
	{
		std::cout << "[multiplayer] Motion trace finished after " << trace.frames << " frames\n";
		remote.motionTrace.reset();
		return;
	}
	using Milliseconds = std::chrono::duration<double, std::milli>;
	const auto &snapshot = remote.transport.playerSnapshots().current;
	const auto *player = remote.localPlayer();
	const auto feet = player ? playerFeet(*player) : glm::dvec3{};
	const auto velocity = player ? player->velocity : wire::PlayerState{}.velocity;
	const auto predicted = remote.prediction.feet();
	const auto eye = camera.getPosition();
	const auto &stats = remote.prediction.stats();
	const auto &controls = trace.controls;
	trace.file << Milliseconds(now - trace.start).count() << ',' << frameDt * 1000.0 << ','
		<< GetProfiler().lastScopeMs("Acquire") << ',' << GetProfiler().lastScopeMs("Present") << ','
		<< m_remoteRenderEpoch << ',' << windowFocused << ',' << mouseCaptured << ',' << remote.ready() << ','
		<< swapchain->isVSync() << ',' << controls.moveX << ',' << controls.moveZ << ','
		<< unsigned(controls.flags) << ',' << trace.sent << ',' << trace.failed << ','
		<< trace.firstInputTick << ',' << trace.lastInputTick << ',' << trace.lastInputSequence << ','
		<< (snapshot ? snapshot->tick : 0) << ',' << (snapshot ? snapshot->lastProcessedInput : 0) << ','
		<< Milliseconds(remote.inputTiming.roundTripEstimate().value_or(-1ms)).count() << ','
		<< (snapshot ? Milliseconds(now - remote.players.arrived).count() : -1.0) << ','
		<< remote.prediction.backlog() << ',' << stats.corrections << ',' << stats.lastCorrection << ','
		<< stats.hardResets << ',' << stats.lastResetReason << ',' << stats.blockedCameraSweeps << ','
		<< remote.prediction.waitingForTerrain() << ',' << (player ? unsigned(player->flags) : 0) << ','
		<< (player ? player->motionReset : 0) << ','
		<< feet.x << ',' << feet.y << ',' << feet.z << ','
		<< velocity[0] << ',' << velocity[1] << ',' << velocity[2] << ','
		<< predicted.x << ',' << predicted.y << ',' << predicted.z << ','
		<< remote.hasCameraPose << ','
		<< (trace.cameraAt == Clock::time_point{} ? -1.0 : Milliseconds(trace.cameraAt - trace.start).count()) << ','
		<< eye.x << ',' << eye.y << ',' << eye.z << ','
		<< camera.getYaw() << ',' << camera.getPitch() << '\n';
	++trace.frames;
	trace.cameraAt = {};
	trace.controls = {};
	trace.firstInputTick = trace.lastInputTick = trace.lastInputSequence = 0;
	trace.sent = trace.failed = 0;
	if (now - trace.lastFlush >= 1s)
	{
		trace.file.flush();
		trace.lastFlush = now;
	}
}

void Engine::updateRemoteEntities()
{
	auto &remote = *m_remote;
	mobStates.clear();
	m_itemStates.clear();
	std::vector<OverlayPlayer> avatars;
	const auto now = RemoteSession::Clock::now();
	const auto &players = remote.transport.playerSnapshots();
	if (remote.hasCameraPose && remote.projection && players.current && remote.players.fresh(now))
	{
		for (const auto &actor : std::span(players.current->players).first(players.current->count))
		{
			if (actor.player == remote.welcome->player)
				continue;
			const auto body = remote.remotePlayers.present(actor.player, now);
			if (!body)
				continue;
			// The server may spawn two players in the same cell. Keep the
			// camera clear until it is outside the other player's box.
			const glm::vec3 relative = camera.getPosition() - glm::vec3(*body);
			if (std::abs(relative.x) < OverlayPlayer::kHalfWidth &&
				std::abs(relative.z) < OverlayPlayer::kHalfWidth &&
				relative.y >= 0.f && relative.y < OverlayPlayer::kHeight)
				continue;
			if (remote.projection->observedVersion(glm::ivec3(glm::floor(*body))))
				avatars.push_back({glm::vec3(*body), actor.player.value});
		}
	}

	const auto present = [now, &remote](const auto &history, const auto &clock, auto member, auto draw) {
		if (!remote.hasCameraPose || !remote.projection || !history.current || !clock.fresh(now))
			return;
		const auto &current = *history.current;
		const float alpha = history.previous ? float(clock.alpha(history.previous->tick, now)) : 1.f;
		for (const auto &state : std::span(current.*member).first(current.count))
		{
			glm::vec3 pos = position(state.motion);
			if (history.previous)
				if (const auto *previous = findState((*history.previous).*member, history.previous->count, state.id))
					pos = glm::mix(position(previous->motion), pos, alpha);
			if (remote.projection->observedVersion(glm::ivec3(glm::floor(pos))))
				draw(state, pos, float(current.tick % 3600) + alpha);
		}
	};
	present(remote.transport.itemSnapshots(), remote.items, &wire::ItemSnapshot::items,
		[this](const wire::ItemState &item, glm::vec3 pos, float phase) {
			const auto block = items::placeableBlockOf(item.stack.item);
			if (!block)
				return;
			const auto light = renderBridge->sampleSmoothedLight(pos);
			m_itemStates.push_back({pos, uint32_t(*block), phase * 0.05f,
				std::sin(phase * 0.1f) * 0.05f, light.skylight, light.blockRgb});
		});
	present(remote.transport.mobSnapshots(), remote.mobs, &wire::MobSnapshot::mobs,
		[this](const wire::MobState &mob, glm::vec3 pos, float phase) {
			entities::MobRenderState state;
			state.species = static_cast<entities::MobSpecies>(mob.species);
			state.position = pos;
			const auto &velocity = mob.motion.velocity;
			state.yaw = std::atan2(float(velocity[0]), float(velocity[2]));
			state.stride = std::min(1.f, glm::length(glm::vec2(velocity[0], velocity[2])) /
				float(wire::kEntityVelocityScale));
			state.gait = phase * 0.1f;
			const auto light = renderBridge->sampleSmoothedLight(entities::mobLightSamplePosition(state));
			state.localSkylight = light.skylight;
			state.localBlockRgb = light.blockRgb;
			mobStates.push_back(state);
		});
	worldRenderer->overlays().setPlayers(std::move(avatars), true);
	worldRenderer->setMobs(mobStates);
	worldRenderer->setItemEntities(m_itemStates);
}

sim::VoxelTargetTrace Engine::remoteTarget() const
{
	if (!m_remote->ready())
		return {};
	const auto aim = cameraAim(camera);
	const auto direction = wire::aimDirection(aim[0], aim[1]);
	const WorldCollisionView world(*viewWorld());
	return sim::GameSimulation::traceVoxelTarget(world, playerFeet(*m_remote->localPlayer()) + kEyeOffset,
		{direction[0], direction[1], direction[2]});
}

void Engine::sendRemoteVoxelAction(bool placing)
{
	auto &remote = *m_remote;
	if (remote.actionPending)
		return;
	const auto hit = remoteTarget();
	const auto now = RemoteSession::Clock::now();
	const auto cell = placing ? hit.adjacent : hit.block;
	if (!hit.hit || (placing && !hit.hasPlaceFace) || cell.y < 0 || cell.y >= CHUNK_HEIGHT)
	{
		remote.feedback = hit.terrainUnavailable ? "Waiting for target terrain" : "No block face in reach";
		remote.feedbackUntil = now + 3s;
		return;
	}
	const auto version = remote.projection->observedVersion(cell);
	const auto &chunks = remote.transport.chunks();
	const auto current = chunks.find({floorDivChunk(cell.x), floorDivChunk(cell.z)});
	if (!version || current == chunks.end() || *version != current->second.version)
	{
		remote.feedback = "Waiting for target terrain";
		remote.feedbackUntil = now + 3s;
		return;
	}
	const auto aim = cameraAim(camera);
	remote.submit({.operation = placing ? wire::ActionKind::Place : wire::ActionKind::Break,
		.target = {cell.x, cell.z, uint8_t(cell.y)}, .expected = *version,
		.yawTurns = aim[0], .pitchTurns = aim[1]});
}

void Engine::selectHotbarSlot(int slot)
{
	if (m_remote)
	{
		// F04 (#316): one submission adapter for HUD and native gestures, so a
		// selection made through the HUD highlights its slot in the inventory.
		submitRemoteInventoryAction({inventoryui::Command::Select, inventoryui::SlotId{slot}});
	}
	else
		sim->selectHotbarSlot(slot);
}

int Engine::selectedHotbarSlot() const
{
	if (!m_remote)
		return sim->inventory().selectedSlot();
	return m_remote->projection ? m_remote->transport.inventory().value_or(wire::Inventory{}).selected : 0;
}

void Engine::dropSelectedItem()
{
	// F04 (#316): the HUD drop takes the same submission adapter as the
	// inventory, so the affected slot shows its pending state on open.
	if (m_remote)
		submitRemoteInventoryAction({inventoryui::Command::DropOne, inventoryui::SlotId{selectedHotbarSlot()}});
	else
		sim->dropFromSlot(selectedHotbarSlot(), 1, glm::dvec3(camera.getFront()));
}

gameui::InventoryView Engine::playerInventoryView() const
{
	using gameui::InventoryAvailability;
	using gameui::style::Status;
	using inventoryui::SlotId;
	gameui::InventoryView view;
	view.epoch = worldGenerationId();
	view.canDrop = !paused && !nativePause();
	const auto now = Clock::now();
	if (switchingSession())
	{
		view.feedback = "Switching session";
		view.feedbackKind = Status::Pending;
		return view;
	}
	if (m_remote)
	{
		const auto &remote = *m_remote;
		const auto connection = remote.connectionStatus(now);
		switch (connection.state)
		{
		using enum ui::MultiplayerState;
		case Connected:
		case Degraded:
			view.availability = InventoryAvailability::Ready;
			if (const auto inventory = remote.transport.inventory())
				view.snapshot = inventoryui::Snapshot{inventory->slots, SlotId{inventory->selected}, inventory->revision};
			view.actionPending = remote.actionPending;
			view.pendingSlots = remote.pendingInventoryAction;
			if (const auto *feedback = remote.actionStatus(now))
				view.feedback = feedback;
			view.feedbackKind = view.actionPending ? Status::Pending :
				remote.actionRejected ? Status::Error : Status::Info;
			if (view.actionPending)
				view.recovery = "Waiting for the server to confirm.";
			break;
		case Connecting:
		case SyncingTerrain:
		case Resyncing:
		case Leaving:
			view.availability = connection.state == Connecting ? InventoryAvailability::Joining :
				InventoryAvailability::Resyncing;
			view.feedback = connection.label;
			view.feedbackKind = Status::Pending;
			break;
		case Disconnected:
		case TimedOut:
		case Rejected:
			view.availability = InventoryAvailability::Disconnected;
			view.feedback = connection.label;
			view.feedbackKind = Status::Error;
			view.recovery = gameui::recoveryInstruction(m_showDeveloperUi, mouseCaptured);
			break;
		}
		return view;
	}
	if (!sim)
	{
		view.feedback = "Waiting for world";
		view.feedbackKind = Status::Pending;
		return view;
	}

	sim::InventorySnapshot snapshot;
	sim->inventorySnapshot(snapshot);
	view.snapshot = inventoryui::Snapshot{snapshot.slots, SlotId{snapshot.selectedSlot}};
	view.availability = InventoryAvailability::Ready;
	if (m_inventoryResult && now < m_inventoryResultUntil)
	{
		view.feedbackKind = Status::Error;
		using enum items::ItemResult;
		switch (*m_inventoryResult)
		{
		case Ok: break;
		case Partial:
			view.feedbackKind = Status::Info;
			view.feedback = "Only part of the stack fit.";
			break;
		case Full: view.feedback = "The destination is full. Choose another slot."; break;
		case EmptyStack: view.feedback = "The source slot is empty. Choose a stack."; break;
		case ItemMismatch: view.feedback = "These stacks cannot merge."; break;
		case IdExhausted: view.feedback = "No more dropped items can be created in this world."; break;
		default: view.feedback = "Inventory action unavailable. Try again."; break;
		}
	}
	return view;
}

void Engine::submitInventoryAction(inventoryui::Action action)
{
	using enum inventoryui::Command;
	m_playerUi->cancelInventoryGesture();
	const auto view = playerInventoryView();
	if (!view.enabled() || !action.from.valid() ||
		((action.command == DropOne || action.command == DropStack) && !view.canDrop))
		return;

	const int from = action.from.index, to = action.to.index;
	if (m_remote)
	{
		submitRemoteInventoryAction(action);
		return;
	}

	items::ItemResult result = items::ItemResult::InvalidSlot;
	switch (action.command)
	{
	case Move: result = sim->moveStack(from, to); break;
	case Swap: result = sim->swapStacks(from, to); break;
	case Split: result = sim->splitStack(from, to); break;
	case Select: result = sim->selectHotbarSlot(from); break;
	case DropOne:
	case DropStack:
	{
		const uint16_t count = action.command == DropOne ? 1 : view.snapshot->slots[size_t(from)].count;
		result = sim->dropFromSlot(from, count, glm::dvec3(camera.getFront()));
		break;
	}
	case None: return;
	}
	m_inventoryResult = result;
	m_inventoryResultUntil = Clock::now() + 3s;
}

bool Engine::submitRemoteInventoryAction(inventoryui::Action action)
{
	using enum inventoryui::Command;
	using wire::ActionKind;
	auto &remote = *m_remote;
	ActionKind operation = ActionKind::Select;
	switch (action.command)
	{
	case Move: operation = ActionKind::Move; break;
	case Swap: operation = ActionKind::Swap; break;
	case Split: operation = ActionKind::SplitHalf; break;
	case Select: operation = ActionKind::Select; break;
	case DropOne: operation = ActionKind::DropOne; break;
	case DropStack: operation = ActionKind::DropStack; break;
	case None: return false;
	}
	// PR #316 F04: HUD shortcuts and native gestures share this adapter. The
	// single-action-in-flight policy stays inside submitSlots(); the affected
	// slots are published only after a successful submission, and the action
	// result, disconnect, resync and session-reset paths (pollRemote, scope
	// change) clear them. Terrain actions keep their own pending flag.
	if (!action.from.valid() ||
		!remote.submitSlots(operation, action.from.index, action.to.valid() ? action.to.index : 0))
		return false;
	remote.pendingInventoryAction = action;
	return true;
}

void Engine::populateRemoteUi(GameUIFrame &frame)
{
	auto &remote = *m_remote;
	const auto now = Clock::now();
	frame.multiplayerStatus = remote.connectionStatus(now);
	frame.multiplayerEndpoint = remote.endpoint;
	frame.multiplayerProfile = remote.namedProfile.empty() ?
		std::string_view{"Startup profile"} : std::string_view{remote.namedProfile};
	frame.multiplayerAction = remote.actionStatus(now);
	const auto status = remote.transport.status();
	if (ui::hasLiveNetworkStats(frame.multiplayerStatus->state))
	{
		auto &network = frame.networkStats;
		const auto rtt = remote.inputTiming.roundTripEstimate();
		if (rtt && remote.roundTripAt)
			network.ping = ui::NetworkStats::Ping{*rtt, now - *remote.roundTripAt};
		network.traffic = remote.trafficRates;
		if (remote.firstPlayerAt != Clock::time_point{})
		{
			network.snapshot = ui::NetworkStats::Snapshot{
				now - remote.players.arrived, now - remote.firstPlayerAt, remote.remotePlayers.missingFraction()};
			const auto &stats = remote.prediction.stats();
			network.prediction = ui::NetworkStats::Prediction{
				remote.prediction.backlog(), stats.corrections, stats.hardResets, stats.lastCorrection, stats.maxCorrection};
		}
	}
	if (!frame.switchingSession && remote.canReconnect())
		frame.joinServer = [this] {
			if (!switchingSession() && !blocksNetworkSessionRequests())
				m_remote->joinRequested = true;
		};
	else if (!frame.switchingSession && status != ClientStatus::Leaving)
		frame.leaveServer = [this] {
			if (!switchingSession() && !blocksNetworkSessionRequests())
				m_remote->leaveRequested = true;
		};
	if (const auto *player = remote.localPlayer())
	{
		frame.player.grounded = (player->flags & wire::PlayerState::grounded) != 0;
		frame.player.waitingForTerrain = (player->flags & wire::PlayerState::waitingForTerrain) != 0;
		frame.player.speed = glm::length(glm::vec3(player->velocity[0], player->velocity[1], player->velocity[2]));
	}
}

int Engine::worldSeed() const
{
	if (m_remote)
		return m_remote->welcome ? int(m_remote->welcome->seed) : 0;
	return sim ? sim->seed() : 0;
}

uint64_t Engine::worldGenerationId() const
{
	if (m_remote)
		return m_remoteRenderEpoch;
	return sim ? sim->worldGenerationId() : 0;
}

VoxelWorld *Engine::viewWorld() const
{
	if (m_remote)
		return m_remote->projection ? &m_remote->projection->world() : nullptr;
	return sim ? sim->world() : nullptr;
}
