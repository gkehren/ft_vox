// Design tokens, state resolution, player UI scaling and the persisted
// override (issue #302, Epic #298 Phase D). CPU-only: no SDL, Vulkan or ImGui.
//
// The point of this suite is that "readable", "consistent states" and
// "independent, persisted scale" are assertions instead of opinions.
#include <Engine/PlayerUiPreferences.hpp>
#include <Engine/PlayerUiScale.hpp>
#include <Engine/PlayerUiStyle.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

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
	CHECK(autoAt({1920.f, 1080.f}, 1.f) == 1.f, "a 1080p window at 100% keeps 100%");
	CHECK(autoAt({1920.f, 1080.f}, 1.5f) == 1.5f, "a 1080p window at 150% follows the OS");
	CHECK(autoAt({3840.f, 2160.f}, 2.f) == 2.f, "a 4K window at 200% follows the OS");
	CHECK(autoAt({2560.f, 1080.f}, 1.5f) == 1.5f, "a 21:9 window keeps the OS scale");
	CHECK(autoAt({1160.f, 680.f}, 1.25f) == 1.25f, "a smaller window keeps the OS scale while it fits");
	CHECK(autoAt({800.f, 480.f}, 1.5f) == kMinScale && autoAt({640.f, 360.f}, 1.f) == kMinScale,
		"a window below the authored fit size steps down to the 75% floor instead of overflowing");
	CHECK(autoAt({0.f, 0.f}, 1.5f) == 1.5f, "a minimized window keeps the last resolved scale");
	CHECK(autoAt({1920.f, 1080.f}, 0.f) == 1.f &&
			autoAt({1920.f, 1080.f}, std::numeric_limits<float>::quiet_NaN()) == 1.f,
		"a degenerate content scale falls back to 100%");
	CHECK(contentScale(1.5f, 1.f) == 1.5f && contentScale(2.f, 2.f) == 1.f &&
			contentScale(0.f, 1.f) == 1.f,
		"content scale is the display/pixel-density ratio with a 100% fallback");

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

	CHECK(settings.serialize() == std::string(kPlayerUiSettingsHeader) + "\nscale fixed 1.50\n",
		"the record format is stable and readable");

	PlayerUiSettings parsed;
	CHECK(!parsed.parse("scale auto\n"), "a record without its version header is corrupt");
	CHECK(!parsed.parse(std::string(kPlayerUiSettingsHeader) + "\n"), "a record without a key is corrupt");
	CHECK(!parsed.parse(std::string(kPlayerUiSettingsHeader) + "\nscale auto\nscale auto\n"),
		"a duplicated key is corrupt");
	CHECK(!parsed.parse(std::string(kPlayerUiSettingsHeader) + "\nmotion off\n"),
		"an unknown key is corrupt");
	CHECK(!parsed.parse(std::string(kPlayerUiSettingsHeader) + "\nscale fixed 0.75\n"),
		"an auto-only step cannot be persisted as an override");
	CHECK(!parsed.parse(std::string(kPlayerUiSettingsHeader) + "\nscale fixed 1.40\n"),
		"a non-step override is rejected instead of silently re-snapped");
	CHECK(!parsed.parse(std::string(kPlayerUiSettingsHeader) + "\nscale fixed 3\n"),
		"an out-of-range override is rejected");
	CHECK(parsed.parse(std::string(kPlayerUiSettingsHeader) + "\nscale fixed 2.00\n") &&
			parsed.scale.value == 2.f,
		"a valid maximum override parses");
	CHECK(parsed.parse(std::string(kPlayerUiSettingsHeader) + "\r\nscale fixed 1.25\r\n") &&
			parsed.scale.value == 1.25f,
		"a CRLF record parses (a Windows editor round-trip stays valid)");
	CHECK(parsed.parse(std::string(kPlayerUiSettingsHeader) + "\nscale auto") &&
			parsed.scale.mode == ScaleMode::Auto,
		"a record saved without its trailing newline still parses");

	{
		std::filesystem::create_directories(path.parent_path(), ec);
		std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
		corrupt << "ftvox-player-ui 2\nscale auto\n";
	}
	PlayerUiSettings untouched;
	untouched.scale = {ScaleMode::Fixed, 1.25f};
	CHECK(untouched.load(path) == LoadStatus::Corrupt && untouched.scale.value == 1.25f,
		"a corrupt record is reported and never mutates the running session");

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
