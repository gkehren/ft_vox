#include <Engine/GameUiSample.hpp>
#include <Engine/UiShortcuts.hpp>

#include <utility>

using namespace gameui;

namespace
{
enum SettingsControl : ControlId
{
	Resize = 200, Scale, Preview, VSync, Fxaa, Sensitivity, Invert, Controls, RestoreDefaults,
	Keep, Revert, RetrySave, Back, Bindings
};

std::string sizeLabel(WindowSize size)
{
	if (size.width <= 0 || size.height <= 0)
		return "Unavailable";
	return std::to_string(size.width) + " x " + std::to_string(size.height);
}

std::string scaleLabel(ScalePolicy policy)
{
	return policy.mode == ScaleMode::Auto ? "Auto" :
		std::to_string(static_cast<int>(policy.value * 100.f)) + "%";
}

ScalePolicy nextScale(ScalePolicy current)
{
	if (current.mode == ScaleMode::Auto)
		return {ScaleMode::Fixed, kUserScales[0]};
	for (float scale : kUserScales)
		if (scale > current.value)
			return {ScaleMode::Fixed, scale};
	return {};
}

WindowSize nextSize(WindowSize current)
{
	for (const auto size : kWindowSizes)
		if (size.width > current.width || (size.width == current.width && size.height > current.height))
			return size;
	return kWindowSizes.front();
}

int nextSensitivity(int current)
{
	const auto next = std::ranges::upper_bound(kSensitivitySteps, current);
	return next != kSensitivitySteps.end() ? *next : kSensitivitySteps.front();
}

std::string onOff(bool enabled)
{
	return enabled ? "On" : "Off";
}
} // namespace

void GameUiSample::setSettingsView(SettingsView view)
{
	const auto phase = [](const SettingsView &v) -> std::optional<DisplayPhase> {
		return v.display ? std::optional{v.display->phase} : std::nullopt;
	};
	if (m_screens.top() == Screen::Settings && phase(m_settingsView) != phase(view))
	{
		// Put the safe choice in view after a shrink or a scale increase.
		m_screens.focus(view.display ? ControlId{Revert} : ControlId{Scale});
		if (!view.display)
			m_displayChoice = {};
		invalidateLayout();
	}
	m_settingsView = std::move(view);
}

std::optional<SettingsRequest> GameUiSample::takeSettingsRequest()
{
	return std::exchange(m_settingsRequest, std::nullopt);
}

void GameUiSample::buildSettingsScreen(Screen screen, StackLayout &column)
{
	m_settingsLabels.clear();
	m_settingsMessage = {};
	const auto add = [&](ControlId id, float height, std::string text, Control control,
		bool enabled = true, std::string_view tooltip = {}) {
		m_settingsLabels.push_back(std::move(text));
		m_widgets.add({id, column.next(height * m_screenScale), m_settingsLabels.back(),
			tooltip, control, enabled});
	};
	const auto label = [&](std::string text) { add(0, 22.f, std::move(text), Label{}); };
	const auto button = [&](ControlId id, std::string text, bool enabled = true, std::string_view tooltip = {}) {
		add(id, 34.f, std::move(text), Button{}, enabled, tooltip);
	};

	if (screen == Screen::Controls)
	{
		label("Controls");
		button(Back, "Back to settings");
		m_controlsRows = {"WASD  Move", "Mouse  Look", "Space  Jump or swim up",
			"Shift  Sprint or swim down", "Left / right click  Break / place block",
			"Mouse wheel  Cycle hotbar"};
		for (const auto key : {SDLK_1, SDLK_Q, SDLK_E, SDLK_C})
			if (const auto *ref = ui::findShortcut(key))
			{
				m_settingsLabels.emplace_back(std::string(ref->key) + "  " + ref->action);
				m_controlsRows.push_back(m_settingsLabels.back());
			}
		m_controlsRows.insert(m_controlsRows.end(), {"Esc  Pause or go back", "Tab / Shift+Tab  Move focus",
			"Enter / Space  Activate focused control", "Ctrl+Shift+D  Toggle developer workspace",
			"F1-F12  Developer tools and shortcuts", "F10  Toggle VSync for this run",
			"V  Debug flight (solo)"});
		const auto &theme = style::baseline();
		const float listHeader = style::lineHeight(style::TextRole::Caption) + theme.space.xxs;
		const float available = size(m_widgets.viewport().clip).y / m_screenScale - 68.f - listHeader;
		m_controlsList.visibleRows = std::clamp(static_cast<int>(available / theme.size.rowHeight), 1, 13);
		m_controlsList.items = m_controlsRows;
		m_controlsList.selection.reconcile(
			static_cast<int>(m_controlsRows.size()), m_controlsList.visibleRows);
		add(Bindings, listHeader + theme.size.rowHeight * m_controlsList.visibleRows,
			"Keyboard and mouse", &m_controlsList, true, "Use Up and Down to browse controls.");
		return;
	}

	const auto &view = m_settingsView;
	label("Settings");
	if (view.display)
	{
		const auto &preview = *view.display;
		if (preview.phase == DisplayPhase::Confirming)
			label(std::string(preview.canKeep ? "Keep these changes? Reverting in " : "Checking display... Reverting in ") +
				std::to_string(preview.seconds) + " s");
		else
			label(preview.phase == DisplayPhase::Applying ? "Applying display preview..." : "Restoring display...");
		if (preview.window)
		{
			label("Requested window  " + sizeLabel(*preview.window));
			label("Current window  " + sizeLabel(view.window));
		}
		if (preview.scale)
		{
			label("Requested UI scale  " + scaleLabel(*preview.scale));
			label("Current UI scale  " + std::to_string(static_cast<int>(playerUiScale() * 100.f)) + "%");
		}
		if (preview.phase == DisplayPhase::Confirming)
			button(Keep, "Keep changes", preview.canKeep);
		if (preview.phase != DisplayPhase::Restoring)
			button(Revert, "Revert changes");
		return;
	}

	button(Resize, "Window size  " + sizeLabel(m_displayChoice.window.value_or(view.window)) +
		(m_displayChoice.window && *m_displayChoice.window != view.window ?
			"  |  Current " + sizeLabel(view.window) : ""), view.canResize,
		"Cycle window sizes. Restore a maximized window before resizing.");
	button(Scale, "UI scale  " + scaleLabel(m_displayChoice.scale.value_or(view.scale)) +
		"  |  Current " + std::to_string(static_cast<int>(playerUiScale() * 100.f)) + "%",
		true, "Cycle between Auto and the available fixed scales.");
	button(Preview, "Preview display changes",
		(view.canResize && m_displayChoice.window && *m_displayChoice.window != view.window) ||
		(m_displayChoice.scale && *m_displayChoice.scale != view.scale));
	button(VSync, "VSync  " + onOff(view.vsync) +
		(view.pendingVSync ? "  |  Applying " + onOff(*view.pendingVSync) :
			view.vsyncOverridden ? "  |  Command-line override" : ""),
		!view.vsyncOverridden && !view.pendingVSync,
		"Synchronize presentation to the display. Off requires immediate presentation.");
	button(Fxaa, "Anti-aliasing (FXAA)  " + onOff(view.fxaa) +
		(view.fxaaOverridden ? "  |  Command-line override" : ""), !view.fxaaOverridden,
		"Smooth jagged edges in the rendered scene.");
	button(Sensitivity, "Mouse sensitivity  " + std::to_string(view.sensitivity) +
		"%  |  Set " + std::to_string(nextSensitivity(view.sensitivity)) + "%");
	button(Invert, "Invert vertical mouse look  " + onOff(view.invertY));
	if (!view.notice.empty())
		m_settingsMessage = column.next(45.f * m_screenScale);
	if (view.saveFailed)
		button(RetrySave, "Retry saving preferences");
	button(Controls, "Controls reference");
	button(RestoreDefaults, "Reset to defaults", !view.pendingVSync);
	button(Back, "Back");
}

void GameUiSample::paintSettingsNotice()
{
	if (!m_settingsMessage.empty())
		surface(m_widgets.viewport().clip).wrappedLabel(m_widgets.viewport().place(m_settingsMessage),
			m_settingsView.notice, style::TextRole::Body, style::baseline().palette.ink, 3);
}

bool GameUiSample::activateSettingsControl(ControlId id)
{
	if (m_screens.top() == Screen::Settings && m_settingsRequest)
		return true;
	switch (id)
	{
	case Resize: m_displayChoice.window = nextSize(m_displayChoice.window.value_or(m_settingsView.window)); break;
	case Scale: m_displayChoice.scale = nextScale(m_displayChoice.scale.value_or(m_settingsView.scale)); break;
	case Preview:
	{
		auto request = m_displayChoice;
		if (!m_settingsView.canResize || request.window == m_settingsView.window)
			request.window.reset();
		if (request.scale == m_settingsView.scale)
			request.scale.reset();
		m_settingsRequest = request;
		break;
	}
	case VSync: m_settingsRequest = SetVSync{!m_settingsView.vsync}; break;
	case Fxaa: m_settingsRequest = SetFxaa{!m_settingsView.fxaa}; break;
	case Sensitivity: m_settingsRequest = SetSensitivity{nextSensitivity(m_settingsView.sensitivity)}; break;
	case Invert: m_settingsRequest = SetInvertY{!m_settingsView.invertY}; break;
	case Controls: openScreen(Screen::Controls); break;
	case RestoreDefaults: m_settingsRequest = SettingsAction::Reset; break;
	case Keep: m_settingsRequest = SettingsAction::Keep; break;
	case Revert: m_settingsRequest = SettingsAction::Revert; break;
	case RetrySave: m_settingsRequest = SettingsAction::RetrySave; break;
	case Back: back(); break;
	default: return false;
	}
	return true;
}
