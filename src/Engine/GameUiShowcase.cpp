#include <Engine/GameUiSample.hpp>

#include <cstdio>

using namespace gameui;

namespace
{
enum Control : ControlId { Resume = 1, Showcase, Quit, Return, Confirm, Name, Option, Amount, Choices, Disabled };
constexpr glm::vec4 kText{0.94f, 0.97f, 1.f, 1.f};
constexpr glm::vec4 kDim{0.70f, 0.77f, 0.81f, 1.f};
constexpr glm::vec4 kFocus{0.44f, 0.94f, 0.78f, 1.f};
constexpr glm::vec4 kCard{0.052f, 0.083f, 0.103f, 0.97f};
constexpr glm::vec4 kControl{0.13f, 0.23f, 0.27f, 1.f};
constexpr glm::vec4 kHover{0.19f, 0.34f, 0.37f, 1.f};
constexpr glm::vec4 kDisabled{0.086f, 0.13f, 0.15f, 1.f};
constexpr std::array<std::string_view, 8> kChoices{
	"Grassland", "Forest", "Desert", "Tundra", "Swamp", "Savanna", "Taiga", "Ocean"};
constexpr int kVisible = 3;

NavKey navKey(SDL_Keycode code)
{
	switch (code)
	{
	case SDLK_TAB: return NavKey::Tab;
	case SDLK_RETURN:
	case SDLK_KP_ENTER: return NavKey::Enter;
	case SDLK_SPACE: return NavKey::Space;
	case SDLK_LEFT: return NavKey::Left;
	case SDLK_RIGHT: return NavKey::Right;
	case SDLK_UP: return NavKey::Up;
	case SDLK_DOWN: return NavKey::Down;
	case SDLK_HOME: return NavKey::Home;
	case SDLK_END: return NavKey::End;
	case SDLK_BACKSPACE: return NavKey::Backspace;
	case SDLK_DELETE: return NavKey::Delete;
	case SDLK_A: return NavKey::A;
	case SDLK_C: return NavKey::C;
	case SDLK_V: return NavKey::V;
	case SDLK_X: return NavKey::X;
	default: return NavKey::Other;
	}
}
} // namespace

void GameUiSample::invalidateLayout()
{
	cancelPointerPress();
	m_layoutRevision = UINT64_MAX;
	m_field.cancelComposition();
}

void GameUiSample::resetScreens()
{
	m_screens.reset();
	m_field = {};
	m_list = {};
	invalidateLayout();
}

void GameUiSample::openPause()
{
	if (m_screens.top() == Screen::Hud && m_screens.push(Screen::Pause))
		invalidateLayout();
}

void GameUiSample::back()
{
	if (m_screens.back())
		invalidateLayout();
}

void GameUiSample::closePause()
{
	while (m_screens.contains(Screen::Pause) && m_screens.back())
		invalidateLayout();
}

bool GameUiSample::syncInventory(bool open)
{
	if (open && m_screens.top() == Screen::Hud)
	{
		m_screens.push(Screen::Inventory);
		invalidateLayout();
	}
	else if (!open && m_screens.top() == Screen::Inventory)
		back();
	return m_screens.top() == Screen::Inventory;
}

void GameUiSample::buildScreen(Clip scene, float scale)
{
	const bool newScreen = m_layoutRevision != m_screens.revision();
	const float offset = newScreen ? 0.f : m_widgets.viewport().offset;
	const auto screen = m_screens.top();
	m_screenScale = scale;
	const float height = screen == Screen::Showcase ? 364.f : (screen == Screen::Pause ? 224.f : 164.f);
	const auto panel = anchor(inset(scene, 8.f), {456.f * scale, height * scale}, {0.5f, 0.5f});
	const bool moved = panel != m_panel;
	m_panel = panel;
	if (moved)
		m_widgets.cancelPress();
	const auto body = inset(m_panel, 14.f * scale);
	const float contentHeight = (height - 28.f) * scale;
	m_widgets.reset(body, contentHeight, offset);
	StackLayout column(rectangle(glm::vec2(body.rect), {size(body).x, contentHeight}), true, 6.f * scale);
	const auto add = [&](ControlId id, WidgetKind kind, float h, std::string_view title,
						 std::string_view tooltip = {}, bool enabled = true) {
		m_widgets.add({id, kind, column.next(h * scale), title, tooltip, enabled});
	};
	if (screen == Screen::Showcase)
	{
		add(0, WidgetKind::Label, 22, "Widget showcase");
		add(Name, WidgetKind::TextField, 36, "Name", "Tab moves focus. Ctrl+C / V uses the clipboard.");
		add(Option, WidgetKind::Toggle, 28, "Preview option");
		add(Amount, WidgetKind::Slider, 32, "Preview level");
		add(Choices, WidgetKind::List, 96, "Biome preview", "Scroll or use the Up and Down keys.");
		const auto row = column.next(30.f * scale);
		m_widgets.add({Return, WidgetKind::Button, gridCell(row, 2, 1, 0, 0, 6.f * scale), "Back", {}});
		m_widgets.add({Disabled, WidgetKind::Button, gridCell(row, 2, 1, 1, 0, 6.f * scale),
			"Unavailable", "Example of a disabled control.", false});
	}
	else if (screen == Screen::ConfirmExit)
	{
		add(0, WidgetKind::Label, 26, "Exit to desktop?");
		add(0, WidgetKind::Label, 27, "The game will use its normal shutdown path.");
		const auto row = column.next(36.f * scale);
		m_widgets.add({Return, WidgetKind::Button, gridCell(row, 2, 1, 0, 0, 8.f * scale), "Cancel", {}});
		m_widgets.add({Confirm, WidgetKind::Button, gridCell(row, 2, 1, 1, 0, 8.f * scale), "Exit", {}});
	}
	else if (screen == Screen::Pause)
	{
		add(0, WidgetKind::Label, 32, "Game menu");
		add(Resume, WidgetKind::Button, 36, "Resume");
		add(Showcase, WidgetKind::Button, 36, "Widget showcase");
		add(Quit, WidgetKind::Button, 36, "Exit to desktop");
	}
	if (newScreen || moved)
	{
		const auto *restored = m_widgets.find(m_screens.focus());
		focusControl(restored && restored->enabled ? restored : m_widgets.step(0, false));
	}
	m_layoutRevision = m_screens.revision();
}

void GameUiSample::paintScreen(glm::vec2 pointer)
{
	const auto clip = m_widgets.viewport().clip;
	if (m_screens.suppressDeveloper())
		m_draws.panel(m_draws.bounds().rect, {0.02f, 0.03f, 0.04f, 0.68f}, m_draws.bounds());
	m_draws.panel(m_panel.rect, kCard, m_draws.bounds());
	const auto *hover = m_widgets.at(pointer);
	const float s = m_screenScale;
	for (const auto &widget : m_widgets.widgets())
	{
		const auto bounds = m_widgets.bounds(widget);
		if (bounds.intersect(clip).empty())
			continue;
		const glm::vec2 origin(bounds.rect);
		const bool focused = widget.id && m_screens.focus() == widget.id;
		const auto fg = widget.enabled ? kText : kDim;
		const auto label = [&](std::string_view value, float x, float y, glm::vec4 color = kText) {
			text(origin + glm::vec2(x, y) * s, value, color, s, clip.intersect(bounds));
		};
		if (widget.kind == WidgetKind::Label)
		{
			label(widget.label, 3, 4);
			continue;
		}
		if (focused)
			m_draws.panel(inset(bounds, -1.f).rect, kFocus, clip);
		m_draws.panel(bounds.rect, !widget.enabled ? kDisabled :
			(hover == &widget || m_widgets.pressed(widget.id) ? kHover : kControl), clip);
		switch (widget.kind)
		{
		case WidgetKind::TextField:
		{
			label(widget.label, 8, 2, kDim);
			const auto live = std::string_view(m_field.value());
			const auto textClip = clip.intersect(inset(bounds, 5.f * s));
			const float caretPx = textWidth(live.substr(0, m_field.caret()), s);
			const float cursorRoom = std::max(0.f, size(textClip).x - 8.f * s);
			const float offset = focused ? std::max(0.f, caretPx - cursorRoom) : 0.f;
			const glm::vec2 start = origin + glm::vec2(8.f * s - offset, 18.f * s);
			const float selectStart = textWidth(live.substr(0, m_field.selectionStart()), s);
			const float selectEnd = textWidth(live.substr(0, m_field.selectionEnd()), s);
			if (focused && selectStart < selectEnd)
				m_draws.panel(rectangle(start + glm::vec2(selectStart, -s),
					{selectEnd - selectStart, 13.f * s}).rect, kHover, textClip);
			const std::string_view preedit = focused ? std::string_view(m_field.composition()) : std::string_view{};
			text(start, live.substr(0, m_field.caret()), kText, s, textClip);
			text(start + glm::vec2(caretPx, 0.f), preedit, kFocus, s, textClip);
			text(start + glm::vec2(caretPx + textWidth(preedit, s), 0.f), live.substr(m_field.caret()),
				kText, s, textClip);
			if (focused)
				m_draws.panel(rectangle(start + glm::vec2(caretPx, 0.f), {s, 13.f * s}).rect, kFocus, textClip);
			break;
		}
		case WidgetKind::Toggle:
			label(widget.label, 8, 7);
			label(m_feature ? "On" : "Off", std::max(8.f, size(bounds).x / s - 40.f), 7, kFocus);
			break;
		case WidgetKind::Slider:
		{
			label(widget.label, 8, 2);
			char value[12];
			std::snprintf(value, sizeof(value), "%.0f%%", m_slider * 100.f);
			label(value, std::max(8.f, size(bounds).x / s - 40.f), 2, kDim);
			const auto bar = rectangle(origin + glm::vec2(8.f, 20.f) * s,
				{std::max(0.f, size(bounds).x - 16.f * s), 5.f * s});
			m_draws.panel(bar.rect, kCard, clip);
			m_draws.panel(rectangle(glm::vec2(bar.rect), {size(bar).x * m_slider, 5.f * s}).rect, kFocus, clip);
			break;
		}
		case WidgetKind::List:
			label(widget.label, 8, 3, kDim);
			for (int i = 0; i < kVisible; ++i)
			{
				const int index = m_list.first + i;
				const auto row = rectangle(origin + glm::vec2(6.f, 19.f + 24.f * i) * s,
					{std::max(0.f, size(bounds).x - 12.f * s), 22.f * s});
				if (index == m_list.selected || hit(row, pointer))
					m_draws.panel(row.rect, kHover, clip);
				label(kChoices[static_cast<size_t>(index)], 12, 24.f + 24.f * i);
			}
			break;
		default:
			label(widget.label, 8, 8, fg);
			break;
		}
	}
	if (hover && !hover->tooltip.empty())
	{
		const float height = 21.f * s;
		const auto extent = glm::min(glm::vec2{textWidth(hover->tooltip, s) + 12.f * s, height}, size(clip));
		const auto desired = pointer + glm::vec2{12.f * s, -height - 4.f * s};
		const auto pos = glm::clamp(desired, glm::vec2(clip.rect), glm::vec2(clip.rect.z, clip.rect.w) - extent);
		const auto tip = rectangle(pos, extent);
		m_draws.panel(tip.rect, kCard, clip);
		text(pos + glm::vec2(6.f, 4.f) * s, hover->tooltip, kText, s, tip.intersect(clip));
	}
}

void GameUiSample::focusControl(const Widget *widget)
{
	const ControlId next = widget ? widget->id : 0;
	if (m_screens.focus() != next)
		m_field.cancelComposition();
	m_screens.focus(next);
	if (widget)
		m_widgets.viewport().reveal(widget->bounds);
}

void GameUiSample::setSlider(const Widget &widget, float x)
{
	const auto bar = inset(m_widgets.bounds(widget), 8.f * m_screenScale);
	if (size(bar).x > 0.f)
		m_slider = std::clamp((x - bar.rect.x) / size(bar).x, 0.f, 1.f);
}

GameUiSample::InputResult GameUiSample::useControl(const Widget &widget, glm::vec2 point)
{
	switch (widget.id)
	{
	case Resume:
	case Return: back(); break;
	case Showcase: m_screens.push(Screen::Showcase); invalidateLayout(); break;
	case Quit: m_screens.push(Screen::ConfirmExit); invalidateLayout(); break;
	case Confirm: return InputResult::Exit;
	case Option: m_feature = !m_feature; break;
	case Choices:
		if (point.y >= m_widgets.bounds(widget).rect.y + 19.f * m_screenScale)
		{
			const int row = int((point.y - m_widgets.bounds(widget).rect.y - 19.f * m_screenScale) /
				(24.f * m_screenScale));
			if (row < kVisible)
				m_list.select(m_list.first + row, int(kChoices.size()), kVisible);
		}
		break;
	default: break;
	}
	return InputResult::Consumed;
}

void GameUiSample::editText(NavKey key, bool shift, bool shortcut)
{
	if (shortcut)
	{
		switch (key)
		{
		case NavKey::A: m_field.selectAll(); break;
		case NavKey::C:
		case NavKey::X:
			if (!m_field.selection().empty() && SDL_SetClipboardText(std::string(m_field.selection()).c_str()) &&
				key == NavKey::X)
				m_field.erase(NavKey::Delete);
			break;
		case NavKey::V:
			if (char *clipboard = SDL_GetClipboardText())
			{
				m_field.insert(clipboard);
				SDL_free(clipboard);
			}
			break;
		default: break;
		}
		return;
	}
	if (!m_field.composition().empty())
		return;
	if (key == NavKey::Backspace || key == NavKey::Delete)
		m_field.erase(key);
	else
		m_field.move(key, shift);
}

void GameUiSample::updateTextInput(SDL_Window *window, bool interactive)
{
	const auto *field = m_widgets.find(m_screens.focus());
	const auto bounds = field ? m_widgets.bounds(*field).intersect(m_widgets.viewport().clip) : Clip{};
	const bool active = interactive && m_screens.nativeScreen() &&
		m_layoutRevision == m_screens.revision() && field && field->kind == WidgetKind::TextField && !bounds.empty();
	if (!active)
	{
		if (m_ownsTextInput)
			SDL_StopTextInput(window);
		m_ownsTextInput = false;
		m_field.cancelComposition();
		return;
	}
	if (!m_ownsTextInput || !SDL_TextInputActive(window))
		m_ownsTextInput = SDL_StartTextInput(window);
	const SDL_Rect area{int(bounds.rect.x), int(bounds.rect.y), int(size(bounds).x), int(size(bounds).y)};
	const float advance = textWidth(std::string_view(m_field.value()).substr(0, m_field.caret()), m_screenScale);
	SDL_SetTextInputArea(window, &area, int(std::clamp(advance, 0.f, size(bounds).x)));
}

void GameUiSample::yieldTextInput(SDL_Window *window)
{
	if (m_ownsTextInput)
	{
		focusControl(nullptr);
		updateTextInput(window, false);
	}
}

GameUiSample::InputResult GameUiSample::handleScreenEvent(const SDL_Event &event)
{
	if (m_layoutRevision != m_screens.revision())
		return InputResult::Consumed;
	const auto *focused = m_widgets.find(m_screens.focus());
	const bool visible = focused && !m_widgets.bounds(*focused).intersect(m_widgets.viewport().clip).empty();
	const bool editing = visible && focused->kind == WidgetKind::TextField;
	switch (event.type)
	{
	case SDL_EVENT_TEXT_INPUT:
		if (editing && event.text.text)
			m_field.insert(event.text.text);
		break;
	case SDL_EVENT_TEXT_EDITING:
		if (editing)
			m_field.preedit(event.edit.text ? event.edit.text : "");
		break;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
		if (event.button.button == SDL_BUTTON_LEFT)
		{
			const glm::vec2 point{event.button.x, event.button.y};
			m_widgets.press(point, m_screens);
			const auto *widget = m_widgets.find(m_screens.focus());
			if (widget && widget->kind == WidgetKind::Slider)
				setSlider(*widget, point.x);
			if (widget && widget->kind == WidgetKind::TextField)
			{
				const auto bounds = m_widgets.bounds(*widget);
				const auto value = std::string_view(m_field.value());
				const float room = std::max(0.f, size(bounds).x - 18.f * m_screenScale);
				const float offset = editing ? std::max(0.f, textWidth(value.substr(0, m_field.caret()), m_screenScale) - room) : 0.f;
				const float local = point.x - bounds.rect.x - 8.f * m_screenScale + offset;
				size_t byte = 0;
				float x = 0.f;
				while (byte < value.size())
				{
					const auto next = nextUtf8(value, byte);
					const float advance = textWidth(value.substr(byte, next - byte), m_screenScale);
					if (local < x + advance * 0.5f)
						break;
					x += advance;
					byte = next;
				}
				m_field.placeCaret(byte, SDL_GetModState() & SDL_KMOD_SHIFT);
			}
		}
		break;
	case SDL_EVENT_MOUSE_MOTION:
		if (focused && focused->kind == WidgetKind::Slider && m_widgets.pressed(focused->id))
			setSlider(*focused, event.motion.x);
		break;
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (event.button.button == SDL_BUTTON_LEFT)
		{
			const glm::vec2 point{event.button.x, event.button.y};
			if (const auto *widget = m_widgets.release(point))
				return useControl(*widget, point);
		}
		break;
	case SDL_EVENT_MOUSE_WHEEL:
	{
		const glm::vec2 point{event.wheel.mouse_x, event.wheel.mouse_y};
		if (!hit(m_widgets.viewport().clip, point) || event.wheel.y == 0.f)
			break;
		m_widgets.cancelPress();
		const auto *hover = m_widgets.at(point);
		const float delta = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
		if (hover && hover->kind == WidgetKind::List)
			m_list.scroll(delta > 0.f ? -1 : 1, int(kChoices.size()), kVisible);
		else
			m_widgets.viewport().setOffset(m_widgets.viewport().offset - delta * 36.f * m_screenScale);
		break;
	}
	case SDL_EVENT_KEY_DOWN:
	{
		const auto key = navKey(event.key.key);
		const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
		const bool shortcut = (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
		if (key == NavKey::Tab || (!editing && focused && focused->kind != WidgetKind::List &&
			(key == NavKey::Up || key == NavKey::Down)))
			focusControl(m_widgets.step(m_screens.focus(), key == NavKey::Up || (key == NavKey::Tab && shift)));
		else if (editing)
			editText(key, shift, shortcut);
		else if (visible && focused->enabled)
		{
			if (focused->kind == WidgetKind::Slider && (key == NavKey::Left || key == NavKey::Right))
				m_slider = std::clamp(m_slider + (key == NavKey::Left ? -0.05f : 0.05f), 0.f, 1.f);
			else if (focused->kind == WidgetKind::List && (key == NavKey::Up || key == NavKey::Down))
				m_list.select(m_list.selected + (key == NavKey::Up ? -1 : 1), int(kChoices.size()), kVisible);
			else if (!event.key.repeat && (key == NavKey::Enter || key == NavKey::Space))
				return useControl(*focused);
		}
		break;
	}
	default: break;
	}
	return InputResult::Consumed;
}
