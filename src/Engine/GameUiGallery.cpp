// Player design-system gallery and HUD mock (issue #302, Epic #298 Phase D).
//
// Both screens are built from the real components (PlayerUiComponents) and the
// real input path (WidgetSet): nothing here is a mock-up of a widget. The
// gallery's sections are the authored table in PlayerUiGallery.hpp, so the
// headless layout tests cover the same geometry the GPU frame paints.
//
// Authority: presentation only. The gallery owns sample data (a thumbnail
// inventory, a name field, a list) and a scale request; it never reads or
// mutates simulation, session, inventory or network state. The HUD mock is
// passive by design — #303 wires the same components to real snapshots.

#include <Engine/GameUiSample.hpp>
#include <utils.hpp>

#include <array>
#include <cstdio>
#include <utility>

using namespace gameui;

namespace
{
/// Gallery control ids. One per interactive element, stable across frames so
/// focus survives scroll and resize.
enum GalleryControl : ControlId
{
	PrimaryAction = 1,
	SecondaryAction,
	QuietAction,
	DangerAction,
	NameField,
	WorldList,
	ToastAction,
	ConfirmCancel,
	ConfirmContinue,
	SlotStrip,
	ScaleAuto,
	Scale100,
	Scale125,
	Scale150,
	Scale175,
	Scale200,
};

constexpr std::array<std::string_view, 5> kSavedWorlds{
	"Harbour Reach", "Quiet Valley", "Ember Flats", "Old Quarry", "Snow Basin"};

struct SampleSlot
{
	uint32_t layer;
	uint32_t count;
};

/// Desktop-first sample stacks; the atlas layers are real TextureType layers so
/// the gallery proves the borrowed-atlas icon path, not a stand-in texture.
constexpr std::array<SampleSlot, 9> kSampleStacks{{
	{COBBLESTONE, 64}, {GRASS_SIDE, 12}, {DIRT, 3}, {GRAVEL, 24}, {STONE, 9},
	{STONE, 1}, {}, {}, {},
}};
constexpr std::array<SampleSlot, 9> kHudStacks{{
	{COBBLESTONE, 42}, {GRASS_SIDE, 8}, {STONE, 64}, {DIRT, 16}, {}, {}, {}, {}, {},
}};

SlotView sampleSlot(const SampleSlot &sample, uint64_t generation, style::State state)
{
	SlotView view{};
	view.state = state;
	if (!sample.count)
		return view;
	view.icon = AtlasIcon{generation, sample.layer};
	view.count = sample.count;
	return view;
}

std::string_view scaleLabel(size_t index)
{
	switch (index)
	{
	case 0: return "Auto";
	case 1: return "100%";
	case 2: return "125%";
	case 3: return "150%";
	case 4: return "175%";
	default: return "200%";
	}
}

ScalePolicy scaleForIndex(size_t index)
{
	return index == 0 ? ScalePolicy{} : ScalePolicy{ScaleMode::Fixed, kUserScales[index - 1]};
}
} // namespace

gameui::Surface GameUiSample::surface(gameui::Clip clip)
{
	return {m_draws, font(), gameui::style::baseline(), m_screenScale, clip, m_atlasLayers,
		m_fontScale};
}

void GameUiSample::openGallery()
{
	if (m_screens.top() == Screen::Pause)
		m_screens.back();
	if (m_screens.push(Screen::Gallery))
		invalidateLayout();
}

void GameUiSample::openScreen(Screen screen)
{
	if (m_screens.push(screen))
		invalidateLayout();
}

void GameUiSample::buildGallery(Clip scene, float scale)
{
	const auto &theme = gameui::style::baseline();
	// Screen entry and layout rebuild are different events (F16): only entering
	// the gallery resets its sample state; a scale or resolution change keeps the
	// field value, the list selection, the chosen slot, the notifications and the
	// scroll offset (clamped by WidgetSet::reset to the new content height).
	const bool entered = m_screenRevision != m_screens.revision();
	const bool rebuilt = m_layoutRevision != m_screens.revision();
	const float offset = entered ? 0.f : m_widgets.viewport().offset;
	m_gallery = galleryLayout(scene, theme, scale);
	m_galleryList.items = kSavedWorlds;
	const bool moved = m_gallery.panel != m_panel;
	m_panel = m_gallery.panel;
	if (entered)
	{
		m_screenRevision = m_screens.revision();
		m_widgets.cancelEditing();
		m_toasts.clear();
		m_toasts.push({.title = "Design system", .body = "All components use the same tokens.",
			.status = style::Status::Info, .lifetime = 8.f, .id = nextToastId()});
		m_gallerySlot = 2;
	}
	else if (moved || rebuilt)
		m_widgets.cancelPress();
	m_widgets.reset(m_gallery.body, m_gallery.contentHeight, offset);
	const auto s = m_gallery.scale;
	// Widgets are positioned from the same geometry the painter uses: the
	// section rect, pushed below its header band.
	const auto section = [&](size_t index) {
		auto area = gallerySectionRect(m_gallery, theme, index);
		const float header = galleryHeaderHeight(theme, s);
		return Clip{{area.rect.x, area.rect.y + header, area.rect.z, area.rect.w}};
	};
	const float columnGap = theme.space.md * s;

	// 0 — Design tokens: the palette swatches the rest of the gallery paints.
	// 1 — Typography: every role plus an over-long label that must elide.
	// 2 — Buttons: four variants, all interactive.
	{
		const auto row = section(2);
		const float gap = theme.space.sm * s;
		const float width = (size(row).x - 3.f * gap) / 4.f;
		const auto cell = [&](int index) {
			return rectangle({row.rect.x + float(index) * (width + gap), row.rect.y}, {width,
				theme.size.buttonHeight * s});
		};
		m_widgets.add({PrimaryAction, cell(0), "Primary", "Primary action of a screen.", Button{},
			true, style::ButtonVariant::Primary});
		m_widgets.add({SecondaryAction, cell(1), "Secondary", "Default action.", Button{}, true,
			style::ButtonVariant::Secondary});
		m_widgets.add({QuietAction, cell(2), "Quiet", "Low-emphasis action.", Button{}, true,
			style::ButtonVariant::Quiet});
		m_widgets.add({DangerAction, cell(3), "Danger", "Destructive action.", Button{}, true,
			style::ButtonVariant::Danger});
	}
	// 3 — List rows: painted states plus one real scrollable list.
	{
		const auto area = section(3);
		const float rowHeight = theme.size.rowHeight * s;
		const float listX = area.rect.x + size(area).x * 0.5f + columnGap * 0.5f;
		const float listWidth = std::max(0.f, area.rect.z - listX);
		const float listHeight = (1.f + float(m_galleryList.visibleRows)) * rowHeight;
		m_widgets.add({WorldList, {{listX, area.rect.y, listX + listWidth,
			area.rect.y + listHeight}}, "Saved worlds", "Up and Down move the selection.",
			&m_galleryList});
	}
	// 4 — Form and validation: the real editable field on its own row, the two
	// feedback states below it.
	{
		const auto area = section(4);
		const float width = (size(area).x - columnGap) * 0.5f;
		const float height = formFieldHeight(theme, s);
		m_widgets.add({NameField, {{area.rect.x, area.rect.y, area.rect.x + width,
			area.rect.y + height}},
			"World name", "Tab moves focus, Ctrl+C / Ctrl+V copy and paste.", &m_galleryName});
	}
	// 6 — Notifications: push real toasts into the real bounded queue.
	{
		const auto area = section(6);
		const float width = theme.size.buttonHeight * s * 5.f;
		m_widgets.add({ToastAction, {{area.rect.x, area.rect.y, area.rect.x + width,
			area.rect.y + theme.size.buttonHeight * s}}, "Push notification",
			"Adds a toast to the bounded queue.", Button{}, true, style::ButtonVariant::Secondary});
	}
	// 7 — Confirmation: the dialog card is painted by the component and the two
	// actions are the real controls, placed exactly where it reserves them.
	{
		const auto area = section(7);
		m_confirmation = confirmationLayout({{area.rect.x, area.rect.y, area.rect.z, area.rect.w}},
			{size(area).x, area.rect.w - area.rect.y}, true, theme, s);
		const auto &layout = m_confirmation;
		m_widgets.add({ConfirmCancel, layout.cancel, "Cancel", {}, Button{}, true,
			style::ButtonVariant::Secondary});
		// R10: the destructive action uses the danger variant, exactly like the
		// real exit confirmation; Cancel stays first so it holds the initial focus.
		m_widgets.add({ConfirmContinue, layout.confirm, "Continue", {}, Button{}, true,
			style::ButtonVariant::Danger});
	}
	// 8 — Inventory slots: painted from the contract, one strip of nine.
	{
		const auto area = section(8);
		const float cell = centeredSlotSize(area, 9, theme.size.slotGap * s, theme.size.slot * s);
		const auto strip = centeredSlotStrip({{area.rect.x, area.rect.y, area.rect.z,
			area.rect.y + cell}}, 9, cell, theme.size.slotGap * s);
		// One focusable target over the strip: activating it cycles the sample
		// selection. Real per-slot interaction is #304 (the contract is the
		// same SlotView this component already consumes).
		m_widgets.add({SlotStrip, strip, {},
			"Slot presentation contract: icon, count, selection, disabled, pending.", Button{}, true,
			style::ButtonVariant::Quiet});
	}
	// 9 — Temporary player UI scale selector.
	{
		const auto area = section(9);
		const float gap = theme.space.xs * s;
		const float width = (size(area).x - 5.f * gap) / 6.f;
		const float height = std::min(size(area).y, theme.size.buttonHeight * s);
		for (int index = 0; index < 6; ++index)
		{
			const float x = area.rect.x + float(index) * (width + gap);
			const auto id = ControlId(ScaleAuto + index);
			m_widgets.add({id, {{x, area.rect.y, x + width, area.rect.y + height}},
				scaleLabel(static_cast<size_t>(index)), "Preview until leaving the gallery.", Button{},
				true, style::ButtonVariant::Secondary});
		}
	}
	auto focus = m_screens.focus();
	m_widgets.focus(focus, focus, entered || rebuilt || moved);
	if (entered && !focus)
		if (const auto *initial = m_widgets.step(0, false))
			m_widgets.focus(focus, initial->id, true);
	m_screens.focus(focus);
	m_layoutRevision = m_screens.revision();
}

void GameUiSample::buildHudMock(Clip scene, float scale)
{
	const auto &theme = gameui::style::baseline();
	m_screenScale = scale;
	m_hudMock = hudMockLayout(scene, theme, scale);
	m_panel = scene;
	if (m_screenRevision != m_screens.revision())
	{
		m_screenRevision = m_screens.revision();
		m_widgets.cancelEditing();
		m_toasts.clear();
		m_toasts.push({.title = "Welcome back", .body = "Seed 1337 - survival",
			.status = style::Status::Ok, .lifetime = 0.f, .id = nextToastId()});
		m_gallerySlot = 1;
	}
	m_widgets.reset(scene, 0.f, 0.f);
	m_layoutRevision = m_screens.revision();
}

void GameUiSample::paintDesignSystem(glm::vec2 pointer)
{
	const auto screen = m_screens.top();
	if (screen == Screen::HudMock)
	{
		const auto &theme = gameui::style::baseline();
		auto paint = surface(m_draws.bounds());
		paint.crosshair(m_hudMock.view, theme.palette.ink);
		std::array<SlotView, 9> slots{};
		for (size_t i = 0; i < slots.size(); ++i)
			slots[i] = sampleSlot(kHudStacks[i], m_draws.atlasGeneration(),
				i == size_t(m_gallerySlot) ? style::State::Selected : style::State::Rest);
		paint.slotGrid(slotGrid(m_hudMock.hotbar, m_hudMock.hotbarSlots, 1, m_hudMock.hotbarGap),
			slots, -1);
		// The tokens' readability contract: HUD text sits on the hudScrim
		// backdrop, never directly on the scene (style::hudContrast pins it).
		paint.backdrop(m_hudMock.status, theme.palette.hudScrim);
		paint.statusIndicator(m_hudMock.status, style::Status::Ok, "60 fps - 12 ms");
		if (const auto *toast = m_toasts.newest())
			paint.toast(m_hudMock.toast, *toast);
		paint.backdrop(m_hudMock.target, theme.palette.hudScrim);
		paint.label(m_hudMock.target, "Press E to open the inventory", style::TextRole::Body,
			theme.palette.ink);
		m_widgets.paint(m_draws, font(), m_screenScale, m_screens.focus(), pointer, theme,
			m_atlasLayers, m_fontScale);
		return;
	}

	const auto &theme = gameui::style::baseline();
	const auto s = m_gallery.scale;
	const auto viewport = m_widgets.viewport();
	// The card is clipped by the viewport and its content by the scroll body
	// (F12): the frame keeps its border and padding while the sections scroll
	// underneath it, and neither needs a per-section scissor.
	surface(m_draws.bounds()).card(m_gallery.panel);
	auto paint = surface(m_gallery.body.intersect(m_draws.bounds()));
	// Sections share the body clip: clipping per section would split the draw
	// list into one scissor per section and exhaust the batch budget. The
	// surface clip already stops anything that scrolls out of the body.
	const auto placed = [&](size_t index) {
		return viewport.place(gallerySectionRect(m_gallery, theme, index));
	};
	// Content of a section starts below its own header band, so a section can
	// never paint over its own title.
	const auto contentRect = [&](size_t index) {
		auto area = placed(index);
		const float header = galleryHeaderHeight(theme, m_gallery.scale);
		return Clip{{area.rect.x, area.rect.y + header, area.rect.z, area.rect.w}};
	};

	// 0 — Tokens: swatches with their semantic names.
	{
		const auto area = contentRect(0);
		const float swatch = theme.size.rowHeight * s;
		const std::array<std::pair<std::string_view, glm::vec4>, 6> swatches{{
			{"accent", theme.palette.accent}, {"surface", theme.palette.surface},
			{"raised", theme.palette.surfaceRaised}, {"ok", theme.palette.ok},
			{"warning", theme.palette.warning}, {"error", theme.palette.error},
		}};
		const float gap = theme.space.sm * s;
		const float width = (size(area).x - 5.f * gap) / 6.f;
		for (size_t i = 0; i < swatches.size(); ++i)
		{
			const auto cell = rectangle({area.rect.x + float(i) * (width + gap), area.rect.y},
				{width, swatch});
			paint.fill(cell, swatches[i].second, paint.px(theme.size.radiusSmall));
			paint.label({{cell.rect.x, cell.rect.w, cell.rect.z, cell.rect.w + theme.space.md * s}},
				swatches[i].first, style::TextRole::Caption, theme.palette.inkMuted);
		}
	}
	// 1 — Typography: one line per role plus an over-long label.
	{
		const auto area = contentRect(1);
		const float line = style::lineHeight(style::TextRole::Heading) * s;
		float y = area.rect.y;
		paint.fittedLabel({{area.rect.x, y, area.rect.z, y + line}}, "Title - The quick brown fox",
			style::TextRole::Title, theme.palette.ink, 0.f);
		y += style::lineHeight(style::TextRole::Title) * s;
		paint.fittedLabel({{area.rect.x, y, area.rect.z, y + line}}, "Heading - 100% to 200%",
			style::TextRole::Heading, theme.palette.ink, 0.f);
		y += line;
		paint.fittedLabel({{area.rect.x, y, area.rect.z, y + line}},
			"Body - variable-length labels elide instead of colliding with the next control.", 
			style::TextRole::Body, theme.palette.ink, 0.f);
		y += line;
		paint.fittedLabel({{area.rect.x, y, area.rect.x + size(area).x * 0.45f, y + line}},
			"Caption - clipped: an intentionally over-long caption that cannot fit", 
			style::TextRole::Caption, theme.palette.inkMuted, 0.f);
	}
	// 3 — List rows: the painted states next to the interactive list.
	{
		const auto area = contentRect(3);
		const float rowHeight = theme.size.rowHeight * s;
		const float columnWidth = size(area).x * 0.5f - theme.space.md * s;
		const std::array<std::pair<std::string_view, style::State>, 3> rows{{
			{"Selected row", style::State::Selected},
			{"Hovered row", style::State::Hovered},
			{"Disabled row", style::State::Disabled},
		}};
		for (size_t i = 0; i < rows.size(); ++i)
			paint.listRow({{area.rect.x, area.rect.y + float(i) * rowHeight, area.rect.x + columnWidth,
					area.rect.y + float(i + 1) * rowHeight}}, rows[i].first, "12", rows[i].second);
	}
	// 4 — Form and validation: pending and error next to the editable field.
	{
		// The row below the editable field shows the two feedback states. Both
		// groups are sized by the component contract, so the message line can
		// never collide with the well border.
		const auto area = contentRect(4);
		const float gap = theme.space.md * s;
		const float half = (size(area).x - gap) * 0.5f;
		const float top = area.rect.y + formFieldHeight(theme, s) + theme.space.sm * s;
		const float height = formFieldHeight(theme, s, true);
		const float second = area.rect.x + half + gap;
		paint.formField({{area.rect.x, top, area.rect.x + half, top + height}}, "Pending",
			"Checking with the server", style::Validation::Pending, style::State::Rest,
			"Waiting for the authority");
		paint.formField({{second, top, second + half, top + height}}, "Rejected", "harbour reach",
			style::Validation::Error, style::State::Focused, "Name already in use");
	}
	// 5 — Status and progress.
	{
		const auto area = contentRect(5);
		const float line = style::lineHeight(style::TextRole::Body) * s + theme.space.xxs * s;
		const std::array<std::pair<std::string_view, style::Status>, 4> states{{
			{"Connected", style::Status::Ok}, {"Reconnecting", style::Status::Warning},
			{"Rejected", style::Status::Error}, {"Saving", style::Status::Pending},
		}};
		const float columnWidth = size(area).x * 0.5f - theme.space.md * s;
		for (size_t i = 0; i < states.size(); ++i)
		{
			const float y = area.rect.y + float(i) * line;
			paint.statusIndicator({{area.rect.x, y, area.rect.x + columnWidth, y + line}},
				states[i].second, states[i].first);
		}
		const float barHeight = theme.space.md * s;
		const float barX = area.rect.x + size(area).x * 0.5f + theme.space.md * s;
		paint.label({{barX, area.rect.y, area.rect.z, area.rect.y + line}}, "Save progress",
			style::TextRole::Caption, theme.palette.inkMuted);
		paint.progress({{barX, area.rect.y + line, area.rect.z, area.rect.y + line + barHeight}}, 0.62f,
			false, 0.f);
		paint.progress({{barX, area.rect.y + line + barHeight + theme.space.sm * s, area.rect.z,
				area.rect.y + line + 2.f * barHeight + theme.space.sm * s}}, 0.f, true, m_uiPhase);
	}
	// 6 — Notifications and tooltip.
	{
		const auto area = contentRect(6);
		const float toastHeight = 2.f * style::lineHeight(style::TextRole::Body) * s + theme.space.md * s;
		if (const auto *toast = m_toasts.newest())
			paint.toast({{area.rect.x, area.rect.y + theme.size.buttonHeight * s + theme.space.md * s,
					area.rect.x + size(area).x * 0.62f,
					area.rect.y + theme.size.buttonHeight * s + theme.space.md * s + toastHeight}},
				*toast);
		const auto anchor = glm::vec2{area.rect.z - 40.f * s, area.rect.y + 30.f * s};
		paint.tooltip(area, anchor, "Instanced tooltip inside the section");
	}
	// 7 — Confirmation. The whole layout — panel, title, body and the two
	// interactive buttons — is placed by the same viewport transform, so the
	// dialog stays coherent while the gallery scrolls (F05).
	{
		const auto area = contentRect(7);
		paint.confirmation(confirmationLayout(area, size(area), true, theme, s),
			"Delete this world?", "The save and its checkpoints are removed.");
	}
	// 8 — Inventory slots: selection, empty, disabled, pending and durability.
	{
		const auto area = contentRect(8);
		const float cell = centeredSlotSize(area, 9, theme.size.slotGap * s, theme.size.slot * s);
		const auto strip = centeredSlotStrip({{area.rect.x, area.rect.y, area.rect.z,
			area.rect.y + cell}}, 9, cell, theme.size.slotGap * s);
		std::array<SlotView, 9> slots{};
		for (size_t i = 0; i < slots.size(); ++i)
			slots[i] = sampleSlot(kSampleStacks[i], m_draws.atlasGeneration(),
				i == size_t(m_gallerySlot) ? style::State::Selected : style::State::Rest);
		slots[3].state |= style::State::Disabled;
		slots[4].state |= style::State::Pending;
		slots[4].progress = 0.45f;
		slots[1].durability = 0.18f;
		paint.slotGrid(slotGrid(strip, 9, 1, theme.size.slotGap * s), slots, -1);
		const float line = style::lineHeight(style::TextRole::Caption) * s;
		char caption[96];
		std::snprintf(caption, sizeof(caption), "Slot %d x%u - durability and progress are optional",
			m_gallerySlot + 1,
			unsigned(kSampleStacks[size_t(m_gallerySlot) % kSampleStacks.size()].count));
		paint.fittedLabel({{area.rect.x, strip.rect.w + theme.space.xs * s, area.rect.z,
				strip.rect.w + theme.space.xs * s + line}}, caption, style::TextRole::Caption,
			theme.palette.inkMuted, 0.f);
	}
	// 9 — Temporary scale preview, restored when the player leaves the gallery.
	{
		const auto area = contentRect(9);
		const float line = style::lineHeight(style::TextRole::Caption) * s;
		const float top = area.rect.y + theme.size.buttonHeight * s + theme.space.xs * s;
		char state[128];
		std::snprintf(state, sizeof(state), "Effective %.0f%% - %s. Restored when leaving the gallery.",
			100.f * m_scale.effective(),
			m_scale.policy().mode == ScaleMode::Auto ? "auto" : "override");
		paint.fittedLabel({{area.rect.x, top, area.rect.z, top + line}}, state,
			style::TextRole::Caption, theme.palette.inkMuted, 0.f);
	}
	// A presentation-only composition. Each slot still uses the same
	// atlas-backed view as inventory; a real crafting screen supplies its
	// own bounded snapshot and routes its actions through the session host.
	{
		const auto layout = craftingGalleryLayout(placed(kCraftingGallerySection), theme, s);
		paint.confirmation(layout.modal, "Crafting table", "");
		std::array<SlotView, 9> inputs{};
		inputs[0] = sampleSlot(kSampleStacks[0], m_draws.atlasGeneration(), style::State::Selected);
		inputs[4] = sampleSlot(kSampleStacks[2], m_draws.atlasGeneration(), style::State::Rest);
		inputs[0].label = "Cobblestone";
		inputs[4].label = "Dirt";
		paint.slotGrid(layout.inputs, inputs);
		if (!layout.output.empty())
		{
			SlotView emptyOutput{};
			emptyOutput.state = style::State::Disabled;
			paint.slot(layout.output, emptyOutput);
			paint.fittedLabel(layout.outputLabel,
				"Output", style::TextRole::Caption, theme.palette.inkMuted, 0.f);
		}
		const auto input = slotIndexAt(layout.inputs, pointer);
		if (input >= 0 && hit(m_gallery.body, pointer) && !inputs[size_t(input)].label.empty())
			paint.tooltip(layout.modal.panel, pointer, inputs[size_t(input)].label);
	}
	// Section headers last so scrolling content never overdraws them.
	for (size_t index = 0; index < gallerySections().size(); ++index)
	{
		const auto area = placed(index);
		if (area.empty())
			continue;
		paint.sectionHeader({{area.rect.x, area.rect.y, area.rect.z,
				area.rect.y + style::lineHeight(style::TextRole::Heading) * s}},
			gallerySections()[index].title);
	}
	// Interactive controls last: they are the same components painted above, so
	// the gallery shows the real focus/hover/pressed path rather than a mock.
	m_widgets.paint(m_draws, font(), m_screenScale, m_screens.focus(), pointer, theme, m_atlasLayers,
		m_fontScale);
}

GameUiSample::InputResult GameUiSample::applyGalleryActivate(ControlId id)
{
	switch (id)
	{
	case PrimaryAction:
		m_toasts.push({.title = "Primary action", .body = "One command, one intent.",
			.status = style::Status::Ok, .lifetime = 4.f, .id = nextToastId()});
		return InputResult::Consumed;
	case SecondaryAction:
		m_toasts.push({.title = "Secondary action", .status = style::Status::Info, .lifetime = 4.f,
			.id = nextToastId()});
		return InputResult::Consumed;
	case QuietAction:
		m_toasts.push({.title = "Quiet action", .status = style::Status::Info, .lifetime = 3.f,
			.id = nextToastId()});
		return InputResult::Consumed;
	case DangerAction:
		m_toasts.push({.title = "Danger action", .body = "Cancelled - nothing was deleted.",
			.status = style::Status::Error, .lifetime = 4.f, .id = nextToastId()});
		return InputResult::Consumed;
	case ToastAction:
		m_toasts.push({.title = "Notification", .body = "Queued, bounded, expires on its own.",
			.status = style::Status::Warning, .lifetime = 4.f, .id = nextToastId()});
		return InputResult::Consumed;
	case ConfirmCancel:
	case ConfirmContinue:
		m_toasts.push({.title = id == ConfirmCancel ? "Cancelled" : "Continued",
			.body = "A destructive confirmation starts on Cancel.", .status = style::Status::Info,
			.lifetime = 4.f, .id = nextToastId()});
		return InputResult::Consumed;
	case SlotStrip:
		m_gallerySlot = (m_gallerySlot + 1) % int(kSampleStacks.size());
		return InputResult::Consumed;
	default: break;
	}
	if (id >= ScaleAuto && id <= Scale200)
	{
		if (!m_galleryPreviousScale)
			m_galleryPreviousScale = m_scale.pendingPolicy();
		m_scale.setPolicy(scaleForIndex(size_t(id - ScaleAuto)));
		return InputResult::Consumed;
	}
	return InputResult::Consumed;
}
