#include <Engine/Engine.hpp>
#include <Engine/EngineMultiplayer.hpp>
#include <Engine/GameUiSample.hpp>
#include <Engine/InputRouting.hpp>
#include <Engine/UiWorkspace.hpp>

#include <iostream>
#include <string>

bool Engine::setMouseCaptured(bool captured)
{
	if (captured && m_captureBeforeScreen)
		return false;
	const bool actualBefore = SDL_GetWindowRelativeMouseMode(window);
	const bool completed = SDL_SetWindowRelativeMouseMode(window, captured);
	const std::string error = completed ? "" : SDL_GetError();
	const auto result = gameui::captureOutcome(captured, completed, SDL_GetWindowRelativeMouseMode(window));
	mouseCaptured = result.relative;
	m_captureFailed = result.failed;
	if (actualBefore != mouseCaptured || result.failed)
	{
		cancelGameUiPointer();
		cancelDeveloperInput();
		clearGameplayInput();
	}
	if (m_captureFailed)
		std::cerr << "[input] Could not " << (captured ? "enable" : "disable") << " SDL relative mouse mode"
				  << (error.empty() ? "" : ": ") << error << " (actual: "
				  << (mouseCaptured ? "relative" : "absolute") << ")\n";
	SDL_GetRelativeMouseState(nullptr, nullptr);
	return !m_captureFailed;
}

void Engine::cancelDeveloperInput()
{
	m_inputRouter.cancelDeveloper();
	if (imgui)
		imgui->cancelInput();
	if (gameUi)
		gameUi->resetInventoryInteraction();
}

void Engine::cancelGameUiPointer()
{
	m_inputRouter.cancelPointer(gameui::InputOwner::PlayerUi);
	if (m_playerUi)
		m_playerUi->cancelPointerPress();
}

void Engine::clearGameplayInput()
{
	m_inputRouter.cancelGameplay();
	jumpPressed = false;
	if (m_remote)
		m_remote->jumpQueuedAt = {};
	else if (sim)
		sim->clearPlayerInput();
}

bool Engine::nativePause() const
{
	return m_playerUi && !m_uiSpike && m_playerUi->screens().pausesWorld(bool(m_remote));
}

bool Engine::suppressDeveloperUi() const
{
	return m_playerUi && !m_uiSpike && m_playerUi->screens().suppressDeveloper();
}

bool Engine::developerUiPresent() const
{
	// Inspection view, --ui-spike proof and the native player screens remove the
	// developer layer entirely; hiding the workspace does not (issue #303 F01).
	const bool nativeSuppressed = m_inspectionView || m_uiSpike || suppressDeveloperUi();
	return ui::developerUiPresent(imgui != nullptr, nativeSuppressed);
}

bool Engine::developerUiAvailable() const
{
	// Input ownership rule (issue #303 F01/F04): while the workspace is hidden
	// only the legacy inventory is an interactive developer surface; every other
	// pointer or keyboard event belongs to gameplay or the native Game UI.
	const bool nativeSuppressed = m_inspectionView || m_uiSpike || suppressDeveloperUi();
	return ui::developerUiAvailable(imgui != nullptr, nativeSuppressed,
		m_showDeveloperUi ? ui::Workspace::Visible : ui::Workspace::Hidden,
		m_inventoryPanelOpen);
}

gameui::InputContext Engine::inputContext() const
{
	const bool devAvailable = developerUiAvailable();
	const bool pendingDevFocus = devAvailable && imgui->pendingKeyboard();
	return {
		.interactive = windowFocused && !m_windowHidden && !m_windowMinimized,
		.locked = m_benchmark.locksInput(),
		.nativeScreen = m_playerUi && !m_uiSpike && m_playerUi->screens().nativeScreen(),
		.modal = suppressDeveloperUi(),
		.inventory = m_inventoryPanelOpen,
		.paused = paused || nativePause(),
		.devKeyboard = pendingDevFocus || (devAvailable &&
			(mouseCaptured ? imgui->ownsActiveInput() : imgui->wantCaptureKeyboard())),
		.devDialog = devAvailable && (imgui->hasModal() || (gameUi && gameUi->isFileDialogOpen())),
		.devText = pendingDevFocus || (devAvailable && imgui->editingText()),
		.transition = switchingSession() || (m_remote && !m_remote->ready()),
		.captureFailed = m_captureFailed,
	};
}

ImGuiLayer::PointerHit Engine::developerPointerAt(float x, float y) const
{
	using enum ImGuiLayer::PointerHit;
	if (!developerUiAvailable())
		return PassThrough;
	return mouseCaptured ? (imgui->ownsActiveInput() ? Interactive : PassThrough) : imgui->pointerHit(x, y);
}

void Engine::setInventoryOpen(bool open)
{
	m_inventoryPanelOpen = open;
	if (!open && gameUi)
		gameUi->resetInventoryInteraction();
	syncUiInput();
}

void Engine::setDeveloperWorkspaceVisible(bool visible)
{
	if (!ui::allowsWorkspaceToggle(m_benchmark.isActive()))
	{
		// A benchmark owns the composition: the request is consumed (the caller
		// already swallowed the key) but the workspace state does not change, so a
		// measurement can never be perturbed by chrome appearing or disappearing
		// (issue #303 F11).
		return;
	}
	if (m_showDeveloperUi == visible)
		return;
	if (ui::endsDeveloperInput(m_showDeveloperUi ? ui::Workspace::Visible : ui::Workspace::Hidden,
			visible ? ui::Workspace::Visible : ui::Workspace::Hidden))
	{
		// Developer-owned input dies with the workspace: no focused text field, no
		// in-flight pointer press, no open resource-pack browser and no inventory
		// drag cursor left behind (F04). The inventory itself stays open and
		// floats, so closing it is still possible with the workspace hidden (F01).
		cancelDeveloperInput();
		if (gameUi)
			gameUi->cancelDeveloperDialogs();
	}
	m_showDeveloperUi = visible;
	syncUiInput();
}

void Engine::syncUiInput()
{
	if (m_uiSpike)
		return;
	applyPlayerUiScaleRequests();
	if (m_playerUi)
	{
		m_inventoryPanelOpen = m_playerUi->syncInventory(m_inventoryPanelOpen);
		const auto revision = m_playerUi->screens().revision();
		if (m_uiScreenRevision != revision)
		{
			m_uiScreenRevision = revision;
			m_inputRouter.cancelPlayerUi();
			clearGameplayInput();
		}
	}
	const bool wantsCursor = m_inventoryPanelOpen || (m_playerUi && m_playerUi->screens().blocksGameplay());
	if (wantsCursor && !m_captureBeforeScreen)
	{
		m_captureBeforeScreen = mouseCaptured;
		setMouseCaptured(false);
	}
	if (!wantsCursor && m_captureBeforeScreen)
	{
		const bool restore = *m_captureBeforeScreen;
		m_captureBeforeScreen.reset();
		setMouseCaptured(restore);
	}
	const bool suppressed = suppressDeveloperUi();
	if (m_developerSuppressed != suppressed)
	{
		m_developerSuppressed = suppressed;
		cancelDeveloperInput();
	}
	auto policy = inputContext();
	const bool movement = policy.allowsMovement() && (!m_remote || mouseCaptured);
	if (m_movementAllowed && !movement)
		clearGameplayInput();
	m_movementAllowed = movement;
	if (m_playerUi)
		m_playerUi->updateTextInput(window, policy.interactive && !policy.devDialog &&
			!policy.devText);
}

// Return true once the event has been handled. Gameplay keeps its existing
// command paths and receives only the events assigned to it by the router.
bool Engine::routeUiEvent(const SDL_Event &event)
{
	using enum gameui::InputOwner;
	using gameui::InputKind;
	auto policy = inputContext();
	gameui::InputOwner owner = None;
	auto deliver = [&](GameUiSample::InputResult result) {
		if (result == GameUiSample::InputResult::Exit)
			requestExit();
	};
	if (m_uiSpike && m_playerUi)
	{
		const auto result = m_playerUi->handleEvent(window, event, true);
		deliver(result);
		return result != GameUiSample::InputResult::Unhandled;
	}

	const SDL_WindowID windowId = SDL_GetWindowID(window);
	if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST)
	{
		if (event.window.windowID != windowId)
			return true;
		if (m_playerUi)
			m_playerUi->handleEvent(window, event, false);
		switch (event.type)
		{
		case SDL_EVENT_WINDOW_MOUSE_LEAVE:
		case SDL_EVENT_WINDOW_RESIZED:
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
		case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
			cancelGameUiPointer();
			break;
		case SDL_EVENT_WINDOW_MOUSE_ENTER:
		{
			const SDL_MouseButtonFlags buttons = SDL_GetGlobalMouseState(nullptr, nullptr);
			for (unsigned button = 1; button < 32; ++button)
				if ((buttons & SDL_BUTTON_MASK(button)) == 0)
					m_inputRouter.forgetReleasedButton(button);
			break;
		}
		default: break;
		}
		if (imgui)
			imgui->processEvent(event);
		return false;
	}
	switch (event.type)
	{
	case SDL_EVENT_KEY_DOWN:
	case SDL_EVENT_KEY_UP:
	{
		if (event.key.windowID != windowId)
			return true;
		const bool down = event.type == SDL_EVENT_KEY_DOWN;
		const auto key = event.key.key;
		const bool back = key == SDLK_ESCAPE || (key == SDLK_P && !m_remote && m_playerUi &&
			m_playerUi->screens().top() == gameui::Screen::Pause);
		const bool global = classifyKeyRoute(static_cast<int>(key), event.key.mod) == KeyRoute::GlobalShortcut;
		const bool closeInventory = key == SDLK_E && policy.inventory;
		// C is the cursor-release shortcut, and the HUD's multiplayer recovery line
		// depends on it: the policy keeps it with gameplay during a session
		// transition so the captured mouse can be freed before Multiplayer >
		// Reconnect (issue #303 F03). It changes only the host's cursor capture.
		const bool captureShortcut = key == SDLK_C;
		if (back && m_playerUi && !policy.devText && developerUiAvailable())
			policy.devKeyboard = imgui->ownsActiveInput();
		const auto candidate = policy.choose(InputKind::Key, global, back, closeInventory, captureShortcut);
		auto scan = event.key.scancode;
		if (scan == SDL_SCANCODE_UNKNOWN)
			scan = SDL_GetScancodeFromKey(key, nullptr);
		owner = m_inputRouter.key(size_t(scan), down, event.key.repeat, candidate);
		// Dev UI uses aggregate modifiers for docking and pointer shortcuts
		// even when the physical modifier is held for gameplay.
		if (imgui && !m_inspectionView && policy.interactive && !policy.modal && owner != DeveloperUi)
			imgui->observeModifiers(event.key.mod);
		if (owner == PlayerUi && down && !event.key.repeat && (back || closeInventory))
		{
			if (policy.inventory && !policy.modal)
				setInventoryOpen(false);
			else if (m_playerUi)
				m_playerUi->back();
			syncUiInput();
			return true;
		}
		break;
	}
	case SDL_EVENT_MOUSE_MOTION:
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
	case SDL_EVENT_MOUSE_WHEEL:
	{
		const bool motion = event.type == SDL_EVENT_MOUSE_MOTION;
		const bool wheel = event.type == SDL_EVENT_MOUSE_WHEEL;
		const auto id = motion ? event.motion.windowID : (wheel ? event.wheel.windowID : event.button.windowID);
		if (id != windowId)
			return true;
		const float x = motion ? event.motion.x : (wheel ? event.wheel.mouse_x : event.button.x);
		const float y = motion ? event.motion.y : (wheel ? event.wheel.mouse_y : event.button.y);
		const auto devHit = developerPointerAt(x, y);
		policy.devPointer = devHit == ImGuiLayer::PointerHit::Interactive;
		policy.devOccludes = devHit == ImGuiLayer::PointerHit::Occludes;
		const auto press = m_inputRouter.pointerOwner();
		policy.nativePointer = !mouseCaptured && m_playerUi && !wheel &&
			(motion ? press == PlayerUi : event.button.button == SDL_BUTTON_LEFT) && m_playerUi->hitsStrip(x, y);
		const auto candidate = m_captureFailed && mouseCaptured ? Blocked : policy.choose(InputKind::Pointer);
		const bool continuing = motion ||
			(event.type == SDL_EVENT_MOUSE_BUTTON_UP && m_inputRouter.activeButton(event.button.button));
		if (m_playerUi && press == PlayerUi && continuing &&
			(!policy.interactive || policy.locked || policy.devDialog || policy.devPointer || policy.devOccludes))
			cancelGameUiPointer();
		if (press == Gameplay && continuing && candidate != Gameplay)
			m_inputRouter.cancelPointer(Gameplay);
		if (wheel)
			owner = m_inputRouter.wheelOwner(candidate);
		else if (motion)
			owner = m_inputRouter.motionOwner(candidate);
		else
			owner = m_inputRouter.button(event.button.button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN, candidate);
		// Position feeds ImGui hover bookkeeping. Only Dev-owned button and
		// wheel edges reach its controls, even when ownership changes mid-press.
		if (imgui && policy.interactive && !policy.modal && !mouseCaptured &&
			((motion && owner != DeveloperUi) || (!motion && owner == DeveloperUi)))
			imgui->observePointer(x, y);
		if (imgui && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT &&
			((owner == PlayerUi && m_playerUi && (policy.nativeScreen ? m_playerUi->canFocusWidget(x, y) : policy.nativePointer)) ||
			 (owner == Gameplay && !mouseCaptured)))
		{
			m_inputRouter.cancelKeyboard(DeveloperUi);
			imgui->yieldKeyboard();
		}
		if (owner == PlayerUi && m_playerUi && !policy.nativeScreen)
		{
			deliver(m_playerUi->handleEvent(window, event, candidate == PlayerUi));
			return true;
		}
		break;
	}
	case SDL_EVENT_TEXT_INPUT:
	case SDL_EVENT_TEXT_EDITING:
		if ((event.type == SDL_EVENT_TEXT_INPUT ? event.text.windowID : event.edit.windowID) != windowId)
			return true;
		owner = policy.choose(InputKind::Text);
		break;
	default:
		if (imgui && !policy.modal)
			imgui->processEvent(event);
		return false;
	}
	if (owner == PlayerUi && m_playerUi)
	{
		deliver(m_playerUi->handleScreenEvent(event));
		syncUiInput();
	}
	else if (owner == DeveloperUi && imgui && !policy.modal)
	{
		const bool press = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
		const bool takingTextInput = press && m_playerUi && m_playerUi->yieldTextInput(window);
		if (takingTextInput)
			m_inputRouter.cancelKeyboard(PlayerUi);
		imgui->processEvent(event, takingTextInput);
		if (press)
			syncUiInput();
	}
	return owner != Gameplay && owner != Global;
}

// ===========================================================================
// Player UI preferences (issue #302, Epic #298 Phase D)
//
// The scale override is presentation-only state in the settings seam next to
// the other client preferences. It is read once at startup, pushed into the
// player UI as a policy (never applied mid-frame) and written back when a
// player surface raises a request. The developer UI scale, its docking layout,
// the simulation, saves and the wire protocol are untouched.
// ===========================================================================

void Engine::loadPlayerUiPreferences()
{
	m_playerUiDirty = false;
	try
	{
		const auto status = m_playerUiSettings.load(m_savesRoot / ".client-profiles" / "player-ui.txt");
		if (status == gameui::LoadStatus::Corrupt)
		{
			// R09: the defaults are shown, but the record stays on disk untouched
			// until the player makes an explicit choice, which then repairs it.
			m_playerUiSettings = {};
			m_playerUiDirty = true;
			m_playerUiError = "Could not read the saved player interface settings. Defaults apply.";
			std::cerr << "[ui] " << m_playerUiError << '\n';
		}
		else
			m_playerUiError.clear();
	}
	catch (const std::exception &)
	{
		// Same message and log on the exception path: a read failure is never
		// silent, whichever way it surfaced (F17), and it also needs repair (R09).
		m_playerUiSettings = {};
		m_playerUiDirty = true;
		m_playerUiError = "Could not read the saved player interface settings. Defaults apply.";
		std::cerr << "[ui] " << m_playerUiError << '\n';
	}
	if (m_playerUi)
	{
		m_playerUi->setScalePolicy(m_playerUiSettings.scale);
		m_playerUi->setSettingsNotice(m_playerUiError);
	}
}

void Engine::persistPlayerUiPreferences()
{
	bool saved = false;
	try
	{
		saved = m_playerUiSettings.save(m_savesRoot / ".client-profiles" / "player-ui.txt");
	}
	catch (const std::exception &)
	{
		saved = false;
	}
	if (saved)
	{
		m_playerUiError.clear();
		m_playerUiDirty = false;
	}
	else
	{
		// Stay dirty: the next request retries rather than pretending the value
		// reached the disk (F17).
		m_playerUiDirty = true;
		m_playerUiError = "Could not save the player interface settings. The change applies for this run.";
		std::cerr << "[ui] " << m_playerUiError << '\n';
	}
	if (m_playerUi)
		m_playerUi->setSettingsNotice(m_playerUiError);
}

void Engine::applyPlayerUiScaleRequests()
{
	if (!m_playerUi)
		return;
	const auto request = m_playerUi->takeScaleRequest();
	if (!request)
		return;
	m_playerUi->setScalePolicy(*request);
	if (*request != m_playerUiSettings.scale)
	{
		m_playerUiSettings.scale = *request;
		m_playerUiDirty = true;
	}
	// Selecting the same value again is a deliberate retry: a dirty record is
	// written again even though the in-memory value already matches (F17). No
	// frame writes anything when nothing was requested.
	if (m_playerUiDirty)
		persistPlayerUiPreferences();
}
