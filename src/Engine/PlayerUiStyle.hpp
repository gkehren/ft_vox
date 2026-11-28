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

inline float linearToSrgb(float channel)
{
	return channel <= 0.0031308f ? 12.92f * channel : 1.055f * std::pow(channel, 1.f / 2.4f) - 0.055f;
}

/// Blend in linear light and return sRGB, matching the native UI attachments.
inline glm::vec3 composite(glm::vec3 over, glm::vec3 under, float alpha)
{
	alpha = std::clamp(alpha, 0.f, 1.f);
	const glm::vec3 linear = srgbToLinear(over) * alpha + srgbToLinear(under) * (1.f - alpha);
	return {linearToSrgb(linear.r), linearToSrgb(linear.g), linearToSrgb(linear.b)};
}

inline glm::vec3 rgb(glm::vec4 color)
{
	return {color.r, color.g, color.b};
}

/// WCAG AA floors used by the tests and the design-system gallery.
inline constexpr float kMinBodyContrast = 4.5f;
inline constexpr float kMinSecondaryContrast = kMinBodyContrast;
inline constexpr float kMinDisabledContrast = kMinBodyContrast;
inline constexpr float kMinIndicatorContrast = 3.f;

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
	glm::vec4 control;      // secondary controls and hovered slots
	glm::vec4 divider;      // decorative section rule
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
inline constexpr float kFontPixels = 15.f;

enum class TextRole : uint8_t { Caption = 0, Body, Heading, Title, Count, Action };
inline constexpr size_t kTextRoleCount = 6;

struct TypeToken
{
	float pixels;
	float lineHeight;
	bool semibold;
};

inline constexpr TypeToken typeToken(TextRole role)
{
	switch (role)
	{
	case TextRole::Caption: return {13.f, 16.f, false};
	case TextRole::Body: return {15.f, 20.f, false};
	case TextRole::Heading: return {18.f, 23.f, true};
	case TextRole::Title: return {24.f, 30.f, true};
	case TextRole::Count: return {14.f, 17.f, true};
	case TextRole::Action: return {15.f, 20.f, true};
	}
	return {15.f, 20.f, false};
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
	float textSelection; // accent opacity behind regular and composed glyphs
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
		const auto srgb = [](uint32_t hex, float alpha = 1.f) -> glm::vec4 {
			return {float((hex >> 16) & 255) / 255.f, float((hex >> 8) & 255) / 255.f,
				float(hex & 255) / 255.f, alpha};
		};
		t.palette.ink = srgb(0xF4F6EF);
		t.palette.inkMuted = srgb(0xB7C7BD);
		t.palette.inkDisabled = srgb(0x96A69C);
		t.palette.inkOnAccent = srgb(0x151B19);
		t.palette.canvas = srgb(0x141A18);
		t.palette.surface = srgb(0x202625);
		t.palette.surfaceRaised = srgb(0x2D3531);
		t.palette.control = srgb(0x303A35);
		t.palette.divider = srgb(0x46594F);
		t.palette.surfaceSunken = srgb(0x151B19);
		t.palette.border = srgb(0x70887D);
		t.palette.borderStrong = srgb(0xABE5C8);
		t.palette.accent = srgb(0xABE5C8);
		t.palette.accentPressed = srgb(0x85BAA0);
		t.palette.focus = srgb(0xFFFBEF);
		t.palette.scrim = srgb(0x0C1310, 0.78f);
		t.palette.hudScrim = t.palette.surfaceSunken;
		t.palette.ok = srgb(0xA4DAA8);
		t.palette.warning = srgb(0xF3CE82);
		t.palette.error = srgb(0xFFAD9C);
		t.palette.info = srgb(0xA4CDEA);
		t.palette.pending = srgb(0xC9BAED);

		t.space = {2.f, 4.f, 6.f, 8.f, 12.f, 16.f};
		t.size = {
			.controlHeight = 28.f,
			.buttonHeight = 28.f,
			.rowHeight = 24.f,
			.sectionHeader = 22.f,
			.slot = 36.f,
			.slotGap = 4.f,
			.hotbarBottom = 14.f,
			.crosshair = 5.f,
			.borderWidth = 1.f,
			.borderStrongWidth = 2.f,
			.focusRing = 2.f,
			.radiusSmall = 0.f,
			.radiusMedium = 0.f,
			.radiusLarge = 0.f,
		};
		t.overlay = {.card = 1.f, .raised = 1.f, .scrim = t.palette.scrim.a,
			.hudScrim = t.palette.hudScrim.a, .toast = 1.f, .textSelection = 0.12f};
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
	out.face = has(state, State::Pressed) ? p.accentPressed : (has(state, State::Hovered) ? p.surfaceRaised : p.control);
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

/// Semantic edge, selection, focus and interaction cues occupy separate lanes.
enum class SlotMark : uint8_t { None, Pending, Rejected };
enum class SlotInteraction : uint8_t { None, Hover, Pressed, Disabled };

struct SlotTokens
{
	glm::vec4 face;
	glm::vec4 border;
	float borderWidth;
	glm::vec4 tint;
	glm::vec4 indicator;
	SlotMark mark;
	SlotInteraction interaction;
	bool selected;
	bool focused;
};

inline SlotTokens slotTokens(const Theme &theme, State requested)
{
	const auto &p = theme.palette;
	const State state = settleState(requested);
	const bool selected = has(state, State::Selected);
	const bool hovered = has(state, State::Hovered);
	const bool pressed = has(state, State::Pressed);
	const bool inactive = disabled(state);
	const auto mark = has(state, State::Error) ? SlotMark::Rejected
		: has(state, State::Pending) ? SlotMark::Pending : SlotMark::None;
	const auto interaction = inactive ? SlotInteraction::Disabled : pressed ? SlotInteraction::Pressed
		: hovered ? SlotInteraction::Hover : SlotInteraction::None;
	const auto edge = mark == SlotMark::Rejected ? p.error : mark == SlotMark::Pending ? p.pending
		: selected ? p.borderStrong : p.border;
	return {hovered || pressed ? p.control : p.surfaceSunken, edge,
		mark != SlotMark::None || selected ? theme.size.borderStrongWidth : theme.size.borderWidth,
		glm::vec4(1.f, 1.f, 1.f, inactive ? 0.65f : 1.f), inactive ? p.inkDisabled : p.ink,
		mark, interaction, selected, has(state, State::Focused)};
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
	.bright = {1.f, 1.f, 1.f},
	.dark = {0.f, 0.f, 0.f},
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
