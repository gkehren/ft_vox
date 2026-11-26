// Reusable component, slot-contract and layout acceptance (issue #302, Epic
// #298 Phase D). CPU-only: glm + the pure UI headers, no SDL/Vulkan/ImGui.
//
// Each CHECK names the acceptance criterion it protects: geometry inside the
// viewport, tokens reaching the draw list, fallback behavior for missing
// glyphs/icons, and the bounded draw budget.
#include <Engine/PlayerHud.hpp>
#include <Engine/PlayerInventory.hpp>
#include <Engine/PlayerUiComponents.hpp>
#include <Engine/PlayerUiGallery.hpp>
#include <Engine/PlayerUiStyle.hpp>
#include <Engine/PlayerUiText.hpp>
#include <Items/ItemRegistry.hpp>

#include <array>
#include <cmath>
#include <string>

#include "TestMain.hpp"

using namespace gameui;

namespace
{
const style::Theme &theme()
{
	return style::baseline();
}

void inventoryScreen()
{
	using namespace inventoryui;
	for (const auto bounds : {Clip{{0, 0, 1920, 1080}}, Clip{{0, 0, 2560, 1080}},
							 Clip{{200, 40, 700, 460}}, Clip{{0, 0, 960, 540}}})
	{
		const auto layout = inventoryLayout(bounds, theme(), 2.f);
		CHECK(layout.scale > 0.f && layout.panel.intersect(bounds) == layout.panel,
			"inventory panel fits 16:9, ultrawide, docked and compact high-scale views");
		CHECK(layout.status.intersect(layout.panel) == layout.status &&
			layout.main.bounds.rect.w < layout.hotbar.bounds.rect.y &&
			layout.hotbar.bounds.rect.w < layout.drop.rect.y,
			"main grid, hotbar, and actions have separate rows");
		bool allSlotsHit = true;
		for (int i = 0; i < kSlotCount; ++i)
		{
			const SlotId id{i};
			const auto rect = layout.slot(id);
			allSlotsHit &= layout.hitSlot(glm::vec2(rect.rect) + size(rect) * 0.5f) == id;
		}
		CHECK(allSlotsHit, "all 36 painted cells hit their original authority slot");
		const auto first = layout.slot({9});
		CHECK(!layout.hitSlot({first.rect.z + layout.main.gap * 0.5f, first.rect.y}).valid(),
			"the gutter hits no slot");
	}
	const auto noAtlas = makeItemSlot(items::makeStack(items::ItemId::Stone, 4), std::nullopt);
	CHECK(noAtlas.count == 4 && noAtlas.label == "Stone" && !noAtlas.icon,
		"missing atlas keeps the item count and tooltip data");
	CHECK(makeItemSlot(items::makeStack(items::ItemId::Stone, 4), 9).icon->generation == 9,
		"item presentation uses the current atlas generation");

	InventoryView view;
	view.availability = InventoryAvailability::Ready;
	view.snapshot = Snapshot{};
	view.snapshot->revision = 1;
	view.snapshot->slots[0] = items::makeStack(items::ItemId::Stone, 5);
	Interaction input;
	const auto observe = [&] { input.observe(view.snapshot ? &*view.snapshot : nullptr, view.epoch, view.enabled()); };
	observe();
	CHECK(input.activate({0}, true).command == Command::None && input.drag().active(),
		"pickup is a presentation change only");
	observe();
	CHECK(input.drag().active(), "repainting the same inventory keeps the drag");
	CHECK(input.activate({9}, false).command == Command::Split && !input.drag().active(),
		"one drop consumes the gesture and produces one half-stack command");
	view.actionPending = true;
	observe();
	CHECK(input.activate({0}, false).command == Command::None && !input.drag().active() &&
		view.snapshot->slots[0].count == 5 && view.snapshot->slots[9].isEmpty(),
		"a delayed inventory packet leaves the confirmed counts and blocks a second submission");
	view.actionPending = false; // rejection with the same confirmed snapshot
	observe();
	CHECK(input.activate({9}, false).command == Command::None && !input.drag().active(),
		"rejection does not restore the old held stack or replay the action");
	input.activate({0}, false);
	view.snapshot->revision++;
	observe();
	CHECK(!input.drag().active() && input.activate({9}, false).command == Command::None,
		"a newer private revision invalidates a stale source even when its count is unchanged");
	input.activate({0}, false);
	view.availability = InventoryAvailability::Disconnected;
	view.snapshot.reset();
	observe();
	CHECK(!input.drag().active() && input.activate({0}, false).command == Command::None,
		"disconnect discards the drag and disables the previous inventory");
	view.availability = InventoryAvailability::Ready;
	view.epoch++;
	view.snapshot = Snapshot{};
	view.snapshot->slots[0] = items::makeStack(items::ItemId::Stone, 5);
	observe();
	input.activate({0}, false);
	view.epoch++;
	observe();
	CHECK(!input.drag().active(), "world and session epochs clear the drag even with equal snapshots");
	input.activate({0}, false);
	input.cancel();
	CHECK(!input.drag().active(), "closing the screen or losing focus cancels without an intent");
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

// PR #316 F03: the held stack stays fully inside the available game view near
// every edge and corner. Pure geometry: preferred offset, flip, then clamp.
void heldStackPlacementTest()
{
	constexpr float cell = 30.f, offset = 10.f;
	const Clip view{{0, 0, 400, 300}};
	const auto rect = [](const Clip &r) { return r.rect; };

	// Center of the room: the ordinary pointer offset is used verbatim.
	CHECK(rect(heldStackPlacement({100.f, 100.f}, cell, offset, view)) == glm::vec4(110, 110, 140, 140),
		"an interior anchor keeps the usual pointer offset");

	// Right edge: the offset flips, so the icon is not clipped away.
	const auto atRight = rect(heldStackPlacement({385.f, 100.f}, cell, offset, view));
	CHECK(atRight.x == 385.f - offset - cell && atRight.z == 385.f - offset,
		"an anchor near the right edge flips the offset to the left");

	// Bottom edge: the offset flips upward.
	const auto atBottom = rect(heldStackPlacement({100.f, 285.f}, cell, offset, view));
	CHECK(atBottom.y == 285.f - offset - cell && atBottom.w == 285.f - offset,
		"an anchor near the bottom edge flips the offset upward");

	// Bottom-right corner: both axes flip, which already fits — the rectangle
	// lands fully inside without clamping.
	const auto atCorner = rect(heldStackPlacement({378.f, 278.f}, cell, offset, view));
	CHECK(atCorner.x == 378.f - offset - cell && atCorner.y == 278.f - offset - cell &&
		atCorner.z <= 400.f && atCorner.w <= 300.f,
		"a corner anchor flips on both axes and stays inside the view");

	// The anchor can never push the icon out of the left/top edge either.
	CHECK(rect(heldStackPlacement({-50.f, -50.f}, cell, offset, view)).x == 0.f &&
		rect(heldStackPlacement({-50.f, -50.f}, cell, offset, view)).y == 0.f,
		"an out-of-view anchor clamps to the top-left corner");

	// A view with a dock offset (the central Developer UI node): clamping uses
	// the view, not the window; the corner anchor flips inside it.
	const Clip docked{{200.f, 40.f, 700.f, 460.f}};
	const auto dockedRect = rect(heldStackPlacement({690.f, 450.f}, cell, offset, docked));
	CHECK(dockedRect == glm::vec4(650, 410, 680, 440),
		"clamping respects the docked game-view rectangle");

	// Compact views: a view narrower than the cell shrinks the square instead
	// of producing a negative or outside rectangle (PR #316 F10).
	const Clip tiny{{10.f, 10.f, 40.f, 50.f}};
	const auto tinyRect = rect(heldStackPlacement({25.f, 25.f}, cell, offset, tiny));
	CHECK(tinyRect.x == 10.f && tinyRect.y >= 10.f && tinyRect.z <= 40.f && tinyRect.w <= 50.f,
		"a view smaller than the cell keeps the stack at its clamped edge");

	// F10: a viewport strictly smaller than the requested cell on both axes
	// shrinks the square; the aspect ratio stays 1:1 and nothing leaves the view.
	const Clip smaller{{10.f, 10.f, 35.f, 30.f}};
	const auto smallerRect = rect(heldStackPlacement({22.f, 20.f}, cell, offset, smaller));
	CHECK(smallerRect.z - smallerRect.x == 20.f && smallerRect.w - smallerRect.y == 20.f &&
		smallerRect.x >= 10.f && smallerRect.y >= 10.f &&
		smallerRect.z <= 35.f && smallerRect.w <= 30.f,
		"a viewport strictly smaller than the cell shrinks the square and stays inside");

	// F10: narrow and short viewports size the square to the missing axis and
	// keep the other axis at the preferred anchor.
	const Clip narrow{{0.f, 0.f, 20.f, 200.f}};
	const auto narrowRect = rect(heldStackPlacement({10.f, 100.f}, cell, offset, narrow));
	CHECK(narrowRect.z - narrowRect.x == 20.f && narrowRect.w - narrowRect.y == 20.f &&
		narrowRect.x == 0.f && narrowRect.z <= 20.f && narrowRect.w <= 200.f,
		"a narrow viewport shrinks the held stack to its width");
	const Clip shortView{{0.f, 0.f, 200.f, 15.f}};
	const auto shortRect = rect(heldStackPlacement({100.f, 8.f}, cell, offset, shortView));
	CHECK(shortRect.z - shortRect.x == 15.f && shortRect.w - shortRect.y == 15.f &&
		shortRect.y == 0.f && shortRect.z <= 200.f && shortRect.w == 15.f,
		"a short viewport shrinks the held stack to its height");

	CHECK(heldStackPlacement({0, 0}, 0.f, offset, view).empty() &&
		heldStackPlacement({0, 0}, cell, offset, Clip{{10.f, 10.f, 5.f, 50.f}}).empty(),
		"degenerate cells and empty views place nothing");
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

	// F07: the queue owns its text. A toast pushed from temporaries survives
	// their destruction, and an over-long field is bounded on a scalar boundary.
	{
		std::string title = "Saved ";
		title += "world";
		ToastQueue owned;
		owned.push({.title = title, .body = std::string(title) + " state", .id = 1});
		title.assign("destroyed");
		CHECK(owned.newest() && owned.newest()->title == "Saved world" &&
				owned.newest()->body == "Saved world state",
			"a toast keeps its own copy after the source strings are gone");
	}
	{
		ToastQueue bounded;
		const std::string longText(size_t(Toast::kMaxText) + 40, 'x');
		bounded.push({.title = longText, .body = longText, .id = 1});
		CHECK(bounded.newest() && bounded.newest()->title.size() == Toast::kMaxText &&
				bounded.newest()->body.size() == Toast::kMaxText,
			"an over-long notification is bounded instead of growing the queue");
	}

	// F10: ageing is driven by elapsed time, so the wall-clock lifetime is the
	// same at every frame rate (30 / 60 / 144 / 240 Hz).
	const auto expiredAfter = [](float hz) {
		ToastQueue timed;
		timed.push({.title = "t", .lifetime = 1.f, .id = 1});
		float elapsed = 0.f;
		for (int frame = 0; frame < int(hz) + 1 && timed.size(); ++frame)
		{
			timed.advance(presentationStep(1.f / hz));
			elapsed += 1.f / hz;
		}
		return timed.size() == 0 ? elapsed : 1000.f;
	};
	float worst = 0.f;
	for (const float hz : {30.f, 60.f, 144.f, 240.f})
		worst = std::max(worst, std::abs(expiredAfter(hz) - 1.f));
	CHECK(worst < 0.05f, "a one-second toast expires after one second at 30-240 FPS");
	CHECK(presentationStep(-1.f) == 0.f && presentationStep(10.f) == 0.25f,
		"a negative or hitch-long frame step is clamped for presentation timers");
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

	// F05: one viewport transform for the whole layout. Placing a scrolled
	// confirmation is identical to laying it out at the placed rect, so the
	// panel and the interactive buttons can never drift apart while scrolling.
	ScrollViewport scrolled{{{0.f, 0.f, 800.f, 400.f}}, 900.f, 0.f};
	scrolled.setOffset(150.f);
	const auto authored = confirmationLayout({{0.f, 600.f, 800.f, 780.f}}, {420.f, 180.f}, true,
		theme(), 1.f);
	const auto placedPanel = scrolled.place(authored.panel);
	const auto placedCancel = scrolled.place(authored.cancel);
	const auto placedConfirm = scrolled.place(authored.confirm);
	const auto rebuilt = confirmationLayout(scrolled.place({{0.f, 600.f, 800.f, 780.f}}),
		{420.f, 180.f}, true, theme(), 1.f);
	CHECK(placedPanel == rebuilt.panel && placedCancel == rebuilt.cancel &&
			placedConfirm == rebuilt.confirm,
		"a scrolled confirmation keeps panel, cancel and confirm in one coordinate space");
	CHECK(rebuilt.cancel.rect.z <= rebuilt.confirm.rect.x &&
			inside(rebuilt.cancel, rebuilt.panel) && inside(rebuilt.confirm, rebuilt.panel),
		"the scrolled confirmation's buttons still align with its panel");
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

	// F12: a fitted label stays inside its own component rect, a border can never
	// be wider than the box it frames and an indeterminate band never leaves its
	// track — regardless of how narrow the caller's geometry is.
	const auto outsideText = [&](const DrawList &list, const Clip &box) {
		for (const auto &quad : list.quads())
			if (quad.layer == -2 && (quad.rect.x < box.rect.x - 0.01f ||
									 quad.rect.z > box.rect.z + 0.01f ||
									 quad.rect.y < box.rect.y - 0.01f ||
									 quad.rect.w > box.rect.w + 0.01f))
				return true;
		return false;
	};
	const Clip buttonBox{{20.f, 300.f, 60.f, 328.f}};
	const Clip ringBox{{100.f, 300.f, 104.f, 304.f}};
	const Clip trackBox{{140.f, 300.f, 146.f, 304.f}};
	DrawList tightDraws;
	tightDraws.reset({640.f, 480.f}, 7);
	Surface tight(tightDraws, kFont, theme(), 1.f, view);
	tight.button(buttonBox, "An extremely long button label", style::ButtonVariant::Primary,
		style::State::Rest);
	DrawList isolated;
	isolated.reset({640.f, 480.f}, 7);
	Surface ringSurface(isolated, kFont, theme(), 1.f, view);
	ringSurface.ring(ringBox, theme().palette.border, 20.f, 2.f);
	bool ringInside = true;
	for (const auto &quad : isolated.quads())
		ringInside = ringInside && inside(Clip{quad.rect}, ringBox);
	CHECK(!outsideText(tightDraws, buttonBox), "a long label is fitted inside its button");
	CHECK(ringInside, "a border wider than its box is clamped to the geometry");
	DrawList band;
	band.reset({640.f, 480.f}, 7);
	Surface bandSurface(band, kFont, theme(), 1.f, view);
	bandSurface.progress(trackBox, 0.f, true, 1.f);
	bool bandInside = true;
	for (const auto &quad : band.quads())
		bandInside = bandInside && quad.rect.x >= trackBox.rect.x - 0.01f &&
			quad.rect.z <= trackBox.rect.z + 0.01f;
	CHECK(bandInside, "an indeterminate band is clamped to its progress track");
}

void notificationLifetime()
{
	// R08: ageing a live queue updates the entries in place — no copy, no move,
	// no allocation — and only an actual expiry compacts the array.
	ToastQueue queue;
	Toast first;
	first.title = "First toast title that is long enough to heap allocate";
	first.body = "Body text that also lives on the heap for this entry";
	first.lifetime = 30.f;
	Toast second;
	second.title = "Second toast";
	second.status = style::Status::Error;
	second.lifetime = 30.f;
	queue.push(first);
	queue.push(second);
	const char *title0 = queue.at(0)->title.data();
	const char *title1 = queue.at(1)->title.data();
	const auto capacity0 = queue.at(0)->title.capacity();
	for (int i = 0; i < 20; ++i)
		queue.advance(presentationStep(1.f / 60.f));
	CHECK(queue.size() == 2 && queue.at(0)->title.data() == title0 &&
			queue.at(1)->title.data() == title1 && queue.at(0)->title.capacity() == capacity0,
		"ageing a live notification queue performs no string copy, move or reallocation");
	CHECK(queue.at(0)->title == second.title && queue.at(1)->title == first.title,
		"in-place ageing preserves the newest-first ordering");
	CHECK(std::abs(queue.at(0)->age - queue.at(1)->age) < 0.0001f && queue.at(0)->age > 0.f,
		"both entries age by the same monotonic step");

	// Expiry still retires the right entry and keeps the survivors intact.
	ToastQueue expiring;
	Toast quick, sticky;
	quick.title = "Quick";
	quick.lifetime = 0.1f;
	sticky.title = "Sticky";
	sticky.lifetime = 0.f;
	expiring.push(quick);
	expiring.push(sticky);
	expiring.advance(0.05f);
	CHECK(expiring.size() == 2, "a notification shorter than its lifetime survives");
	expiring.advance(0.06f);
	CHECK(expiring.size() == 1 && expiring.newest()->title == "Sticky",
		"expiry removes only the aged entry and keeps the sticky one");
	expiring.advance(10.f);
	CHECK(expiring.size() == 1 && expiring.newest()->title == "Sticky",
		"a sticky notification never expires");
	CHECK(expiring.dismiss(expiring.newest()->id) && expiring.size() == 0,
		"dismissing the last notification empties the queue");

	// The bounded capacity still drops the oldest entry.
	ToastQueue bounded;
	for (int i = 0; i < int(ToastQueue::kCapacity) + 2; ++i)
	{
		Toast toast;
		toast.title = "Toast " + std::to_string(i);
		toast.lifetime = 30.f;
		bounded.push(toast);
	}
	CHECK(bounded.size() == ToastQueue::kCapacity && bounded.newest()->title == "Toast 5" &&
			bounded.at(ToastQueue::kCapacity - 1)->title == "Toast 2",
		"the bounded queue keeps the newest capacity entries in order");
}

void degenerateGeometry()
{
	// R05: every component clamps to its own rectangle, including empty,
	// fractional, narrow and vertically constrained geometry.
	const auto &t = theme();
	const auto outside = [](const DrawList &list, Clip box) {
		for (const auto &quad : list.quads())
			if (quad.rect.x < box.rect.x - 0.01f || quad.rect.y < box.rect.y - 0.01f ||
				quad.rect.z > box.rect.z + 0.01f || quad.rect.w > box.rect.w + 0.01f)
				return true;
		return false;
	};
	struct Case
	{
		const char *name;
		Clip box;
	};
	const Case cases[] = {
		{"an empty", {{40.f, 40.f, 40.f, 40.f}}},
		{"an inverted", {{60.f, 60.f, 40.f, 40.f}}},
		{"a fractional", {{20.f, 20.f, 20.5f, 20.5f}}},
		{"a narrow", {{0.f, 0.f, 3.f, 60.f}}},
		{"a flat", {{0.f, 0.f, 60.f, 0.5f}}},
	};
	for (const auto &test : cases)
	{
		DrawList draws;
		draws.reset({640.f, 480.f}, 3);
		Surface surface(draws, kFont, t, 1.f, {{0.f, 0.f, 640.f, 480.f}}, 16);
		surface.fill(test.box, t.palette.surface, 4.f);
		surface.ring(test.box, t.palette.border, 20.f, 4.f);
		surface.frame(test.box, t.palette.surface, t.palette.border, 9.f, 3.f);
		surface.sectionHeader(test.box, "A section header that cannot fit");
		surface.divider(test.box);
		surface.labelAligned(test.box, "A long aligned label", style::TextRole::Body, t.palette.ink,
			1.f);
		surface.fittedLabel(test.box, "A long fitted label", style::TextRole::Body, t.palette.ink,
			0.f);
		surface.progress(test.box, 0.5f, false, 0.f);
		surface.progress(test.box, 0.f, true, 1.f);
		// S02: composite components clamp their decorations to the same rect.
		surface.statusIndicator(test.box, style::Status::Warning, "Reconnecting to the world");
		surface.toast(test.box, Toast{.title = "Welcome back", .body = "Seed 1337",
			.status = style::Status::Error, .lifetime = 4.f, .id = 1});
		surface.slot(test.box, SlotView{.icon = AtlasIcon{3, 1}, .count = 64,
			.progress = 0.8f});
		CHECK(!outside(draws, test.box),
			"no primitive leaves " << test.name << " component rectangle");
		CHECK(draws.dropped() == 0, "degenerate geometry never exhausts the draw budget");
	}

	// S02: a toggle and a slider clamp their own decorations to the widget bounds,
	// so an undersized row never paints outside itself.
	{
		Toggle toggle{true};
		Slider slider{0.35f};
		const Clip boxes[] = {
			{{10.f, 10.f, 90.f, 40.f}},   // nominal
			{{10.f, 10.f, 24.f, 40.f}},   // narrower than the switch
			{{10.f, 10.f, 90.f, 14.f}},   // shorter than the switch
			{{10.f, 10.f, 12.f, 12.f}},   // undersized in both axes
		};
		for (const auto &box : boxes)
		{
			WidgetSet set;
			set.reset({{0.f, 0.f, 200.f, 200.f}}, 200.f, 0.f);
			set.add({1, box, "Toggle label that is too long", {}, &toggle});
			set.add({2, Clip{{box.rect.x, box.rect.w + 4.f, box.rect.z, box.rect.w + 44.f}},
				"Slider label that is too long", {}, &slider});
			DrawList draws;
			draws.reset({200.f, 200.f}, 3);
			set.paint(draws, kFont, 1.f, {}, {-10.f, -10.f}, theme());
			// The two widgets are separated by a 4 px gap, so a primitive is
			// attributed by its vertical center and must stay inside its own box.
			const Clip row{{box.rect.x, box.rect.y, box.rect.z, box.rect.w}};
			const Clip track{{box.rect.x, box.rect.w + 4.f, box.rect.z, box.rect.w + 44.f}};
			const auto within = [](const Quad &quad, Clip box) {
				return quad.rect.x >= box.rect.x - 0.01f && quad.rect.z <= box.rect.z + 0.01f &&
					quad.rect.y >= box.rect.y - 0.01f && quad.rect.w <= box.rect.w + 0.01f;
			};
			bool toggleInside = true, sliderInside = true;
			for (const auto &quad : draws.quads())
			{
				const float center = (quad.rect.y + quad.rect.w) * 0.5f;
				if (center < box.rect.w + 2.f)
					toggleInside = toggleInside && within(quad, row);
				else
					sliderInside = sliderInside && within(quad, track);
			}
			CHECK(toggleInside && sliderInside,
				"a toggle and a slider stay inside a " << size(box).x << 'x' << size(box).y
					<< " widget");
			CHECK(draws.dropped() == 0, "clamped toggle and slider never drop a primitive");
		}
	}

	// S01: a label between half and a full line height clips its glyphs
	// geometrically instead of spilling over the neighbouring component.
	{
		const style::TextRole role = style::TextRole::Body;
		const float nominal = style::lineHeight(role);
		for (const float scale : {1.f, 1.5f, 2.f})
			for (const float ratio : {0.6f, 0.75f, 0.9f})
			{
				const Clip box{{10.f, 10.f, 130.f, 10.f + nominal * scale * ratio}};
				DrawList draws;
				draws.reset({640.f, 480.f}, 3);
				Surface surface(draws, kFont, theme(), scale, {{0.f, 0.f, 640.f, 480.f}}, 16);
				surface.fittedLabel(box, "Clipped caption", role, theme().palette.ink, 0.f);
				size_t glyphs = 0, remapped = 0;
				bool inside = true;
				for (const auto &quad : draws.quads())
					if (quad.layer == -2)
					{
						++glyphs;
						inside = inside && quad.rect.x >= box.rect.x - 0.01f &&
							quad.rect.z <= box.rect.z + 0.01f && quad.rect.y >= box.rect.y - 0.01f &&
							quad.rect.w <= box.rect.w + 0.01f;
						// A clipped glyph keeps its UVs inside the source glyph.
						remapped += quad.uv.y >= -0.0001f && quad.uv.w <= 1.0001f &&
							quad.uv.w > quad.uv.y;
					}
				CHECK(glyphs > 0 && inside,
					"a label at " << ratio * 100.f << "% of its line height stays inside its rect at "
						<< scale * 100.f << "% scale");
				CHECK(remapped == glyphs,
					"clipped glyphs keep a valid, non-inverted UV rectangle");
				CHECK(draws.batches().size() <= 2,
					"glyph clipping does not add a scissor batch (batches="
						<< draws.batches().size() << ')');
			}
	}

	// A section header and a divider keep their rules inside a short rect.
	DrawList shortDraws;
	shortDraws.reset({640.f, 480.f}, 3);
	Surface shortSurface(shortDraws, kFont, t, 1.f, {{0.f, 0.f, 640.f, 480.f}}, 16);
	const Clip header{{10.f, 10.f, 130.f, 34.f}};
	const Clip rule{{10.f, 60.f, 130.f, 61.f}};
	shortSurface.sectionHeader(header, "Streaming");
	shortSurface.divider(rule);
	CHECK(!outside(shortDraws, {{10.f, 10.f, 130.f, 61.f}}),
		"section separators and dividers stay inside their bounds");
}

void slotPaintingAndFallbacks()
{
	const auto paint = [](const SlotView &entry, DrawList &draws) {
		Surface surface(draws, kFont, theme(), 1.f, draws.bounds(), 16);
		surface.slot({{0.f, 0.f, 36.f, 36.f}}, entry);
	};
	DrawList valid;
	valid.reset({200.f, 200.f}, 9);
	paint({.icon = AtlasIcon{9, 3}, .count = 42}, valid);
	CHECK(countLayer(valid, 3) == 1, "a current icon paints exactly one atlas quad");
	CHECK(countLayer(valid, -2) > 0, "the stack count paints its glyphs");

	DrawList stale;
	stale.reset({200.f, 200.f}, 9);
	paint({.icon = AtlasIcon{8, 3}, .count = 42}, stale);
	CHECK(countLayer(stale, 3) == 0 && countLayer(stale, -2) > 0 && stale.dropped() == 0,
		"a stale icon falls back to a placeholder that keeps the slot, count and frame");

	DrawList missing;
	missing.reset({200.f, 200.f}, 9);
	paint({.icon = AtlasIcon{9, 40}, .count = 1}, missing);
	CHECK(countLayer(missing, 40) == 0 && missing.quads().size() > 0,
		"a missing atlas layer is replaced by the placeholder, not by nothing");

	DrawList durable;
	durable.reset({200.f, 200.f}, 9);
	paint({.icon = AtlasIcon{9, 3}, .count = 1, .state = style::State::Selected, .durability = 0.15f}, durable);
	CHECK(countColor(durable, theme().palette.error) > 0,
		"an optional durability indicator colours itself from the status tokens");
	DrawList pending;
	pending.reset({200.f, 200.f}, 9);
	paint({.icon = AtlasIcon{9, 3}, .count = 1, .state = style::State::Pending, .progress = 0.5f}, pending);
	CHECK(countColor(pending, theme().palette.accent) > 0 &&
			countColor(pending, theme().palette.pending) > 0,
		"an optional progress indicator and the pending border come from tokens");

	const auto grid = slotGrid({{0.f, 0.f, 400.f, 80.f}}, 9, 1, 4.f);
	DrawList gridDraws;
	gridDraws.reset({400.f, 80.f}, 9);
	std::array<SlotView, 9> slots{};
	slots[0] = {.icon = AtlasIcon{9, 1}, .count = 3, .state = style::State::Selected};
	slots[4] = {.icon = AtlasIcon{9, 2}, .count = 64};
	Surface surface(gridDraws, kFont, theme(), 1.f, gridDraws.bounds(), 16);
	surface.slotGrid(grid, slots, 4);
	CHECK(countLayer(gridDraws, 1) == 1 && countLayer(gridDraws, 2) == 1,
		"an empty slot paints its frame without an icon");
	CHECK(gridDraws.dropped() == 0, "a nine-slot grid stays inside the frame budget");

	// F08: occupancy and icon validity are separate facts. Atlas layer 0 is a
	// real texture (BEDROCK), an occupied slot without a drawable icon keeps its
	// count and its frame, and an empty slot stays empty.
	DrawList bedrock;
	bedrock.reset({200.f, 200.f}, 9);
	paint({.icon = AtlasIcon{9, 0}, .count = 5}, bedrock);
	CHECK(countLayer(bedrock, 0) == 1 && countColor(bedrock, theme().palette.error) == 0,
		"atlas layer 0 is a valid icon, so an occupied BEDROCK-style slot draws");

	DrawList noIcon;
	noIcon.reset({200.f, 200.f}, 9);
	paint({.count = 5}, noIcon);
	CHECK(countLayer(noIcon, -2) > 0 && countLayer(noIcon, 0) == 0,
		"an occupied slot without an icon keeps its count and paints the placeholder");

	DrawList emptySlot;
	emptySlot.reset({200.f, 200.f}, 9);
	paint({}, emptySlot);
	CHECK(countLayer(emptySlot, -2) == 0 && SlotView{}.empty(),
		"an empty slot paints no placeholder and no badge");
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
		slots[2] = {.icon = AtlasIcon{5, 1}, .count = 12, .state = style::State::Selected};
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

void liveHud()
{
	using enum style::State;
	std::array<items::ItemStack, items::kInventorySize> snapshot{};
	snapshot[0] = {items::ItemId::Stone, 12};
	snapshot[1] = {items::ItemId::Dirt, 3};
	snapshot[5] = {static_cast<items::ItemId>(UINT16_MAX), 4};
	snapshot[6] = {items::ItemId::Stone, UINT16_MAX};
	const auto first = makeHudHotbar(snapshot, 0, 15, true);
	CHECK(first.available && first.selectedName == "Stone" && first.canPlace &&
		first.slots[0].count == 12 && first.slots[0].state == (Selected | Pending),
		"pending preserves the authoritative count and selection");
	CHECK(first.slots[0].icon && first.slots[0].icon->generation == 15 &&
		first.slots[0].icon->layer == STONE, "the real atlas layer is tagged with this frame's generation");
	CHECK(first.slots[5].empty() && !first.slots[5].icon && first.slots[6].empty(),
		"unknown item and invalid quantity paint empty slots");

	snapshot[0].count = 11;
	const auto second = makeHudHotbar(snapshot, 1, 16, false);
	CHECK(second.slots[0].count == 11 && second.slots[0].state == Rest &&
		second.slots[0].icon && second.slots[0].icon->generation == 16 &&
		second.slots[1].state == Selected && second.selectedName == "Dirt",
		"next snapshot replaces counts, selection and atlas handles");
	CHECK(!makeHudHotbar({}, 0, 16, false).available,
		"absent snapshot produces no hotbar, including after an occupied frame");
	const auto invalid = makeHudHotbar(snapshot, -1, 16, false);
	CHECK(invalid.selectedName.empty() && invalid.slots[0].state == Rest,
		"invalid selection neither indexes a slot nor highlights a substitute");
	CHECK(makeHudHotbar(snapshot, 8, 16, false).selectedName == "Empty slot",
		"authoritative selection of an empty slot remains visible");

	struct Case { Clip scene, safe; bool aim; };
	const std::array cases{
		Case{{{0, 0, 1920, 1080}}, {{0, 0, 1920, 1080}}, true},
		Case{{{0, 0, 2560, 1080}}, {{590, 26, 1940, 766}}, true},
		Case{{{0, 0, 960, 540}}, {{224, 24, 695, 385}}, true},
		Case{{{0, 0, 960, 540}}, {{520, 24, 900, 385}}, false},
		Case{{{0, 0, 960, 540}}, {{310, 20, 740, 295}}, false},
	};
	for (const auto &test : cases)
	{
		for (const float scale : {1.f, 1.5f, 2.f})
		{
			const auto layout = hudLayout(test.scene, test.safe, theme(), scale);
			const auto &bar = layout.hotbar.bounds;
			const auto last = slotCell(layout.hotbar, items::kHotbarSize - 1, 0);
			CHECK(layout.scale > 0.f && layout.scale <= scale && inside(bar, test.safe) &&
				!last.empty() && inside(last, bar) &&
				std::abs(bar.rect.x + bar.rect.z - test.safe.rect.x - test.safe.rect.z) < 0.01f,
				"all nine slots stay at the bottom center of the safe area");
			CHECK(inside(layout.status, test.safe) && inside(layout.activity, test.safe) &&
				inside(layout.recovery, test.safe) && inside(layout.hint, test.safe) &&
				inside(layout.selection, test.safe) &&
				layout.activity.rect.w < layout.recovery.rect.y &&
				layout.recovery.rect.w < layout.hint.rect.y &&
				layout.hint.rect.w < layout.selection.rect.y &&
				layout.selection.rect.w < bar.rect.y,
				"feedback, hints, selection and the bar occupy separate bands");
			CHECK(layout.detailLines >= 1 && layout.detailLines <= kHudDetailLines,
				"the detail keeps at least one line at scale " << scale);
			// F02: geometry is content-independent. The aim point follows the FULL
			// window projection (never the safe area's centre) and empty bands
			// cannot take it away.
			const glm::vec2 aim = glm::vec2(test.scene.rect) + size(test.scene) * 0.5f;
			CHECK((glm::vec2(layout.reticle.rect) + size(layout.reticle) * 0.5f) == aim,
				"the reticle geometry is the scene's exact centre at scale " << scale);
			HudView empty{};
			empty.hotbar = second;
			empty.aim = test.aim;
			CHECK(resolveHud(layout, empty).reticle == test.aim,
				"empty status and activity bands cannot hide the reticle at scale " << scale);

			DrawList draws;
			draws.reset(size(test.scene), 16);
			const Surface paint(draws, kFont, theme(), layout.scale, layout.view);
			HudView view{};
			view.hotbar = second;
			view.status = "Network degraded";
			view.activity = "Player snapshots or input acknowledgments are delayed.";
			view.recovery = "Gameplay continues; try Reconnect if it persists.";
			view.aim = view.captured = view.target = true;
			const auto resolved = resolveHud(layout, view);
			CHECK(resolved.reticle == test.aim,
				"true aim point visibility at scale " << scale << " in safe area starting at "
					<< test.safe.rect.x << ", " << test.safe.rect.y);
			paintHud(paint, layout, view);
			CHECK(draws.dropped() == 0 && countLayer(draws, STONE) == 1 && countLayer(draws, DIRT) == 1,
				"the HUD budget carries exactly one icon per populated slot");
			for (const auto &batch : draws.batches())
				CHECK(inside(batch.clip, test.safe), "HUD draw batches never extend into developer docks");
			for (const auto &quad : draws.quads())
				CHECK(inside({quad.rect}, layout.view), "no HUD quad leaves the unobstructed area");
		}
	}

	// F02: the reticle wins over copy that happens to sit on the aim point, and
	// only visible content is allowed to collide with it.
	auto overlapped = hudLayout(cases[1].scene, cases[1].safe, theme(), 1.f);
	overlapped.status = overlapped.reticle;
	overlapped.activity = overlapped.reticle;
	overlapped.recovery = overlapped.reticle;
	HudView blank{};
	blank.hotbar = second;
	blank.aim = true;
	auto resolved = resolveHud(overlapped, blank);
	CHECK(resolved.reticle && !resolved.status && !resolved.activity && !resolved.recovery,
		"empty bands reserve nothing at the aim point");
	HudView occupied = blank;
	occupied.status = "Timed out";
	occupied.activity = "The server did not respond in time.";
	occupied.recovery = recoveryInstruction(false, true);
	resolved = resolveHud(overlapped, occupied);
	CHECK(resolved.reticle && !resolved.status && !resolved.activity && !resolved.recovery,
		"secondary copy yields to the reticle instead of hiding it");

	// A short safe area puts the optional hint band on the aim point: the hint
	// yields, the aim point survives.
	const Clip shortScene{{0.f, 0.f, 640.f, 180.f}};
	const auto shortLayout = hudLayout(shortScene, shortScene, theme(), 1.f);
	CHECK(!shortLayout.reticle.empty() && !shortLayout.hint.intersect(shortLayout.reticle).empty(),
		"the compact layout really overlaps the hint band and the aim point");
	HudView hints{};
	hints.hotbar = second;
	hints.aim = true;
	hints.hints = true;
	CHECK(resolveHud(shortLayout, hints).reticle && !resolveHud(shortLayout, hints).hint,
		"the optional hint yields to the reticle");
	hints.aim = false;
	CHECK(resolveHud(shortLayout, hints).hint, "without an aim point the hint is painted again");

	// F03: bounded multiline feedback — the wrap never overflows, the message
	// keeps its beginning, and the block stays above the hint band.
	std::array<WrappedLine, kMaxWrappedLines> lines{};
	const std::string_view timeout =
		"The server did not respond in time. Check the endpoint or try Reconnect.";
	const float lineWidth = 120.f; // six pixels per glyph in the synthetic face
	const int bounded = wrapText(kFont, timeout, style::TextRole::Body, 1.f, lineWidth,
		std::span<WrappedLine>(lines.data(), 2));
	CHECK(bounded == 2 && lines[0].begin == 0 && lines[0].end > lines[0].begin,
		"a long session message keeps its beginning and is bounded to the reserved lines");
	CHECK(lines[1].elided, "the last reserved line asks for the ellipsis");
	CHECK(std::string_view(timeout.substr(lines[0].begin, lines[0].end - lines[0].begin)) ==
			"The server did not", "words stay whole and the first line is what fits");
	for (int index = 0; index < bounded; ++index)
		CHECK(kFont.measure(timeout.substr(lines[size_t(index)].begin,
			lines[size_t(index)].end - lines[size_t(index)].begin), 1.f) <= lineWidth,
			"a wrapped line never exceeds the band width");
	const int single = wrapText(kFont, "Timed out", style::TextRole::Body, 1.f, lineWidth,
		std::span<WrappedLine>(lines.data(), 2));
	CHECK(single == 1 && !lines[0].elided, "a short headline stays on one line without an ellipsis");
	const std::string longWord(40, 'A');
	const int broken = wrapText(kFont, longWord, style::TextRole::Body, 1.f, lineWidth,
		std::span<WrappedLine>(lines.data(), 2));
	CHECK(broken == 2 && lines[0].end - lines[0].begin == 20,
		"a word wider than the band breaks at the glyph budget instead of overflowing");

	// F06: the creative block selection is explicit, bounded and independent of
	// the optional hints.
	char creativeBuffer[48];
	CHECK(creativeLabel("Stone", creativeBuffer) == "Creative: Stone",
		"the creative selection carries an explicit prefix");
	CHECK(creativeLabel({}, creativeBuffer) == "Creative",
		"a creative session without a resolved block still names the mode");
	const auto longLabel = creativeLabel(std::string(60, 'X'), creativeBuffer);
	CHECK(longLabel.size() == sizeof(creativeBuffer) - 1 && creativeBuffer[longLabel.size()] == '\0',
		"a long block name is bounded by the caller's buffer");
	HudView creative{};
	creative.hotbar = second;
	creative.creative = true;
	creative.creativeBlock = "Stone";
	creative.hints = false;
	const auto creativeLayout = hudLayout(cases[0].scene, cases[0].safe, theme(), 1.f);
	CHECK(resolveHud(creativeLayout, creative).selection,
		"the creative selection is painted with the hints disabled");
	DrawList creativeDraws;
	creativeDraws.reset(size(cases[0].scene), 16);
	paintHud(Surface(creativeDraws, kFont, theme(), creativeLayout.scale, creativeLayout.view),
		creativeLayout, creative);
	HudView plain = creative;
	plain.creative = false;
	DrawList plainDraws;
	plainDraws.reset(size(cases[0].scene), 16);
	paintHud(Surface(plainDraws, kFont, theme(), creativeLayout.scale, creativeLayout.view),
		creativeLayout, plain);
	CHECK(creativeDraws.quads().size() > plainDraws.quads().size(),
		"the explicit prefix paints its own glyphs on the selection band");

	const auto layout = hudLayout(cases[1].scene, cases[1].safe, theme(), 1.f);
	DrawList reticle;
	reticle.reset(size(cases[1].scene), 16);
	HudView aimOnly{};
	aimOnly.aim = true;
	paintHud(Surface(reticle, kFont, theme(), layout.scale, layout.view), layout, aimOnly);
	CHECK(reticle.quads().size() == 4, "a single outlined reticle costs four quads");
	for (const auto &quad : reticle.quads())
		CHECK((glm::vec2(quad.rect) + glm::vec2(quad.rect.z, quad.rect.w)) * 0.5f == glm::vec2(1280, 540),
			"an asymmetric dock does not recenter the aim");
}

// Issue #303 F07: the session table maps transport health onto HUD copy, the
// bar and the reticle, so a degraded session cannot accidentally keep a stale
// inventory or an aim cursor over blocked gameplay.
void hudSessionRows()
{
	using enum HudSessionState;
	auto session = hudSession(Connected, true, "Connected", {});
	CHECK(session.status.empty() && session.showHotbar && session.allowAim,
		"connected and ready shows the normal HUD without a headline");

	session = hudSession(Degraded, true, "Degraded network",
		"Player snapshots or input acknowledgments are delayed.");
	CHECK(session.showHotbar && session.allowAim && session.kind == style::Status::Warning &&
			session.status == "Degraded network" && !session.activity.empty(),
		"degraded and ready keeps the normal HUD and adds a network warning");

	session = hudSession(Degraded, false, "Degraded network",
		"Player snapshots or input acknowledgments are delayed.");
	CHECK(!session.showHotbar && !session.allowAim && session.kind == style::Status::Pending &&
			session.status == "Synchronizing session" &&
			session.recovery == "The hotbar returns when the session is ready.",
		"degraded and not ready replaces the HUD with synchronization feedback");

	session = hudSession(Connected, false, "Connected", {});
	CHECK(!session.showHotbar && !session.allowAim && session.kind == style::Status::Pending &&
			session.status == "Synchronizing session" &&
			session.recovery == "The hotbar returns when the session is ready.",
		"readiness outranks the connected label: synchronization, no bar, no reticle");

	session = hudSession(Pending, false, "Resyncing", "Rejoining to refresh the world replica.");
	CHECK(!session.showHotbar && !session.allowAim && session.kind == style::Status::Pending &&
			session.status == "Resyncing" &&
			session.activity == "Rejoining to refresh the world replica.",
		"resyncing shows pending feedback and no inventory");

	struct EndState { std::string_view label, detail; };
	const std::array endStates{
		EndState{"Timed out",
			"The server did not respond in time. Check the endpoint or try Reconnect."},
		EndState{"Rejected", "Protocol version mismatch. Use matching client and server builds."},
		EndState{"Rejected", "Server is full"},
		EndState{"Disconnected", "The server shut down. Reconnect when it is running again."},
	};
	for (const auto &end : endStates)
	{
		session = hudSession(Blocked, false, end.label, end.detail);
		// The steps depend on the presentation state only the host knows, so the
		// row flags the block and the host fills `recovery` (F03/F12).
		CHECK(session.kind == style::Status::Error && session.status == end.label &&
				session.activity == end.detail && session.blocked && session.recovery.empty() &&
				!session.showHotbar && !session.allowAim,
			"a blocked session keeps its actionable detail and flags recovery: " << end.label);
	}
	session = hudSession(Blocked, false, "Disconnected", {});
	CHECK(session.activity == "The session ended." && session.blocked,
		"a blocked session without transport detail still explains itself");

	// F05: the optional showcase strip yields to real session copy only.
	HudView quiet{};
	CHECK(!hudHasSessionFeedback(quiet), "the showcase strip keeps the band while the HUD is quiet");
	quiet.status = "Synchronizing session";
	CHECK(hudHasSessionFeedback(quiet), "a status headline suppresses the demonstration strip");
	quiet.status = {};
	quiet.recovery = recoveryInstruction(false, false);
	CHECK(hudHasSessionFeedback(quiet), "a recovery line alone also suppresses the strip");

	// F03: the blocked copy plus its reconnect steps fit the bounded feedback
	// block at every supported scale and in both workspace states, without a
	// drop and without leaving the gameplay rectangle.
	for (const float scale : {1.f, 1.5f, 2.f})
	{
		for (const bool workspaceVisible : {true, false})
		{
			const auto layout =
				hudLayout({{0.f, 0.f, 960.f, 540.f}}, {{0.f, 0.f, 960.f, 540.f}}, theme(), scale);
			const auto timedOut = hudSession(Blocked, false, "Timed out",
				"The server did not respond in time. Check the endpoint or try Reconnect.");
			HudView view{};
			view.status = timedOut.status;
			view.activity = timedOut.activity;
			view.recovery = recoveryInstruction(workspaceVisible, true);
			view.statusKind = timedOut.kind;
			DrawList draws;
			draws.reset({960.f, 540.f}, 16);
			paintHud(Surface(draws, kFont, theme(), layout.scale, layout.view), layout, view);
			CHECK(draws.dropped() == 0 && !draws.quads().empty(),
				"the timeout copy paints at " << scale * 100.f << "% scale");
			for (const auto &quad : draws.quads())
				CHECK(inside({quad.rect}, layout.view),
					"session feedback never leaves the gameplay rectangle at " << scale * 100.f << "%");
			const auto resolved = resolveHud(layout, view);
			CHECK(resolved.status && resolved.activity && resolved.recovery && !resolved.reticle,
				"a blocked session shows all three feedback bands and no reticle");
		}
	}
}

// Issue #303 F03/F12: the reconnect steps follow the actual presentation state
// instead of a fixed menu path, and only name controls that exist.
void recoveryInstructions()
{
	const auto hiddenCaptured = recoveryInstruction(false, true);
	const auto hiddenFree = recoveryInstruction(false, false);
	const auto visibleCaptured = recoveryInstruction(true, true);
	const auto visibleFree = recoveryInstruction(true, false);
	CHECK(hiddenCaptured == "Ctrl+Shift+D, C, then Multiplayer > Reconnect." &&
			hiddenFree == "Ctrl+Shift+D, then Multiplayer > Reconnect." &&
			visibleCaptured == "C to free the mouse, then Multiplayer > Reconnect." &&
			visibleFree == "Multiplayer > Reconnect.",
		"each workspace/capture state names exactly the steps it still needs");
	CHECK(hiddenCaptured != hiddenFree && hiddenCaptured != visibleCaptured &&
			visibleCaptured != visibleFree && hiddenFree != visibleFree,
		"the instruction follows the state instead of being a fixed literal");
	for (const auto text : {hiddenCaptured, hiddenFree, visibleCaptured, visibleFree})
	{
		const auto menu = text.find("Multiplayer > Reconnect");
		const auto chord = text.find("Ctrl+Shift+D");
		const auto mouse = text.find("free the mouse");
		CHECK(menu != std::string_view::npos && text.size() <= 52,
			"the recovery line stays one short sentence: " << text);
		CHECK((chord == std::string_view::npos || chord < menu) &&
				(mouse == std::string_view::npos || mouse < menu) &&
				text.find("Esc") == std::string_view::npos,
			"the steps are ordered and never point at a menu that does not exist: " << text);
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
	inventoryScreen();
	heldStackPlacementTest();
	tooltips();
	notifications();
	notificationLifetime();
	degenerateGeometry();
	confirmations();
	progressAndIcons();
	textFitting();
	formFields();
	painting();
	slotPaintingAndFallbacks();
	hudMock();
	liveHud();
	hudSessionRows();
	recoveryInstructions();
	gallery();
	if (testmain::fails == 0)
		std::cout << "PASS: Game UI components (" << testmain::checks << " checks)" << std::endl;;
	return testmain::exitCode();
}
