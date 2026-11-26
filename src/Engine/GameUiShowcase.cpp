#include <Engine/GameUiSample.hpp>

#include <utility>

using namespace gameui;

namespace
{
enum ScreenControl : ControlId
{
	Resume = 1, Showcase, Quit, Return, Confirm, Name, Option, Amount, Choices,
	SecondaryName, EnableSecond, SecondaryAmount, SecondaryChoices, Disabled, Gallery, HudMock, ToggleHud, Worlds, NewWorld, OpenWorld, CreateWorld, WorldList, WorldName, WorldSeed,
	Save, Leave, ConfirmLeave, Multiplayer, Settings, Retry, CancelFailure, ForceStop, ConfirmForceStop, FailedQuit, Refresh
};
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
	// Geometry only (issue #302 F16): a scale or resolution change rebuilds the
	// layout but keeps focus, selection, notifications, field values and the
	// scroll offset. Screen entry is detected separately by m_screenRevision and
	// is the only place that resets presentation state or cancels composition.
	cancelPointerPress();
	m_layoutRevision = UINT64_MAX;
}

void GameUiSample::resetScreens(Screen root)
{
	invalidateLayout();
	setInventoryView({});
	cancelInventoryGesture();
	m_screens.reset(root);
	m_sessionRequest.reset();
	m_settingsRequest.reset();
	m_displayChoice = {};
	m_multiplayerRequest.reset();
	m_multiplayerConfirm.reset();
	m_mpError.clear();
	m_mpErrorOnHost = false;
	// The explicit administration target and the clipboard note belong to the
	// multiplayer visit being left (corrections F01/F08).
	m_mpAdminTarget = {};
	m_mpClipboard.clear();
	m_sessionStatus.clear();
	m_worldName = {};
	m_worldSeed = {};
	m_worldList.selection = {};
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
	if (holdingForSession())
		return;
	if (m_screens.top() == Screen::Settings && m_settingsView.display)
	{
		m_settingsRequest = SettingsAction::Revert;
		return;
	}
	if (m_screens.top() == Screen::Settings)
		m_displayChoice = {};
	if (m_screens.top() == Screen::SessionError)
	{
		m_sessionRequest = SessionRequest{SessionAction::Cancel, {}, {}};
		return;
	}
	if (m_screens.top() == Screen::MultiplayerConfirm)
		m_multiplayerConfirm.reset();
	if (m_screens.top() == Screen::Inventory)
	{
		setInventoryView({});
		cancelInventoryGesture();
	}
	if (m_screens.back())
		invalidateLayout();
}

void GameUiSample::closePause()
{
	while (m_screens.contains(Screen::Pause) && m_screens.back())
		invalidateLayout();
}

void GameUiSample::setInventoryOpen(bool open)
{
	if (open && m_screens.top() == Screen::Hud)
	{
		m_screens.push(Screen::Inventory);
		invalidateLayout();
	}
	else if (!open && m_screens.top() == Screen::Inventory)
		back();
}

void GameUiSample::buildScreen(Clip scene, float scale)
{
	const bool entered = m_screenRevision != m_screens.revision();
	const bool rebuilt = m_layoutRevision != m_screens.revision();
	const float offset = entered ? 0.f : m_widgets.viewport().offset;
	if (entered)
	{
		m_screenRevision = m_screens.revision();
		m_widgets.cancelEditing();
	}
	else if (rebuilt)
		m_widgets.cancelPress();
	const auto screen = m_screens.top();
	const bool settings = screen == Screen::Settings || screen == Screen::Controls;
	m_screenScale = scale;
	const bool multiplayer = multiplayerScreen(screen);
	const bool displayPreview = screen == Screen::Settings && m_settingsView.display.has_value();
	const float settingsHeight = screen == Screen::Controls ? 520.f : displayPreview ? 278.f :
		(m_settingsView.notice.empty() ? 470.f : 560.f);
	const float multiplayerHeight = screen == Screen::MultiplayerConfirm ? 310.f : 524.f;
	const float height = settings ? settingsHeight : multiplayer ? multiplayerHeight : screen == Screen::Showcase ? 364.f :
		(screen == Screen::Pause ? 450.f : (screen == Screen::Worlds ? 355.f : 260.f));
	const float width = settings ? (displayPreview ? 480.f : 620.f) :
		multiplayer ? 560.f : screen == Screen::Showcase ? 640.f : 456.f;
	const auto panel = anchor(inset(scene, 8.f), {width * scale, height * scale}, {0.5f, 0.5f});
	const bool moved = panel != m_panel;
	m_panel = panel;
	if (moved)
		m_widgets.cancelPress();
	const auto body = inset(m_panel, 14.f * scale);
	const float contentHeight = (settings ? 680.f : multiplayer ? 760.f : height - 28.f) * scale;
	m_widgets.reset(body, contentHeight, offset);
	StackLayout column(rectangle(glm::vec2(body.rect), {size(body).x, contentHeight}), true, 6.f * scale);
	const auto add = [&](ControlId id, Control control, float h, std::string_view title) {
		m_widgets.add({id, column.next(h * scale), title, {}, control});
	};
	m_sessionMessage = {};
	if (settings || multiplayer)
	{
		if (settings)
			buildSettingsScreen(screen, column);
		else
			buildMultiplayerScreen(screen, column);
		if (const auto widgets = m_widgets.widgets(); !widgets.empty())
			m_widgets.fitContent(widgets.back().bounds.rect.w - body.rect.y);
	}
	else if (buildSessionScreen(screen, column))
	{
		// Session controls use the same focus, clipping and input path.
	}
	else if (screen == Screen::Showcase)
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
		add(0, Label{}, 27.f, m_hostedSession ? "Save and stop the hosted server before exiting." :
			(m_persistentSession ? "Save the world before exiting." : "Close the game window."));
		const auto row = column.next(36.f * scale);
		// R10: destructive actions are visually distinct. Cancel is added first,
		// so it is also the initial focus: a stray Enter can never exit the game.
		m_widgets.add({Return, gridCell(row, 2, 1, 0, 0, 8.f * scale), "Cancel", {}, Button{}});
		m_widgets.add({Confirm, gridCell(row, 2, 1, 1, 0, 8.f * scale), "Exit", {}, Button{},
			true, style::ButtonVariant::Danger});
	}
	else if (screen == Screen::Pause)
	{
		add(0, Label{}, 32.f, "Game menu");
		add(Resume, Button{}, 36.f, "Resume");
		if (m_persistentSession)
			add(Save, Button{}, 32.f, "Save world");
		add(Leave, Button{}, 32.f, m_remoteSession ? "Leave server" :
			(m_persistentSession ? "Save and return to menu" : "Return to menu"));
		add(Multiplayer, Button{}, 32.f, "Multiplayer");
		add(Settings, Button{}, 32.f, "Settings");
		add(Showcase, Button{}, 30.f, "Widget showcase");
		add(Quit, Button{}, 32.f, "Exit to desktop");
		// Design-system previews (issue #302) stay after the game actions until
		// #303-#308 move them into the real screens.
		add(Gallery, Button{}, 36.f, "Design system");
		if (m_previewStrip)
			add(HudMock, Button{}, 36.f, "HUD mock");
		add(ToggleHud, Button{}, 36.f, m_hudVisible ? "Hide HUD" : "Show HUD");
	}
	auto focus = m_screens.focus();
	m_widgets.focus(focus, focus, entered || rebuilt || moved);
	if (entered && !focus)
		if (const auto *initial = m_widgets.step(0, false))
			m_widgets.focus(focus, initial->id, true);
	m_screens.focus(focus);
	m_layoutRevision = m_screens.revision();
}

void GameUiSample::paintScreen(glm::vec2 pointer)
{
	const auto &theme = style::baseline();
	auto paint = surface(m_draws.bounds());
	if (m_screens.suppressDeveloper())
		paint.backdrop(m_draws.bounds(), theme.palette.scrim);
	paint.card(m_panel);
	if (multiplayerScreen(m_screens.top()) && !m_mpStatusMessage.empty())
	{
		auto bodyPaint = surface(m_widgets.viewport().clip);
		bodyPaint.wrappedLabel(m_widgets.viewport().place(m_mpStatusMessage), m_mpStatus,
			style::TextRole::Body, theme.palette.ink, 5);
	}
	if (multiplayerScreen(m_screens.top()) && !m_mpMessage.empty())
	{
		auto bodyPaint = surface(m_widgets.viewport().clip);
		bodyPaint.wrappedLabel(m_widgets.viewport().place(m_mpMessage), m_mpDetail,
			style::TextRole::Body, theme.palette.ink, 5);
	}
	if (m_screens.top() == Screen::SessionError)
		paint.wrappedLabel(m_sessionMessage, m_sessionStatus, style::TextRole::Body, theme.palette.ink, 5);
	if (m_screens.top() == Screen::Loading)
		paint.progress(m_sessionMessage, 0.f, true, m_uiPhase);
	if (m_screens.top() == Screen::Settings)
		paintSettingsNotice();
	m_widgets.paint(m_draws, font(), m_screenScale, m_screens.focus(), pointer, theme, m_atlasLayers,
		m_fontScale);
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
		if (m_screens.top() == Screen::Gallery)
			return applyGalleryActivate(event.id);
		if (activateSettingsControl(event.id) || activateMultiplayerControl(event.id) || activateSessionControl(event.id))
			return InputResult::Consumed;
		switch (event.id)
		{
		case Resume:
		case Return: back(); break;
		case Showcase: m_screens.push(Screen::Showcase); invalidateLayout(); break;
		case Gallery: m_screens.push(Screen::Gallery); invalidateLayout(); break;
		case HudMock: m_screens.push(Screen::HudMock); invalidateLayout(); break;
		case ToggleHud: m_hudVisible = !m_hudVisible; break;
		case Quit: m_screens.push(Screen::ConfirmExit); invalidateLayout(); break;
		case Confirm: return InputResult::Exit;
		default: break;
		}
	}
	if (event.action == WidgetAction::Change)
		changeMultiplayerControl(event.id);
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
	auto *field = interactive && m_screens.nativeScreen() && m_screens.top() != Screen::Inventory &&
		m_layoutRevision == m_screens.revision()
		? m_widgets.editor(m_screens.focus()) : nullptr;
	const auto geometry = field
		? m_widgets.editorGeometry(m_screens.focus(), font(), m_screenScale, m_fontScale)
		: TextGeometry{};
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
	// The input area is the editor's own content rect and line: no hardcoded
	// line height can drift away from the painted caret (issue #302 F01).
	const SDL_Rect area{left, top, std::max(1, int(std::ceil(clip.z)) - left),
		std::max(1, int(std::ceil(std::min(clip.w, geometry.origin.y + geometry.lineHeight))) - top)};
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
	if (m_screens.top() == Screen::Inventory)
		return m_layoutRevision == m_screens.revision() && inventoryControlAt({x, y}) != 0;
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
				(SDL_GetModState() & SDL_KMOD_SHIFT) != 0, m_fontScale, style::baseline());
		break;
	case SDL_EVENT_MOUSE_MOTION:
		result = m_widgets.drag({event.motion.x, event.motion.y}, m_screenScale,
			style::baseline());
		break;
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (event.button.button == SDL_BUTTON_LEFT)
			result = m_widgets.release({event.button.x, event.button.y}, m_screenScale,
				style::baseline());
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

void GameUiSample::setWorldCatalog(WorldCatalog catalog)
{
	m_worldCatalog = std::move(catalog);
	m_worldNames.clear();
	for (const auto &name : m_worldCatalog.names)
		m_worldNames.emplace_back(name);
	m_worldList.items = m_worldNames;
	m_worldList.selection = {};
	invalidateLayout();
}

void GameUiSample::setSessionStatus(std::string message, bool retry, bool force, bool failedQuit)
{
	if (m_sessionStatus == message && m_retrySession == retry && m_forceStop == force &&
		m_failedQuit == failedQuit)
		return;
	m_sessionStatus = std::move(message);
	m_retrySession = retry;
	m_forceStop = force;
	m_failedQuit = failedQuit;
	invalidateLayout();
}

void GameUiSample::setSessionContext(bool remote, bool persistent, bool hosted)
{
	if (remote != m_remoteSession || persistent != m_persistentSession || hosted != m_hostedSession)
		invalidateLayout();
	m_remoteSession = remote;
	m_persistentSession = persistent;
	m_hostedSession = hosted;
}

std::optional<SessionRequest> GameUiSample::takeSessionRequest()
{
	return std::exchange(m_sessionRequest, std::nullopt);
}

bool GameUiSample::holdingForSession() const
{
	return m_screens.top() == Screen::Loading;
}

bool GameUiSample::buildSessionScreen(Screen screen, StackLayout &column)
{
	const auto label = [&](std::string_view title) {
		m_widgets.add({0, column.next(26.f * m_screenScale), title, {}, Label{}});
	};
	const auto button = [&](ControlId id, std::string_view title, bool enabled = true,
		style::ButtonVariant variant = style::ButtonVariant::Secondary) {
		m_widgets.add({id, column.next(34.f * m_screenScale), title, {}, Button{}, enabled, variant});
	};
	switch (screen)
	{
	case Screen::MainMenu:
		label("ft_vox");
		button(Worlds, "Local worlds");
		button(Multiplayer, "Multiplayer");
		button(Settings, "Settings");
		button(Quit, "Exit to desktop");
		break;
	case Screen::Worlds:
		label("Local worlds");
		label(m_worldCatalog.notice.empty() ?
			(m_worldNames.empty() ? "No saved worlds found." : "Choose a saved world.") : m_worldCatalog.notice);
		m_worldList.visibleRows = 4;
		m_widgets.add({WorldList, column.next(112.f * m_screenScale), "Saved worlds", {}, &m_worldList,
			!m_worldNames.empty()});
		button(OpenWorld, "Open selected world", !m_worldNames.empty());
		button(NewWorld, "Create world");
		button(Refresh, "Refresh");
		button(Return, "Back");
		break;
	case Screen::CreateWorld:
		label("Create local world");
		label("Use 1-32 letters, digits, dashes or underscores.");
		m_widgets.add({WorldName, column.next(38.f * m_screenScale), "World name", {}, &m_worldName});
		m_widgets.add({WorldSeed, column.next(38.f * m_screenScale), "Seed (blank for random)", {}, &m_worldSeed});
		button(CreateWorld, "Create and play");
		button(Return, "Back");
		break;
	case Screen::Loading:
		label(m_sessionStatus);
		m_sessionMessage = column.next(12.f * m_screenScale);
		if (m_forceStop)
			button(ForceStop, "Force stop...", true, style::ButtonVariant::Danger);
		break;
	case Screen::SessionError:
		label("Session action failed");
		// The body is painted with wrapping below, above the recovery actions.
		m_sessionMessage = column.next(100.f * m_screenScale);
		button(CancelFailure, m_failedQuit ? "Stay in game" : "Back");
		if (m_retrySession)
			button(Retry, "Retry");
		if (m_failedQuit)
			button(FailedQuit, "Exit after server failure", true, style::ButtonVariant::Danger);
		break;
	case Screen::ConfirmLeave:
		label(m_remoteSession ? "Leave this server?" : "Return to the main menu?");
		label(m_hostedSession ? "The hosted server will keep running." :
			(m_persistentSession ? "The world will be saved before leaving." :
			(m_remoteSession ? "The server will keep running." : "This transient world will be discarded.")));
		button(Return, "Cancel");
		button(ConfirmLeave, "Leave", true, style::ButtonVariant::Danger);
		break;
	case Screen::ConfirmForceStop:
		label("Force stop the hosted server?");
		label("Recent server changes may be lost.");
		button(Return, "Keep waiting");
		button(ConfirmForceStop, "Force stop", true, style::ButtonVariant::Danger);
		break;
	default: return false;
	}
	return true;
}

bool GameUiSample::activateSessionControl(ControlId id)
{
	const auto request = [&](SessionAction action, std::string name = {}, std::string seed = {}) {
		if (!m_sessionRequest)
			m_sessionRequest = SessionRequest{action, std::move(name), std::move(seed)};
	};
	switch (id)
	{
	case Worlds: openScreen(Screen::Worlds); request(SessionAction::List); break;
	case Refresh: request(SessionAction::List); break;
	case NewWorld: m_worldName = {}; m_worldSeed = {}; openScreen(Screen::CreateWorld); break;
	case OpenWorld:
		if (const auto row = static_cast<size_t>(m_worldList.selection.selected); row < m_worldNames.size())
			request(SessionAction::Open, std::string(m_worldNames[row]));
		break;
	case CreateWorld: request(SessionAction::Create, m_worldName.value(), m_worldSeed.value()); break;
	case Save: request(SessionAction::Save); break;
	case Leave: openScreen(Screen::ConfirmLeave); break;
	case ConfirmLeave: request(SessionAction::Leave); break;
	case Multiplayer: openScreen(Screen::Multiplayer); break;
	case Settings: openScreen(Screen::Settings); break;
	case Retry: request(SessionAction::Retry); break;
	case CancelFailure: request(SessionAction::Cancel); break;
	case ForceStop: openScreen(Screen::ConfirmForceStop); break;
	case ConfirmForceStop: request(SessionAction::ForceStop); back(); break;
	case FailedQuit: request(SessionAction::ConfirmFailedQuit); break;
	default: return false;
	}
	return true;
}

void GameUiSample::finishSessionScreen()
{
	while (m_screens.top() == Screen::Loading || m_screens.top() == Screen::SessionError ||
		m_screens.top() == Screen::ConfirmForceStop)
		m_screens.back();
	invalidateLayout();
}
