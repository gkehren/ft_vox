// Design tokens, state resolution, player UI scaling and the persisted
// override (issue #302, Epic #298 Phase D). CPU-only: no SDL, Vulkan or ImGui.
//
// The point of this suite is that "readable", "consistent states" and
// "independent, persisted scale" are assertions instead of opinions.
#include <Engine/PlayerUiPreferences.hpp>
#include <Engine/PlayerUiScale.hpp>
#include <Engine/PlayerUiStyle.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "TestMain.hpp"

using namespace gameui;

namespace
{
const auto &theme()
{
	return style::baseline();
}

void paletteContrast()
{
	const auto &p = theme().palette;
	CHECK(style::contrastRatio(style::rgb(p.ink), style::rgb(p.surface)) >= style::kMinBodyContrast,
		"body ink is AA-legible on the card surface");
	CHECK(style::contrastRatio(style::rgb(p.inkMuted), style::rgb(p.surface)) >=
			style::kMinBodyContrast,
		"secondary ink is AA-legible on the card surface");
	CHECK(style::contrastRatio(style::rgb(p.ink), style::rgb(p.surfaceRaised)) >=
			style::kMinBodyContrast,
		"body ink stays AA-legible on a hovered/selected row");
	CHECK(style::contrastRatio(style::rgb(p.inkMuted), style::rgb(p.surfaceRaised)) >=
			style::kMinSecondaryContrast,
		"secondary ink keeps the large-text floor on raised rows");
	CHECK(style::contrastRatio(style::rgb(p.inkOnAccent), style::rgb(p.accent)) >=
			style::kMinBodyContrast,
		"labels on an accent-filled primary button are AA-legible");
	CHECK(style::contrastRatio(style::rgb(p.error), style::rgb(p.surface)) >= style::kMinSecondaryContrast &&
			style::contrastRatio(style::rgb(p.pending), style::rgb(p.surface)) >=
				style::kMinSecondaryContrast,
		"error and pending markers are distinguishable on the card surface");
	const float disabled = style::contrastRatio(style::rgb(p.inkDisabled), style::rgb(p.surfaceSunken));
	CHECK(disabled >= style::kMinDisabledContrast && disabled < style::kMinBodyContrast,
		"disabled content is perceivable but visibly below the interactive floor");

	// The HUD paints text on the hudScrim token, never directly on the scene.
	for (const bool bright : {true, false})
	{
		const char *scene = bright ? "bright" : "dark";
		CHECK(style::hudContrast(theme(), p.ink, bright) >= style::kMinBodyContrast,
			"HUD body text is AA-legible over a " << scene << " scene (ratio="
				<< style::hudContrast(theme(), p.ink, bright) << ')');
		CHECK(style::hudContrast(theme(), p.inkMuted, bright) >= style::kMinSecondaryContrast,
			"HUD secondary text keeps the large-text floor on a " << scene << " scene (ratio="
				<< style::hudContrast(theme(), p.inkMuted, bright) << ')');
	}
	CHECK(std::abs(style::composite({1.f, 1.f, 1.f}, {0.f, 0.f, 0.f}, 0.5f).r - 0.5f) < 0.0001f,
		"composite blends with the authored linear alpha");
	CHECK(style::contrastRatio({1.f, 1.f, 1.f}, {0.f, 0.f, 0.f}) > 20.f &&
			style::contrastRatio({0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}) == 1.f,
		"contrast ratio is 21:1 for black on white and 1:1 for a pair with itself");
}

void typography()
{
	const float sizes[] = {style::typeToken(style::TextRole::Caption).pixels,
		style::typeToken(style::TextRole::Body).pixels,
		style::typeToken(style::TextRole::Heading).pixels,
		style::typeToken(style::TextRole::Title).pixels};
	for (size_t i = 1; i < 4; ++i)
		CHECK(sizes[i] > sizes[i - 1], "text roles are strictly ordered by size");
	for (const auto role : {style::TextRole::Caption, style::TextRole::Body, style::TextRole::Heading,
			 style::TextRole::Title})
		CHECK(style::lineHeight(role) >= style::typeToken(role).pixels,
			"a role's line height never clips its own glyphs");
	CHECK(std::abs(style::typeScale(style::TextRole::Body) - 1.f) < 0.0001f,
		"the body role renders the baked face at 1:1");
	CHECK(style::typeScale(style::TextRole::Caption) < 1.f &&
			style::typeScale(style::TextRole::Title) > 1.f,
		"caption shrinks and title grows relative to the baked face");

	const auto &sizing = theme().size;
	const float spacing[] = {theme().space.xxs, theme().space.xs, theme().space.sm, theme().space.md,
		theme().space.lg, theme().space.xl};
	for (size_t i = 1; i < 6; ++i)
		CHECK(spacing[i] > spacing[i - 1], "the spacing scale is strictly increasing");
	CHECK(sizing.borderWidth <= sizing.focusRing && sizing.focusRing <= sizing.borderStrongWidth,
		"focus rings are at least as visible as a resting border");
	CHECK(sizing.slot > sizing.slotGap && sizing.buttonHeight >= sizing.rowHeight,
		"slot and control metrics leave room for their own gaps");
	CHECK(theme().overlay.scrim > theme().overlay.hudScrim &&
			theme().overlay.card > theme().overlay.hudScrim,
		"a modal scrim is darker than the HUD backdrop and cards stay readable");
}

void states()
{
	using namespace style;
	const auto &p = theme().palette;
	const auto rest = interactiveState(true, false, false, false, false);
	const auto hover = interactiveState(true, true, false, false, false);
	const auto pressed = interactiveState(true, true, true, false, false);
	const auto focused = interactiveState(true, false, false, true, false);
	const auto disabledState = interactiveState(false, true, true, true, true);
	CHECK(rest == State::Rest && has(hover, State::Hovered) && has(pressed, State::Pressed) &&
			has(disabledState, State::Disabled),
		"interactiveState maps the input signals onto one flag set");

	const auto primary = buttonTokens(theme(), ButtonVariant::Primary, rest);
	CHECK(primary.face == p.accent && primary.text == p.inkOnAccent,
		"a primary button uses the accent face with inverse ink");
	CHECK(buttonTokens(theme(), ButtonVariant::Primary, pressed).face == p.accentPressed,
		"a pressed primary button darkens to the pressed accent");
	CHECK(buttonTokens(theme(), ButtonVariant::Primary, disabledState).face == p.surfaceSunken &&
			buttonTokens(theme(), ButtonVariant::Primary, disabledState).text == p.inkDisabled,
		"disabled outranks every other button state");
	CHECK(buttonTokens(theme(), ButtonVariant::Secondary, hover).face == p.surfaceRaised &&
			buttonTokens(theme(), ButtonVariant::Secondary, hover).border == p.borderStrong,
		"a hovered secondary button raises its face and border");
	CHECK(buttonTokens(theme(), ButtonVariant::Quiet, rest).face.a == 0.f &&
			buttonTokens(theme(), ButtonVariant::Quiet, hover).face.a > 0.f,
		"a quiet button has no face until it is hovered");
	CHECK(buttonTokens(theme(), ButtonVariant::Danger, rest).face == p.error &&
			buttonTokens(theme(), ButtonVariant::Danger, hover).text == p.inkOnAccent,
		"the danger variant uses the error token");
	CHECK(buttonTokens(theme(), ButtonVariant::Primary, focused).face == p.accent,
		"focus never changes a button's fill (it is a ring, drawn by the surface)");

	const auto selectedRow = rowTokens(theme(), State::Selected);
	const auto hoveredRow = rowTokens(theme(), State::Hovered);
	const auto bothRow = rowTokens(theme(), State::Selected | State::Hovered);
	CHECK(selectedRow.text == p.ink && hoveredRow.text == p.inkMuted && bothRow.text == p.ink,
		"selection outranks hover for a list row's text");
	CHECK(bothRow.face == p.surfaceRaised && rowTokens(theme(), State::Rest).face.a == 0.f,
		"selected and hovered rows share one raised face");

	const auto idle = fieldTokens(theme(), State::Rest, Validation::None);
	const auto editing = fieldTokens(theme(), State::Focused, Validation::None);
	const auto rejected = fieldTokens(theme(), State::Focused, Validation::Error);
	const auto waiting = fieldTokens(theme(), State::Rest, Validation::Pending);
	CHECK(idle.border == p.border && editing.border == p.focus,
		"a focused field shows the focus ink");
	CHECK(rejected.border == p.error && waiting.border == p.pending,
		"validation outranks focus so a rejected value stays obviously rejected");
	CHECK(fieldTokens(theme(), State::Disabled, Validation::Error).text == p.inkDisabled,
		"a disabled field keeps disabled text even with a validation state");

	CHECK(statusColor(theme(), Status::Ok) == p.ok && statusColor(theme(), Status::Error) == p.error &&
			statusColor(theme(), Status::Pending) == p.pending,
		"status semantics resolve to distinct tokens");

	// F15: one precedence for every combined state. Disabled is final and clears
	// the interaction flags, so a disabled control can never paint a focus ring;
	// selection survives because it tells the player what is selected.
	CHECK(!has(disabledState, State::Focused) && !has(disabledState, State::Hovered) &&
			!has(disabledState, State::Pressed) && has(disabledState, State::Selected),
		"a disabled control keeps its selection but never looks interactively focused");
	const auto settled = settleState(State::Selected | State::Focused | State::Disabled | State::Pending);
	CHECK(has(settled, State::Selected) && has(settled, State::Pending) &&
			has(settled, State::Disabled) && !has(settled, State::Focused),
		"settleState keeps validation and selection, drops focus when disabled");
	CHECK(slotState(State::Selected | State::Focused) == (State::Selected | State::Focused) &&
			slotState(State::Error | State::Focused) == (State::Error | State::Focused),
		"an enabled slot keeps its focus, selection and validation flags");
	CHECK(has(slotState(State::Selected | State::Focused | State::Disabled), State::Selected) &&
			!has(slotState(State::Selected | State::Focused | State::Disabled), State::Focused),
		"a disabled slot keeps the selection marker without a focus ring");
	CHECK(toggleFace(theme(), State::Rest, true) == p.accent &&
			toggleFace(theme(), State::Disabled, true) != p.accent,
		"a disabled toggle never paints the on-state accent");
	CHECK(sliderFill(theme(), State::Rest) == p.accent &&
			sliderFill(theme(), State::Disabled) == p.inkDisabled,
		"a disabled slider fill is the disabled ink, not the accent");
	CHECK(sliderFill(theme(), State::Pressed) == p.accentPressed &&
			sliderTrackFace(theme(), State::Hovered) == p.surfaceRaised &&
			sliderTrackFace(theme(), State::Rest) == p.surfaceSunken,
		"a slider shows pressed and hover feedback from the tokens");
	CHECK(toggleFace(theme(), State::Pressed, true) == p.accentPressed &&
			toggleFace(theme(), State::Hovered, false) == p.surfaceRaised,
		"a toggle shows pressed and hover feedback from the tokens");

	// R06: the resolved slot visuals for combined states, not just the flags.
	const auto resting = slotTokens(theme(), State::Rest);
	const auto selectedSlot = slotTokens(theme(), State::Selected);
	const auto slotRejected = slotTokens(theme(), State::Error | State::Focused);
	const auto slotPending = slotTokens(theme(), State::Pending | State::Focused);
	const auto disabledSlot = slotTokens(theme(), State::Selected | State::Disabled | State::Focused);
	CHECK(resting.border == p.border && resting.borderWidth == theme().size.borderWidth,
		"a resting slot uses the resting border token");
	CHECK(selectedSlot.border == p.borderStrong &&
			selectedSlot.borderWidth == theme().size.borderStrongWidth,
		"a selected slot is stronger than a resting one");
	CHECK(slotRejected.border == p.error && slotRejected.borderWidth == theme().size.focusRing,
		"focus widens the ring but never hides a rejected value");
	CHECK(slotPending.border == p.pending && slotPending.borderWidth == theme().size.focusRing,
		"focus widens the ring but never hides a pending value");
	CHECK(disabledSlot.border == p.borderStrong &&
			disabledSlot.borderWidth == theme().size.borderStrongWidth &&
			disabledSlot.indicator == p.inkDisabled && disabledSlot.tint.a < 1.f,
		"a disabled slot keeps its selection marker, drops the focus ring and dims its content");
	CHECK(slotTokens(theme(), State::Disabled | State::Error).border == p.error,
		"validation still colours a disabled slot's border");
}

void scaling()
{
	CHECK(snapDown(0.74f) == kMinScale && snapDown(1.f) == 1.f && snapDown(1.74f) == 1.5f &&
			snapDown(9.f) == kMaxScale,
		"snapDown lands on the step at or below the request");
	CHECK(nearestUserScale(1.4f) == 1.5f && nearestUserScale(1.1f) == 1.f &&
			nearestUserScale(0.5f) == 1.f && nearestUserScale(9.f) == 2.f,
		"an override snaps onto a 100-200% user step");
	CHECK(isUserScale(1.25f) && !isUserScale(0.75f), "75% is auto-only");

	const auto autoAt = [](glm::vec2 size, float content) {
		return autoScale({size, content});
	};
	// The accepted Auto table (issue #302 F04): base = snapDown(content scale),
	// fit = min(width/900, height/480), result = snapDown(min(base, fit)).
	struct AutoCase { glm::vec2 size; float content; float expected; };
	const AutoCase autoCases[] = {
		{{1920.f, 1080.f}, 2.f, 2.f},  // 200% fits the authored box exactly
		{{1600.f, 900.f}, 2.f, 1.75f}, // fit 1.777 caps the 200% request
		{{1280.f, 720.f}, 2.f, 1.25f}, // fit 1.422 caps the 200% request
		{{1160.f, 680.f}, 1.25f, 1.25f},
		{{900.f, 480.f}, 2.f, 1.f},    // at the fit box the UI is exactly authored size
		{{800.f, 480.f}, 1.5f, kMinScale},
		{{3840.f, 2160.f}, 2.f, 2.f},
		{{2560.f, 1080.f}, 1.5f, 1.5f},
		{{1920.f, 1080.f}, 1.f, 1.f},
	};
	for (const auto &test : autoCases)
		CHECK(autoAt(test.size, test.content) == test.expected,
			"auto scale of " << test.size.x << 'x' << test.size.y << " at " << test.content * 100.f
				<< "% is " << test.expected * 100.f << '%');

	// No discontinuity at the reference size: just above 900x480 the scale is
	// still the authored one, not the raw content scale.
	CHECK(autoAt({910.f, 490.f}, 2.f) == 1.f && autoAt({899.f, 479.f}, 2.f) == kMinScale,
		"crossing the 900x480 fit box changes the scale by one step, never by a jump");
	CHECK(autoAt({1125.f, 600.f}, 2.f) == 1.25f && autoAt({1124.f, 600.f}, 2.f) == 1.f,
		"the fit cap lands on the next step only when the window really grew");
	CHECK(autoAt({0.f, 0.f}, 1.5f) == 1.5f, "a minimized window falls back to the snapped content scale");
	CHECK(autoAt({1920.f, 1080.f}, 0.f) == 1.f &&
			autoAt({1920.f, 1080.f}, std::numeric_limits<float>::quiet_NaN()) == 1.f,
		"a degenerate content scale falls back to 100%");
	CHECK(contentScale(1.5f, 1.f) == 1.5f && contentScale(2.f, 2.f) == 1.f &&
			contentScale(0.f, 1.f) == 1.f,
		"content scale is the display/pixel-density ratio with a 100% fallback");

	// R02: a failed DPI query stays invalid all the way to the controller instead
	// of being laundered into a valid 100% by the platform conversion.
	const auto queried = scaleInputs({1600.f, 900.f}, 2.f, 1.f);
	const auto failed = scaleInputs({1600.f, 900.f}, 0.f, 1.f);
	const auto nanScale = scaleInputs({1600.f, 900.f}, std::numeric_limits<float>::quiet_NaN(), 1.f);
	const auto zeroDensity = scaleInputs({1600.f, 900.f}, 2.f, 0.f);
	CHECK(queried.contentScale == 2.f && queried.contentScaleValid && validScaleInputs(queried),
		"a successful DPI query produces exact, valid controller inputs");
	CHECK(!failed.contentScaleValid && !validScaleInputs(failed) && !nanScale.contentScaleValid &&
			!zeroDensity.contentScaleValid,
		"a failed, non-finite or zero DPI query is reported as invalid input");

	ScaleController dpi;
	CHECK(dpi.resolve(queried) && dpi.effective() == 1.75f,
		"a 1600x900 window at 200% resolves to the 175% step");
	const auto dpiRevision = dpi.revision();
	CHECK(!dpi.resolve(failed) && dpi.effective() == 1.75f && dpi.revision() == dpiRevision,
		"a temporary DPI query failure does not change the effective Auto scale");
	CHECK(!dpi.resolve(nanScale) && dpi.revision() == dpiRevision,
		"a non-finite DPI value is treated exactly like a failed query");
	CHECK(!dpi.resolve(queried) && dpi.revision() == dpiRevision,
		"restoring identical DPI information does not invalidate the layout");
	dpi.setPolicy({ScaleMode::Fixed, 1.25f});
	CHECK(dpi.resolve(failed) && dpi.effective() == 1.25f,
		"a Fixed override stays independent of DPI query failures");
	dpi.setPolicy({ScaleMode::Auto, 1.f});
	CHECK(dpi.resolve(failed) && dpi.effective() == 1.75f,
		"Auto after a Fixed override restores the last valid Auto scale from invalid inputs");

	CHECK(resolveScale({ScaleMode::Fixed, 1.4f}, {{800.f, 480.f}, 2.f}) == 1.5f,
		"a fixed policy ignores the window size (that is the point of the override)");
	CHECK(resolveScale({ScaleMode::Fixed, 5.f}, {{1920.f, 1080.f}, 1.f}) == 2.f,
		"an out-of-range fixed policy clamps to the maximum");

	ScaleController controller;
	CHECK(controller.resolve({{1920.f, 1080.f}, 1.f}) && controller.effective() == 1.f &&
			controller.revision() == 1,
		"the first resolve publishes the effective scale");
	CHECK(!controller.resolve({{1920.f, 1080.f}, 1.f}) && controller.revision() == 1,
		"an unchanged frame does not invalidate the layout");
	controller.setPolicy({ScaleMode::Fixed, 1.5f});
	CHECK(controller.hasPending() && controller.effective() == 1.f,
		"a policy request never changes the scale mid-frame");
	CHECK(controller.resolve({{1920.f, 1080.f}, 1.f}) && controller.effective() == 1.5f &&
			controller.revision() == 2 && !controller.hasPending(),
		"the next frame boundary applies the request exactly once");
	controller.setPolicy({ScaleMode::Auto, 1.f});
	CHECK(controller.resolve({{2560.f, 1080.f}, 1.25f}) && controller.effective() == 1.25f,
		"switching back to Auto follows the display again");
	CHECK(controller.policy().mode == ScaleMode::Auto && !controller.hasPending(),
		"the applied policy is reported back for persistence");

	// Degenerate inputs never change an Auto scale that was already valid, and
	// restoring the window does not invalidate the layout either (F04).
	ScaleController degenerate;
	CHECK(degenerate.resolve({{1600.f, 900.f}, 2.f}) && degenerate.effective() == 1.75f &&
			degenerate.revision() == 1,
		"a 1600x900 window at 200% resolves to the 175% step");
	const auto revision = degenerate.revision();
	CHECK(!degenerate.resolve({{0.f, 0.f}, 0.f}) && degenerate.effective() == 1.75f &&
			degenerate.revision() == revision,
		"a minimized window keeps the last valid Auto scale without rebuilding the layout");
	CHECK(!degenerate.resolve({{1600.f, 900.f}, 2.f}) && degenerate.effective() == 1.75f &&
			degenerate.revision() == revision,
		"restoring the window re-resolves to the same scale, so nothing is invalidated");
	degenerate.setPolicy({ScaleMode::Fixed, 1.25f});
	CHECK(degenerate.resolve({{800.f, 480.f}, 1.5f}) && degenerate.effective() == 1.25f,
		"a Fixed override ignores the window size even when Auto would step down");
	degenerate.setPolicy({ScaleMode::Auto, 1.f});
	CHECK(degenerate.resolve({{0.f, 0.f}, 0.f}) && degenerate.effective() == 1.75f,
		"switching back to Auto with no valid inputs restores the last Auto scale");
}

void preferences()
{
	const auto &dir = std::filesystem::temp_directory_path() / "ftvox-ui-302-prefs";
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	const auto path = dir / "player-ui.txt";

	PlayerUiSettings settings;
	CHECK(settings.load(path) == LoadStatus::Missing && settings.scale.mode == ScaleMode::Auto,
		"a fresh install has no record and keeps the auto default");
	std::filesystem::create_directories(dir, ec);
	CHECK(!settings.save(dir), "saving onto a directory fails instead of throwing");
	CHECK(settings.save(path) && std::filesystem::exists(path),
		"saving creates the directory and the record");

	PlayerUiSettings reloaded;
	CHECK(reloaded.load(path) == LoadStatus::Ok && reloaded.scale.mode == ScaleMode::Auto,
		"the auto policy round-trips through the file");

	settings.scale = {ScaleMode::Fixed, 1.5f};
	CHECK(settings.save(path), "the override replaces the previous record");
	CHECK(reloaded.load(path) == LoadStatus::Ok && reloaded.scale.mode == ScaleMode::Fixed &&
			reloaded.scale.value == 1.5f,
		"the fixed override round-trips through the file");

	CHECK(settings.serialize() == std::string(kPlayerUiSettingsHeader) + "\nscale fixed 1.50\n"
			"window 1920 1080\nvsync on\nfxaa on\nsensitivity 100\ninvert-y off\n",
		"the record format is stable and readable");

	const PlayerUiSettings custom{{ScaleMode::Fixed, 1.75f}, {1280, 720}, false, false, 250, true};
	CHECK(custom.save(path) && reloaded.load(path) == LoadStatus::Ok && reloaded == custom,
		"all client preferences round-trip together");
	for (const auto window : kWindowSizes)
		for (const int sensitivity : kSensitivitySteps)
		{
			auto offered = custom;
			offered.window = window;
			offered.sensitivity = sensitivity;
			PlayerUiSettings parsed;
			CHECK(offered.valid() && parsed.parse(offered.serialize()) && parsed == offered,
				"every advertised window size and sensitivity pair is valid and persists");
		}
	const auto rejects = [&](std::string_view from, std::string_view to) {
		auto malformed = custom.serialize();
		malformed.replace(malformed.find(from), from.size(), to);
		const PlayerUiSettings witness{{ScaleMode::Fixed, 2.f}, {960, 540}, true, false, 125, true};
		auto destination = witness;
		return !destination.parse(malformed) && destination == witness;
	};
	CHECK(rejects("scale fixed 1.75", "scale manual 1.75"), "reject unknown scale mode");
	CHECK(rejects("scale fixed 1.75", "scale fixed 0.75"), "reject auto-only fixed scale");
	CHECK(rejects("scale fixed 1.75", "scale fixed 1.40"), "reject unsupported fixed scale step");
	CHECK(rejects("scale fixed 1.75", "scale fixed 2.50"), "reject out-of-range fixed scale");
	CHECK(rejects("scale fixed 1.75", "scale fixed 1,75"), "reject locale-dependent decimal separator");
	CHECK(rejects("1280 720", "1280 721"), "reject unsupported window height");
	CHECK(rejects("1280 720", "0 720"), "reject out-of-range window width");
	CHECK(rejects("1280 720", "2147483648 720"), "reject overflow in window width");
	CHECK(rejects("1280 720", "1280x 720"), "reject trailing junk in window width");
	CHECK(rejects("sensitivity 250", "sensitivity 275"), "reject unsupported sensitivity step");
	CHECK(rejects("sensitivity 250", "sensitivity 301"), "reject out-of-range sensitivity");
	CHECK(rejects("sensitivity 250", "sensitivity 2.50"), "reject malformed sensitivity integer");
	CHECK(rejects("vsync off", "vsync yes"), "reject invalid VSync boolean");
	CHECK(rejects("fxaa off", "fxaa yes"), "reject invalid FXAA boolean");
	CHECK(rejects("invert-y on", "invert-y yes"), "reject invalid inversion boolean");
	CHECK(rejects("fxaa off\n", "fxaa off\nfxaa off\n"), "reject duplicate field");
	CHECK(rejects("invert-y on\n", ""), "reject missing field");
	CHECK(rejects("invert-y on", "motion on"), "reject unknown field");
	CHECK(rejects(kPlayerUiSettingsHeader, "ftvox-player-settings 2"), "reject unknown version");
	CHECK(rejects("ftvox-player-settings 1\n", ""), "reject missing header");

	const std::string legacyHeader(kLegacyPlayerUiSettingsHeader);
	ScalePolicy legacyScale;
	CHECK(parseLegacyPlayerScale(legacyHeader + "\nscale fixed 2.00\n", legacyScale) &&
			legacyScale == (ScalePolicy{ScaleMode::Fixed, 2.f}), "legacy scale parses independently");
	CHECK(parseLegacyPlayerScale(legacyHeader + "\r\nscale fixed 1.25\r\n", legacyScale) &&
			legacyScale == (ScalePolicy{ScaleMode::Fixed, 1.25f}), "legacy parser handles CRLF");
	const auto oldScale = legacyScale;
	CHECK(!parseLegacyPlayerScale(legacyHeader + "\nscale fixed 1,25\n", legacyScale) &&
			legacyScale == oldScale, "legacy parser rejects comma decimals without changing the destination");
	CHECK(!parseLegacyPlayerScale(legacyHeader + "\nscale auto\nscale auto\n", legacyScale) &&
			legacyScale == oldScale, "legacy parser rejects duplicate scale lines");
	CHECK(!parseLegacyPlayerScale(custom.serialize(), legacyScale) && legacyScale == oldScale,
		"migration reads only the legacy schema");
	CHECK(!reloaded.parse(legacyHeader + "\nscale auto\n") && reloaded == custom,
		"the current parser rejects a valid legacy record without changing its destination");
	CHECK(parseLegacyPlayerScale(legacyHeader + "\nscale auto", legacyScale) &&
			legacyScale == ScalePolicy{}, "legacy parser accepts a missing trailing newline");
	auto invalid = custom;
	invalid.window.height = 0;
	CHECK(!invalid.save(path) && reloaded.load(path) == LoadStatus::Ok && reloaded == custom,
		"an invalid in-memory record cannot replace good preferences");
	invalid = custom;
	invalid.scale.mode = static_cast<ScaleMode>(99);
	CHECK(!invalid.save(path) && reloaded.load(path) == LoadStatus::Ok && reloaded == custom,
		"an invalid scale mode cannot replace good preferences");

	{
		std::filesystem::create_directories(path.parent_path(), ec);
		std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
		corrupt << "ftvox-player-ui 2\nscale auto\n";
	}
	PlayerUiSettings untouched;
	untouched.scale = {ScaleMode::Fixed, 1.25f};
	CHECK(untouched.load(path) == LoadStatus::Corrupt && untouched.scale.value == 1.25f,
		"a corrupt record is reported and never mutates the running session");

	// R09: a corrupted record is repaired only by an explicit write of the
	// defaults; a missing record stays a fresh-install case.
	{
		PlayerUiSettings repair;
		CHECK(repair.save(path), "an explicit selection writes a repair record");
		PlayerUiSettings repaired;
		CHECK(repaired.load(path) == LoadStatus::Ok && repaired.scale.mode == ScaleMode::Auto,
			"the repaired record parses and holds the default policy");
	}

	// F06: the write is atomic and concurrent-safe. Four writers, staggered
	// values: the record always parses, and no staging file is left behind.
	{
		std::atomic<int> failures{0};
		std::vector<std::thread> writers;
		for (int i = 0; i < 4; ++i)
			writers.emplace_back([&path, &failures, i] {
				PlayerUiSettings local;
				local.scale = i % 2 ? ScalePolicy{ScaleMode::Fixed, 1.25f}
									: ScalePolicy{ScaleMode::Auto, 1.f};
				for (int n = 0; n < 20; ++n)
					if (!local.save(path))
						++failures;
			});
		for (auto &writer : writers)
			writer.join();
		CHECK(failures.load() == 0, "a concurrent writer always succeeds");
		PlayerUiSettings after;
		CHECK(after.load(path) == LoadStatus::Ok && isUserScale(after.scale.value),
			"concurrent writers never leave a partial or corrupt record");
		bool stagingLeft = false;
		for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
			stagingLeft = stagingLeft || entry.path().filename().string().find(".tmp.") != std::string::npos;
		CHECK(!stagingLeft && !ec, "a completed write leaves no staging file behind");
	}

	// A failed write preserves the previous valid record.
	{
		PlayerUiSettings kept;
		kept.scale = {ScaleMode::Fixed, 1.5f};
		CHECK(kept.save(path), "the previous record is on disk");
		std::ifstream before(path, std::ios::binary);
		const std::string previous((std::istreambuf_iterator<char>(before)),
			std::istreambuf_iterator<char>());
		PlayerUiSettings replacement;
		replacement.scale = {ScaleMode::Fixed, 2.f};
		CHECK(!replacement.save(dir), "saving onto a directory still fails");
		std::ifstream after(path, std::ios::binary);
		const std::string current((std::istreambuf_iterator<char>(after)),
			std::istreambuf_iterator<char>());
		CHECK(current == previous && !current.empty(),
			"a failed write leaves the last valid record untouched");
	}

	// R03: readers can never observe a half-replaced record, because the read
	// opens the file before sizing it and reads from that same handle.
	{
		std::atomic<bool> stop{false};
		std::atomic<int> falseCorrupt{0};
		std::atomic<int> badValues{0};
		std::vector<std::thread> readers;
		for (int i = 0; i < 2; ++i)
			readers.emplace_back([&path, &stop, &falseCorrupt, &badValues] {
				while (!stop.load())
				{
					PlayerUiSettings seen;
					const auto status = seen.load(path);
					if (status != LoadStatus::Ok && status != LoadStatus::Missing)
						++falseCorrupt;
					else if (status == LoadStatus::Ok && seen.scale.mode == ScaleMode::Fixed &&
							 !isUserScale(seen.scale.value))
						++badValues;
				}
			});
		PlayerUiSettings autoRecord, fixedRecord;
		autoRecord.scale = {ScaleMode::Auto, 1.f};
		fixedRecord.scale = {ScaleMode::Fixed, 1.5f};
		for (int i = 0; i < 40; ++i)
			CHECK((i % 2 ? fixedRecord : autoRecord).save(path),
				"a writer alternating records succeeds while readers load");
		stop = true;
		for (auto &reader : readers)
			reader.join();
		CHECK(falseCorrupt.load() == 0 && badValues.load() == 0,
			"concurrent readers never report a false corrupt record or a partial value");
	}

#if defined(_WIN32)
	// An open handle with DELETE access models the sharing requirements of
	// a replacement. A concurrent reader must share that access.
	{
		PlayerUiSettings kept;
		kept.scale = {ScaleMode::Fixed, 1.5f};
		CHECK(kept.save(path), "the record for a concurrent read is on disk");
		const HANDLE holder = CreateFileW(path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		CHECK(holder != INVALID_HANDLE_VALUE, "another instance can hold a delete-enabled handle");
		PlayerUiSettings loaded;
		CHECK(loaded.load(path) == LoadStatus::Ok && loaded == kept,
			"the preferences reader shares delete access with an in-progress replacement");
		if (holder != INVALID_HANDLE_VALUE)
			CloseHandle(holder);
	}

	// R03: a real replacement failure against the actual destination. A second
	// instance holds the record without FILE_SHARE_DELETE, so MoveFileEx cannot
	// land: save reports false, the previous record survives and the staging file
	// is cleaned up.
	{
		PlayerUiSettings kept;
		kept.scale = {ScaleMode::Fixed, 1.25f};
		CHECK(kept.save(path), "the record to survive a sharing violation is on disk");
		const auto wide = path.wstring();
		const HANDLE holder = CreateFileW(wide.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		CHECK(holder != INVALID_HANDLE_VALUE, "the record can be held exclusively by another instance");
		PlayerUiSettings replacement;
		replacement.scale = {ScaleMode::Fixed, 2.f};
		CHECK(!replacement.save(path), "a replace against a held destination is reported as failed");
		if (holder != INVALID_HANDLE_VALUE)
			CloseHandle(holder);
		PlayerUiSettings after;
		CHECK(after.load(path) == LoadStatus::Ok && after.scale.value == 1.25f,
			"the previous record survives a failed replacement against the same destination");
		bool stagingLeft = false;
		for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
			stagingLeft = stagingLeft || entry.path().filename().string().find(".tmp.") != std::string::npos;
		CHECK(!stagingLeft, "a failed replacement against the destination leaves no staging file");
	}
#endif

	// Same contract on every platform: a destination whose parent is a regular
	// file cannot be staged, and the existing record is untouched.
	{
		PlayerUiSettings kept;
		kept.scale = {ScaleMode::Fixed, 1.25f};
		CHECK(kept.save(path), "the record to preserve is on disk");
		std::ifstream before(path, std::ios::binary);
		const std::string previous((std::istreambuf_iterator<char>(before)),
			std::istreambuf_iterator<char>());
		// The destination's parent is a regular file, so staging cannot be created.
		const auto blocked = path / "nested" / "player-ui.txt";
		PlayerUiSettings replacement;
		replacement.scale = {ScaleMode::Fixed, 2.f};
		CHECK(!replacement.save(blocked), "an injected replacement failure is reported");
		std::ifstream after(path, std::ios::binary);
		const std::string current((std::istreambuf_iterator<char>(after)),
			std::istreambuf_iterator<char>());
		CHECK(current == previous && !current.empty(),
			"the previous valid record survives an injected replacement failure");
		bool stagingLeft = false;
		for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
			stagingLeft = stagingLeft || entry.path().filename().string().find(".tmp.") != std::string::npos;
		CHECK(!stagingLeft, "an injected replacement failure leaves no staging file behind");
	}

	// Locale independence: formatting is integer-based and parsing accepts only
	// '.' as the decimal separator, so a system locale can never corrupt a
	// record (a comma-decimal locale would with printf/strtod).
	PlayerUiSettings localeIndependent;
	localeIndependent.scale = {ScaleMode::Fixed, 1.5f};
	const auto serialized = localeIndependent.serialize();
	CHECK(serialized.find("1.50") != std::string::npos && serialized.find(',') == std::string::npos,
		"the record always uses a dot decimal separator");
	auto shortDecimal = serialized;
	shortDecimal.replace(shortDecimal.find("1.50"), 4, "1.5");
	PlayerUiSettings strict;
	CHECK(strict.parse(shortDecimal) && strict == localeIndependent,
		"a short decimal parses without a locale-dependent conversion");

	std::filesystem::remove_all(dir, ec);
}
} // namespace

int main()
{
	paletteContrast();
	typography();
	states();
	scaling();
	preferences();
	if (testmain::fails == 0)
		std::cout << "PASS: Game UI style (" << testmain::checks << " checks)" << std::endl;;
	return testmain::exitCode();
}
