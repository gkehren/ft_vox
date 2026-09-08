#pragma once

/// HDR auto-exposure (issue #140): CPU reference math + GPU state layout.
///
/// The live implementation runs entirely on the GPU (no per-frame CPU/GPU
/// sync): a graphics reduction meters the HDR scene before tone mapping and
/// exposure_adapt.frag.glsl integrates the temporal adaptation. The formulas
/// below are the CPU mirror — unit-tested in tests/test_auto_exposure.cpp —
/// and MUST stay in sync with the GLSL.
///
/// Metering policy — clipped log-average:
///   HDR (R16G16B16A16) -> 64x64 -> 16x16 -> 4x4 (luminance_downsample.frag,
///   16 spread taps per stage, each sample's log2 luminance clipped to
///   [kMinMeteredLogLum, kMaxMeteredLogLum]) -> exposure_adapt.frag, which
///   averages the 4x4 into the meter reading. Clipping the log bounds how
///   much a handful of sun-disc / emissive pixels can move the mean.
///
/// Target exposure (all in EV, log2):
///   targetEv = log2(middleGrey) - meteredLogLum + compensationEv
///   targetEv = clamp(targetEv, minEv, maxEv)
///   exposure = 2^targetEv   (composite multiplies HDR color by it)
///
/// Temporal adaptation (frame-rate independent):
///   alpha    = 1 - exp(-speed * dt)
///   adapted  = mix(adapted, target, alpha)   (in log2 space)
///   speed    = speedUp   when the scene brightens (exposure must drop)
///            = speedDown when the scene darkens   (exposure must rise)
///   dt <= 0 leaves the state strictly unchanged; the exact-settle rule
///   fires only when |target - adapted| < 1e-4 (arrival, never step size).
///
/// GPU state model: ONE shared history buffer holds the temporal state (a
/// single logical value across frames-in-flight; see PostStack::recordExposure
/// for the ordering argument), plus per-frame-in-flight CPU-visible snapshots
/// used only for the debug readout.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace autoexposure
{
/// Per-frame-in-flight GPU state, written by exposure_adapt.frag, read by
/// composite.frag (adaptedExposure) and by the CPU for debug readouts after
/// the owning frame's fence has been waited. Keep in sync with the GLSL
/// buffer declaration — scalar members only so std430 matches trivially.
struct ExposureGpuState
{
	float adaptedExposure{1.25f}; ///< linear exposure multiplier used by composite
	float targetExposure{1.25f};  ///< clamped target exposure this frame (debug)
	float meteredLogLum{0.0f};	///< log2 of clipped geometric-mean luminance (debug)
	uint32_t clampState{0};		  ///< 0 = in range, 1 = min clamp, 2 = max clamp (debug)
};
static_assert(sizeof(ExposureGpuState) == 16, "must match the GLSL std430 layout");

/// Metering clip window, in EV of scene luminance. Mirrored in
/// luminance_downsample.frag.glsl (kMinMeteredLogLum / kMaxMeteredLogLum /
/// kMinLuminanceEps).
inline constexpr float kMinMeteredLogLum = -8.0f;
inline constexpr float kMaxMeteredLogLum = 8.0f;
inline constexpr float kMinLuminanceEps = 1e-4f;

/// Auto-exposure tunables (mirrors the PostProcessSettings auto* fields).
struct Params
{
	float middleGrey{1.0f};	 ///< scene luminance mapped to exposure 1.0
	float compensationEv{0.0f}; ///< exposure compensation in EV stops (+1 = 2x brighter)
	float minEv{-4.0f};			///< target exposure clamp, low (exposure = 2^minEv)
	float maxEv{4.0f};			///< target exposure clamp, high
	float speedUp{3.0f};		///< 1/s while the scene brightens (exposure drops)
	float speedDown{1.25f};		///< 1/s while the scene darkens (exposure rises)
};

/// 0 = target in range, 1 = clamped at minEv, 2 = clamped at maxEv.
inline uint32_t clampStateFor(float targetLogEv, const Params &p)
{
	if (targetLogEv <= p.minEv)
		return 1;
	if (targetLogEv >= p.maxEv)
		return 2;
	return 0;
}

/// Log2 of the clamped target exposure for a meter reading. clampState
/// (optional) reports whether the raw target hit the limits.
inline float targetLogExposure(float meteredLogLum, const Params &p,
							   uint32_t *clampState = nullptr)
{
	const float raw = std::log2(std::max(p.middleGrey, kMinLuminanceEps)) -
					  meteredLogLum + p.compensationEv;
	if (clampState)
		*clampState = clampStateFor(raw, p);
	return std::clamp(raw, p.minEv, p.maxEv);
}

/// Frame-rate independent exponential blend factor.
inline float adaptationAlpha(float dt, float speed)
{
	return 1.0f - std::exp(-std::max(speed, 0.0f) * std::max(dt, 0.0f));
}

/// One adaptation step in log2 space. dt <= 0 (paused frame) leaves the
/// state strictly unchanged; the exact-settle rule fires only when the
/// ADAPTED value is already within 1e-4 EV of the target — never on step
/// size, so tiny steps can never snap the exposure to the target.
inline float adaptLogExposure(float currentLogEv, float targetLogEv, float dt,
							  const Params &p)
{
	if (dt <= 0.0f)
		return currentLogEv;
	const float speed = targetLogEv < currentLogEv ? p.speedUp : p.speedDown;
	float next = currentLogEv + (targetLogEv - currentLogEv) * adaptationAlpha(dt, speed);
	if (std::abs(targetLogEv - next) < 1e-4f)
		next = targetLogEv;
	return next;
}

/// Re-engaging auto mode seeds the adaptation from the manual exposure so
/// the transition has no hidden jump.
inline float seedLogExposure(float manualExposure)
{
	return std::log2(std::max(manualExposure, kMinLuminanceEps));
}

} // namespace autoexposure
