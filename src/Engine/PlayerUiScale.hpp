#pragma once

// Player UI scaling — issue #302, Epic #298 Phase D.
//
// The player interface owns its own scale. It is NOT the ImGui/Developer UI
// scale (Engine/UiScale.hpp + ImGuiLayer): changing the player scale never
// touches the developer style, the developer docking layout, or the
// simulation. The two values are resolved from the same display, but they are
// stored, snapped and applied independently.
//
// Policy
//   * Auto (default): follow the OS content scale, capped so the authored
//     metrics always fit a 900x480 logical view, floored at 75%.
//   * Fixed: an explicit user override, snapped onto the 100–200% steps.
//
// Frames
//   `ScaleController::resolve` is called once at the frame boundary, before a
//   screen is built. The effective value only ever changes there, so a scale
//   change cancels the in-flight press and rebuilds the next layout — it never
//   invalidates the layout currently being painted (no mid-frame invalidation,
//   no stale hit tests).
//
// Pure and header-only (glm): the policy and the frame-boundary state machine
// are unit-tested without SDL, Vulkan or ImGui (tests/test_game_ui_style.cpp).

#include <glm/common.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

namespace gameui
{
enum class ScaleMode : uint8_t
{
	Auto = 0,
	Fixed,
};

/// The complete step grid. 0.75 is auto-only: an explicit override may not
/// shrink below 100% (legibility), while a very small window may.
inline constexpr float kAutoScales[] = {0.75f, 1.f, 1.25f, 1.5f, 1.75f, 2.f};
/// Steps the player may select as an override (100–200%).
inline constexpr float kUserScales[] = {1.f, 1.25f, 1.5f, 1.75f, 2.f};
inline constexpr float kMinScale = 0.75f;
inline constexpr float kMaxScale = 2.f;

/// Smallest logical view the authored metrics are designed for. Above it the
/// auto policy keeps the content scale; below it the UI steps down (and, at the
/// floor, clamps/scrolls rather than overflowing).
inline constexpr glm::vec2 kFitSize{900.f, 480.f};

/// A user-visible policy. `value` is ignored in Auto mode.
struct ScalePolicy
{
	ScaleMode mode{ScaleMode::Auto};
	float value{1.f};

	bool operator==(const ScalePolicy &) const = default;
};

struct ScaleInputs
{
	glm::vec2 logicalSize{}; // SDL logical window size (not framebuffer pixels)
	float contentScale{1.f};
	/// False when the platform could not report the DPI (query failure,
	/// non-positive or non-finite value). Auto then keeps the last valid scale
	/// instead of being handed a laundered 100% (issue #302 R02).
	bool contentScaleValid{true};
};

inline float clampScale(float scale)
{
	if (!(scale > 0.f)) // also catches NaN
		return 1.f;
	return std::clamp(scale, kMinScale, kMaxScale);
}

/// OS content scale from the two SDL queries the host already performs:
/// display scale (DPI) divided by pixel density. Non-positive or non-finite
/// inputs fall back to 100%.
inline bool contentScaleValid(float displayScale, float pixelDensity)
{
	return displayScale > 0.f && pixelDensity > 0.f && std::isfinite(displayScale) &&
		std::isfinite(pixelDensity);
}

/// Value-only helper (100% fallback). Hosts that must distinguish "100%" from
/// "the query failed" use `scaleInputs` instead.
inline float contentScale(float displayScale, float pixelDensity)
{
	if (!contentScaleValid(displayScale, pixelDensity))
		return 1.f;
	return displayScale / pixelDensity;
}

/// Platform queries -> controller inputs, preserving invalidity (R02).
inline ScaleInputs scaleInputs(glm::vec2 logicalSize, float displayScale, float pixelDensity)
{
	ScaleInputs inputs{};
	inputs.logicalSize = logicalSize;
	inputs.contentScaleValid = contentScaleValid(displayScale, pixelDensity);
	inputs.contentScale = inputs.contentScaleValid ? displayScale / pixelDensity : 1.f;
	return inputs;
}

/// Largest step at or below `scale` (clamped into the grid).
inline float snapDown(float scale)
{
	scale = clampScale(scale);
	float result = kMinScale;
	for (const float step : kAutoScales)
		if (step <= scale + 0.0001f)
			result = step;
	return result;
}

/// Nearest user-selectable step (100–200%) to an arbitrary request.
inline float nearestUserScale(float scale)
{
	scale = clampScale(scale);
	float best = kUserScales[0];
	float bestDistance = std::abs(scale - best);
	for (const float step : kUserScales)
	{
		const float distance = std::abs(scale - step);
		if (distance < bestDistance)
		{
			best = step;
			bestDistance = distance;
		}
	}
	return best;
}

inline bool isUserScale(float scale)
{
	return std::any_of(std::begin(kUserScales), std::end(kUserScales),
		[scale](float step) { return std::abs(step - scale) < 0.001f; });
}

/// True when both inputs describe a real window: a minimized 0x0 window and a
/// lost DPI query are "no information", not "tiny window".
inline bool validScaleInputs(ScaleInputs inputs)
{
	return inputs.contentScaleValid && inputs.logicalSize.x > 0.f && inputs.logicalSize.y > 0.f &&
		std::isfinite(inputs.logicalSize.x) && std::isfinite(inputs.logicalSize.y) &&
		inputs.contentScale > 0.f && std::isfinite(inputs.contentScale);
}

/// Auto policy: the content scale, capped so the authored metrics fit the
/// window, snapped down onto a supported step.
///
///   fit = min(width / 900, height / 480)          (the authored fit box)
///   scale = snapDown(min(contentScale, fit))
///
/// There is no branch at `fit >= 1`: crossing the reference size is continuous,
/// which is what makes the same window keep the same scale while it is resized
/// around 900x480 (issue #302 F04). Degenerate inputs fall back to the snapped
/// content scale; `ScaleController` keeps the last valid value instead.
inline float autoScale(ScaleInputs inputs)
{
	const float base = snapDown(inputs.contentScale);
	if (!validScaleInputs(inputs))
		return base;
	const float fit = std::min(inputs.logicalSize.x / kFitSize.x, inputs.logicalSize.y / kFitSize.y);
	if (!(fit > 0.f) || !std::isfinite(fit))
		return base;
	return snapDown(std::min(base, fit));
}

/// Resolve a policy against one frame's inputs. A Fixed policy is snapped onto
/// a user step, so a corrupt or hand-edited persisted value still resolves.
inline float resolveScale(ScalePolicy policy, ScaleInputs inputs)
{
	return policy.mode == ScaleMode::Fixed ? nearestUserScale(policy.value) : autoScale(inputs);
}

/// Frame-boundary owner of the effective player UI scale.
class ScaleController
{
public:
	explicit ScaleController(ScalePolicy policy = {}) : m_policy(policy) {}

	/// User request. Applied at the next resolve, never mid-frame.
	void setPolicy(ScalePolicy policy) { m_pending = policy; m_hasPending = true; }

	/// Resolves this frame's scale. Returns true when the effective value
	/// changed (the host must then cancel in-flight input and rebuild layout).
	///
	/// An Auto policy with invalid inputs (minimized window, lost DPI query)
	/// keeps the last valid Auto scale instead of jumping to the 100% fallback,
	/// so restoring the window never flashes a different layout (F04). Fixed mode
	/// never looks at the window size at all.
	bool resolve(ScaleInputs inputs)
	{
		if (m_hasPending)
		{
			m_policy = m_pending;
			m_hasPending = false;
		}
		m_inputs = inputs;
		if (m_policy.mode == ScaleMode::Auto && !validScaleInputs(inputs) && m_hasAutoScale)
		{
			// No fresh information: keep the last valid Auto scale. If the policy
			// just switched back from Fixed, restore it instead of resurrecting
			// the override.
			m_resolved = true;
			if (std::abs(m_lastAuto - m_effective) < 0.0001f)
				return false;
			m_effective = m_lastAuto;
			++m_revision;
			return true;
		}
		const float next = resolveScale(m_policy, inputs);
		if (m_policy.mode == ScaleMode::Auto && validScaleInputs(inputs))
		{
			m_lastAuto = next;
			m_hasAutoScale = true;
		}
		if (m_resolved && std::abs(next - m_effective) < 0.0001f)
		{
			m_resolved = true;
			return false;
		}
		m_resolved = true;
		m_effective = next;
		++m_revision;
		return true;
	}

	ScalePolicy policy() const { return m_policy; }
	ScalePolicy pendingPolicy() const { return m_hasPending ? m_pending : m_policy; }
	bool hasPending() const { return m_hasPending; }
	float effective() const { return m_effective; }
	ScaleInputs inputs() const { return m_inputs; }
	uint64_t revision() const { return m_revision; }

private:
	ScalePolicy m_policy{};
	ScalePolicy m_pending{};
	ScaleInputs m_inputs{};
	float m_effective{1.f};
	float m_lastAuto{1.f};
	uint64_t m_revision{};
	bool m_hasPending{}, m_resolved{}, m_hasAutoScale{};
};
} // namespace gameui
