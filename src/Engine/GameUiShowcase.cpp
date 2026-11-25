#include <Engine/GameUiSample.hpp>

using namespace gameui;

namespace
{
enum ScreenControl : ControlId
{
	Resume = 1, Showcase, Quit, Return, Confirm, Name, Option, Amount, Choices,
	SecondaryName, EnableSecond, SecondaryAmount, SecondaryChoices, Disabled
};
constexpr glm::vec4 kCard{0.052f, 0.083f, 0.103f, 0.97f};
constexpr std::array<std::string_view, 8> kBiomes{
	"Grassland", "Forest", "Desert", "Tundra", "Swamp", "Savanna", "Taiga", "Ocean"};
constexpr std::array<std::string_view, 6> kMaterials{
	"Stone", "Sand", "Dirt", "Snow", "Gravel", "Clay"};

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
	m_widgets.cancelEditing();
}

void GameUiSample::resetScreens()
{
	invalidateLayout();
	m_screens.reset();
	m_field = {};
	m_secondaryField = {};
	m_list.selection = {};
	m_secondaryList.selection = {};
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
	const float width = screen == Screen::Showcase ? 640.f : 456.f;
	const auto panel = anchor(inset(scene, 8.f), {width * scale, height * scale}, {0.5f, 0.5f});
	const bool moved = panel != m_panel;
	m_panel = panel;
	if (moved)
		m_widgets.cancelPress();
	const auto body = inset(m_panel, 14.f * scale);
	const float contentHeight = (height - 28.f) * scale;
	m_widgets.reset(body, contentHeight, offset);
	StackLayout column(rectangle(glm::vec2(body.rect), {size(body).x, contentHeight}), true, 6.f * scale);
	const auto add = [&](ControlId id, Control control, float h, std::string_view title) {
		m_widgets.add({id, column.next(h * scale), title, {}, control});
	};
	if (screen == Screen::Showcase)
	{
		m_list.items = kBiomes;
		m_secondaryList.items = kMaterials;
		add(0, Label{}, 22.f, "Widget showcase");
		const auto controls = column.next(210.f * scale);
		StackLayout left(gridCell(controls, 2, 1, 0, 0, 8.f * scale), true, 6.f * scale);
		StackLayout right(gridCell(controls, 2, 1, 1, 0, 8.f * scale), true, 6.f * scale);
		m_widgets.add({Name, left.next(36.f * scale), "Name", "Use Tab to advance or Ctrl+C / V for the clipboard.", &m_field});
		m_widgets.add({Option, left.next(28.f * scale), "Preview option", {}, &m_feature});
		m_widgets.add({Amount, left.next(32.f * scale), "Preview level", {}, &m_slider});
		m_widgets.add({Choices, left.next(96.f * scale), "Biome preview", "Scroll or use the Up and Down keys.", &m_list});
		m_widgets.add({SecondaryName, right.next(36.f * scale), "Secondary name", {}, &m_secondaryField, m_enableSecond.value});
		m_widgets.add({EnableSecond, right.next(28.f * scale), "Enable secondary name", {}, &m_enableSecond});
		m_widgets.add({SecondaryAmount, right.next(32.f * scale), "Secondary level", {}, &m_secondarySlider});
		m_widgets.add({SecondaryChoices, right.next(96.f * scale), "Material preview", {}, &m_secondaryList});
		const auto row = column.next(30.f * scale);
		m_widgets.add({Return, gridCell(row, 2, 1, 0, 0, 6.f * scale), "Back", {}, Button{}});
		m_widgets.add({Disabled, gridCell(row, 2, 1, 1, 0, 6.f * scale),
			"Unavailable", "Example of a disabled control.", Button{}, false});
	}
	else if (screen == Screen::ConfirmExit)
	{
		add(0, Label{}, 26.f, "Exit to desktop?");
		add(0, Label{}, 27.f, "The game will use its normal shutdown path.");
		const auto row = column.next(36.f * scale);
		m_widgets.add({Return, gridCell(row, 2, 1, 0, 0, 8.f * scale), "Cancel", {}, Button{}});
		m_widgets.add({Confirm, gridCell(row, 2, 1, 1, 0, 8.f * scale), "Exit", {}, Button{}});
	}
	else if (screen == Screen::Pause)
	{
		add(0, Label{}, 32.f, "Game menu");
		add(Resume, Button{}, 36.f, "Resume");
		add(Showcase, Button{}, 36.f, "Widget showcase");
		add(Quit, Button{}, 36.f, "Exit to desktop");
	}
	auto focus = m_screens.focus();
	m_widgets.focus(focus, focus, newScreen || moved);
	if (newScreen && !focus)
		if (const auto *initial = m_widgets.step(0, false))
			m_widgets.focus(focus, initial->id, true);
	m_screens.focus(focus);
	m_layoutRevision = m_screens.revision();
}

void GameUiSample::paintScreen(glm::vec2 pointer)
{
	if (m_screens.suppressDeveloper())
		m_draws.panel(m_draws.bounds().rect, {0.02f, 0.03f, 0.04f, 0.68f}, m_draws.bounds());
	m_draws.panel(m_panel.rect, kCard, m_draws.bounds());
	m_widgets.paint(m_draws, font(), m_screenScale, m_screens.focus(), pointer);
}

GameUiSample::InputResult GameUiSample::applyWidgetEvent(WidgetEvent event)
{
	if (m_sdlEditor && m_sdlEditor == m_widgets.editor(event.id) && event.id == m_screens.focus())
	{
		switch (event.action)
		{
		case WidgetAction::Copy:
		case WidgetAction::Cut:
		{
			const bool cut = event.action == WidgetAction::Cut;
			const auto selected = m_sdlEditor->selection();
			event = !selected.empty() && SDL_SetClipboardText(std::string(selected).c_str()) && cut
				? m_widgets.completeCut(event.id) : WidgetEvent{};
			break;
		}
		case WidgetAction::Paste:
		{
			const auto target = event.id;
			event = {};
			if (char *clipboard = SDL_GetClipboardText())
			{
				event = m_widgets.commitText(target, clipboard);
				SDL_free(clipboard);
			}
			break;
		}
		default: break;
		}
	}
	if (event.action == WidgetAction::Activate)
	{
		switch (event.id)
		{
		case Resume:
		case Return: back(); break;
		case Showcase: m_screens.push(Screen::Showcase); invalidateLayout(); break;
		case Quit: m_screens.push(Screen::ConfirmExit); invalidateLayout(); break;
		case Confirm: return InputResult::Exit;
		default: break;
		}
	}
	if (event.action == WidgetAction::Change && event.id == EnableSecond)
	{
		auto focus = m_screens.focus();
		m_widgets.setEnabled(SecondaryName, m_enableSecond.value, focus);
		m_screens.focus(focus);
	}
	return InputResult::Consumed;
}

void GameUiSample::updateTextInput(SDL_Window *window, bool interactive)
{
	auto *field = interactive && m_screens.nativeScreen() && m_layoutRevision == m_screens.revision()
		? m_widgets.editor(m_screens.focus()) : nullptr;
	const auto geometry = field ? m_widgets.editorGeometry(m_screens.focus(), font(), m_screenScale) : TextGeometry{};
	if (geometry.clip.empty())
		field = nullptr;
	if (m_sdlEditor && m_sdlEditor != field)
	{
		m_sdlEditor->cancelComposition();
		SDL_ClearComposition(window);
		SDL_StopTextInput(window);
		m_sdlEditor = nullptr;
	}
	if (!field || (!SDL_TextInputActive(window) && !SDL_StartTextInput(window)))
		return;
	m_sdlEditor = field;
	const auto &clip = geometry.clip.rect;
	const int left = int(std::floor(clip.x));
	const int top = int(std::floor(std::clamp(geometry.origin.y, clip.y, clip.w)));
	const SDL_Rect area{left, top, std::max(1, int(std::ceil(clip.z)) - left),
		std::max(1, int(std::ceil(std::min(clip.w, geometry.origin.y + 13.f * m_screenScale))) - top)};
	SDL_SetTextInputArea(window, &area, int(std::lround(geometry.caretX)) - left);
}

bool GameUiSample::yieldTextInput(SDL_Window *window)
{
	if (!m_sdlEditor)
		return false;
	auto focus = m_screens.focus();
	m_widgets.focus(focus, 0, false);
	m_screens.focus(focus);
	updateTextInput(window, false);
	return true;
}

bool GameUiSample::canFocusWidget(float x, float y) const
{
	return m_layoutRevision == m_screens.revision() && m_widgets.focusableAt({x, y});
}

GameUiSample::InputResult GameUiSample::handleScreenEvent(const SDL_Event &event)
{
	if (m_layoutRevision != m_screens.revision())
		return InputResult::Consumed;
	auto focus = m_screens.focus();
	WidgetEvent result;
	switch (event.type)
	{
	case SDL_EVENT_TEXT_INPUT:
		if (m_sdlEditor && m_sdlEditor == m_widgets.editor(focus) && event.text.text)
			result = m_widgets.commitText(focus, event.text.text);
		break;
	case SDL_EVENT_TEXT_EDITING:
		if (m_sdlEditor && m_sdlEditor == m_widgets.editor(focus))
			m_sdlEditor->preedit(event.edit.text ? event.edit.text : "", event.edit.start, event.edit.length);
		break;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
		if (event.button.button == SDL_BUTTON_LEFT)
			result = m_widgets.press({event.button.x, event.button.y}, focus, font(), m_screenScale,
				(SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
		break;
	case SDL_EVENT_MOUSE_MOTION:
		result = m_widgets.drag({event.motion.x, event.motion.y}, m_screenScale);
		break;
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (event.button.button == SDL_BUTTON_LEFT)
			result = m_widgets.release({event.button.x, event.button.y}, m_screenScale);
		break;
	case SDL_EVENT_MOUSE_WHEEL:
		m_widgets.scroll({event.wheel.mouse_x, event.wheel.mouse_y},
			event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y, m_screenScale, focus);
		break;
	case SDL_EVENT_KEY_DOWN:
		result = m_widgets.key(focus, navKey(event.key.key),
			(event.key.mod & SDL_KMOD_SHIFT) != 0,
			(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0, event.key.repeat);
		break;
	default: break;
	}
	m_screens.focus(focus);
	return applyWidgetEvent(result);
}
