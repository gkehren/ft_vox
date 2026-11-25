#include <Engine/GameUiSample.hpp>

#include <cstdio>

using namespace gameui;
using namespace inventoryui;

namespace
{
constexpr ControlId kDrop = kSlotCount + 1;
constexpr ControlId kClose = kDrop + 1;

SlotId controlSlot(ControlId control)
{
	return slotAtCell(int(control) - 1);
}

ControlId slotControl(SlotId slot)
{
	return ControlId(cellOfSlot(slot) + 1);
}

bool canDropStack(const InventoryView &view)
{
	return view.enabled() && view.canDrop && view.snapshot->selected.valid() &&
		!view.snapshot->slots[size_t(view.snapshot->selected.index)].isEmpty();
}

std::string_view describeSlot(const SlotView &slot, char (&buffer)[96])
{
	if (slot.empty())
		return "Empty slot";
	std::snprintf(buffer, sizeof(buffer), "%.*s x%u", int(slot.label.size()), slot.label.data(), unsigned(slot.count));
	return buffer;
}
} // namespace

void GameUiSample::setInventoryView(InventoryView view)
{
	const auto *snapshot = view.snapshot ? &*view.snapshot : nullptr;
	if (m_inventoryInteraction.observe(snapshot, view.epoch, view.enabled()) &&
		m_inventoryPress && m_inventoryPress->control != kClose)
		m_inventoryPress.reset();
	m_inventoryView = view;
}

void GameUiSample::cancelInventoryGesture()
{
	m_inventoryPress.reset();
	m_inventoryInteraction.cancel();
}

ControlId GameUiSample::inventoryControlAt(glm::vec2 point) const
{
	if (hit(m_inventoryLayout.close, point))
		return kClose;
	if (hit(m_inventoryLayout.drop, point))
		return kDrop;
	return slotControl(m_inventoryLayout.hitSlot(point));
}

Action GameUiSample::handleInventoryEvent(const SDL_Event &event)
{
	if (m_layoutRevision != m_screens.revision())
		return {};

	const bool enabled = m_inventoryView.enabled();
	const SlotId selected = m_inventoryView.snapshot ? m_inventoryView.snapshot->selected : SlotId{};
	auto activate = [&](ControlId control, bool half) -> Action {
		if (control == kClose && !half)
			back();
		else if (control == kDrop && !half && canDropStack(m_inventoryView))
		{
			cancelInventoryGesture();
			return {Command::DropStack, selected};
		}
		else if (control <= kSlotCount || half)
			return m_inventoryInteraction.activate(controlSlot(control), half);
		return {};
	};

	switch (event.type)
	{
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
	{
		const auto button = event.button.button;
		if (button != SDL_BUTTON_LEFT && button != SDL_BUTTON_RIGHT)
			return {};
		const auto control = inventoryControlAt({event.button.x, event.button.y});
		if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
		{
			if (!m_inventoryPress)
			{
				m_inventoryPress = InventoryPress{control, button};
				if (control == kClose || (control && enabled))
					m_screens.focus(control);
			}
			return {};
		}
		if (!m_inventoryPress || m_inventoryPress->button != button)
			return {};
		const auto armed = m_inventoryPress->control;
		m_inventoryPress.reset();
		if (control == armed)
			return activate(control, button == SDL_BUTTON_RIGHT);
		return {};
	}
	case SDL_EVENT_MOUSE_WHEEL:
	{
		if (!enabled || !selected.valid() || event.wheel.y == 0.f)
			return {};
		const bool previous = (event.wheel.y > 0.f) != (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED);
		const int delta = previous ? -1 : 1;
		return {Command::Select, {(selected.index + delta + items::kHotbarSize) % items::kHotbarSize}};
	}
	case SDL_EVENT_KEY_DOWN:
		break;
	default:
		return {};
	}

	auto focus = m_screens.focus();
	const auto key = event.key.key;
	const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
	const bool modified = (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT)) != 0;
	if (modified)
		return {};
	if (enabled)
	{
		if (!event.key.repeat && key >= SDLK_1 && key <= SDLK_9)
			return {Command::Select, {int(key - SDLK_1)}};
		if (!event.key.repeat && key == SDLK_Q && m_inventoryView.canDrop && selected.valid())
			return {Command::DropOne, selected};
	}
	const bool dropEnabled = canDropStack(m_inventoryView);
	if (key == SDLK_TAB)
	{
		if (event.key.repeat)
			return {};
		if (!enabled)
			focus = kClose;
		else if (shift)
			focus = focus <= 1 ? kClose : focus == kClose && !dropEnabled ? kSlotCount : focus - 1;
		else
			focus = focus == kClose ? 1 : focus == kSlotCount && !dropEnabled ? kClose : focus + 1;
	}
	else if (enabled && controlSlot(focus).valid())
	{
		const int col = (focus - 1) % kColumns;
		if (key == SDLK_LEFT && col > 0)
			--focus;
		if (key == SDLK_RIGHT && col < kColumns - 1)
			++focus;
		if (key == SDLK_UP && focus > kColumns)
			focus -= kColumns;
		if (key == SDLK_DOWN && focus <= kSlotCount - kColumns)
			focus += kColumns;
	}
	m_screens.focus(focus);
	if (!event.key.repeat && (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE))
	{
		m_inventoryPress.reset();
		return activate(focus, shift);
	}
	return {};
}

void GameUiSample::paintInventory(Clip scene, glm::vec2 pointer, std::optional<uint64_t> atlasGeneration)
{
	const auto &theme = style::baseline();
	const auto layout = inventoryLayout(scene, theme, m_screenScale);
	if (layout.panel != m_inventoryLayout.panel)
		m_inventoryPress.reset();
	m_inventoryLayout = layout;
	m_layoutRevision = m_screens.revision();
	if (layout.panel.empty())
		return;

	const auto &view = m_inventoryView;
	const auto *snapshot = view.snapshot ? &*view.snapshot : nullptr;
	auto focus = m_screens.focus();
	if (!view.enabled())
	{
		// Keep the slot focus during a pending action so it can resume naturally.
		if (!focus)
			focus = kClose;
	}
	else if (!focus || (focus == kDrop && !canDropStack(view)))
		focus = snapshot && snapshot->selected.valid() ? slotControl(snapshot->selected) : 1;
	m_screens.focus(focus);

	const Surface paint(m_draws, font(), theme, layout.scale, scene, m_atlasLayers, m_fontScale);
	paint.backdrop(scene, theme.palette.scrim);
	paint.card(layout.panel, true);
	paint.sectionHeader(layout.title, "Inventory");
	paint.wrappedLabel(layout.instructions,
		"Click or Enter to move. Right-click or Shift+Enter for half. "
		"Arrows / Tab focus. 1-9 / wheel select. Q drops one.",
		style::TextRole::Caption, theme.palette.inkMuted, 2);
	paint.label(layout.hotbarLabel, "Hotbar", style::TextRole::Body, theme.palette.inkMuted);

	const auto hover = inventoryControlAt(pointer);
	const auto controlState = [&](ControlId id, bool active) {
		style::State state = active ? style::State{} : style::State::Disabled;
		if (id == hover)
			state |= style::State::Hovered;
		if (id == focus)
			state |= style::State::Focused;
		if (m_inventoryPress && m_inventoryPress->control == id && id == hover)
			state |= style::State::Pressed;
		return state;
	};
	paint.button(layout.close, "Close", style::ButtonVariant::Quiet, controlState(kClose, true));
	paint.button(layout.drop, "Drop selected stack", style::ButtonVariant::Secondary,
		controlState(kDrop, canDropStack(view)));

	const auto &drag = m_inventoryInteraction.drag();
	std::array<SlotView, kSlotCount> slots{};
	for (int cell = 0; cell < kSlotCount; ++cell)
	{
		const auto id = slotAtCell(cell);
		auto &entry = slots[size_t(cell)];
		if (snapshot)
		{
			entry = makeItemSlot(displayedSlotStack(id, drag, snapshot->slots), atlasGeneration);
			if (id == snapshot->selected)
				entry.state |= style::State::Selected;
		}
		if (view.pendingSlots && view.pendingSlots->affects(id))
			entry.state |= style::State::Pending;
		else if (!view.enabled())
			entry.state |= style::State::Disabled;
		entry.state |= controlState(slotControl(id), true);
	}
	const std::span<const SlotView> grid{slots};
	paint.slotGrid(layout.main, grid.first(kColumns * kMainRows));
	paint.slotGrid(layout.hotbar, grid.last(kColumns));

	if (!view.feedback.empty())
	{
		const auto line = rectangle(glm::vec2(layout.status.rect), {size(layout.status).x, 20.f * layout.scale});
		paint.statusIndicator(line, view.feedbackKind, view.feedback);
		const auto next = rectangle({line.rect.x, line.rect.w}, {size(line).x, size(line).y});
		paint.fittedLabel(next, view.recovery, style::TextRole::Caption, theme.palette.inkMuted, 0.f);
	}
	if (!snapshot)
		return;
	const auto focused = controlSlot(focus);
	if (focused.valid())
	{
		char label[96];
		paint.fittedLabel(layout.detail, describeSlot(makeItemSlot(snapshot->slots[size_t(focused.index)],
			atlasGeneration), label), style::TextRole::Caption, theme.palette.ink, 0.f);
	}
	if (drag.active())
	{
		const float cell = layout.main.cell;
		const auto anchor = hit(scene, pointer) ? pointer :
			glm::vec2(layout.slot(focused.valid() ? focused : drag.sourceSlot).rect);
		paint.slot(rectangle(anchor + glm::vec2(10.f * layout.scale), {cell, cell}),
			makeItemSlot(cursorStack(drag, snapshot->slots), atlasGeneration));
	}
	else if (const auto hovered = controlSlot(hover); hovered.valid())
	{
		char label[96];
		paint.tooltip(scene, pointer, describeSlot(makeItemSlot(snapshot->slots[size_t(hovered.index)],
			atlasGeneration), label));
	}
}
