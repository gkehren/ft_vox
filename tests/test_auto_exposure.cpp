// Unit tests for the CPU auto-exposure reference math (issue #140).
// Renderer/AutoExposure.hpp is the CPU mirror of exposure_adapt.frag.glsl and
// MUST stay in sync with it — these checks pin the EV semantics, the clamps,
// the frame-rate-independent adaptation and the GLSL SSBO state layout.

#include <Renderer/AutoExposure.hpp>
#include <Engine/EngineDefs.hpp>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
bool fail(const std::string &msg)
{
	std::cerr << "FAIL: " << msg << "\n";
	return false;
}
} // namespace

int main()
{
	bool ok = true;

	// --- EV semantics of the target exposure (middleGrey 1, comp 0) ---
	{
		autoexposure::Params p{};
		p.middleGrey = 1.0f; // explicit unit-key fixture, independent of artistic defaults
		p.maxEv = 4.0f;
		uint32_t state = 0xFFFFFFFFu;
		const float zero = autoexposure::targetLogExposure(0.0f, p, &state);
		if (std::abs(zero) > 1e-6f || state != 0u)
			ok = fail("metered 0 with defaults must target 0 EV, in range");
		if (std::abs(std::exp2(zero) - 1.0f) > 1e-6f)
			ok = fail("target 0 EV must mean linear exposure 1.0");

		autoexposure::Params up = p;
		up.compensationEv = 1.0f; // +1 stop
		const float plus = autoexposure::targetLogExposure(0.0f, up, &state);
		if (std::abs(plus - 1.0f) > 1e-6f || state != 0u)
			ok = fail("compensation +1 must target +1 EV");
		if (std::abs(std::exp2(plus) - 2.0f) > 1e-5f)
			ok = fail("+1 EV compensation must double the exposure (one stop brighter)");

		autoexposure::Params down = p;
		down.compensationEv = -1.0f;
		if (std::abs(autoexposure::targetLogExposure(0.0f, down) + 1.0f) > 1e-6f)
			ok = fail("compensation -1 must target -1 EV (half the exposure)");

		// The meter enters negatively: a brighter scene maps back to middle
		// grey with a LOWER exposure.
		if (std::abs(autoexposure::targetLogExposure(2.0f, p) + 2.0f) > 1e-6f)
			ok = fail("metered +2 EV must target -2 EV (exposure 0.25)");
	}

	// --- middleGrey semantics ---
	{
		autoexposure::Params p{};
		p.middleGrey = 0.5f;
		// Metered -1 EV is luminance 0.5, which middleGrey 0.5 maps to
		// exposure 1.0: log2(0.5) - (-1) = 0 EV.
		if (std::abs(autoexposure::targetLogExposure(-1.0f, p)) > 1e-6f)
			ok = fail("middleGrey 0.5 with metered -1 EV must target 0 EV (0.5/0.5 = 1.0)");
	}

	// --- Clamps + clampState reporting ---
	{
		autoexposure::Params p{};
		p.middleGrey = 1.0f; // explicit unit-key fixture, independent of artistic defaults
		p.maxEv = 4.0f;
		uint32_t state = 0;
		// Very dark scene: raw target far above maxEv -> clamped high.
		const float dark = autoexposure::targetLogExposure(-20.0f, p, &state);
		if (std::abs(dark - p.maxEv) > 1e-6f || state != 2u)
			ok = fail("metered -20 EV must clamp at maxEv with clampState 2");
		// Very bright scene: raw target far below minEv -> clamped low.
		const float bright = autoexposure::targetLogExposure(20.0f, p, &state);
		if (std::abs(bright - p.minEv) > 1e-6f || state != 1u)
			ok = fail("metered +20 EV must clamp at minEv with clampState 1");
		// In-range meter must report 0 and pass through unscaled.
		const float mid = autoexposure::targetLogExposure(0.5f, p, &state);
		if (state != 0u || std::abs(mid + 0.5f) > 1e-6f)
			ok = fail("metered 0.5 EV with defaults must be in range (clampState 0)");
		// Exact boundary semantics of clampStateFor: <= min -> 1, >= max -> 2.
		if (autoexposure::clampStateFor(p.minEv, p) != 1u ||
			autoexposure::clampStateFor(p.maxEv, p) != 2u ||
			autoexposure::clampStateFor(0.0f, p) != 0u)
			ok = fail("clampStateFor boundary semantics wrong (min inclusive 1, max inclusive 2)");
		// Optional out-param: calling without it must not disturb the result.
		if (std::abs(autoexposure::targetLogExposure(-20.0f, p) - dark) > 0.0f)
			ok = fail("targetLogExposure must be identical with and without the clampState out-param");
	}

	// --- Frame-rate independence of the adaptation ---
	{
		const autoexposure::Params p{};
		const auto run = [&](float target, const std::vector<float> &dts) {
			float cur = 0.0f;
			for (const float dt : dts)
				cur = autoexposure::adaptLogExposure(cur, target, dt, p);
			return cur;
		};
		std::vector<float> hour(60, 1.0f / 60.0f), half(30, 1.0f / 30.0f), mixed;
		mixed.insert(mixed.end(), 10, 1.0f / 30.0f); // 1/3 s ...
		mixed.insert(mixed.end(), 40, 1.0f / 60.0f); // ... + 2/3 s = 1 s total

		// Same simulated second must land on the same value: the alpha
		// 1 - exp(-speed*dt) integrates the exponential exactly.
		const float a = run(3.0f, hour), b = run(3.0f, half), c = run(3.0f, mixed);
		if (std::abs(a - b) > 2e-3f || std::abs(a - c) > 2e-3f)
			ok = fail("adaptation must be frame-split independent (60x1/60 vs 30x1/30 vs mixed)");

		// Brightening scene (target < current) uses speedUp — still exact.
		const float d = run(-4.0f, hour), e = run(-4.0f, half);
		if (std::abs(d - e) > 2e-3f)
			ok = fail("speedUp adaptation must be frame-split independent too");
	}

	// --- Asymmetric rates: dilation is slower than stopping down ---
	{
		const autoexposure::Params p{};
		const auto oneSecond = [&](float target) {
			float cur = 0.0f;
			for (int i = 0; i < 60; ++i)
				cur = autoexposure::adaptLogExposure(cur, target, 1.0f / 60.0f, p);
			return std::abs(cur - target); // remaining distance in EV
		};
		// Scene darkening (exposure must RISE, speedDown 1.25/s) leaves more
		// distance after 1 s than scene brightening (speedUp 3.0/s).
		const float remainingDarkening = oneSecond(4.0f);
		const float remainingBrightening = oneSecond(-4.0f);
		if (!(remainingDarkening > remainingBrightening))
			ok = fail("speedDown (darkening) must adapt slower than speedUp (brightening)");

		// Alpha math spot check: 1 - exp(-3.0 * 0.5) = 0.7769.
		if (std::abs(autoexposure::adaptationAlpha(0.5f, 3.0f) - 0.7769f) > 1e-3f)
			ok = fail("adaptationAlpha(0.5, 3.0) must be ~0.7769 (1 - exp(-1.5))");
	}

	// --- dt = 0 and negative dt ---
	// dt <= 0 (paused frame) must leave the exposure STRICTLY unchanged:
	// the settle rule only fires on |target - adapted| < 1e-4 (arrival),
	// never on step size, so a zero step can never jump to the target.
	{
		const autoexposure::Params p{};
		if (autoexposure::adaptLogExposure(0.0f, 4.0f, 0.0f, p) != 0.0f)
			ok = fail("dt=0 must leave the current exposure unchanged");
		if (autoexposure::adaptLogExposure(0.0f, 4.0f, -1.0f, p) != 0.0f)
			ok = fail("negative dt must be treated as dt=0 (strict no-op)");
		// Already at the target: dt=0 keeps it there.
		if (autoexposure::adaptLogExposure(4.0f, 4.0f, 0.0f, p) != 4.0f)
			ok = fail("dt=0 at the target must stay exactly at the target");
		// Tiny dt far from the target: a small step must NOT snap.
		const float tiny = autoexposure::adaptLogExposure(0.0f, 4.0f, 1e-6f, p);
		if (tiny == 4.0f || std::abs(tiny - 0.0f) > 1e-4f)
			ok = fail("tiny dt far from the target must produce a tiny step, not a snap");
	}

	// --- Exact settle: no endless micro-adaptation on a static scene ---
	{
		const autoexposure::Params p{};
		float cur = 0.0f;
		bool settled = false;
		for (int i = 0; i < 500 && !settled; ++i)
		{
			cur = autoexposure::adaptLogExposure(cur, 4.0f, 0.1f, p);
			settled = (cur == 4.0f); // exact equality: the snap assigns the target
		}
		if (!settled)
			ok = fail("adaptation must settle exactly on the target within 500 frames");
		else
			for (int i = 0; i < 10; ++i)
			{
				cur = autoexposure::adaptLogExposure(cur, 4.0f, 0.1f, p);
				if (cur != 4.0f)
				{
					ok = fail("settled adaptation must stay exactly at the target");
					break;
				}
			}
	}

	// --- No overshoot for any dt ---
	{
		const autoexposure::Params p{};
		for (const float dt : {0.25f, 1.0f, 50.0f, 1e6f})
		{
			const float up = autoexposure::adaptLogExposure(0.0f, 4.0f, dt, p);
			const float down = autoexposure::adaptLogExposure(0.0f, -4.0f, dt, p);
			if (!(0.0f <= up && up <= 4.0f))
				ok = fail("adaptation toward +4 EV must stay between current and target");
			if (!(-4.0f <= down && down <= 0.0f))
				ok = fail("adaptation toward -4 EV must stay between current and target");
		}
	}

	// --- seedLogExposure: re-engaging auto starts from the manual exposure ---
	{
		if (std::abs(autoexposure::seedLogExposure(1.25f) - std::log2(1.25f)) > 1e-6f)
			ok = fail("seedLogExposure(1.25) must be log2(1.25)");
		const float clamped = autoexposure::seedLogExposure(0.0f);
		if (!std::isfinite(clamped) || std::abs(clamped - std::log2(autoexposure::kMinLuminanceEps)) > 1e-6f)
			ok = fail("seedLogExposure(0) must clamp to log2(kMinLuminanceEps) and stay finite");
		if (std::abs(autoexposure::seedLogExposure(-3.0f) - clamped) > 0.0f)
			ok = fail("seedLogExposure must clamp negative manual exposures identically");
	}

	// Default look: adaptation must not turn ambient-only rooms into daylight.
	{
		const autoexposure::Params p{};
		const auto exposed = [&](float luminance) {
			return luminance * std::exp2(autoexposure::targetLogExposure(std::log2(luminance), p));
		};
		if (std::abs(exposed(0.18f) - 0.18f) > 1e-5f)
			ok = fail("middle gray must remain middle gray, not map to HDR white");
		if (exposed(0.02f) > 0.045f || exposed(0.01f) >= exposed(0.02f))
			ok = fail("night/cave luminance must stay dark and retain relative brightness");
		if (exposed(0.02f) >= exposed(0.18f) * 0.3f)
			ok = fail("auto exposure must preserve separation between dark and daylight scenes");
	}

	// --- GLSL SSBO state layout ---
	{
		if (sizeof(autoexposure::ExposureGpuState) != 16)
			ok = fail("ExposureGpuState must be 16 bytes (GLSL std430 mirror)");
		if (offsetof(autoexposure::ExposureGpuState, adaptedExposure) != 0 ||
			offsetof(autoexposure::ExposureGpuState, targetExposure) != 4 ||
			offsetof(autoexposure::ExposureGpuState, meteredLogLum) != 8 ||
			offsetof(autoexposure::ExposureGpuState, clampState) != 12)
			ok = fail("ExposureGpuState member offsets must match the GLSL buffer declaration");
	}

	// --- CPU settings <-> math defaults stay in sync ---
	{
		const PostProcessSettings pp{};
		const autoexposure::Params p{};
		if (p.middleGrey != pp.autoExposureMiddleGrey || p.compensationEv != pp.exposureCompensation ||
			p.minEv != pp.autoExposureMinEv || p.maxEv != pp.autoExposureMaxEv ||
			p.speedUp != pp.autoExposureSpeedUp || p.speedDown != pp.autoExposureSpeedDown)
			ok = fail("Params defaults must equal the PostProcessSettings auto-exposure defaults");
		// Pin the literal values too, so accidental dual drift is caught.
		if (pp.exposureCompensation != 0.0f || pp.autoExposureMiddleGrey != 0.18f ||
			pp.autoExposureMinEv != -4.0f || pp.autoExposureMaxEv != 1.0f ||
			pp.autoExposureSpeedUp != 3.0f || pp.autoExposureSpeedDown != 1.25f)
			ok = fail("PostProcessSettings auto-exposure defaults drifted "
					  "(comp 0, middleGrey 0.18, minEv -4, maxEv +1, speedUp 3, speedDown 1.25)");
	}

	if (!ok)
	{
		std::cerr << "test_auto_exposure: FAILED\n";
		return EXIT_FAILURE;
	}
	std::cout << "test_auto_exposure: OK (EV semantics + clamps + adaptation + seed + GLSL layout + settings sync)\n";
	return EXIT_SUCCESS;
}
