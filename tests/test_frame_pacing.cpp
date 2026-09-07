// VSync frame pacing (PR #145): pure computePacedDeltaTime contract.
// No Vulkan, no engine - header-only.

#include <Engine/FramePacing.hpp>

#include <cmath>
#include <iostream>

static int g_fails = 0;

#define CHECK(cond, msg)                                                       \
	do                                                                         \
	{                                                                          \
		if (!(cond))                                                           \
		{                                                                      \
			std::cerr << "FAIL: " << msg << " (" << __LINE__ << ")\n";         \
			++g_fails;                                                         \
		}                                                                      \
	} while (0)

static bool approx(double a, double b, double eps = 1e-9)
{
	return std::abs(a - b) <= eps;
}

int main()
{
	constexpr double r165 = 1.0 / 165.0;
	constexpr double r60 = 1.0 / 60.0;

	// 165 Hz nominal: a raw delta close to one interval snaps to it exactly.
	CHECK(approx(frame_pacing::computePacedDeltaTime(6.04 / 1000.0, true, 165.0), r165),
		  "165 Hz nominal raw 6.04 ms snaps to 1/165");
	CHECK(approx(frame_pacing::computePacedDeltaTime(r165, true, 165.0), r165),
		  "165 Hz exact interval unchanged");

	// Dropped frame x2: raw near two intervals snaps to exactly 2/165.
	CHECK(approx(frame_pacing::computePacedDeltaTime(12.1 / 1000.0, true, 165.0), 2.0 * r165),
		  "165 Hz x2 dropped frame snaps to 2/165");

	// x3 and x4 multiples are allowed.
	CHECK(approx(frame_pacing::computePacedDeltaTime(3.0 * r165 + r165 * 0.1, true, 165.0), 3.0 * r165),
		  "165 Hz x3 snap");
	CHECK(approx(frame_pacing::computePacedDeltaTime(4.0 * r165 - r165 * 0.1, true, 165.0), 4.0 * r165),
		  "165 Hz x4 snap");

	// x5 is outside the allowed multiple range: raw passes through.
	CHECK(approx(frame_pacing::computePacedDeltaTime(5.0 * r165, true, 165.0), 5.0 * r165),
		  "165 Hz x5 out of range keeps raw");

	// Out of tolerance (halfway between multiples): raw unchanged.
	const double halfway = 1.5 * r165;
	CHECK(approx(frame_pacing::computePacedDeltaTime(halfway, true, 165.0), halfway),
		  "165 Hz halfway between multiples keeps raw");

	// VSync off: raw high-precision delta passes through untouched.
	CHECK(approx(frame_pacing::computePacedDeltaTime(6.04 / 1000.0, false, 165.0), 6.04 / 1000.0),
		  "vsync off keeps raw");
	CHECK(approx(frame_pacing::computePacedDeltaTime(12.1 / 1000.0, false, 60.0), 12.1 / 1000.0),
		  "vsync off keeps raw (60 Hz display)");

	// Invalid refresh rate: raw unchanged even with vsync on.
	CHECK(approx(frame_pacing::computePacedDeltaTime(6.04 / 1000.0, true, 0.0), 6.04 / 1000.0),
		  "refresh 0 keeps raw");
	CHECK(approx(frame_pacing::computePacedDeltaTime(6.04 / 1000.0, true, 5.0), 6.04 / 1000.0),
		  "refresh <= 10 keeps raw");
	CHECK(approx(frame_pacing::computePacedDeltaTime(6.04 / 1000.0, true, std::nan("")), 6.04 / 1000.0),
		  "refresh NaN keeps raw");

	// Non-finite / non-positive deltas fall back to 1/60.
	CHECK(approx(frame_pacing::computePacedDeltaTime(std::nan(""), true, 165.0), 1.0 / 60.0),
		  "NaN raw falls back to 1/60");
	CHECK(approx(frame_pacing::computePacedDeltaTime(0.0, true, 165.0), 1.0 / 60.0),
		  "zero raw falls back to 1/60");
	CHECK(approx(frame_pacing::computePacedDeltaTime(-1.0, false, 0.0), 1.0 / 60.0),
		  "negative raw falls back to 1/60");

	// Long stall clamps to 0.25 s (restore/alt-tab safety).
	CHECK(approx(frame_pacing::computePacedDeltaTime(5.0, false, 0.0), 0.25),
		  "long stall clamps to 0.25");
	CHECK(approx(frame_pacing::computePacedDeltaTime(5.0, true, 165.0), 0.25),
		  "long stall clamps to 0.25 even with vsync");

	// 60 Hz display: nominal and x2.
	CHECK(approx(frame_pacing::computePacedDeltaTime(16.9 / 1000.0, true, 60.0), r60),
		  "60 Hz nominal snaps to 1/60");
	CHECK(approx(frame_pacing::computePacedDeltaTime(33.6 / 1000.0, true, 60.0), 2.0 * r60),
		  "60 Hz x2 snaps to 2/60");

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "PASS: frame pacing - vsync snapping, multiples, fallbacks, clamps\n";
	return 0;
}
