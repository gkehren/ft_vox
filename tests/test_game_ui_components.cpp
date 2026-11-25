// Reusable component, slot-contract and layout acceptance (issue #302, Epic
// #298 Phase D). CPU-only: glm + the pure UI headers, no SDL/Vulkan/ImGui.
//
// Each CHECK names the acceptance criterion it protects: geometry inside the
// viewport, tokens reaching the draw list, fallback behavior for missing
// glyphs/icons, and the bounded draw budget.
#include <Engine/PlayerUiComponents.hpp>
#include <Engine/PlayerUiGallery.hpp>
#include <Engine/PlayerUiStyle.hpp>
#include <Engine/PlayerUiText.hpp>

#include <array>
#include <cmath>

#include "TestMain.hpp"

using namespace gameui;

namespace
{
const style::Theme &theme()
{
	return style::baseline();
}

/// Synthetic face: printable ASCII, 6 px advance, one atlas UV rectangle. The
/// real face is baked by PlayerUiFonts (graphical), the policy is identical.
std::array<Glyph, 128> makeGlyphs()
{
	std::array<Glyph, 128> glyphs{};
	for (size_t c = 32; c < glyphs.size(); ++c)
		glyphs[c] = {{0.f, 0.f, 6.f, 10.f}, {0.f, 0.f, 0.1f, 0.1f}, 6.f, true};
	return glyphs;
}

const std::array<Glyph, 128> kGlyphs = makeGlyphs();
const FontView kFont{kGlyphs};

size_t countLayer(const DrawList &list, int32_t layer)
{
	size_t count = 0;
	for (const auto &quad : list.quads())
		if (quad.layer == layer)
			++count;
	return count;
}

size_t countColor(const DrawList &list, glm::vec4 color)
{
	size_t count = 0;
	for (const auto &quad : list.quads())
		if (quad.color == color)
			++count;
	return count;
}

bool inside(Clip inner, Clip outer)
{
	return inner.rect.x >= outer.rect.x - 0.01f && inner.rect.y >= outer.rect.y - 0.01f &&
		inner.rect.z <= outer.rect.z + 0.01f && inner.rect.w <= outer.rect.w + 0.01f;
}

bool overlaps(Clip a, Clip b)
{
	return a.rect.x < b.rect.z && b.rect.x < a.rect.z && a.rect.y < b.rect.w && b.rect.y < a.rect.w;
}

void slotGeometry()
{
	const Clip strip{{0.f, 100.f, 400.f, 144.f}};
	const auto grid = slotGrid(strip, 9, 1, 4.f);
	CHECK(std::abs(grid.cell - (400.f - 8.f * 4.f) / 9.f) < 0.01f,
		"a nine-slot strip sizes its cells from the width and the gaps");
	CHECK(std::abs(slotCell(grid, 1, 0).rect.x - (grid.cell + 4.f)) < 0.01f &&
			std::abs(slotCell(grid, 0, 0).rect.y - 100.f) < 0.01f &&
			std::abs(slotCell(grid, 0, 0).rect.w - (100.f + grid.cell)) < 0.01f,
		"cells are row-major and aligned to the grid origin");
	CHECK(slotCell(grid, 8, 0).rect.z <= strip.rect.z + 0.01f && slotCell(grid, 9, 0).empty(),
		"the last cell stays inside the grid and out-of-range cells are empty");
	const glm::vec2 third{(slotCell(grid, 3, 0).rect.x + slotCell(grid, 3, 0).rect.z) * 0.5f, 118.f};
	CHECK(slotIndexAt(grid, third) == 3, "a press inside a cell resolves to its slot");
	const glm::vec2 gutter{slotCell(grid, 3, 0).rect.z + 2.f, 118.f};
	CHECK(slotIndexAt(grid, gutter) == -1, "a press in the gutter hits no slot");
	CHECK(slotIndexAt(grid, {-4.f, 118.f}) == -1 && slotIndexAt(grid, {200.f, 300.f}) == -1,
		"presses outside the grid hit no slot");
	CHECK(slotGrid(strip, 9, 0, 4.f).rows == 1, "a zero-row grid degrades to one row");
	const auto heightLimited = slotGrid({{0.f, 0.f, 400.f, 20.f}}, 9, 1, 4.f);
	CHECK(std::abs(heightLimited.cell - 20.f) < 0.01f, "the cell never exceeds the grid height");

	const Clip view{{0.f, 0.f, 1280.f, 720.f}};
	const float cell = centeredSlotSize(view, 9, 4.f, 36.f);
	CHECK(std::abs(cell - 36.f) < 0.01f, "a wide viewport keeps the authored slot size");
	const auto centred = centeredSlotStrip(view, 9, cell, 4.f);
	CHECK(std::abs((centred.rect.x + centred.rect.z) * 0.5f - 640.f) < 0.01f &&
			std::abs(centred.rect.w - 720.f) < 0.01f,
		"the hotbar strip is horizontally centred and bottom-aligned");
	const auto firstCell = slotCell(slotGrid(centred, 9, 1, 4.f), 0, 0);
	CHECK(centeredSlotIndexAt(view, 9, cell, 4.f,
			  {(firstCell.rect.x + firstCell.rect.z) * 0.5f,
				  (firstCell.rect.y + firstCell.rect.w) * 0.5f}) == 0,
		"the centred strip resolves the same cells it paints");
	CHECK(centeredSlotSize({{0.f, 0.f, 120.f, 40.f}}, 9, 4.f, 36.f) < 10.f,
		"a narrow viewport shrinks the cells instead of overflowing");

	char buffer[8]{};
	CHECK(formatCount(0, buffer).empty() && formatCount(1, buffer).empty(),
		"a single item paints no count badge");
	CHECK(formatCount(2, buffer) == "2" && formatCount(99, buffer) == "99",
		"small stacks print their exact count");
	CHECK(formatCount(100, buffer) == "99+" && formatCount(4000, buffer) == "99+",
		"large stacks clamp to the badge width");
}

void tooltips()
{
	const Clip view{{0.f, 0.f, 400.f, 300.f}};
	const auto above = tooltipLayout(view, {100.f, 200.f}, {80.f, 20.f}, 4.f);
	CHECK(!above.below && above.rect.rect.x == 104.f && above.rect.rect.w == 196.f &&
			above.rect.rect.y == 176.f,
		"a tooltip prefers above-right of the pointer");
	const auto flipped = tooltipLayout(view, {380.f, 200.f}, {80.f, 20.f}, 4.f);
	CHECK(flipped.rect.rect.z <= view.rect.z && flipped.rect.rect.x == 296.f,
		"a tooltip near the right edge flips to the pointer's left");
	const auto below = tooltipLayout(view, {100.f, 10.f}, {80.f, 20.f}, 4.f);
	CHECK(below.below && below.rect.rect.y == 14.f,
		"a tooltip without room above flips below the pointer");
	const auto clamped = tooltipLayout(view, {200.f, 150.f}, {800.f, 600.f}, 4.f);
	CHECK(inside(clamped.rect, view) && clamped.rect.rect.z == 400.f,
		"an oversized tooltip clamps to the viewport instead of leaving it");
	CHECK(tooltipLayout({{0.f, 0.f, 0.f, 0.f}}, {0.f, 0.f}, {10.f, 10.f}, 4.f).rect.empty(),
		"a degenerate viewport produces no tooltip geometry");
	const float captionAdvance = kFont.measure("abc", style::typeScale(style::TextRole::Caption));
	CHECK(std::abs(tooltipExtent(kFont, "abc", theme(), 1.f).x -
			  (captionAdvance + theme().space.sm * 2.f)) < 0.01f,
		"the tooltip extent follows the caption metrics");
}

void notifications()
{
	ToastQueue queue;
	CHECK(queue.push({.title = "Saved", .lifetime = 2.f, .id = 1}) && queue.size() == 1,
		"a toast enters the queue");
	CHECK(queue.newest() && queue.newest()->id == 1 && queue.newest()->age == 0.f,
		"the newest toast is first and starts unaged");
	CHECK(queue.push({.title = "Later", .lifetime = 2.f, .id = 2}) && queue.size() == 2 &&
			queue.newest()->id == 2,
		"toasts stack newest-first");
	queue.advance(1.f);
	CHECK(queue.newest()->age == 1.f && queue.size() == 2, "advancing ages the queue");
	// Distinct lifetimes: the older toast retires first and leaves the newer.
	queue.clear();
	queue.push({.title = "Short", .lifetime = 2.f, .id = 1});
	queue.push({.title = "Long", .lifetime = 6.f, .id = 2});
	queue.advance(3.f);
	CHECK(queue.size() == 1 && queue.newest()->id == 2,
		"an expired toast retires without disturbing the others");
	CHECK(queue.push({.title = "Sticky", .lifetime = 0.f, .id = 3}), "a sticky toast is accepted");
	queue.advance(1000.f);
	CHECK(queue.size() == 1 && queue.newest()->id == 3,
		"a sticky toast survives any amount of time while the rest retire");
	CHECK(queue.dismiss(3) && queue.size() == 0 && !queue.dismiss(99),
		"dismissing removes exactly the requested toast");
	queue.clear();
	CHECK(queue.size() == 0 && queue.newest() == nullptr, "clearing empties the queue");
	for (uint32_t i = 0; i < ToastQueue::kCapacity; ++i)
		CHECK(queue.push({.title = "x", .id = 10 + i}), "the queue accepts up to its capacity");
	CHECK(!queue.push({.title = "overflow", .id = 99}) && queue.size() == ToastQueue::kCapacity &&
			queue.newest()->id == 99,
		"an overflowing toast drops the oldest instead of growing the queue");
	CHECK(queue.at(ToastQueue::kCapacity) == nullptr, "the queue is bounded");
}

void confirmations()
{
	const Clip view{{0.f, 0.f, 800.f, 600.f}};
	const auto layout = confirmationLayout(view, {420.f, 180.f}, true, theme(), 1.f);
	CHECK(inside(layout.panel, view) && inside(layout.title, layout.panel) &&
			inside(layout.body, layout.panel) && inside(layout.cancel, layout.panel) &&
			inside(layout.confirm, layout.panel),
		"every confirmation part stays inside its own panel");
	CHECK(std::abs((layout.panel.rect.x + layout.panel.rect.z) * 0.5f - 400.f) < 0.01f,
		"the confirmation panel is centred");
	CHECK(layout.cancel.rect.z <= layout.confirm.rect.x && layout.body.rect.w <= layout.cancel.rect.y,
		"cancel sits left of the primary action and the body above both");
	CHECK(confirmationFocusesCancel(true) && !confirmationFocusesCancel(false),
		"a destructive confirmation initially focuses Cancel");
	const auto small = confirmationLayout({{0.f, 0.f, 320.f, 200.f}}, {420.f, 180.f}, false, theme(), 1.f);
	CHECK(inside(small.panel, {{0.f, 0.f, 320.f, 200.f}}),
		"a confirmation clamps into a window smaller than its preferred size");
}

void progressAndIcons()
{
	CHECK(progressFraction(-1.f) == 0.f && progressFraction(0.f) == 0.f &&
			progressFraction(0.5f) == 0.5f && progressFraction(4.f) == 1.f,
		"progress fractions clamp to 0..1");
	const auto fill = progressFill({{0.f, 0.f, 100.f, 8.f}}, 0.25f);
	CHECK(std::abs(size(fill).x - 25.f) < 0.01f && fill.rect.z == 25.f,
		"a determinate bar fills its track proportionally");
	CHECK(progressFill({{0.f, 0.f, 100.f, 8.f}}, 0.f).rect.z == 0.f,
		"an empty bar paints no fill");

	CHECK(iconStatus({7, 3}, 7, 16) == IconStatus::Valid && drawable(IconStatus::Valid),
		"a current atlas handle with a live layer is drawable");
	CHECK(iconStatus({6, 3}, 7, 16) == IconStatus::StaleGeneration &&
			!drawable(IconStatus::StaleGeneration),
		"an icon from a retired atlas generation is not drawable");
	CHECK(iconStatus({7, 16}, 7, 16) == IconStatus::MissingLayer,
		"an out-of-range atlas layer is not drawable");
}

void textFitting()
{
	const auto whole = fitText(kFont, "abc", style::TextRole::Body, 1.f, 100.f);
	CHECK(whole.bytes == 3 && !whole.elided, "a label that fits is painted whole");
	const auto clipped = fitText(kFont, "abcdefghij", style::TextRole::Body, 1.f, 42.f);
	CHECK(clipped.bytes == 4 && clipped.elided,
		"an over-long label keeps the prefix that fits and asks for an ellipsis");
	CHECK(fitText(kFont, "abc", style::TextRole::Body, 1.f, 0.f).bytes == 0 &&
			fitText(kFont, "abc", style::TextRole::Body, 1.f, 10.f).bytes == 0,
		"no partial glyph is ever painted when nothing fits");
	const auto scaled = fitText(kFont, "abcdef", style::TextRole::Title, 1.f, 40.f);
	CHECK(scaled.elided && scaled.bytes < 3, "a display role fits fewer characters at the same width");
	CHECK(fitText(kFont, "a\xc3\xa9", style::TextRole::Body, 1.f, 100.f).bytes == 3,
		"fitting counts scalars, never splitting a UTF-8 sequence");

	CHECK(!usesFallbackGlyph(kFont, "abc", 1) && usesFallbackGlyph(kFont, "a\xc3\xa9", 1),
		"a non-ASCII scalar resolves to the fallback glyph");
	CHECK(glyphIndex(kFont, "a\xc3\xa9", 1) == kFallbackGlyphIndex,
		"the fallback glyph is the documented '?'");
	CHECK(drawableScalars(kFont, "a\xc3\xa9" "b") == 2,
		"one fallback glyph per unrepresentable scalar, not per byte");
	CHECK(measureRole(kFont, "abc", style::TextRole::Body, 2.f).width == 36.f &&
			measureRole(kFont, "abc", style::TextRole::Caption, 1.f).height ==
				style::lineHeight(style::TextRole::Caption),
		"role measurement applies the role ratio and the player scale");
}

void formFields()
{
	const auto &t = theme();
	const float label = style::lineHeight(style::TextRole::Caption);
	const float bare = formFieldHeight(t, 1.f);
	const float withMessage = formFieldHeight(t, 1.f, true);
	CHECK(bare == label + t.size.controlHeight,
		"a field group reserves its caption label and one control height");
	CHECK(withMessage == bare + label + t.space.xxs,
		"feedback reserves exactly one message line and its gap");
	CHECK(formFieldHeight(t, 2.f, true) == withMessage * 2.f,
		"the field group height follows the player UI scale");

	// The message is painted only when the group has room for it: in a rect a
	// caller squeezed to the bare height it is dropped instead of drawn over the
	// field border, which is the collision this contract removes.
	const auto quads = [&](float height, std::string_view message) {
		DrawList draws;
		draws.reset({640.f, 480.f}, 3);
		Surface surface(draws, kFont, t, 1.f, {{0.f, 0.f, 640.f, 480.f}});
		surface.formField({{20.f, 20.f, 300.f, 20.f + height}}, "World name", "Survival",
			style::Validation::Error, style::State::Rest, message);
		return draws.quads().size();
	};
	const size_t tallWith = quads(withMessage, "Name already in use");
	const size_t tallWithout = quads(withMessage, {});
	const size_t shortWith = quads(bare, "Name already in use");
	CHECK(tallWith > tallWithout, "a group with room paints its validation message");
	CHECK(shortWith == tallWithout, "a squeezed group drops the message instead of overlapping the well");
}

void painting()
{
	const Clip view{{0.f, 0.f, 640.f, 480.f}};
	DrawList draws;
	draws.reset({640.f, 480.f}, 7);
	Surface surface(draws, kFont, theme(), 1.f, view, 16);

	surface.card({{20.f, 20.f, 360.f, 220.f}});
	CHECK(draws.quads().size() > 0 && draws.dropped() == 0, "a card paints inside the draw list");
	surface.sectionHeader({{36.f, 36.f, 344.f, 58.f}}, "Design tokens");
	surface.button({{36.f, 80.f, 160.f, 108.f}}, "Primary", style::ButtonVariant::Primary,
		style::State::Rest);
	CHECK(countColor(draws, theme().palette.accent) >= 5,
		"a primary button is filled with the accent token");
	const size_t beforeDisabled = draws.quads().size();
	surface.button({{176.f, 80.f, 300.f, 108.f}}, "Off", style::ButtonVariant::Primary,
		style::State::Disabled);
	CHECK(draws.quads().size() > beforeDisabled,
		"a disabled button still paints a face and a border");

	surface.listRow({{36.f, 120.f, 344.f, 144.f}}, "Selected row", "12",
		style::State::Selected | style::State::Hovered);
	surface.formField({{36.f, 152.f, 344.f, 200.f}}, "World name", "Survival",
		style::Validation::Error, style::State::Focused, "Already exists");
	surface.statusIndicator({{36.f, 206.f, 344.f, 224.f}}, style::Status::Warning, "Streaming");
	surface.progress({{36.f, 208.f, 240.f, 220.f}}, 0.4f, false, 0.f);
	surface.toast({{380.f, 20.f, 620.f, 76.f}},
		{.title = "Saved", .body = "world.state", .status = style::Status::Ok});
	surface.tooltip(view, {300.f, 300.f}, "Tooltip label");
	surface.confirmation(confirmationLayout(view, {360.f, 160.f}, false, theme(), 1.f),
		"Leave world?", "Unsaved edits are kept in the save.");
	CHECK(draws.dropped() == 0 && draws.batches().size() <= DrawList::kMaxBatches,
		"the full component set fits one frame budget");

	// Clipping is a scissor, not a geometry rewrite: every batch the component
	// emits is clipped to the surface clip, so the renderer can never paint
	// outside it.
	DrawList narrow;
	narrow.reset({640.f, 480.f}, 7);
	Surface clipped(narrow, kFont, theme(), 1.f, {{500.f, 400.f, 600.f, 460.f}});
	clipped.button({{480.f, 380.f, 700.f, 430.f}}, "Wide", style::ButtonVariant::Secondary,
		style::State::Rest);
	bool scissored = !narrow.batches().empty();
	for (const auto &batch : narrow.batches())
		scissored = scissored && inside(batch.clip, {{500.f, 400.f, 600.f, 460.f}});
	CHECK(scissored, "component batches are scissored to the surface clip, never beyond it");
}

void slotPaintingAndFallbacks()
{
	const auto paint = [](const SlotView &entry, DrawList &draws) {
		Surface surface(draws, kFont, theme(), 1.f, draws.bounds(), 16);
		surface.slot({{0.f, 0.f, 36.f, 36.f}}, entry);
	};
	DrawList valid;
	valid.reset({200.f, 200.f}, 9);
	paint({.icon = {9, 3}, .count = 42}, valid);
	CHECK(countLayer(valid, 3) == 1, "a current icon paints exactly one atlas quad");
	CHECK(countLayer(valid, -2) > 0, "the stack count paints its glyphs");

	DrawList stale;
	stale.reset({200.f, 200.f}, 9);
	paint({.icon = {8, 3}, .count = 42}, stale);
	CHECK(countLayer(stale, 3) == 0 && countLayer(stale, -2) > 0 && stale.dropped() == 0,
		"a stale icon falls back to a placeholder that keeps the slot, count and frame");

	DrawList missing;
	missing.reset({200.f, 200.f}, 9);
	paint({.icon = {9, 40}, .count = 1}, missing);
	CHECK(countLayer(missing, 40) == 0 && missing.quads().size() > 0,
		"a missing atlas layer is replaced by the placeholder, not by nothing");

	DrawList durable;
	durable.reset({200.f, 200.f}, 9);
	paint({.icon = {9, 3}, .count = 1, .state = style::State::Selected, .durability = 0.15f}, durable);
	CHECK(countColor(durable, theme().palette.error) > 0,
		"an optional durability indicator colours itself from the status tokens");
	DrawList pending;
	pending.reset({200.f, 200.f}, 9);
	paint({.icon = {9, 3}, .count = 1, .state = style::State::Pending, .progress = 0.5f}, pending);
	CHECK(countColor(pending, theme().palette.accent) > 0 &&
			countColor(pending, theme().palette.pending) > 0,
		"an optional progress indicator and the pending border come from tokens");

	const auto grid = slotGrid({{0.f, 0.f, 400.f, 80.f}}, 9, 1, 4.f);
	DrawList gridDraws;
	gridDraws.reset({400.f, 80.f}, 9);
	std::array<SlotView, 9> slots{};
	slots[0] = {.icon = {9, 1}, .count = 3, .state = style::State::Selected};
	slots[4] = {.icon = {9, 2}, .count = 64};
	Surface surface(gridDraws, kFont, theme(), 1.f, gridDraws.bounds(), 16);
	surface.slotGrid(grid, slots, 4);
	CHECK(countLayer(gridDraws, 1) == 1 && countLayer(gridDraws, 2) == 1,
		"an empty slot paints its frame without an icon");
	CHECK(gridDraws.dropped() == 0, "a nine-slot grid stays inside the frame budget");
}

void hudMock()
{
	struct Case
	{
		glm::vec2 view;
		float scale;
	};
	const Case cases[] = {{{1920.f, 1080.f}, 1.f}, {{1920.f, 1080.f}, 1.5f},
		{{1920.f, 1080.f}, 2.f}, {{2560.f, 1080.f}, 1.f}, {{2560.f, 1080.f}, 2.f},
		{{1160.f, 680.f}, 1.f}, {{960.f, 540.f}, 1.25f}, {{640.f, 360.f}, 0.75f}};
	for (const auto &test : cases)
	{
		const Clip view{{0.f, 0.f, test.view.x, test.view.y}};
		const auto layout = hudMockLayout(view, theme(), test.scale);
		CHECK(layout.fits, "the HUD mock fits its viewport at every supported size and scale");
		CHECK(layout.hotbarCell > 0.f && layout.hotbar.rect.z <= view.rect.z,
			"nine hotbar slots fit side by side");
		CHECK(!overlaps(layout.status, layout.toast) && !overlaps(layout.target, layout.hotbar) &&
				!overlaps(layout.toast, layout.target),
			"the HUD mock stacks never overlap");
		const auto centre = layout.crosshair.rect;
		CHECK(std::abs((centre.x + centre.z) * 0.5f - test.view.x * 0.5f) < 0.01f &&
				std::abs((centre.y + centre.w) * 0.5f - test.view.y * 0.5f) < 0.01f,
			"the crosshair is centred on the viewport");

		DrawList draws;
		draws.reset(test.view, 5);
		Surface surface(draws, kFont, theme(), test.scale, view, 16);
		surface.crosshair(view, theme().palette.ink);
		const auto grid = slotGrid(layout.hotbar, layout.hotbarSlots, 1, layout.hotbarGap);
		std::array<SlotView, 9> slots{};
		slots[2] = {.icon = {5, 1}, .count = 12, .state = style::State::Selected};
		surface.slotGrid(grid, slots, 2);
		surface.statusIndicator(layout.status, style::Status::Ok, "60 fps  1/2/3");
		surface.toast(layout.toast,
			{.title = "Welcome", .body = "seed 42", .status = style::Status::Info});
		surface.button(layout.target, "Press E to open the inventory", style::ButtonVariant::Quiet,
			style::State::Rest);
		// The design-system budget for a HUD frame is <= 8 draws (one per scissor):
		// the hotbar strip, the crosshair, the status line, the toast, the prompt
		// and their text runs.
		bool bounded = draws.dropped() == 0 && draws.batches().size() <= 8;
		for (const auto &batch : draws.batches())
			bounded = bounded && inside(batch.clip, view);
		CHECK(bounded, "the HUD mock stays inside the 8-draw budget with no dropped quads");
		CHECK(draws.quads().size() < DrawList::kMaxQuads / 4,
			"the HUD mock leaves most of the quad budget for later surfaces");
	}
}

void gallery()
{
	CHECK(gallerySections().size() >= 8, "the gallery documents every component class");
	bool hasSlots = false, hasButtons = false, hasFields = false;
	for (const auto &section : gallerySections())
	{
		hasSlots = hasSlots || section.title.find("slots") != std::string_view::npos;
		hasButtons = hasButtons || section.title.find("Buttons") != std::string_view::npos;
		hasFields = hasFields || section.title.find("Form") != std::string_view::npos;
	}
	CHECK(hasSlots && hasButtons && hasFields,
		"slots, buttons and form/validation each have a gallery section");

	// The scale selector puts its buttons on the first line and prints the
	// effective state plus a settings notice underneath, so the section has to
	// own both caption lines.
	const auto &t = theme();
	for (const auto &section : gallerySections())
		if (section.title.find("scale") != std::string_view::npos)
			CHECK(section.height >= galleryHeaderHeight(t, 1.f) + t.size.buttonHeight + t.space.xs +
					2.f * style::lineHeight(style::TextRole::Caption) + t.space.xxs,
				"the scale section fits its buttons and both caption lines");

	const Clip wide{{0.f, 0.f, 1920.f, 1080.f}};
	const auto one = galleryLayout(wide, theme(), 1.f);
	CHECK(inside(one.panel, wide) && inside(one.body, one.panel),
		"the gallery card and its content area stay inside the viewport");
	CHECK(one.columns == 2 && one.perColumn * 2 >= gallerySections().size(),
		"a wide card lays the sections out in two columns");
	CHECK(one.contentHeight <= size(one.body).y + 0.01f,
		"the authored gallery fits its card at 1080p with no scrolling");
	for (size_t i = 0; i < gallerySections().size(); ++i)
	{
		const auto rect = gallerySectionRect(one, theme(), i);
		const size_t column = i / one.perColumn;
		const float expectedX = one.body.rect.x +
			float(column) * (one.columnWidth + one.sectionSpacing);
		CHECK(std::abs(rect.rect.x - expectedX) < 0.01f &&
				std::abs(rect.rect.z - (expectedX + one.columnWidth)) < 0.01f,
			"a gallery section spans exactly its own column");
		CHECK(rect.rect.w - rect.rect.y > galleryHeaderHeight(theme(), 1.f),
			"a gallery section is taller than its own header band");
	}
	const auto firstSection = gallerySectionRect(one, theme(), 0);
	const auto secondSection = gallerySectionRect(one, theme(), 1);
	const auto otherColumn = gallerySectionRect(one, theme(), one.perColumn);
	CHECK(std::abs(firstSection.rect.y - one.body.rect.y) < 0.01f &&
			secondSection.rect.y >= firstSection.rect.w,
		"sections in one column stack without overlapping");
	CHECK(std::abs(otherColumn.rect.y - one.body.rect.y) < 0.01f &&
			otherColumn.rect.x > firstSection.rect.z,
		"the second column restarts at the top, to the right of the first");

	// Scale: the same two-column composition at 200% keeps the same proportions.
	const auto doubled = galleryLayout({{0.f, 0.f, 3840.f, 2160.f}}, theme(), 2.f);
	CHECK(doubled.columns == 2 && std::abs(doubled.contentHeight - one.contentHeight * 2.f) < 0.5f,
		"gallery content scales with the player UI scale");

	// A small window keeps one scrolling column instead of squeezing two.
	const auto narrow = galleryLayout({{0.f, 0.f, 640.f, 360.f}}, theme(), 1.f);
	CHECK(narrow.columns == 1 && narrow.columnWidth == size(narrow.body).x,
		"a narrow window keeps a single column");
	CHECK(narrow.panel.rect.z - narrow.panel.rect.x <= 640.f &&
			narrow.contentHeight > size(narrow.body).y,
		"a narrow window scrolls its content instead of overflowing the card");
}
} // namespace

int main()
{
	slotGeometry();
	tooltips();
	notifications();
	confirmations();
	progressAndIcons();
	textFitting();
	formFields();
	painting();
	slotPaintingAndFallbacks();
	hudMock();
	gallery();
	if (testmain::fails == 0)
		std::cout << "PASS: Game UI components (" << testmain::checks << " checks)" << std::endl;;
	return testmain::exitCode();
}
