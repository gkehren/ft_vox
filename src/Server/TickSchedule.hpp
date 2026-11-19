#pragma once

#include <Server/ServerHost.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace server
{

/// Live pacing only. The host always receives one fixed tick at a time.
/// Use an absolute epoch so scheduler wake-up jitter never shifts the rate.
class TickSchedule
{
public:
	using Clock = std::chrono::steady_clock;
	using Tick = std::chrono::duration<int64_t, std::ratio<1, kTickRate>>;

	struct Batch
	{
		unsigned run;
		uint64_t dropped;
	};

	TickSchedule(Clock::time_point start, unsigned maxCatchUp)
		: m_start(start), m_maxCatchUp(maxCatchUp) {}

	Batch takeDue(Clock::time_point now)
	{
		const auto elapsed = now - m_start;
		// Split off whole seconds before the tick conversion to avoid scaling
		// a long nanosecond uptime by the tick rate in one integer operation.
		const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed);
		const auto total = seconds.count() * kTickRate +
			std::chrono::duration_cast<Tick>(elapsed - seconds).count();
		if (total <= m_accounted)
			return {};
		const auto due = static_cast<uint64_t>(total - m_accounted);
		m_accounted = total;
		const auto run = static_cast<unsigned>(std::min<uint64_t>(due, m_maxCatchUp));
		return {run, due - run};
	}

	Clock::time_point nextDeadline() const
	{
		const std::chrono::seconds seconds{m_accounted / kTickRate};
		const Tick remainder{m_accounted % kTickRate + 1};
		return m_start + seconds + std::chrono::ceil<Clock::duration>(remainder);
	}

private:
	Clock::time_point m_start;
	unsigned m_maxCatchUp;
	int64_t m_accounted{0};
};

} // namespace server
