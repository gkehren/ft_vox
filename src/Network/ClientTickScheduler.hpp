#pragma once

#include "WireProtocol.hpp"

#include <cmath>
#include <limits>

namespace net::udp
{

struct InputSchedule
{
	uint64_t firstTick{};
	unsigned count{};
};

// Frame-clock estimate only. The server still decides which inputs arrive in
// its tick window. All observations use the caller's steady-clock domain.
class ClientTickScheduler
{
public:
	using Clock = std::chrono::steady_clock;
	using Seconds = std::chrono::duration<double>;
	static constexpr unsigned kMaxBatch = 3;
	static constexpr auto kMaxSnapshotAge = std::chrono::milliseconds(250);
	static constexpr auto kFallbackRtt = std::chrono::milliseconds(50);
	static constexpr auto kMaxRtt = std::chrono::milliseconds(500);
	static_assert(kMaxBatch < wire::kInputLeadTicks);

	explicit ClientTickScheduler(unsigned safetyTicks = 3)
		: m_safetyTicks(std::clamp(safetyTicks, 1u, kMaxBatch)) {}

	void reset() { *this = ClientTickScheduler(m_safetyTicks); }

	void observeSnapshot(uint64_t tick, Clock::time_point receivedAt)
	{
		if (m_snapshot && (tick <= m_snapshot->tick || receivedAt < m_snapshot->receivedAt))
			return;
		const bool fresh = hasFreshClock(receivedAt);
		double lead = 0.0;
		if (fresh)
		{
			// A late delivery measures queueing delay, not a slower tick
			// clock. Use the least delayed observation in the recent window.
			for (const auto &sample : std::span(m_clockSamples).first(m_clockCount))
			{
				const auto elapsed = between(receivedAt, sample.receivedAt);
				if (elapsed <= kClockWindow)
					lead = std::max(lead, elapsed.count() * wire::kTickRate - double(tick - sample.tick));
			}
		}
		// A prolonged gap or a clock more than five ticks behind its old
		// cadence requires a fresh anchor, for example after a server stall.
		if (!fresh || lead >= wire::kInputLeadTicks - 1)
		{
			m_clockCount = m_nextClockSample = 0;
			lead = 0.0;
		}
		m_snapshot = Snapshot{tick, receivedAt};
		m_clockLead = lead;
		m_clockSamples[m_nextClockSample] = *m_snapshot;
		m_nextClockSample = (m_nextClockSample + 1) % m_clockSamples.size();
		m_clockCount = std::min(m_clockCount + 1, m_clockSamples.size());
	}

	bool observeRoundTrip(Seconds rtt)
	{
		if (!std::isfinite(rtt.count()) || rtt < Seconds::zero() || rtt > kMaxRtt)
			return false;
		// Limit one sample's influence to 16 ms while allowing repeated
		// observations to follow a sustained increase or decrease.
		constexpr Seconds maxChange = std::chrono::milliseconds(64);
		m_rtt = m_rtt ? *m_rtt + std::clamp(rtt - *m_rtt, -maxChange, maxChange) / 4.0 : rtt;
		return true;
	}

	std::optional<Seconds> roundTripEstimate() const { return m_rtt; }

	bool hasFreshClock(Clock::time_point now) const
	{
		return m_snapshot && now >= m_snapshot->receivedAt && between(now, m_snapshot->receivedAt) <= kMaxSnapshotAge;
	}

	InputSchedule schedule(Clock::time_point now) const
	{
		if (!hasFreshClock(now))
			return {};

		// Budget a round trip from the snapshot timestamp to input arrival.
		const auto age = between(now, m_snapshot->receivedAt);
		const double arrivalTicks = (age + m_rtt.value_or(kFallbackRtt)).count() * wire::kTickRate + m_clockLead;
		const uint64_t arrivalOffset = static_cast<uint64_t>(arrivalTicks);
		const uint64_t targetOffset = arrivalOffset + m_safetyTicks;
		constexpr auto maxTick = std::numeric_limits<uint64_t>::max();
		if (m_snapshot->tick > maxTick - targetOffset)
			return {};
		const uint64_t target = m_snapshot->tick + targetOffset;
		if (target <= m_lastSentTick)
			return {};

		// During normal play, send intervening ticks even if the arrival
		// estimate moved past them. The server's clock decides if they are
		// still open. For a join or larger jump, send only the newest batch.
		const bool consecutive = m_lastSentTick > 0 && target - m_lastSentTick <= kMaxBatch;
		const uint64_t earliest = m_snapshot->tick + (consecutive ? 1 : arrivalOffset + 1);
		const uint64_t first = std::max(m_lastSentTick + 1, earliest);
		return {first, static_cast<unsigned>(target - first + 1)};
	}

	// Return true only after the jump edge was sent. Held controls apply
	// throughout the batch. The latest tick is safest for a one-shot edge.
	template<class Sender>
	bool send(Clock::time_point now, wire::Input controls, Sender &&emit)
	{
		const auto batch = schedule(now);
		const bool jump = (controls.flags & wire::Input::jump) != 0;
		controls.flags &= ~wire::Input::jump;
		for (unsigned i = 0; i < batch.count; ++i)
		{
			controls.intendedTick = batch.firstTick + i;
			const bool edge = jump && i + 1 == batch.count;
			if (edge)
				controls.flags |= wire::Input::jump;
			if (!emit(controls))
				break;
			m_lastSentTick = controls.intendedTick;
			if (edge)
				return true;
		}
		return false;
	}

private:
	static constexpr auto kClockWindow = std::chrono::seconds(2);
	static constexpr size_t kClockSamples = kClockWindow.count() * wire::kTickRate /
		wire::kPlayerSnapshotIntervalTicks;

	struct Snapshot
	{
		uint64_t tick;
		Clock::time_point receivedAt;
	};

	static Seconds between(Clock::time_point now, Clock::time_point then)
	{
		// Convert before subtracting to avoid signed duration overflow at
		// the extremes of the steady-clock representation.
		return Seconds(now.time_since_epoch()) - Seconds(then.time_since_epoch());
	}

	unsigned m_safetyTicks;
	uint64_t m_lastSentTick{};
	std::optional<Snapshot> m_snapshot;
	std::array<Snapshot, kClockSamples> m_clockSamples{};
	size_t m_clockCount{}, m_nextClockSample{};
	double m_clockLead{};
	std::optional<Seconds> m_rtt;
};

} // namespace net::udp
