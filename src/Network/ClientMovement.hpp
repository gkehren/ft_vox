#pragma once

#include "PlayerInput.hpp"
#include "ClientWorld.hpp"

namespace net::udp
{

glm::dvec3 playerFeet(const wire::PlayerState &player);

struct PredictionStats
{
	uint64_t corrections{}, hardResets{};
	double lastCorrection{}, maxCorrection{};
	uint64_t blockedCameraSweeps{};
	const char *lastResetReason{"none"};
};

// Presentation-only controller. All times are supplied by the frame owner.
class ClientPrediction
{
public:
	using Clock = std::chrono::steady_clock;
	static constexpr auto kMaxAge = std::chrono::milliseconds(250);
	static constexpr size_t kMaxTicks = 32;
	static constexpr double kSmoothDistance = 0.25, kHardCorrection = 0.75;
	static constexpr auto kSmoothTime = std::chrono::milliseconds(100);

	void observe(const wire::PlayerSnapshot &snapshot, const wire::PlayerState &local,
		Clock::time_point receivedAt, const physics::VoxelCollisionWorld &world);
	void observe(const wire::PlayerSnapshot &snapshot, const wire::PlayerState &local,
		Clock::time_point receivedAt, const ClientWorld::CollisionView &world);
	void record(uint64_t sequence, const wire::Input &input, Clock::time_point now,
		const physics::VoxelCollisionWorld &world);
	void checkTerrain(const ClientWorld::CollisionView &world);
	void invalidateTerrain();
	void expire(Clock::time_point now);
	std::optional<glm::dvec3> present(Clock::time_point now, const physics::VoxelCollisionWorld &world);
	bool waitingForTerrain() const { return m_waitForTerrain; }
	glm::dvec3 feet() const { return m_controller.body.position; }
	size_t backlog() const { return m_count; }
	const PredictionStats &stats() const { return m_stats; }

private:
	struct Pending
	{
		uint64_t sequence{};
		wire::Input input;
		Clock::time_point sentAt;
	};
	struct Anchor
	{
		uint64_t tick, ack;
		uint32_t motionReset;
		uint8_t flags;
		Clock::time_point arrived;
		physics::Body body;
		physics::PlayerController::Timers timers;
	};
	struct Pose
	{
		uint64_t tick;
		glm::dvec3 midpoint, end;
	};
	struct Playback
	{
		Clock::time_point frame;
		uint64_t tick;
		double lag;
	};

	void restore();
	void discard();
	void observe(const wire::PlayerSnapshot &snapshot, const wire::PlayerState &local,
		Clock::time_point receivedAt, const physics::VoxelCollisionWorld &world,
		const ClientWorld::CollisionView *replica);
	void stepTo(const wire::Input &input, const physics::VoxelCollisionWorld &world);
	void step(const physics::PlayerInput &input, const physics::VoxelCollisionWorld &world);
	const Pose *poseAt(uint64_t tick) const;
	glm::dvec3 interpolatedFeet(Clock::time_point now);
	void smoothFrom(glm::dvec3 displayed, Clock::time_point now);
	glm::dvec3 offset(Clock::time_point now) const;

	std::optional<Anchor> m_anchor;
	physics::PlayerController m_controller;
	uint64_t m_tick{}, m_lastSequence{};
	std::array<Pending, kMaxTicks> m_pending{};
	size_t m_head{}, m_count{};
	bool m_waitForSnapshot{}, m_waitForTerrain{};
	glm::dvec3 m_offset{};
	Clock::time_point m_smoothedAt{};
	static constexpr size_t kPoseHistory = 5;
	std::array<std::optional<Pose>, kPoseHistory> m_poses;
	std::optional<Playback> m_playback;
	// A hold is purely visual. Reconciliation and replay only use m_anchor.
	struct Presentation { std::optional<glm::dvec3> lastSafeFeet; } m_presentation;
	PredictionStats m_stats;
};

// The latest accepted snapshot always determines membership. The older
// samples may only describe the path of players still in that active set.
class RemotePlayers
{
public:
	using Clock = ClientPrediction::Clock;
	static constexpr size_t kHistory = 8;
	static constexpr uint64_t kDelayTicks = 6, kMaxDelayTicks = 12;
	static constexpr auto kTimeout = std::chrono::milliseconds(300);

	void observe(const wire::PlayerSnapshot &snapshot, Clock::time_point receivedAt);
	std::optional<glm::dvec3> present(sim::PlayerId player, Clock::time_point now) const;
	std::optional<double> missingFraction() const;

private:
	std::array<wire::PlayerSnapshot, kHistory> m_samples{};
	size_t m_latest{}, m_count{};
	Clock::time_point m_arrived{};
	double m_lag{};
	uint64_t m_received{}, m_missing{};
};

} // namespace net::udp
