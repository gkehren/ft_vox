#include "ClientMovement.hpp"

namespace net::udp
{
namespace
{
using Seconds = std::chrono::duration<double>;
constexpr double kTickSeconds = 1.0 / wire::kTickRate;
static_assert(kTickSeconds == 2 * physics::PlayerController::fixedStep);
constexpr uint8_t kBlocked = wire::PlayerState::waitingForTerrain | wire::PlayerState::flight;
constexpr physics::PlayerSettings kSettings;

bool canPredict(const physics::Body &body)
{
	// Wire velocity bounds are deliberately wider than walking physics.
	// Keep speculative sweeps within normal walking distances.
	return glm::length(glm::dvec2(body.velocity.x, body.velocity.z)) <= kSettings.sprintSpeed + 0.01 &&
		body.velocity.y >= -kSettings.terminalSpeed && body.velocity.y <= kSettings.jumpSpeed + 0.01;
}

Seconds elapsed(ClientPrediction::Clock::time_point now, ClientPrediction::Clock::time_point then)
{
	return Seconds(now.time_since_epoch()) - Seconds(then.time_since_epoch());
}

const wire::PlayerState *findPlayer(const wire::PlayerSnapshot &snapshot, sim::PlayerId id)
{
	const auto players = std::span(snapshot.players).first(snapshot.count);
	const auto found = std::ranges::lower_bound(players, id.value, {},
		[](const wire::PlayerState &state) { return state.player.value; });
	return found != players.end() && found->player == id ? &*found : nullptr;
}
}

glm::dvec3 playerFeet(const wire::PlayerState &player)
{
	return {player.feet[0], player.feet[1], player.feet[2]};
}

void ClientPrediction::restore()
{
	m_controller.restoreTick(m_anchor->body, m_anchor->timers);
	m_tick = m_anchor->tick;
}

void ClientPrediction::discard()
{
	m_head = m_count = 0;
	m_offset = {};
	restore();
}

void ClientPrediction::checkTerrain(const ClientWorld::CollisionView &world)
{
	if (world.terrainChanges().invalidated)
		invalidateTerrain();
}

void ClientPrediction::invalidateTerrain()
{
	if (!m_anchor || m_waitForTerrain)
		return;
	if (!m_waitForSnapshot)
		++m_stats.hardResets;
	discard();
	m_waitForSnapshot = m_waitForTerrain = true;
}

void ClientPrediction::expire(Clock::time_point now)
{
	if (!m_anchor || m_waitForSnapshot)
		return;
	if (elapsed(now, m_anchor->arrived) > kMaxAge ||
	    (m_count && elapsed(now, m_pending[m_head].sentAt) > kMaxAge))
	{
		discard();
		m_waitForSnapshot = true;
		++m_stats.hardResets;
	}
}

void ClientPrediction::stepTo(const wire::Input &input, const physics::VoxelCollisionWorld &world)
{
	while (++m_tick < input.intendedTick)
		m_controller.advance(kTickSeconds, {}, world);
	m_controller.advance(kTickSeconds, net::playerInput(input), world);
}

glm::dvec3 ClientPrediction::offset(Clock::time_point now) const
{
	return m_offset * (1.0 - std::clamp(elapsed(now, m_smoothedAt) / kSmoothTime, 0.0, 1.0));
}

void ClientPrediction::smoothFrom(glm::dvec3 displayed, Clock::time_point now)
{
	const auto delta = displayed - m_controller.body.position;
	m_offset = delta * (kSmoothDistance / std::max(glm::length(delta), kSmoothDistance));
	m_smoothedAt = now;
}

void ClientPrediction::observe(const wire::PlayerSnapshot &snapshot, const wire::PlayerState &local,
	Clock::time_point receivedAt, const physics::VoxelCollisionWorld &world)
{
	observe(snapshot, local, receivedAt, world, nullptr);
}

void ClientPrediction::observe(const wire::PlayerSnapshot &snapshot, const wire::PlayerState &local,
	Clock::time_point receivedAt, const ClientWorld::CollisionView &world)
{
	observe(snapshot, local, receivedAt, world, &world);
}

void ClientPrediction::observe(const wire::PlayerSnapshot &snapshot, const wire::PlayerState &local,
	Clock::time_point receivedAt, const physics::VoxelCollisionWorld &world,
	const ClientWorld::CollisionView *replica)
{
	const auto tick = snapshot.tick;
	const auto ack = snapshot.lastProcessedInput;
	if (ack > wire::kMaxSequence || (m_anchor &&
	    (tick <= m_anchor->tick || ack < m_anchor->ack || receivedAt < m_anchor->arrived)))
		return;

	const bool hadAnchor = m_anchor.has_value();
	const bool teleport = m_anchor && m_anchor->motionReset != local.motionReset;
	if (teleport)
		m_presentation = {};
	bool awaitingPublication = false;
	if (replica)
	{
		const auto changes = teleport ? ClientWorld::TerrainChanges{} : replica->terrainChanges();
		if (changes.invalidated)
			invalidateTerrain();
		awaitingPublication = changes.awaitingPublication;
		// Keep the old dependencies until their in-flight updates publish.
		// Otherwise the new anchor and replay build a fresh footprint.
		if (!awaitingPublication)
			replica->resetDependencies();
	}
	const bool wasSuspended = m_waitForSnapshot;
	const bool resumingTerrain = m_waitForTerrain;
	const auto before = m_controller.body.position;
	const auto displayed = before + offset(receivedAt);
	const auto beforeTick = m_tick;
	const bool discontinuity = m_anchor && (teleport ||
		((m_anchor->flags ^ local.flags) & kBlocked) ||
		elapsed(receivedAt, m_anchor->arrived) > kMaxAge);
	physics::Body body;
	body.position = playerFeet(local);
	body.velocity = glm::dvec3(local.velocity[0], local.velocity[1], local.velocity[2]);
	body.grounded = (local.flags & wire::PlayerState::grounded) != 0;
	body.waitingForTerrain = (local.flags & wire::PlayerState::waitingForTerrain) != 0;
	const auto &timers = snapshot.localTimers;
	m_anchor = Anchor{tick, ack, local.motionReset, local.flags, receivedAt, body,
		{timers[0], timers[1], timers[2]}};
	restore();
	physics::QueryStats queries;
	if (awaitingPublication || body.waitingForTerrain || !physics::clear(world, body.bounds(), queries))
	{
		if (hadAnchor && !wasSuspended)
			++m_stats.hardResets;
		discard();
		m_waitForSnapshot = m_waitForTerrain = true;
		return;
	}
	m_waitForSnapshot = m_waitForTerrain = false;
	if (discontinuity || (local.flags & kBlocked) || !canPredict(body))
	{
		if (discontinuity && !wasSuspended)
			++m_stats.hardResets;
		discard();
		return;
	}
	if (resumingTerrain && m_presentation.lastSafeFeet)
	{
		const auto held = *m_presentation.lastSafeFeet;
		if (glm::length(held - body.position) < kHardCorrection)
			smoothFrom(held, receivedAt);
		else
			m_presentation = {};
	}

	while (m_count)
	{
		const auto &entry = m_pending[m_head];
		// The server uses neutral input for a missing tick. An unacknowledged
		// command for an already completed tick can never be applied late.
		if (entry.sequence > ack && entry.input.intendedTick > tick &&
		    elapsed(receivedAt, entry.sentAt) <= kMaxAge)
			break;
		m_head = (m_head + 1) % m_pending.size();
		--m_count;
	}
	for (size_t i = 0; i < m_count; ++i)
		stepTo(m_pending[(m_head + i) % m_pending.size()].input, world);

	if (beforeTick != m_tick || beforeTick == 0)
		return;
	const double correction = glm::length(before - m_controller.body.position);
	if (correction < 0.002)
		return;
	++m_stats.corrections;
	m_stats.lastCorrection = correction;
	m_stats.maxCorrection = std::max(m_stats.maxCorrection, correction);
	if (correction >= kHardCorrection)
	{
		++m_stats.hardResets;
		discard();
		m_presentation = {};
		return;
	}
	smoothFrom(displayed, receivedAt);
}

void ClientPrediction::record(uint64_t sequence, const wire::Input &input, Clock::time_point now,
	const physics::VoxelCollisionWorld &world)
{
	expire(now);
	if (!m_anchor || m_waitForSnapshot || (m_anchor->flags & kBlocked) || !canPredict(m_anchor->body) ||
	    sequence <= std::max(m_lastSequence, m_anchor->ack) || sequence > wire::kMaxSequence ||
	    input.intendedTick <= m_tick || input.intendedTick - m_anchor->tick > kMaxTicks ||
	    now < m_anchor->arrived || m_count == m_pending.size() || !wire::validInput(input))
		return;
	m_pending[(m_head + m_count) % m_pending.size()] = {sequence, input, now};
	m_lastSequence = sequence;
	++m_count;
	stepTo(input, world);
}

std::optional<glm::dvec3> ClientPrediction::present(Clock::time_point now,
	const physics::VoxelCollisionWorld &world)
{
	if (!m_anchor)
		return std::nullopt;
	physics::QueryStats queries;
	auto pose = m_controller.body;
	if (!m_waitForTerrain && physics::clear(world, pose.bounds(), queries))
	{
		const auto delta = offset(now);
		// Check the full body along the correction, even when both endpoints
		// are clear. A wall or unknown voxel cancels the visual offset.
		if (delta != glm::dvec3{} && physics::sweep(world, pose.bounds(), delta, queries).hit)
			m_offset = {};
		else
			pose.position += delta;
		return m_presentation.lastSafeFeet = pose.position;
	}
	m_offset = {};
	if (!m_waitForTerrain && physics::recover(world, pose, queries))
		return m_presentation.lastSafeFeet = pose.position;
	if (m_presentation.lastSafeFeet)
	{
		pose.position = *m_presentation.lastSafeFeet;
		// A frozen pose may straddle unavailable terrain. If publication
		// establishes a solid overlap, show the waiting view instead.
		bool obstructed = false;
		physics::visitCells(pose.bounds(), [&](glm::ivec3 cell) {
			const auto voxel = world.sample(cell);
			obstructed |= voxel.available && voxel.solid;
		});
		if (!obstructed)
			return m_presentation.lastSafeFeet;
		m_presentation = {};
	}
	return std::nullopt;
}

void RemotePlayers::observe(const wire::PlayerSnapshot &snapshot, Clock::time_point receivedAt)
{
	if (snapshot.count == 0 || snapshot.count > snapshot.players.size())
		return;
	if (m_received)
	{
		const auto previous = m_samples[m_latest].tick;
		if (snapshot.tick <= previous || receivedAt < m_arrived)
			return;
		const auto gap = snapshot.tick - previous;
		m_missing += (gap - 1) / wire::kPlayerSnapshotIntervalTicks;
		const double age = elapsed(receivedAt, m_arrived).count();
		if (age >= Seconds(kTimeout).count())
			m_count = 0;
		// Advance the playback clock through jitter and reordering. Clamp
		// an overfull buffer by skipping forward, never by rewinding.
		m_lag = std::clamp(m_lag + double(gap) - age * wire::kTickRate, 0.0, double(kMaxDelayTicks));
	}
	if (!m_count)
		m_lag = kDelayTicks;
	m_latest = (m_latest + 1) % m_samples.size();
	m_samples[m_latest] = snapshot;
	m_count = std::min(m_count + 1, m_samples.size());
	++m_received;
	m_arrived = receivedAt;
}

std::optional<glm::dvec3> RemotePlayers::present(sim::PlayerId player, Clock::time_point now) const
{
	const auto age = elapsed(now, m_arrived);
	if (!m_count || age >= kTimeout || age < Seconds::zero())
		return std::nullopt;
	const auto &latest = m_samples[m_latest];
	const auto *newer = findPlayer(latest, player);
	if (!newer)
		return std::nullopt;
	const double lag = m_lag - age.count() * wire::kTickRate;
	if (lag <= 0.0)
		return playerFeet(*newer); // zero extrapolation
	double newerLag = 0.0;
	for (size_t i = 1; i < m_count; ++i)
	{
		const auto &sample = m_samples[(m_latest + m_samples.size() - i) % m_samples.size()];
		const auto *older = findPlayer(sample, player);
		const double olderLag = double(latest.tick - sample.tick);
		if (!older || older->motionReset != newer->motionReset ||
		    olderLag - newerLag > kMaxDelayTicks)
			break;
		if (olderLag >= lag)
			return glm::mix(playerFeet(*newer), playerFeet(*older), (lag - newerLag) / (olderLag - newerLag));
		newer = older;
		newerLag = olderLag;
	}
	return playerFeet(*newer);
}

double RemotePlayers::missingFraction() const
{
	return m_received ? double(m_missing) / (double(m_missing) + double(m_received)) : 0.0;
}

} // namespace net::udp
