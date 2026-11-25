#pragma once

// Player (Game UI) design tokens — issue #302, Epic #298 Phase D.
//
// One baseline theme for the whole player interface: colors, typography,
// spacing, sizing, corner/border treatment and overlay opacity, plus the
// single resolution rule every component shares for hover/focus/pressed/
// disabled/selected/error/pending. There is deliberately no second theme:
// the epic asks for one coherent baseline, not speculative palettes.
//
// Pure and header-only (glm + the standard library): every token, the state
// precedence and the contrast gate are unit-tested without SDL, Vulkan or
// ImGui (tests/test_game_ui_style.cpp), matching the UiScale.hpp /
// InventoryUi.hpp policy-header pattern.
//
// Colors are authored in sRGB with linear alpha, the same convention the
// native renderer expects (see Renderer/GameUiDrawList.hpp). Contrast helpers
// linearize before comparing, so a token pair can be asserted against the
// WCAG thresholds in the tests instead of being eyeballed.

#include <glm/common.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gameui::style
{
// --- Color helpers ---------------------------------------------------------

inline float srgbToLinear(float channel)
{
	channel = std::clamp(channel, 0.f, 1.f);
	return channel <= 0.04045f ? channel / 12.92f : std::pow((channel + 0.055f) / 1.055f, 2.4f);
}

inline glm::vec3 srgbToLinear(glm::vec3 color)
{
	return {srgbToLinear(color.r), srgbToLinear(color.g), srgbToLinear(color.b)};
}

/// WCAG 2.x relative luminance of an sRGB color.
inline float relativeLuminance(glm::vec3 color)
{
	const glm::vec3 linear = srgbToLinear(color);
	return 0.2126f * linear.r + 0.7152f * linear.g + 0.0722f * linear.b;
}

/// WCAG contrast ratio (1..21) between two sRGB colors.
inline float contrastRatio(glm::vec3 a, glm::vec3 b)
{
	const float first = relativeLuminance(a), second = relativeLuminance(b);
	const float lighter = std::max(first, second), darker = std::min(first, second);
	return (lighter + 0.05f) / (darker + 0.05f);
}

/// Source-over composite in sRGB space with a linear alpha, the order the
/// renderer blends SCRGB-authored tokens onto the scene.
inline glm::vec3 composite(glm::vec3 over, glm::vec3 under, float alpha)
{
	alpha = std::clamp(alpha, 0.f, 1.f);
	return over * alpha + under * (1.f - alpha);
}

inline glm::vec3 rgb(glm::vec4 color)
{
	return {color.r, color.g, color.b};
}

/// WCAG AA floors used by the tests and the design-system gallery.
inline constexpr float kMinBodyContrast = 4.5f;
inline constexpr float kMinSecondaryContrast = 3.f;
/// Disabled content is intentionally below the floor but must stay perceivable.
inline constexpr float kMinDisabledContrast = 2.f;

// --- Palette ---------------------------------------------------------------

/// Foreground/background/status tokens. One accent (cool mint) separates the
/// player interface from the blue developer chrome (Engine/UiTheme.hpp) while
/// keeping the same cool-dark base.
struct Palette
{
	glm::vec4 ink;          // primary text on player surfaces
	glm::vec4 inkMuted;     // secondary text, captions, trailing values
	glm::vec4 inkDisabled;  // disabled labels and glyphs
	glm::vec4 inkOnAccent;  // text on an accent-filled control
	glm::vec4 canvas;       // opaque full-screen backdrop behind a screen
	glm::vec4 surface;      // card face
	glm::vec4 surfaceRaised;// headers, hovered/selected rows, fields
	glm::vec4 surfaceSunken;// wells, slots, list bodies
	glm::vec4 border;       // resting outline
	glm::vec4 borderStrong; // selected outline
	glm::vec4 accent;       // primary action, selection, progress
	glm::vec4 accentPressed;// pressed primary action
	glm::vec4 focus;        // keyboard focus ring
	glm::vec4 scrim;        // modal scrim (rgba; alpha is the token)
	glm::vec4 hudScrim;     // HUD text backdrop over the live scene
	glm::vec4 ok;
	glm::vec4 warning;
	glm::vec4 error;
	glm::vec4 info;
	glm::vec4 pending;
};

// --- Typography ------------------------------------------------------------

/// Display roles are measured from the one audited face; the renderer scales
/// the baked glyph table by pixels / kFontPixels (see PlayerUiText.hpp).
inline constexpr float kFontPixels = 13.f;

enum class TextRole : uint8_t { Caption = 0, Body, Heading, Title };
inline constexpr size_t kTextRoleCount = 4;

struct TypeToken
{
	float pixels;
	float lineHeight;
};

inline constexpr TypeToken typeToken(TextRole role)
{
	switch (role)
	{
	case TextRole::Caption: return {11.f, 14.f};
	case TextRole::Body: return {13.f, 17.f};
	case TextRole::Heading: return {18.f, 23.f};
	case TextRole::Title: return {26.f, 32.f};
	}
	return {13.f, 17.f};
}

/// Scale multiplier applied to the baked glyph table for a role.
inline float typeScale(TextRole role)
{
	return typeToken(role).pixels / kFontPixels;
}

inline float lineHeight(TextRole role)
{
	return typeToken(role).lineHeight;
}

// --- Spacing, sizing, radii, opacity --------------------------------------

struct Spacing
{
	float xxs; // 2 — hairline gaps, count offsets
	float xs;  // 4 — inline gaps, slot gaps
	float sm;  // 6 — label/value gaps
	float md;  // 8 — row padding, control padding
	float lg;  // 12 — section padding
	float xl;  // 16 — card padding
};

struct Sizing
{
	float controlHeight;     // text field / slider row
	float buttonHeight;      // primary and secondary buttons
	float rowHeight;         // list rows
	float sectionHeader;     // section/header strip
	float slot;              // inventory slot edge (hotbar + grid)
	float slotGap;           // inventory slot gap
	float hotbarBottom;      // HUD hotbar distance from the viewport bottom
	float crosshair;         // HUD crosshair arm length
	float borderWidth;       // resting outline
	float borderStrongWidth; // selected outline
	float focusRing;         // keyboard focus ring
	float radiusSmall;       // 2 — slots, list rows, fields
	float radiusMedium;      // 4 — buttons, cards
	float radiusLarge;       // 6 — modal cards, toasts
};

struct Overlay
{
	float card;     // card face opacity over the scene
	float raised;   // header/row opacity
	float scrim;    // modal scrim opacity
	float hudScrim; // HUD text backdrop opacity
	float toast;    // toast face opacity
};

// --- Theme -----------------------------------------------------------------

struct Theme
{
	Palette palette;
	Spacing space;
	Sizing size;
	Overlay overlay;
};

/// The single baseline theme. Authored at 100%; the player UI scale is applied
/// by the callers (see PlayerUiScale.hpp) so tokens stay resolution-free.
inline const Theme &baseline()
{
	static const Theme theme = [] {
		Theme t{};
		// Cool dark slate base; the mint accent is the only saturated color.
		t.palette.ink = {0.941f, 0.969f, 1.f, 1.f};
		t.palette.inkMuted = {0.702f, 0.769f, 0.812f, 1.f};
		t.palette.inkDisabled = {0.408f, 0.451f, 0.478f, 1.f};
		t.palette.inkOnAccent = {0.031f, 0.055f, 0.071f, 1.f};
		t.palette.canvas = {0.027f, 0.039f, 0.050f, 1.f};
		t.palette.surface = {0.052f, 0.083f, 0.103f, 1.f};
		t.palette.surfaceRaised = {0.192f, 0.341f, 0.369f, 1.f};
		t.palette.surfaceSunken = {0.033f, 0.055f, 0.070f, 1.f};
		t.palette.border = {0.157f, 0.226f, 0.259f, 1.f};
		t.palette.borderStrong = {0.310f, 0.878f, 0.760f, 1.f};
		t.palette.accent = {0.310f, 0.878f, 0.760f, 1.f};
		t.palette.accentPressed = {0.226f, 0.702f, 0.612f, 1.f};
		t.palette.focus = {0.941f, 0.969f, 1.f, 1.f};
		t.palette.scrim = {0.016f, 0.024f, 0.031f, 0.680f};
		t.palette.hudScrim = {0.020f, 0.031f, 0.047f, 0.620f};
		t.palette.ok = {0.478f, 0.847f, 0.541f, 1.f};
		t.palette.warning = {0.961f, 0.769f, 0.353f, 1.f};
		t.palette.error = {0.929f, 0.443f, 0.396f, 1.f};
		t.palette.info = {0.522f, 0.757f, 0.949f, 1.f};
		t.palette.pending = {0.694f, 0.639f, 0.910f, 1.f};

		t.space = {2.f, 4.f, 6.f, 8.f, 12.f, 16.f};
		t.size = {
			.controlHeight = 28.f,
			.buttonHeight = 28.f,
			.rowHeight = 24.f,
			.sectionHeader = 22.f,
			.slot = 36.f,
			.slotGap = 4.f,
			.hotbarBottom = 10.f,
			.crosshair = 5.f,
			.borderWidth = 1.f,
			.borderStrongWidth = 2.f,
			.focusRing = 2.f,
			.radiusSmall = 2.f,
			.radiusMedium = 4.f,
			.radiusLarge = 6.f,
		};
		t.overlay = {.card = 0.960f, .raised = 1.f, .scrim = 0.680f, .hudScrim = 0.620f, .toast = 0.940f};
		return t;
	}();
	return theme;
}

// --- Interactive state -----------------------------------------------------

/// Bit flags, not an ordered enum: a control is frequently several at once
/// (focused + hovered + selected). Precedence is resolved by the resolvers
/// below, never by the caller.
enum class State : uint8_t
{
	Rest = 0,
	Hovered = 1u << 0,
	Selected = 1u << 1,
	Pressed = 1u << 2,
	Focused = 1u << 3,
	Disabled = 1u << 4,
	Pending = 1u << 5,
	Error = 1u << 6,
};

inline constexpr State operator|(State a, State b)
{
	return static_cast<State>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline constexpr State &operator|=(State &a, State b)
{
	return a = a | b;
}

inline constexpr bool has(State set, State flag)
{
	return (static_cast<uint8_t>(set) & static_cast<uint8_t>(flag)) != 0;
}

inline constexpr bool disabled(State state)
{
	return has(state, State::Disabled);
}

/// Disabled is final: it clears the *interaction* flags (focus, hover, press)
/// so a disabled control can never paint an active focus ring or hover face.
/// Selection survives, because a selected-but-disabled row or slot still tells
/// the player what is selected (issue #302 F15).
inline constexpr State settleState(State state)
{
	if (!disabled(state))
		return state;
	const auto interaction = static_cast<uint8_t>(State::Focused | State::Hovered | State::Pressed);
	const auto flags = static_cast<uint8_t>(state) & static_cast<uint8_t>(~interaction);
	return static_cast<State>(flags);
}

/// The flag combination for an interactive control. One function so the
/// button, row, field, toggle, slider and slot components cannot drift apart.
inline constexpr State interactiveState(bool enabled, bool hovered, bool pressed, bool focused, bool selected)
{
	State state = State::Rest;
	if (enabled && hovered)
		state |= State::Hovered;
	if (enabled && pressed)
		state |= State::Pressed;
	if (enabled && focused)
		state |= State::Focused;
	if (selected)
		state |= State::Selected;
	if (!enabled)
		state |= State::Disabled;
	return settleState(state);
}

/// A slot arrives with its own combination (selected, disabled, pending, error,
/// focused). One precedence: disabled clears focus, validation outranks
/// selection for the border, selection outranks focus.
inline constexpr State slotState(State state)
{
	return settleState(state);
}

// --- Resolved component tokens --------------------------------------------

enum class ButtonVariant : uint8_t { Primary = 0, Secondary, Quiet, Danger };

struct ControlTokens
{
	glm::vec4 face;
	glm::vec4 text;
	glm::vec4 border;
};

/// Face/text/border for a button. Precedence: disabled beats every other
/// state, then pressed, then primary/selected accent, then hover. Focus is a
/// ring drawn around the face, so it never changes the fill.
inline ControlTokens buttonTokens(const Theme &theme, ButtonVariant variant, State state)
{
	const Palette &p = theme.palette;
	const bool danger = variant == ButtonVariant::Danger;
	const glm::vec4 accent = danger ? p.error : p.accent;
	const glm::vec4 accentPressed = danger
		? glm::vec4(p.error.r * 0.82f, p.error.g * 0.82f, p.error.b * 0.82f, p.error.a)
		: p.accentPressed;
	ControlTokens out{};
	if (disabled(state))
		return {p.surfaceSunken, p.inkDisabled, p.border};
	if (variant == ButtonVariant::Primary || danger)
	{
		out.face = has(state, State::Pressed) ? accentPressed : accent;
		out.text = p.inkOnAccent;
		out.border = danger ? p.error : p.borderStrong;
		return out;
	}
	if (variant == ButtonVariant::Quiet)
	{
		out.face = has(state, State::Pressed) ? p.surfaceRaised : (has(state, State::Hovered) ? p.surfaceRaised : glm::vec4(0.f));
		out.face.a = has(state, State::Hovered) || has(state, State::Pressed) ? 0.55f : 0.f;
		out.text = has(state, State::Hovered) || has(state, State::Pressed) ? p.ink : p.inkMuted;
		out.border = glm::vec4(0.f);
		return out;
	}
	out.face = has(state, State::Pressed) ? p.accentPressed : (has(state, State::Hovered) ? p.surfaceRaised : p.surface);
	out.text = has(state, State::Pressed) ? p.inkOnAccent : p.ink;
	out.border = has(state, State::Hovered) || has(state, State::Pressed) ? p.borderStrong : p.border;
	return out;
}

struct RowTokens
{
	glm::vec4 face;
	glm::vec4 text;
	glm::vec4 trailing;
};

/// List row: selection outranks hover (a hovered row inside a selected row
/// must stay visibly selected), hover raises the face.
inline RowTokens rowTokens(const Theme &theme, State state)
{
	const Palette &p = theme.palette;
	if (disabled(state))
		return {{0.f, 0.f, 0.f, 0.f}, p.inkDisabled, p.inkDisabled};
	const bool active = has(state, State::Selected) || has(state, State::Hovered) || has(state, State::Pressed);
	return {active ? p.surfaceRaised : glm::vec4(0.f, 0.f, 0.f, 0.f),
		has(state, State::Selected) ? p.ink : p.inkMuted,
		has(state, State::Selected) ? p.ink : p.inkMuted};
}

/// Form-field validation states. Pending is the "authority has not answered
/// yet" presentation (inventory/admin commands), Error the rejected one.
enum class Validation : uint8_t { None = 0, Pending, Error };

/// Toggle indicator face: off is the sunken well, on is the accent, disabled is
/// never accent (it reads as unavailable, not as "on"). Hover raises the off
/// face and pressed darkens the on face (R06).
inline glm::vec4 toggleFace(const Theme &theme, State state, bool on)
{
	state = settleState(state);
	if (disabled(state))
		return theme.palette.surfaceSunken;
	if (!on)
		return has(state, State::Hovered) || has(state, State::Pressed) ? theme.palette.surfaceRaised
																		: theme.palette.surfaceSunken;
	return has(state, State::Pressed) ? theme.palette.accentPressed : theme.palette.accent;
}

/// Slider fill: the accent, the pressed accent while dragging, or the disabled
/// ink so a dead slider never looks draggable (R06).
inline glm::vec4 sliderFill(const Theme &theme, State state)
{
	state = settleState(state);
	if (disabled(state))
		return theme.palette.inkDisabled;
	return has(state, State::Pressed) ? theme.palette.accentPressed : theme.palette.accent;
}

/// Slider track: the sunken well, raised on hover and while dragging (R06).
inline glm::vec4 sliderTrackFace(const Theme &theme, State state)
{
	state = settleState(state);
	if (disabled(state))
		return theme.palette.surfaceSunken;
	return has(state, State::Hovered) || has(state, State::Pressed) ? theme.palette.surfaceRaised
																	: theme.palette.surfaceSunken;
}

/// Field: sunken well, border color carries the validation state. A focused
/// field with no validation feedback keeps the neutral focus ink so validation
/// outranks focus (a rejected value must stay visibly rejected while typing).
inline ControlTokens fieldTokens(const Theme &theme, State state, Validation validation)
{
	const Palette &p = theme.palette;
	ControlTokens out{};
	out.face = p.surfaceSunken;
	out.text = disabled(state) ? p.inkDisabled : p.ink;
	if (validation == Validation::Error)
		out.border = p.error;
	else if (validation == Validation::Pending)
		out.border = p.pending;
	else if (has(state, State::Focused))
		out.border = p.focus;
	else
		out.border = p.border;
	return out;
}

/// Resolved slot presentation (issue #302 R06). One precedence for combined
/// Selected / Disabled / Focused / Error / Pending states:
///
///   1. disabled clears the interaction flags (see settleState) and dims the
///      content; it keeps the selection marker because selection is a fact, not
///      an affordance;
///   2. validation (Error > Pending) owns the border COLOR, so focus can never
///      hide a rejected or in-flight value;
///   3. focus owns the border WIDTH (the ring), which stays visible on top of a
///      validation color;
///   4. selection owns the resting border strength.
struct SlotTokens
{
	glm::vec4 face;
	glm::vec4 border;
	float borderWidth;
	glm::vec4 tint;      // icon tint
	glm::vec4 indicator; // badge / foreground ink
};

inline SlotTokens slotTokens(const Theme &theme, State requested)
{
	const auto &p = theme.palette;
	const auto state = settleState(requested);
	SlotTokens tokens{p.surfaceSunken, p.border, theme.size.borderWidth, glm::vec4(1.f), p.ink};
	const bool selected = has(state, State::Selected);
	tokens.border = selected ? p.borderStrong : p.border;
	tokens.borderWidth = selected ? theme.size.borderStrongWidth : theme.size.borderWidth;
	if (has(state, State::Error))
		tokens.border = p.error;
	else if (has(state, State::Pending))
		tokens.border = p.pending;
	if (has(state, State::Focused))
	{
		if (!has(state, State::Error) && !has(state, State::Pending))
			tokens.border = p.focus;
		tokens.borderWidth = theme.size.focusRing;
	}
	if (disabled(state))
	{
		tokens.tint = {1.f, 1.f, 1.f, 0.45f};
		tokens.indicator = p.inkDisabled;
	}
	return tokens;
}

inline glm::vec4 validationColor(const Theme &theme, Validation validation)
{
	const Palette &p = theme.palette;
	switch (validation)
	{
	case Validation::Pending: return p.pending;
	case Validation::Error: return p.error;
	case Validation::None: break;
	}
	return p.inkMuted;
}

// --- Status ----------------------------------------------------------------

/// Status semantics for the status indicator, toasts and progress feedback.
enum class Status : uint8_t { Info = 0, Ok, Warning, Error, Pending };

inline glm::vec4 statusColor(const Theme &theme, Status status)
{
	const Palette &p = theme.palette;
	switch (status)
	{
	case Status::Info: return p.info;
	case Status::Ok: return p.ok;
	case Status::Warning: return p.warning;
	case Status::Error: return p.error;
	case Status::Pending: return p.pending;
	}
	return p.info;
}

// --- Scene backdrops (contrast contract) ----------------------------------

/// Extreme live-scene colors the HUD has to stay readable over: midday sky /
/// sunlit snow, and a night-time cave or unlit grass silhouette. Keeping them
/// as tokens makes the readability requirement testable instead of anecdotal.
struct SceneBackdrop
{
	glm::vec3 bright; // 0.86,0.90,0.95 — sunlit sky / snow
	glm::vec3 dark;   // 0.031,0.047,0.067 — night, cave
};

inline constexpr SceneBackdrop kSceneBackdrop{
	.bright = {0.863f, 0.902f, 0.949f},
	.dark = {0.031f, 0.047f, 0.067f},
};

/// Legible text color over the live scene: the HUD paints its text on the
/// hudScrim token, and this resolves the backdrop the tokens were designed for.
inline glm::vec3 hudBackdrop(const Theme &theme, bool bright)
{
	const glm::vec3 scene = bright ? kSceneBackdrop.bright : kSceneBackdrop.dark;
	return composite(rgb(theme.palette.hudScrim), scene, theme.palette.hudScrim.a);
}

/// Contrast of a HUD text token over the scrimmed scene.
inline float hudContrast(const Theme &theme, glm::vec4 text, bool bright)
{
	return contrastRatio(rgb(text), hudBackdrop(theme, bright));
}
} // namespace gameui::style
