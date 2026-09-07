#pragma once

/// VSync frame pacing (PR #145): pure helpers, no Vulkan/engine dependency,
/// unit-testable in isolation.

#include <cmath>

namespace frame_pacing
{
/// Snap a raw frame delta to the nearest multiple of the display refresh
/// interval when VSync is active and the raw delta is close to such a
/// multiple (a frame that waited on presentation actually took N refreshes,
/// so the simulation should advance by exactly N intervals, not by the raw
/// jittery time). Falls back to the raw delta when VSync is off, the
/// refresh rate is unknown, or the raw delta is not near a 1..4 multiple.
/// Long stalls (> 0.25 s) clamp to 0.25 s so a skipped stretch never
/// becomes a huge gameplay timestep; non-finite or non-positive deltas
/// fall back to 1/60.
inline double computePacedDeltaTime(double rawDt, bool vsyncActive, double refreshRate)
{
	if (!std::isfinite(rawDt) || rawDt <= 0.0)
		return 1.0 / 60.0;
	if (rawDt > 0.25)
		return 0.25;

	if (vsyncActive && refreshRate > 10.0)
	{
		const double targetInterval = 1.0 / refreshRate;
		const int multiple = static_cast<int>(std::lround(rawDt / targetInterval));
		if (multiple >= 1 && multiple <= 4)
		{
			const double expected = static_cast<double>(multiple) * targetInterval;
			if (std::abs(rawDt - expected) <= targetInterval * 0.25)
				return expected;
		}
	}
	return rawDt;
}
} // namespace frame_pacing
