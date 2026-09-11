// UI shell pure logic (issue #183): scale normalization, status-strip
// degradation, and shortcut metadata consistency with the InputRouting
// binding policy. No ImGui and no rendering involved.
#include <Engine/InputRouting.hpp>
#include <Engine/UiScale.hpp>
#include <Engine/UiShortcuts.hpp>
#include <Engine/UiStatus.hpp>

#include <cmath>
#include <iostream>
#include <limits>

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

static void checkNear(float a, float b, float eps, const char *name)
{
	CHECK(std::fabs(a - b) <= eps, name);
}

static void checkScale()
{
	// clampScale: invalid input falls back to 100%, others clamp to the
	// supported range [100%, 200%].
	CHECK(ui::clampScale(std::numeric_limits<float>::quiet_NaN()) == 1.f, "clampScale NaN -> 1");
	CHECK(ui::clampScale(0.f) == 1.f, "clampScale 0 -> 1");
	CHECK(ui::clampScale(-2.f) == 1.f, "clampScale negative -> 1");
	checkNear(ui::clampScale(0.1f), 1.0f, 1e-6f, "clampScale clamps low to 100%");
	checkNear(ui::clampScale(100.f), 2.0f, 1e-6f, "clampScale clamps high to 200%");
	checkNear(ui::clampScale(1.3f), 1.3f, 1e-6f, "clampScale passes through in-range");

	// snapScale: nearest canonical step, always (the five menu entries are
	// the only valid values, so a selected entry always exists).
	checkNear(ui::snapScale(1.2499f), 1.25f, 1e-4f, "snapScale 1.2499 -> 125%");
	checkNear(ui::snapScale(0.999877f), 1.0f, 1e-4f, "snapScale X11 ~1.0 -> 100%");
	checkNear(ui::snapScale(1.5001f), 1.5f, 1e-4f, "snapScale 1.5001 -> 150%");
	checkNear(ui::snapScale(1.1f), 1.0f, 1e-4f, "snapScale 1.1 -> 100% (nearest)");
	checkNear(ui::snapScale(1.38f), 1.5f, 1e-4f, "snapScale 1.38 -> 150% (nearest)");
	checkNear(ui::snapScale(1.9f), 2.0f, 1e-4f, "snapScale 1.9 -> 200% (nearest)");
	checkNear(ui::snapScale(1.6f), 1.5f, 1e-4f, "snapScale 1.6 -> 150% (midpoint rounds down-nearest)");
	checkNear(ui::snapScale(5.f), 2.0f, 1e-6f, "snapScale clamps first");
	checkNear(ui::snapScale(0.5f), 1.0f, 1e-6f, "snapScale below range -> 100%");

	// Bounds are exactly the canonical 100-200% set.
	checkNear(ui::kMinScale, 1.0f, 1e-6f, "kMinScale is 100%");
	checkNear(ui::kMaxScale, 2.0f, 1e-6f, "kMaxScale is 200%");
	for (float s : ui::kSupportedScales)
		CHECK(ui::isSupportedScale(s), "supported step recognized");
	CHECK(!ui::isSupportedScale(1.3f), "off-step value not flagged supported");
	CHECK(!ui::isSupportedScale(0.f), "zero not supported");

	// scaled(): canonical 100% metrics follow the UI scale.
	checkNear(ui::scaled(360.f, 2.0f), 720.f, 1e-4f, "scaled doubles at 200%");
	checkNear(ui::scaled(12.f, 1.25f), 15.f, 1e-4f, "scaled 12 @125% = 15");
	checkNear(ui::scaled(80.f, 2.0f), 160.f, 1e-4f, "scaled 80 @200% = 160");
	checkNear(ui::scaled(220.f, 1.5f), 330.f, 1e-4f, "scaled 220 @150% = 330");
	checkNear(ui::scaled(0.f, 1.75f), 0.f, 1e-4f, "scaled zero stays zero");
	checkNear(ui::scaled(480.f, 1.0f), 480.f, 1e-4f, "scaled identity at 100%");

	// effectiveScale(): invalid frame scales fall back to 100%.
	checkNear(ui::effectiveScale(2.0f), 2.0f, 1e-6f, "effectiveScale passthrough");
	checkNear(ui::effectiveScale(0.f), 1.f, 1e-6f, "effectiveScale zero -> 100%");
	checkNear(ui::effectiveScale(-1.f), 1.f, 1e-6f, "effectiveScale negative -> 100%");
}

static void checkStatusRegion()
{
	// Normal case: menus end at cursorX, gap follows, right padding kept.
	const ui::StatusRegion r = ui::statusRegion(120.f, 1000.f, 9.f, 8.f);
	checkNear(r.start, 128.f, 1e-4f, "region starts after cursor + gap");
	checkNear(r.width, 1000.f - 9.f - 128.f, 1e-4f, "region width reaches right padding");

	// Menus overflow the bar: region is empty, never negative.
	const ui::StatusRegion tiny = ui::statusRegion(995.f, 1000.f, 9.f, 8.f);
	checkNear(tiny.start, 1003.f, 1e-4f, "start still advances by gap");
	CHECK(tiny.width == 0.f, "degenerate region clamps to zero width");

	// No gap / no padding edge cases.
	checkNear(ui::statusRegion(0.f, 500.f, 0.f, 0.f).width, 500.f, 1e-4f, "full-width region");
	checkNear(ui::statusRegion(500.f, 500.f, 0.f, 0.f).width, 0.f, 1e-4f, "empty at right edge");
}

static void checkStatusStrip()
{
	const float fpsW = 60.f, msW = 70.f, badgeW = 50.f, sepW = 10.f;

	// Plenty of room: all fields, right-aligned.
	ui::StatusStripPlan p = ui::planStatusStrip(1000.f, 0.f, fpsW, msW, badgeW, sepW);
	CHECK(p.showFps && p.showMs && p.showBadge, "all fields shown when wide");
	checkNear(p.fpsX, 1000.f - (fpsW + sepW + msW + sepW + badgeW), 1e-4f, "right-aligned start");
	checkNear(p.badgeX + badgeW, 1000.f, 1e-4f, "badge flush right");

	// Exactly enough room still shows everything.
	p = ui::planStatusStrip(fpsW + sepW + msW + sepW + badgeW, 0.f, fpsW, msW, badgeW, sepW);
	CHECK(p.showFps && p.showMs && p.showBadge, "exact fit keeps all fields");

	// Crowded: frame time drops first, fps + badge survive, still right-aligned.
	p = ui::planStatusStrip(fpsW + sepW + msW + sepW + badgeW - 1.f, 0.f, fpsW, msW, badgeW, sepW);
	CHECK(p.showFps && !p.showMs && p.showBadge, "ms degrades first");
	checkNear(p.fpsX, msW + sepW - 1.f, 1e-4f, "degraded start stays right-aligned");
	checkNear(p.badgeX + badgeW, fpsW + sepW + msW + sepW + badgeW - 1.f, 1e-4f, "degraded badge flush right");

	// Very narrow: only the badge survives.
	p = ui::planStatusStrip(60.f, 0.f, fpsW, msW, badgeW, sepW);
	CHECK(!p.showFps && !p.showMs && p.showBadge, "badge survives last");
	checkNear(p.badgeX + badgeW, 60.f, 1e-4f, "badge-only flush right");

	// Absurdly narrow: nothing draws, no negative offsets.
	p = ui::planStatusStrip(40.f, 0.f, fpsW, msW, badgeW, sepW);
	CHECK(!p.showFps && !p.showMs && !p.showBadge, "nothing drawn when unusable");

	// padRight is respected.
	p = ui::planStatusStrip(200.f, 10.f, fpsW, msW, badgeW, sepW);
	CHECK(!p.showMs, "padRight participates in the fit decision");
	checkNear(p.badgeX + badgeW, 190.f, 1e-4f, "badge respects right padding");
}

static void checkShortcutMetadata()
{
	// Every entry: non-empty display strings, unique key, and the `global`
	// flag must agree with the InputRouting classification policy.
	int seen[64];
	int seenCount = 0;
	int globalCount = 0;
	for (const ui::ShortcutRef &s : ui::kShortcuts)
	{
		CHECK(s.key && s.key[0], "shortcut has key label");
		CHECK(s.action && s.action[0], "shortcut has action label");
		bool duplicate = false;
		for (int i = 0; i < seenCount; ++i)
			if (seen[i] == s.keycode)
				duplicate = true;
		CHECK(!duplicate, "shortcut keys are unique");
		if (seenCount < 64)
			seen[seenCount++] = s.keycode;

		const KeyRoute route = classifyKeyRoute(s.keycode);
		if (s.global)
		{
			++globalCount;
			CHECK(route == KeyRoute::GlobalShortcut, "global metadata matches GlobalShortcut policy");
		}
		else
		{
			CHECK(route == KeyRoute::GameplayShortcut, "gameplay metadata matches GameplayShortcut policy");
		}
	}

	// The documented global set is exactly F1-F12 (F10 = VSync).
	CHECK(globalCount == 12, "F1-F12 documented");
	for (int key : {SDLK_F1, SDLK_F2, SDLK_F3, SDLK_F4, SDLK_F5, SDLK_F6, SDLK_F7, SDLK_F8,
					SDLK_F9, SDLK_F10, SDLK_F11, SDLK_F12,
					SDLK_P, SDLK_C, SDLK_B, SDLK_T, SDLK_V, SDLK_X})
	{
		const ui::ShortcutRef *ref = ui::findShortcut(key);
		CHECK(ref != nullptr, "documented binding present in metadata table");
		CHECK(classifyKeyRoute(key) != KeyRoute::NotGameUIShortcut, "metadata keys are routed shortcuts");
		(void)ref;
	}

	// Escape is special-cased by the policy and must never be table metadata.
	CHECK(ui::findShortcut(SDLK_ESCAPE) == nullptr, "escape not listed as a shortcut");
}

int main()
{
	checkScale();
	checkStatusRegion();
	checkStatusStrip();
	checkShortcutMetadata();

	if (g_fails != 0)
	{
		std::cerr << g_fails << " failure(s)\n";
		return 1;
	}
	std::cout << "test_ui_shell: all checks passed\n";
	return 0;
}
