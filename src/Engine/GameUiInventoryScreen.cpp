#include <Engine/GameUiSample.hpp>

#include <cstdio>

using namespace gameui;
using namespace inventoryui;

namespace
{
constexpr ControlId kDrop = kSlotCount + 1;
constexpr ControlId kClose = kDrop + 1;
constexpr ControlId kFeedback = kClose + 1;
constexpr ControlId kPreviousPage = kFeedback + 1;
constexpr ControlId kNextPage = kPreviousPage + 1;

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
	if (view.epoch != m_inventoryView.epoch)
		resetInventoryFeedback();
	if (m_inventoryInteraction.observe(snapshot, view.epoch, view.enabled()) &&
		m_inventoryPress && m_inventoryPress->control <= kDrop)
	{
		m_inventoryPress.reset();
	}
	m_inventoryView = view;
}

void GameUiSample::resetInventoryFeedback()
{
	m_inventoryFeedbackText.clear();
	m_inventoryFeedbackOffsets.clear();
	m_inventoryFeedbackNext = 0;
	m_inventoryFeedbackFocus = 0;
	m_inventoryFeedbackAvailable = false;
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
	if ((m_inventoryFeedbackAvailable || !m_inventoryFeedbackText.empty()) &&
		hit(m_inventoryLayout.feedback, point))
		return kFeedback;
	if (!m_inventoryFeedbackText.empty())
	{
		if (hit(m_inventoryLayout.previousPage, point))
			return kPreviousPage;
		if (hit(m_inventoryLayout.nextPage, point))
			return kNextPage;
		return 0;
	}
	return slotControl(m_inventoryLayout.hitSlot(point));
}

Action GameUiSample::handleInventoryEvent(const SDL_Event &event)
{
	if (m_layoutRevision != m_screens.revision())
		return {};

	const bool enabled = m_inventoryView.enabled();
	const bool reading = !m_inventoryFeedbackText.empty();
	const bool dropEnabled = canDropStack(m_inventoryView);
	const SlotId selected = m_inventoryView.snapshot ? m_inventoryView.snapshot->selected : SlotId{};
	const auto reachable = [&](ControlId control, bool half = false) {
		if (controlSlot(control).valid())
			return enabled && !reading;
		if (half)
			return false;
		switch (control)
		{
		case kClose: return true;
		case kDrop: return dropEnabled;
		case kFeedback: return reading || (m_inventoryFeedbackAvailable &&
			(!m_inventoryView.recovery.empty() || !m_inventoryView.feedback.empty()));
		case kPreviousPage: return reading && m_inventoryFeedbackOffsets.size() > 1;
		case kNextPage: return reading && m_inventoryFeedbackNext != 0;
		default: return false;
		}
	};
	auto activate = [&](ControlId control, bool half) -> Action {
		if (!control && !reading)
			return m_inventoryInteraction.activate({}, half); // outside cancels a held preview
		if (!reachable(control, half))
			return {};
		switch (control)
		{
		case kClose: back(); return {};
		case kDrop:
			cancelInventoryGesture();
			return {Command::DropStack, selected};
		case kFeedback:
			m_inventoryInteraction.cancel();
			if (reading)
			{
				const auto prior = m_inventoryFeedbackFocus;
				resetInventoryFeedback();
				m_screens.focus(enabled && controlSlot(prior).valid() ? prior : kClose);
			}
			else
			{
				m_inventoryFeedbackText = m_inventoryView.recovery;
				if (!m_inventoryFeedbackText.empty() && !m_inventoryView.feedback.empty())
					m_inventoryFeedbackText += "\n\n";
				m_inventoryFeedbackText += m_inventoryView.feedback;
				m_inventoryFeedbackOffsets.push_back(0);
				m_screens.focus(kFeedback);
			}
			return {};
		case kPreviousPage:
			m_inventoryFeedbackOffsets.pop_back();
			m_inventoryFeedbackNext = 0;
			return {};
		case kNextPage:
			m_inventoryFeedbackOffsets.push_back(m_inventoryFeedbackNext);
			m_inventoryFeedbackNext = 0;
			return {};
		default:
			return m_inventoryInteraction.activate(controlSlot(control), half);
		}
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
			if (!m_inventoryPress && (!control || reachable(control, button == SDL_BUTTON_RIGHT)))
			{
				m_inventoryPress = InventoryPress{control, button};
				// F11 (round 3): the modality follows only a validated
				// press on an actual control — an inert press on empty
				// panel space leaves the presentation alone.
				if (reachable(control, button == SDL_BUTTON_RIGHT))
				{
					// Opening the reader remembers the previous inventory target.
					if (control != kFeedback)
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
			if (reachable(control, button == SDL_BUTTON_RIGHT) ||
				(!control && !reading && m_inventoryInteraction.drag().active()))
				m_inventoryModality = InventoryModality::Pointer;
			return activate(control, button == SDL_BUTTON_RIGHT);
		}
		return {};
	}
	case SDL_EVENT_MOUSE_WHEEL:
	{
		if (event.wheel.y == 0.f)
			return {};
		if (reading)
		{
			const bool backPage = (event.wheel.y > 0.f) != (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED);
			return activate(backPage ? kPreviousPage : kNextPage, false);
		}
		if (!enabled || !selected.valid())
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
	if (reading && (key == SDLK_PAGEUP || key == SDLK_PAGEDOWN))
	{
		m_inventoryModality = InventoryModality::Keyboard;
		return activate(key == SDLK_PAGEUP ? kPreviousPage : kNextPage, false);
	}
	if (enabled && !reading)
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
	// F11 (round 3): the focus is recorded before navigation so the
	// modality follows exactly the supported navigations that move it.
	const auto previousFocus = focus;
	if (key == SDLK_TAB)
	{
		if (event.key.repeat)
			return {};
		const ControlId last = reading ? kNextPage : kFeedback;
		const int direction = shift ? -1 : 1;
		int candidate = focus >= 1 && focus <= last ? int(focus) - 1 : (shift ? 0 : int(last) - 1);
		do { candidate = (candidate + direction + int(last)) % int(last); }
		while (!reachable(ControlId(candidate + 1)));
		focus = ControlId(candidate + 1);
	}
	else if (enabled && !reading && controlSlot(focus).valid())
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
	{
		m_inventoryPress.reset();
		if (!m_inventoryFeedbackOffsets.empty())
		{
			m_inventoryFeedbackOffsets.resize(1);
			m_inventoryFeedbackNext = 0;
		}
	}
	m_inventoryLayout = layout;
	m_layoutRevision = m_screens.revision();
	if (layout.panel.empty())
		return;

	const auto &view = m_inventoryView;
	const auto *snapshot = view.snapshot ? &*view.snapshot : nullptr;
	const auto &drag = m_inventoryInteraction.drag();
	const bool reading = !m_inventoryFeedbackText.empty();
	const bool feedback = !view.feedback.empty() || !view.recovery.empty();
	const bool dropEnabled = canDropStack(view);
	const Surface paint(m_draws, font(), theme, layout.scale, scene, m_atlasLayers, m_fontScale);
	using enum style::TextRole;
	const float marker = paint.px(theme.space.xs);
	const float gap = paint.px(theme.space.sm);
	const glm::vec2 start(layout.status.rect);
	const Clip message{{start.x + marker + gap, start.y, layout.status.rect.z, layout.status.rect.w}};
	const auto summary = paint.layoutTextBlock(message, view.feedback, Caption, 2, true);
	const auto recovery = paint.layoutTextBlock(layout.recovery, view.recovery, Caption, 1, true);
	m_inventoryFeedbackAvailable = summary.page.next < view.feedback.size() || recovery.page.next < view.recovery.size();

	auto focus = m_screens.focus();
	if (reading)
	{
		if (focus != kClose && focus != kDrop && focus != kPreviousPage && focus != kNextPage)
			focus = kFeedback;
	}
	else if (!focus || (focus == kDrop && view.enabled() && !dropEnabled) ||
		focus > kFeedback || (focus == kFeedback && !m_inventoryFeedbackAvailable))
		focus = view.enabled() && snapshot->selected.valid() ? slotControl(snapshot->selected) : kClose;
	m_screens.focus(focus);
	if (!reading && controlSlot(focus).valid())
		m_inventoryFeedbackFocus = focus;

	const auto hover = inventoryControlAt(pointer);
	const auto pressed = [&](ControlId id) {
		return m_inventoryPress && m_inventoryPress->control == id && hover == id;
	};
	const auto controlState = [&](ControlId id, bool active) {
		return style::interactiveState(active, hover == id, pressed(id), focus == id, false);
	};
	const bool validSelection = snapshot && snapshot->selected.valid();
	const auto selected = validSelection
		? makeItemSlot(snapshot->slots[size_t(snapshot->selected.index)], atlasGeneration) : SlotView{};
	const bool pendingDrop = view.actionPending && view.pendingSlots && view.pendingSlots->command == Command::DropStack;
	char drop[192];

	paint.backdrop(scene, theme.palette.scrim);
	paint.card(layout.panel);
	paint.fittedLabel(layout.title, reading ? "Feedback" : "Inventory", Title, theme.palette.ink, 0.f);
	paint.button(layout.close, "Close", style::ButtonVariant::Secondary, controlState(kClose, true));
	paint.button(layout.drop, dropLabel(paint, layout.drop, selected, bool(snapshot), pendingDrop, dropEnabled, drop),
		style::ButtonVariant::DangerOutline, controlState(kDrop, dropEnabled));
	if (reading || m_inventoryFeedbackAvailable)
		paint.button(layout.feedback, reading ? "Back to items" : "View feedback",
			style::ButtonVariant::Secondary, controlState(kFeedback, true));

	const auto focused = controlSlot(focus);
	if (reading)
	{
		paint.frame(layout.reader, theme.palette.surfaceSunken, theme.palette.divider,
			paint.px(theme.size.borderWidth), 0.f);
		const auto body = inset(layout.reader, paint.px(theme.space.md));
		const size_t offset = m_inventoryFeedbackOffsets.back();
		const auto text = std::string_view(m_inventoryFeedbackText).substr(offset);
		const auto block = paint.layoutTextBlock(body, text, Body, int(kMaxWrappedLines), false);
		m_inventoryFeedbackNext = block.page.next > 0 && block.page.next < text.size() ? offset + block.page.next : 0;
		paint.paintTextBlock(body, text, Body, theme.palette.ink, block);
		paint.button(layout.previousPage, "Previous", style::ButtonVariant::Secondary,
			controlState(kPreviousPage, m_inventoryFeedbackOffsets.size() > 1));
		paint.button(layout.nextPage, "Next", style::ButtonVariant::Secondary,
			controlState(kNextPage, m_inventoryFeedbackNext != 0));
	}
	else
	{
		paint.label(layout.hotbarLabel, "Hotbar", Caption, theme.palette.inkMuted);
		std::array<SlotView, kSlotCount> slots{};
		for (int cell = 0; cell < kSlotCount; ++cell)
		{
			const auto id = slotAtCell(cell);
			const auto control = slotControl(id);
			auto &entry = slots[size_t(cell)];
			if (snapshot)
				entry = makeItemSlot(displayedSlotStack(id, drag, snapshot->slots), atlasGeneration);
			entry.state = inventorySlotState(view, id, control == hover, pressed(control), control == focus);
		}
		const std::span<const SlotView> grid{slots};
		paint.slotGrid(layout.main, grid.first(kColumns * kMainRows));
		paint.slotGrid(layout.hotbar, grid.last(kColumns));

		if (feedback)
		{
			if (!view.feedback.empty())
				paint.fill(rectangle(start + glm::vec2(0.f, gap), {marker, marker}),
					style::statusColor(theme, view.feedbackKind), 0.f);
			paint.paintTextBlock(message, view.feedback, Caption, theme.palette.ink, summary);
			paint.paintTextBlock(layout.recovery, view.recovery, Caption, theme.palette.inkMuted, recovery);
		}
		else
			paint.wrappedLabel(layout.instructions,
				"Click or Enter to move. Right-click or Shift+Enter for half. "
				"Arrows / Tab focus. 1-9 / wheel select. Q drops one.",
				Caption, theme.palette.inkMuted, 2);

		if (snapshot && focused.valid() && (!layout.compact || !feedback))
		{
			char description[96];
			const auto item = describeSlot(makeItemSlot(snapshot->slots[size_t(focused.index)], atlasGeneration), description);
			char label[112];
			std::snprintf(label, sizeof(label), "Focused: %.*s", int(item.size()), item.data());
			paint.fittedLabel(layout.detail, label, Caption, theme.palette.ink, 0.f);
		}
	}

	if (drag.active() && snapshot && !reading)
	{
		// Passive pointer motion does not displace a keyboard-anchored preview (#316).
		const auto anchorRect = layout.slot(focused.valid() ? focused : drag.sourceSlot);
		const auto anchor = m_inventoryModality == InventoryModality::Keyboard
			? glm::vec2(anchorRect.rect) + size(anchorRect) * 0.5f
			: hit(scene, pointer) ? pointer : glm::vec2(anchorRect.rect);
		paint.slot(heldStackPlacement(anchor, layout.main.cell, 10.f * layout.scale, scene),
			makeItemSlot(cursorStack(drag, snapshot->slots), atlasGeneration));
		return;
	}

	// Choose one source. Keyboard modality only considers the focused control,
	// even if passive mouse motion is over a different candidate.
	const bool keyboard = m_inventoryModality == InventoryModality::Keyboard;
	const auto target = keyboard ? focus : hover;
	auto tipAnchor = pointer;
	std::string_view tip;
	char label[96];
	if (!reading && target == kFeedback && m_inventoryFeedbackAvailable)
	{
		tip = "Read the complete inventory message";
		if (keyboard)
			tipAnchor = glm::vec2(layout.feedback.rect);
	}
	else if (!keyboard && !reading && feedback &&
		(hit(layout.recovery, pointer) || hit(layout.status, pointer)))
	{
		tip = m_inventoryFeedbackAvailable ? "Open View feedback for the complete message"
			: hit(layout.recovery, pointer) ? view.recovery : view.feedback;
	}
	else if (target == kDrop && !selected.empty() && !pendingDrop)
	{
		tip = describeSlot(selected, label);
		if (keyboard)
			tipAnchor = glm::vec2(layout.drop.rect);
	}
	else if (!reading && snapshot && !keyboard)
	{
		const auto hovered = controlSlot(target);
		if (hovered.valid())
			tip = describeSlot(makeItemSlot(snapshot->slots[size_t(hovered.index)], atlasGeneration), label);
	}
	if (!tip.empty())
		paint.tooltip(scene, tipAnchor, tip);
}
