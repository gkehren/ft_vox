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
std::string_view dropLabel(const Surface &paint, Clip button, const SlotView &selected, bool known,
	bool pendingDrop, bool enabled, char (&buffer)[192])
{
	if (pendingDrop)
		return "Drop pending";
	if (selected.empty())
		return known ? "Nothing to drop" : "Drop unavailable";
	constexpr std::string_view verb = "Drop ";
	char quantity[16];
	std::snprintf(quantity, sizeof(quantity), " x%u", unsigned(selected.count));
	const auto role = enabled ? style::TextRole::Action : style::TextRole::Body;
	const auto font = paint.font().forRole(role);
	const float glyphs = paint.glyphScale(role);
	const float padding = std::min(paint.px(paint.theme().space.md), size(button).x * 0.25f);
	const auto name = fitText(font, selected.label, role, paint.scale() * paint.fontScale(),
		size(button).x - 2.f * padding - font.measure(verb, glyphs) - font.measure(quantity, glyphs));
	std::snprintf(buffer, sizeof(buffer), "%s%.*s%s%s", verb.data(), int(name.bytes), selected.label.data(),
		name.elided ? "..." : "", quantity);
	return buffer;
}

} // namespace

void GameUiSample::setInventoryView(InventoryView view)
{
	const auto *snapshot = view.snapshot ? &*view.snapshot : nullptr;
	if (m_inventoryInteraction.observe(snapshot, view.epoch, view.enabled()) &&
		m_inventoryPress && m_inventoryPress->control != kClose)
	{
		m_inventoryPress.reset();
	}
	m_inventoryView = view;
}

void GameUiSample::cancelInventoryGesture()
{
	m_inventoryPress.reset();
	m_inventoryInteraction.cancel();
	// F05 (#316): modality is transient presentation state. It resets with the
	// gesture on screen close, session reset, focus loss and workspace handoff.
	m_inventoryModality = InventoryModality::Pointer;
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
		// F11 (#316): only a supported pointer press is a meaningful pointer
		// interaction — unsupported buttons never move the preview, and
		// passive motion can still route here but never reaches this branch.
		if (button != SDL_BUTTON_LEFT && button != SDL_BUTTON_RIGHT)
			return {};
		const auto control = inventoryControlAt({event.button.x, event.button.y});
		if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
		{
			if (!m_inventoryPress)
			{
				m_inventoryPress = InventoryPress{control, button};
				// F11 (round 3): the modality follows only a validated
				// press on an actual control — an inert press on empty
				// panel space leaves the presentation alone.
				if (control == kClose || (control && enabled))
				{
					m_screens.focus(control);
					m_inventoryModality = InventoryModality::Pointer;
				}
			}
			return {};
		}
		// F11 (round 3): an unmatched release — nothing armed, a different
		// button, or a release that left the pressed control — is not a
		// meaningful interaction and never moves the preview.
		if (!m_inventoryPress || m_inventoryPress->button != button)
			return {};
		const auto armed = m_inventoryPress->control;
		m_inventoryPress.reset();
		if (control == armed)
		{
			m_inventoryModality = InventoryModality::Pointer;
			return activate(control, button == SDL_BUTTON_RIGHT);
		}
		return {};
	}
	case SDL_EVENT_MOUSE_WHEEL:
	{
		if (!enabled || !selected.valid() || event.wheel.y == 0.f)
			return {};
		// F11 (#316): the wheel is a supported pointer interaction — it
		// restores pointer anchoring.
		m_inventoryModality = InventoryModality::Pointer;
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
	// F11 (#316): an unsupported or inert keyboard event never moves the
	// preview — modifier-only keys, chords and keys the screen does not
	// consume leave the presentation on its current modality.
	const auto modifierKey = key == SDLK_LSHIFT || key == SDLK_RSHIFT ||
		key == SDLK_LCTRL || key == SDLK_RCTRL || key == SDLK_LALT || key == SDLK_RALT ||
		key == SDLK_LGUI || key == SDLK_RGUI || key == SDLK_CAPSLOCK ||
		key == SDLK_NUMLOCKCLEAR || key == SDLK_SCROLLLOCK;
	if (modified || modifierKey)
		return {};
	if (enabled)
	{
		// F11: a supported activation key is a meaningful keyboard
		// interaction — it also anchors the held stack to the focus.
		if (!event.key.repeat && key >= SDLK_1 && key <= SDLK_9)
		{
			m_inventoryModality = InventoryModality::Keyboard;
			return {Command::Select, {int(key - SDLK_1)}};
		}
		if (!event.key.repeat && key == SDLK_Q && m_inventoryView.canDrop && selected.valid())
		{
			m_inventoryModality = InventoryModality::Keyboard;
			return {Command::DropOne, selected};
		}
	}
	const bool dropEnabled = canDropStack(m_inventoryView);
	// F11 (round 3): the focus is recorded before navigation so the
	// modality follows exactly the supported navigations that move it.
	const auto previousFocus = focus;
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
	// F11 (#316, round 3): any supported navigation that actually moves the
	// focus — Tab, Shift+Tab, or an arrow that leaves a grid edge — anchors
	// the held stack to the newly focused slot; inert keys change nothing.
	if (focus != previousFocus)
		m_inventoryModality = InventoryModality::Keyboard;
	m_screens.focus(focus);
	if (!event.key.repeat && (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE))
	{
		m_inventoryPress.reset();
		// F11: activation is a meaningful keyboard interaction.
		m_inventoryModality = InventoryModality::Keyboard;
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
	paint.card(layout.panel);
	paint.fittedLabel(layout.title, "Inventory", style::TextRole::Title, theme.palette.ink, 0.f);
	if (view.feedback.empty() && !layout.instructions.empty())
		paint.wrappedLabel(layout.instructions,
			"Click or Enter to move. Right-click or Shift+Enter for half. "
			"Arrows / Tab focus. 1-9 / wheel select. Q drops one.",
			style::TextRole::Caption, theme.palette.inkMuted, 2);
	paint.label(layout.hotbarLabel, "Hotbar", style::TextRole::Caption, theme.palette.inkMuted);

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
	const bool validSelection = snapshot && snapshot->selected.valid();
	const auto selected = validSelection
		? makeItemSlot(snapshot->slots[size_t(snapshot->selected.index)], atlasGeneration) : SlotView{};
	const bool pendingDrop = view.actionPending && view.pendingSlots && view.pendingSlots->command == Command::DropStack;
	const bool dropEnabled = canDropStack(view);
	char drop[192];
	paint.button(layout.drop, dropLabel(paint, layout.drop, selected, bool(snapshot), pendingDrop, dropEnabled, drop),
		style::ButtonVariant::Danger, controlState(kDrop, dropEnabled));

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
		const float marker = paint.px(theme.space.xs);
		const float gap = paint.px(theme.space.sm);
		const glm::vec2 start(layout.status.rect);
		paint.fill(rectangle(start + glm::vec2(0.f, gap), {marker, marker}),
			style::statusColor(theme, view.feedbackKind), 0.f);
		const Clip message{{start.x + marker + gap, start.y, layout.status.rect.z, layout.status.rect.w}};
		paint.wrappedLabel(message, view.feedback, style::TextRole::Caption, theme.palette.ink, 2);
		paint.fittedLabel(layout.recovery, view.recovery, style::TextRole::Caption, theme.palette.inkMuted,
			layout.compact ? 1.f : 0.f);
	}
	const auto focused = controlSlot(focus);
	if (snapshot && focused.valid() && (!layout.compact || view.feedback.empty()))
	{
		char description[96];
		const auto item = describeSlot(makeItemSlot(snapshot->slots[size_t(focused.index)], atlasGeneration), description);
		char label[112];
		std::snprintf(label, sizeof(label), "Focused: %.*s", int(item.size()), item.data());
		paint.fittedLabel(layout.detail, label, style::TextRole::Caption, theme.palette.ink, 0.f);
	}
	if (hit(layout.status, pointer) && !view.feedback.empty())
		paint.tooltip(scene, pointer, view.feedback);
	else if ((hover == kDrop || focus == kDrop) && !selected.empty() && !pendingDrop)
	{
		char label[96];
		const auto detail = describeSlot(selected, label);
		const auto at = hover == kDrop ? pointer : glm::vec2(layout.drop.rect);
		paint.tooltip(scene, at, detail);
	}
	if (!snapshot)
		return;
	if (drag.active())
	{
		// F03/F05 (#316): the held stack follows the active input modality —
		// the pointer while the player interacts by pointer, the focused
		// destination slot while the player navigates by keyboard (falling
		// back to the source slot when no valid destination has focus). Each
		// anchor goes through heldStackPlacement, which flips the offset near
		// the right/bottom edge of the available game view and clamps the
		// rectangle fully inside it (dock area included).
		const float cell = layout.main.cell;
		const auto anchorRect = layout.slot(focused.valid() ? focused : drag.sourceSlot);
		glm::vec2 anchor;
		if (m_inventoryModality == InventoryModality::Keyboard)
			anchor = glm::vec2(anchorRect.rect) + size(anchorRect) * 0.5f;
		else if (hit(scene, pointer))
			anchor = pointer;
		else
			anchor = glm::vec2(anchorRect.rect);
		paint.slot(heldStackPlacement(anchor, cell, 10.f * layout.scale, scene),
			makeItemSlot(cursorStack(drag, snapshot->slots), atlasGeneration));
	}
	else if (const auto hovered = controlSlot(hover); hovered.valid())
	{
		char label[96];
		paint.tooltip(scene, pointer, describeSlot(makeItemSlot(snapshot->slots[size_t(hovered.index)],
			atlasGeneration), label));
	}
}
