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
	{
		imgui->cancelInput();
		// F01 (round 2): the release path of a workspace, capture or session
		// change also resets persistent keyboard ownership.
		imgui->releaseKeyboardOwnership();
	}
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

bool Engine::developerUiAvailable() const
{
	const bool nativeSuppressed = m_inspectionView || m_uiSpike || suppressDeveloperUi();
	const bool benchmarkMap = m_benchmark.isActive() && m_benchmark.config().biomeMapZoom > 0.f;
	return ui::developerUiAvailable(imgui && developerUi, nativeSuppressed,
		(m_showDeveloperUi || benchmarkMap) ? ui::Workspace::Visible : ui::Workspace::Hidden);
}

gameui::InputContext Engine::inputContext() const
{
	const bool devAvailable = developerInputReady();
	const bool pendingDevFocus = devAvailable && imgui->pendingKeyboard();
	return {
		.interactive = windowFocused && !m_windowHidden && !m_windowMinimized,
		.locked = m_benchmark.locksInput(),
		.nativeScreen = m_playerUi && !m_uiSpike && m_playerUi->screens().nativeScreen(),
		.modal = suppressDeveloperUi(),
		.inventory = inventoryPanelOpen(),
		.paused = paused || nativePause(),
		.devKeyboard = pendingDevFocus || (devAvailable &&
			(mouseCaptured ? imgui->ownsActiveInput() : imgui->ownsKeyboard())),
		.devDialog = devAvailable && (imgui->hasModal() || (developerUi && developerUi->isFileDialogOpen())),
		.devText = pendingDevFocus || (devAvailable && imgui->editingText()),
		.pointerGeometryPending = m_uiGeometryPending,
		.transition = m_playerSessionRequest.has_value() || switchingSession() || (m_remote && !m_remote->ready()),
		.captureFailed = m_captureFailed,
	};
}

ImGuiLayer::PointerHit Engine::developerPointerAt(float x, float y) const
{
	using enum ImGuiLayer::PointerHit;
	if (!developerInputReady())
		return PassThrough;
	return mouseCaptured ? (imgui->ownsActiveInput() ? Interactive : PassThrough) : imgui->pointerHit(x, y);
}

void Engine::setInventoryOpen(bool open)
{
	if (m_playerUi)
		m_playerUi->setInventoryOpen(open && (sim || m_remote));
	// F01 (round 2): a freshly opened native screen owns the keyboard —
	// opening E clears the Developer UI latch (a real Developer UI owner —
	// an editing field or a popup — re-latches itself next frame).
	if (open && imgui)
		imgui->releaseKeyboardOwnership();
	m_inventoryResult.reset();
	syncUiInput();
}

bool Engine::inventoryPanelOpen() const
{
	return m_playerUi && m_playerUi->screens().top() == gameui::Screen::Inventory;
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
	m_showDeveloperUi = visible;
	syncUiInput();
}

void Engine::invalidateUiGeometry()
{
	m_uiGeometryPending = true;
	cancelGameUiPointer();
	m_inputRouter.cancelPointer(gameui::InputOwner::Gameplay);
	cancelDeveloperInput();
	if (m_playerUi)
	{
		m_playerUi->cancelInventoryGesture();
		m_playerUi->invalidateLayout();
	}
}

void Engine::syncUiInput()
{
	if (m_uiSpike)
		return;
	if (m_playerUi)
	{
		const auto revision = m_playerUi->screens().revision();
		if (m_uiScreenRevision != revision)
		{
			m_uiScreenRevision = revision;
			m_inputRouter.cancelPlayerUi();
			clearGameplayInput();
		}
	}
	const bool wantsCursor = m_playerUi && m_playerUi->screens().blocksGameplay();
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
	const bool composed = developerUiAvailable();
	if (m_developerComposed != composed || m_developerSuppressed != suppressed)
	{
		invalidateUiGeometry();
		if (m_developerComposed && !composed)
		{
			developerUi->cancelDeveloperDialogs();
			developerUi->suspendSampling();
			imgui->suspendPresentation();
		}
		m_developerComposed = composed;
		m_developerSuppressed = suppressed;
	}
	// F01 (#316): Developer UI keyboard ownership (an open editor, an
	// armed widget, a popup or a latched navigation focus) forfeits an
	// armed inventory gesture without waiting for the next event edge.
	// Passive wantCaptureKeyboard — retained by mere hover focus — never
	// latches ownership (round 2), so it cannot cancel a gesture the
	// player is making in the native screen.
	if (m_playerUi && inventoryPanelOpen() && developerInputReady() &&
		imgui && imgui->ownsKeyboard())
		m_playerUi->cancelInventoryGesture();
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
		case SDL_EVENT_WINDOW_FOCUS_LOST:
		case SDL_EVENT_WINDOW_HIDDEN:
		case SDL_EVENT_WINDOW_MINIMIZED:
			if (m_playerUi)
				m_playerUi->cancelInventoryGesture();
			// F01 (round 2): focus loss, hiding and minimization release
			// keyboard ownership — the next focus owner gets a clean slate
			// instead of a stale Developer UI latch.
			if (imgui)
				imgui->releaseKeyboardOwnership();
			break;
		case SDL_EVENT_WINDOW_MOUSE_LEAVE:
			cancelGameUiPointer();
			break;
		case SDL_EVENT_WINDOW_RESIZED:
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
		case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
		case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
			invalidateUiGeometry();
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
		const bool inventoryShortcut = key == SDLK_E;
		// C is the cursor-release shortcut, and the HUD's multiplayer recovery line
		// depends on it: the policy keeps it with gameplay during a session
		// transition so the captured mouse can be freed before Multiplayer >
		// Reconnect (issue #303 F03). It changes only the host's cursor capture.
		const bool captureShortcut = key == SDLK_C;
		// F01 (#316, round 2): no special case here — inputContext()'s
		// persistent ownership model already separates meaningful Developer
		// UI signals (an active widget, a popup, an editing field or a
		// visible navigation highlight) from a passively retained focus,
		// so the contextual keys of an open native screen reach the player
		// UI unless the Developer UI really owns them. InputContext
		// (PlayerUiInteraction) keeps the modal and text-input priorities.
		// The workspace toggle changes the requested visibility even under a
		// native modal. The modal keeps input and suppresses the workspace.
		const auto candidate = policy.interactive && isWorkspaceToggle(key, event.key.mod)
			? Global : policy.choose(InputKind::Key, global, back, inventoryShortcut, captureShortcut);
		auto scan = event.key.scancode;
		if (scan == SDL_SCANCODE_UNKNOWN)
			scan = SDL_GetScancodeFromKey(key, nullptr);
		owner = m_inputRouter.key(size_t(scan), down, event.key.repeat, candidate);
		// Dev UI uses aggregate modifiers for docking and pointer shortcuts
		// even when the physical modifier is held for gameplay.
		if (developerInputReady() && policy.interactive && owner != DeveloperUi)
			imgui->observeModifiers(event.key.mod);
		if (owner == PlayerUi && down && !event.key.repeat && (back || (inventoryShortcut && policy.inventory)))
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
		policy.nativePointer = !policy.pointerGeometryPending && !mouseCaptured && m_playerUi && !wheel &&
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
		if (policy.inventory && policy.devOccludes && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
			m_playerUi->cancelInventoryGesture();
		// Position feeds ImGui hover bookkeeping. Only Dev-owned button and
		// wheel edges reach its controls, even when ownership changes mid-press.
		if (developerInputReady() && policy.interactive && !mouseCaptured &&
			((motion && owner != DeveloperUi) || (!motion && owner == DeveloperUi)))
			imgui->observePointer(x, y);
		if (developerInputReady() && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
			((event.button.button == SDL_BUTTON_LEFT &&
				 ((owner == PlayerUi && m_playerUi &&
					  (policy.nativeScreen ? m_playerUi->canFocusWidget(x, y) : policy.nativePointer)) ||
				  (owner == Gameplay && !mouseCaptured))) ||
			 (event.button.button == SDL_BUTTON_RIGHT && owner == PlayerUi && policy.inventory &&
				 m_playerUi && m_playerUi->canFocusWidget(x, y))))
		{
			m_inputRouter.cancelKeyboard(DeveloperUi);
			imgui->yieldKeyboard();
			// F01 (round 2): the press is a meaningful move toward the
			// native screen — clicking an inventory slot restores native
			// keyboard navigation.
			// F01 (round 3): a right-click on an inventory control hands
			// the keyboard back just the same, so a half-stack gesture can
			// start right after Developer UI interaction. Gameplay and the
			// other native screens keep the left-click-only behavior.
			imgui->releaseKeyboardOwnership();
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
		if (imgui && (developerUiAvailable() || event.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
			event.type == SDL_EVENT_WINDOW_FOCUS_LOST))
			imgui->processEvent(event);
		return false;
	}
	if (owner == PlayerUi && m_playerUi)
	{
		if (policy.inventory)
		{
			m_playerUi->setInventoryView(playerInventoryView());
			const auto action = m_playerUi->handleInventoryEvent(event);
			if (action.command != inventoryui::Command::None)
				submitInventoryAction(action);
		}
		else
			deliver(m_playerUi->handleScreenEvent(event));
		syncUiInput();
	}
	else if (owner == DeveloperUi && imgui && !policy.modal)
	{
		const bool press = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
		// F01 (#316): the Developer UI taking a key while the inventory is
		// open forfeits the armed gesture and the held stack, exactly like a
		// developer press or a dialog claiming the surface.
		if (policy.inventory && (press || (event.type == SDL_EVENT_KEY_DOWN && (policy.devKeyboard || policy.devText)) ||
			policy.devDialog || policy.devText))
			m_playerUi->cancelInventoryGesture();
		if (press)
			imgui->retainKeyboardOwnership();
		const bool takingTextInput = press && m_playerUi && m_playerUi->yieldTextInput(window);
		if (takingTextInput)
			m_inputRouter.cancelKeyboard(PlayerUi);
		imgui->processEvent(event, takingTextInput);
		if (press)
			syncUiInput();
	}
	return owner != Gameplay && owner != Global;
}
