#include <Engine/Engine.hpp>
#include <Engine/GameUiSample.hpp>

#include <cmath>
#include <iostream>
#include <utility>

using namespace gameui;
using namespace std::chrono_literals;

namespace
{
using Clock = std::chrono::steady_clock;
constexpr auto kResizeTimeout = 3s;
constexpr auto kConfirmTimeout = 15s;

WindowSize windowSize(SDL_Window *window)
{
	WindowSize size;
	SDL_GetWindowSize(window, &size.width, &size.height);
	return size;
}

bool canResize(SDL_Window *window)
{
	return (SDL_GetWindowFlags(window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN |
		SDL_WINDOW_MINIMIZED)) == 0;
}
} // namespace

void Engine::loadPlayerUiPreferences()
{
	m_playerUiSettings = {};
	m_playerUiDirty = false;
	m_playerUiError.clear();
	LoadStatus status = LoadStatus::Corrupt;
	bool hasCurrentRecord = false;
	try
	{
		status = m_playerUiSettings.load(m_savesRoot / kPlayerSettingsPath);
		hasCurrentRecord = status == LoadStatus::Ok;
		if (status == LoadStatus::Missing)
			status = loadLegacyPlayerScale(m_savesRoot / kLegacyPlayerSettingsPath, m_playerUiSettings.scale);
	}
	catch (const std::exception &)
	{
		status = LoadStatus::Corrupt;
	}
	if (status == LoadStatus::Corrupt)
	{
		m_playerUiDirty = true;
		m_playerUiError = "Could not read saved player settings. Defaults apply.";
		std::cerr << "[ui] " << m_playerUiError << '\n';
	}
	if (m_playerUi)
		m_playerUi->setScalePolicy(m_playerUiSettings.scale);
	if (!hasCurrentRecord)
		return;

	applyPlayerMouseSensitivity(m_playerUiSettings.sensitivity);
	if (!m_playerFxaaOverride)
		worldRenderer->postSettings().fxaaEnabled = m_playerUiSettings.fxaa;
	if (!m_playerVSyncOverride && !m_pendingVSync)
		requestPlayerVSync(m_playerUiSettings.vsync, false);

	const auto size = m_playerUiSettings.window;
	if (size != windowSize(window) &&
		(!canResize(window) || !SDL_SetWindowSize(window, size.width, size.height) ||
			!SDL_SyncWindow(window) || size != windowSize(window)))
	{
		m_playerSettingsNotice = "The window manager could not restore the saved window size.";
		std::cerr << "[ui] " << m_playerSettingsNotice << '\n';
	}
}

void Engine::persistPlayerUiPreferences()
{
	bool saved = false;
	try
	{
		saved = m_playerUiSettings.save(m_savesRoot / kPlayerSettingsPath);
	}
	catch (const std::exception &)
	{
		saved = false;
	}
	m_playerUiDirty = !saved;
	m_playerUiError = saved ? "" : "Could not save player settings. Changes apply for this run.";
	if (!saved)
		std::cerr << "[ui] " << m_playerUiError << '\n';
}

void Engine::applyPlayerMouseSensitivity(int percent)
{
	camera.setMouseSensitivity(cameraSensitivity(percent));
	if (m_suspendedSolo)
		m_suspendedSolo->camera.setMouseSensitivity(cameraSensitivity(percent));
}

void Engine::requestPlayerVSync(bool enabled, bool persist)
{
	if (m_playerVSyncOverride)
		return;
	setVSync(enabled);
	m_playerVSyncRequest = persist;
	if (!m_pendingVSync)
		finishPlayerVSync(true);
}

void Engine::finishPlayerVSync(bool applied)
{
	const auto save = std::exchange(m_playerVSyncRequest, std::nullopt);
	if (!applied)
	{
		if (!m_playerSettingsNotice.empty())
			m_playerSettingsNotice += ' ';
		m_playerSettingsNotice += "Could not change VSync on this display. The previous mode is still active.";
		return;
	}
	if (save.value_or(false))
	{
		m_playerUiSettings.vsync = swapchain->isVSync();
		persistPlayerUiPreferences();
	}
}

void Engine::beginPlayerDisplayPreview(std::optional<WindowSize> size, std::optional<ScalePolicy> scale,
	bool reset)
{
	const auto actual = windowSize(window);
	if (size && !canResize(window))
	{
		m_playerSettingsNotice = "Restore the window before previewing a new size.";
		return;
	}
	const auto previousScale = m_playerUi->scalePolicy();
	m_playerDisplayPreview = DisplayPreview{
		.view = {DisplayPhase::Applying, size, scale}, .previousWindow = actual,
		.previousScale = previousScale, .deadline = Clock::now() + kResizeTimeout, .restoreCheck = {},
		.reset = reset, .previousVSync = swapchain->isVSync()};
	if (size && *size != actual && !SDL_SetWindowSize(window, size->width, size->height))
	{
		revertPlayerDisplayPreview("The window manager refused this size. Try a different size.");
		return;
	}
	if (scale)
		m_playerUi->setScalePolicy(*scale);
}

void Engine::revertPlayerDisplayPreview(std::string_view notice)
{
	auto &preview = *m_playerDisplayPreview;
	if (preview.view.phase == DisplayPhase::Restoring)
	{
		if (!notice.empty())
			m_playerSettingsNotice = notice;
		return;
	}
	m_playerSettingsNotice = notice;
	if (preview.view.scale)
	{
		m_playerUi->setScalePolicy(preview.previousScale);
		preview.view.scale = preview.previousScale;
	}
	if (preview.view.window)
	{
		// Resend the old size after an in-flight resize, even if SDL still
		// reports that size. A hidden platform window may defer the request.
		preview.restoreWindowPending = preview.view.phase == DisplayPhase::Applying ||
			windowSize(window) != preview.previousWindow;
		preview.view.window = preview.previousWindow;
	}
	preview.view.phase = DisplayPhase::Restoring;
}

void Engine::resetPlayerSettingsDefaults(bool resetWindow)
{
	const PlayerUiSettings defaults;
	m_playerUiSettings.scale = defaults.scale;
	if (resetWindow)
		m_playerUiSettings.window = defaults.window;
	else
		m_playerSettingsNotice = "Window size was kept. Restore the window to reset its size.";
	m_playerUiSettings.invertY = defaults.invertY;
	m_playerUiSettings.sensitivity = defaults.sensitivity;
	applyPlayerMouseSensitivity(defaults.sensitivity);
	if (!m_playerFxaaOverride)
		worldRenderer->postSettings().fxaaEnabled = m_playerUiSettings.fxaa = defaults.fxaa;
	if (!m_playerVSyncOverride && swapchain->isVSync() == defaults.vsync)
		m_playerUiSettings.vsync = defaults.vsync;
	if (m_playerVSyncOverride || m_playerFxaaOverride)
	{
		if (!m_playerSettingsNotice.empty())
			m_playerSettingsNotice += ' ';
		m_playerSettingsNotice += m_playerVSyncOverride && m_playerFxaaOverride ?
			"VSync and FXAA were kept because of command-line overrides." :
			m_playerVSyncOverride ? "VSync was kept because of --vsync." : "FXAA was kept because of --quality.";
	}
	persistPlayerUiPreferences();
	if (!m_playerVSyncOverride && swapchain->isVSync() != defaults.vsync)
		requestPlayerVSync(defaults.vsync, true);
}

void Engine::handlePlayerSettingsRequest(const SettingsRequest &request)
{
	if (m_playerDisplayPreview)
	{
		const auto *action = std::get_if<SettingsAction>(&request);
		if (action && *action == SettingsAction::Revert)
		{
			revertPlayerDisplayPreview();
			return;
		}
		if (!action || *action != SettingsAction::Keep ||
			m_playerDisplayPreview->view.phase != DisplayPhase::Confirming ||
			!playerDisplayPreviewMatches(playerFramebufferSize()))
			return;
		auto preview = *std::exchange(m_playerDisplayPreview, std::nullopt);
		if (preview.reset)
			resetPlayerSettingsDefaults(canResize(window) && windowSize(window) == kDefaultWindowSize);
		else
		{
			if (preview.view.window)
				m_playerUiSettings.window = *preview.view.window;
			if (preview.view.scale)
				m_playerUiSettings.scale = *preview.view.scale;
			persistPlayerUiPreferences();
		}
		return;
	}

	const auto *action = std::get_if<SettingsAction>(&request);
	if (action && (*action == SettingsAction::Keep || *action == SettingsAction::Revert))
		return;
	m_playerSettingsNotice.clear();
	if (const auto *display = std::get_if<PreviewDisplay>(&request))
	{
		const auto &size = display->window;
		const auto &scale = display->scale;
		const bool validSize = !size || std::ranges::find(kWindowSizes, *size) != kWindowSizes.end();
		const bool validScale = !scale || scale->mode == ScaleMode::Auto ||
			(scale->mode == ScaleMode::Fixed && isUserScale(scale->value));
		if ((size || scale) && validSize && validScale)
			beginPlayerDisplayPreview(size, scale);
	}
	else if (const auto *vsync = std::get_if<SetVSync>(&request))
	{
		if (!m_pendingVSync)
			requestPlayerVSync(vsync->enabled, true);
	}
	else if (const auto *fxaa = std::get_if<SetFxaa>(&request))
	{
		if (!m_playerFxaaOverride)
		{
			worldRenderer->postSettings().fxaaEnabled = m_playerUiSettings.fxaa = fxaa->enabled;
			persistPlayerUiPreferences();
		}
	}
	else if (const auto *sensitivity = std::get_if<SetSensitivity>(&request))
	{
		if (std::ranges::find(kSensitivitySteps, sensitivity->percent) != kSensitivitySteps.end())
		{
			applyPlayerMouseSensitivity(sensitivity->percent);
			m_playerUiSettings.sensitivity = sensitivity->percent;
			persistPlayerUiPreferences();
		}
	}
	else if (const auto *invert = std::get_if<SetInvertY>(&request))
	{
		m_playerUiSettings.invertY = invert->enabled;
		persistPlayerUiPreferences();
	}
	else if (action)
	{
		switch (*action)
		{
		case SettingsAction::Reset:
		{
			if (m_pendingVSync)
				break;
			const auto framebuffer = playerFramebufferSize();
			const auto extent = swapchain->getExtent();
			const bool resizable = canResize(window) && framebuffer.width > 0 && framebuffer.height > 0;
			const bool defaultWindowApplied = resizable && windowSize(window) == kDefaultWindowSize &&
				extent.width == static_cast<uint32_t>(framebuffer.width) &&
				extent.height == static_cast<uint32_t>(framebuffer.height);
			const auto size = resizable && !defaultWindowApplied ?
				std::optional{kDefaultWindowSize} : std::nullopt;
			const auto scale = m_playerUi->scalePolicy().mode == ScaleMode::Fixed ?
				std::optional{ScalePolicy{}} : std::nullopt;
			if (size || scale)
				beginPlayerDisplayPreview(size, scale, true);
			else
				resetPlayerSettingsDefaults(defaultWindowApplied);
			break;
		}
		case SettingsAction::RetrySave:
			if (m_playerUiDirty)
				persistPlayerUiPreferences();
			break;
		case SettingsAction::Revert:
		case SettingsAction::Keep: break;
		}
	}
}

void Engine::pumpPlayerSettings()
{
	if (!m_playerUi || m_uiSpike)
		return;
	const bool settingsOpen = m_playerUi->screens().top() == Screen::Settings && !m_benchmark.isActive();
	if (m_playerDisplayPreview)
	{
		const auto &preview = *m_playerDisplayPreview;
		const auto now = Clock::now();
		if (preview.view.phase != DisplayPhase::Restoring && !settingsOpen)
			revertPlayerDisplayPreview();
		else if (preview.view.phase != DisplayPhase::Restoring && now >= preview.deadline)
			revertPlayerDisplayPreview(preview.view.phase == DisplayPhase::Applying ?
				"The display did not apply the requested settings." : "Display preview expired.");
		else if (preview.view.phase == DisplayPhase::Confirming && preview.view.window &&
			playerFramebufferSize().width > 0 && windowSize(window) != *preview.view.window)
			revertPlayerDisplayPreview("The window size changed during the preview.");
	}
	const auto request = m_playerUi->takeSettingsRequest();
	if (settingsOpen && request)
		handlePlayerSettingsRequest(*request);

	if (!m_playerDisplayPreview || m_playerDisplayPreview->view.phase != DisplayPhase::Restoring)
		return;
	auto &preview = *m_playerDisplayPreview;
	const auto framebuffer = playerFramebufferSize();
	if (framebuffer.width <= 0 || framebuffer.height <= 0)
	{
		preview.restoreCheck.reset();
		return;
	}
	const auto now = Clock::now();
	if (preview.restoreCheck)
		preview.restoreElapsed += now - *preview.restoreCheck;
	preview.restoreCheck = now;
	if (std::exchange(preview.restoreWindowPending, false) &&
		!SDL_SetWindowSize(window, preview.previousWindow.width, preview.previousWindow.height))
		std::cerr << "[ui] SDL refused a display rollback request: " << SDL_GetError() << '\n';
}

WindowSize Engine::playerFramebufferSize() const
{
	if (m_windowHidden || m_windowMinimized ||
		(SDL_GetWindowFlags(window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)))
		return {};
	WindowSize framebuffer;
	SDL_GetWindowSizeInPixels(window, &framebuffer.width, &framebuffer.height);
	return framebuffer;
}

bool Engine::playerDisplayPreviewMatches(WindowSize framebuffer) const
{
	const auto &preview = *m_playerDisplayPreview;
	const auto extent = swapchain->getExtent();
	return framebuffer.width > 0 && framebuffer.height > 0 && swapchain->getSwapchain() &&
		extent.width == static_cast<uint32_t>(framebuffer.width) &&
		extent.height == static_cast<uint32_t>(framebuffer.height) &&
		(!preview.view.window || windowSize(window) == *preview.view.window) &&
		m_playerUi->scaleAppliedForWindow(window, preview.view.scale.value_or(preview.previousScale)) &&
		(!preview.recoveringSwapchain || swapchain->isVSync() == preview.previousVSync);
}

void Engine::updatePlayerDisplayPreview(WindowSize framebuffer, bool swapchainReady)
{
	if (m_playerDisplayPreview)
	{
		auto &preview = *m_playerDisplayPreview;
		const bool ready = swapchainReady && playerDisplayPreviewMatches(framebuffer);
		if (ready && preview.view.phase == DisplayPhase::Applying)
		{
			preview.view.phase = DisplayPhase::Confirming;
			preview.deadline = Clock::now() + kConfirmTimeout;
		}
		else if (ready && preview.view.phase == DisplayPhase::Restoring)
		{
			if (!m_playerSettingsNotice.empty())
				m_playerSettingsNotice += " Previous display settings restored.";
			m_playerDisplayPreview.reset();
		}
		else if (preview.view.phase == DisplayPhase::Restoring && preview.restoreCheck &&
			preview.restoreElapsed >= kResizeTimeout)
		{
			if (!swapchainReady)
				throw std::runtime_error("Could not recreate a usable Vulkan swapchain after rolling back display settings");
			m_playerSettingsNotice = "Could not verify the previous display settings. Check the window size and try again.";
			m_playerDisplayPreview.reset();
		}
	}
	publishPlayerSettingsView();
}

void Engine::publishPlayerSettingsView()
{
	if (!m_playerUi || m_playerUi->screens().top() != Screen::Settings)
		return;
	SettingsView view;
	view.window = windowSize(window);
	view.scale = m_playerUiSettings.scale;
	view.canResize = canResize(window);
	view.vsync = swapchain->isVSync();
	view.pendingVSync = m_pendingVSync;
	view.vsyncOverridden = m_playerVSyncOverride;
	view.fxaa = worldRenderer->postSettings().fxaaEnabled;
	view.fxaaOverridden = m_playerFxaaOverride;
	view.sensitivity = static_cast<int>(std::lround(camera.getMouseSensitivity() * 100.f / kDefaultCameraSensitivity));
	view.invertY = m_playerUiSettings.invertY;
	view.saveFailed = m_playerUiDirty;
	view.notice = m_playerSettingsNotice;
	if (!m_playerUiError.empty())
	{
		if (!view.notice.empty())
			view.notice += ' ';
		view.notice += m_playerUiError;
	}
	if (m_playerDisplayPreview)
	{
		view.display = m_playerDisplayPreview->view;
		view.display->seconds = static_cast<int>(std::max(0s,
			std::chrono::ceil<std::chrono::seconds>(m_playerDisplayPreview->deadline - Clock::now())).count());
	}
	m_playerUi->setSettingsView(std::move(view));
}
