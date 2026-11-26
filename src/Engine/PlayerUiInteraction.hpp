#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace gameui
{
enum class Screen { Hud, Inventory, Pause, Showcase, ConfirmExit, MainMenu, Loading, Gallery, HudMock, Worlds, CreateWorld, SessionError, ConfirmLeave, Multiplayer, Settings, ConfirmForceStop };
using ControlId = uint16_t;

class ScreenStack
{
public:
	Screen top() const { return m_entries[m_size - 1].screen; }
	ControlId focus() const { return m_entries[m_size - 1].focus; }
	void focus(ControlId id) { m_entries[m_size - 1].focus = id; }
	bool contains(Screen screen) const
	{
		return std::any_of(m_entries.begin(), m_entries.begin() + m_size,
			[screen](const Entry &entry) { return entry.screen == screen; });
	}
	bool push(Screen screen)
	{
		if (m_size == m_entries.size() || contains(screen))
			return false;
		m_entries[m_size++] = {screen, 0};
		++m_revision;
		return true;
	}
	bool back()
	{
		if (m_size == 1)
			return false;
		--m_size;
		++m_revision;
		return true;
	}
	void reset(Screen root = Screen::Hud)
	{
		m_size = 1;
		m_entries[0] = {root, 0};
		++m_revision;
	}
	uint64_t revision() const { return m_revision; }
	bool suppressDeveloper() const
	{
		switch (top())
		{
		case Screen::MainMenu: case Screen::Worlds: case Screen::CreateWorld:
		case Screen::Loading: case Screen::SessionError: case Screen::ConfirmLeave:
		case Screen::ConfirmExit: case Screen::ConfirmForceStop:
			return true;
		default: return false;
		}
	}
	bool nativeScreen() const { return top() != Screen::Hud; }
	bool blocksGameplay() const { return top() != Screen::Hud; }
	bool pausesWorld(bool connected) const { return !connected && (contains(Screen::Pause) || contains(Screen::Loading) || contains(Screen::SessionError)); }

private:
	struct Entry { Screen screen{}; ControlId focus{}; };
	std::array<Entry, 6> m_entries{};
	size_t m_size{1};
	uint64_t m_revision{};
};

enum class InputOwner : uint8_t { None, Blocked, Global, PlayerUi, DeveloperUi, Gameplay };
enum class InputKind { Key, Pointer, Text };

struct CaptureOutcome
{
	bool relative{};
	bool failed{};
};

constexpr CaptureOutcome captureOutcome(bool requested, bool completed, bool actual)
{
	return {actual, !completed || actual != requested};
}

struct InputContext
{
	bool interactive{true};
	bool locked{};
	bool nativeScreen{};
	bool modal{};
	bool inventory{};
	bool paused{};
	bool devKeyboard{};
	bool devPointer{};
	bool devOccludes{};
	bool devDialog{};
	bool devText{};
	bool nativePointer{};
	bool transition{};
	bool captureFailed{};

	InputOwner choose(InputKind kind, bool global = false, bool back = false, bool inventoryShortcut = false,
					  bool captureShortcut = false) const
	{
		if (!interactive)
			return InputOwner::Blocked;
		if (global && !modal)
			return InputOwner::Global;
		if (locked)
			return InputOwner::Blocked;
		if (modal)
			return InputOwner::PlayerUi;
		if (devDialog)
			return InputOwner::DeveloperUi;
		if (kind == InputKind::Key)
		{
			// Explicit Developer UI keyboard ownership (PR #316 F01): an active
			// developer control, keyboard navigation or a non-modal popup owns
			// every key while it holds the keyboard — including back keys and
			// the keys of an open native screen, whose navigation would
			// otherwise fire inventory operations through a shared keyboard.
			// The host neutralizes only passive ImGui focus before entering
			// the policy (routeUiEvent) and transfers ownership back when the
			// player clicks the native surface. Dev text (an editing field or a
			// queued activation of one) keeps its historical first priority.
			if (devText || devKeyboard)
				return InputOwner::DeveloperUi;
			if (back && (nativeScreen || inventory))
				return InputOwner::PlayerUi;
		}
		if (kind == InputKind::Pointer)
		{
			if (devOccludes)
				return InputOwner::Blocked;
			if (devPointer)
				return InputOwner::DeveloperUi;
			if (nativePointer)
				return InputOwner::PlayerUi;
		}
		else if (devText)
			return InputOwner::DeveloperUi;
		if (nativeScreen)
			return InputOwner::PlayerUi;
		if (inventory && inventoryShortcut)
			return InputOwner::PlayerUi;
		if (kind != InputKind::Pointer && devKeyboard)
			return InputOwner::DeveloperUi;
		// The host can open Pause while joining or disconnected, and C must stay
		// reachable to recover from a connection failure. E can open the
		// inventory to show the current session state while commands are disabled.
		if (transition && !inventory && kind == InputKind::Key && (back || captureShortcut || inventoryShortcut))
			return InputOwner::Gameplay;
		if (inventory || transition || kind == InputKind::Text || (captureFailed && !back && !captureShortcut))
			return InputOwner::Blocked;
		return InputOwner::Gameplay;
	}
	bool allowsMovement() const
	{
		return interactive && !locked && !nativeScreen && !inventory && !paused &&
			!devKeyboard && !devDialog && !transition && !captureFailed;
	}
};

// SDL scancodes and mouse button numbers are translated by Engine. The
// destination of a release is fixed at press time, including a cancelled press.
class InputRouter
{
public:
	static constexpr size_t kKeys = 512;
	InputOwner key(size_t code, bool down, bool repeat, InputOwner candidate)
	{
		if (code == 0 || code >= m_keys.size())
			return down && !repeat ? candidate : InputOwner::Blocked;
		auto &owner = m_keys[code];
		if (!down)
			return release(owner);
		if (repeat)
			return owner == candidate && (owner == InputOwner::PlayerUi || owner == InputOwner::DeveloperUi)
				? owner : InputOwner::Blocked;
		return press(owner, candidate);
	}
	InputOwner button(size_t button, bool down, InputOwner candidate)
	{
		if (button == 0 || button >= m_buttons.size())
			return InputOwner::Blocked;
		auto &owner = m_buttons[button];
		if (!down)
		{
			const auto previous = release(owner);
			updateGesture();
			return previous;
		}
		// The first live press claims the pointer. Further buttons may join
		// that surface only. A rejected button stays quarantined until up.
		if (!isPointerOwner(candidate) || (m_gesture != InputOwner::None && candidate != m_gesture))
			candidate = InputOwner::Blocked;
		const auto accepted = press(owner, candidate);
		if (isPointerOwner(accepted))
			m_gesture = accepted;
		return accepted;
	}
	InputOwner pointerOwner() const { return m_gesture; }
	bool activeButton(size_t button) const
	{
		return button < m_buttons.size() && isPointerOwner(m_buttons[button]);
	}
	InputOwner motionOwner(InputOwner candidate) const
	{
		return m_gesture == InputOwner::None ? candidate : m_gesture;
	}
	InputOwner wheelOwner(InputOwner candidate) const
	{
		return m_gesture == InputOwner::None || m_gesture == candidate ? candidate : InputOwner::Blocked;
	}
	void cancelPointer(InputOwner owner)
	{
		if (!isPointerOwner(owner))
			return;
		for (auto &button : m_buttons)
			if (button == owner)
				button = InputOwner::Blocked;
		updateGesture();
	}
	bool gameplayKey(size_t code) const { return code < m_keys.size() && m_keys[code] == InputOwner::Gameplay; }
	void cancelGameplay() { cancel(InputOwner::Gameplay); }
	void cancelDeveloper() { cancel(InputOwner::DeveloperUi); }
	void cancelPlayerUi() { cancel(InputOwner::PlayerUi); }
	void cancelKeyboard(InputOwner owner)
	{
		for (auto &key : m_keys)
			if (key == owner)
				key = InputOwner::Blocked;
	}
	void cancelAll()
	{
		cancelGameplay();
		cancelDeveloper();
		cancelPlayerUi();
	}
	// Some window systems omit releases while unfocused. Reconcile against
	// the physical state before accepting new actions on focus regain.
	void reconcileKey(size_t code, bool held)
	{
		if (code && code < m_keys.size())
			m_keys[code] = held ? InputOwner::Blocked : InputOwner::None;
	}
	void reconcileButton(size_t button, bool held)
	{
		if (button && button < m_buttons.size())
		{
			m_buttons[button] = held ? InputOwner::Blocked : InputOwner::None;
			updateGesture();
		}
	}
	void forgetReleasedButton(size_t button)
	{
		if (button && button < m_buttons.size() && m_buttons[button] == InputOwner::Blocked)
			m_buttons[button] = InputOwner::None;
	}

private:
	static bool isPointerOwner(InputOwner owner)
	{
		return owner == InputOwner::PlayerUi || owner == InputOwner::DeveloperUi || owner == InputOwner::Gameplay;
	}
	void updateGesture()
	{
		if (m_gesture != InputOwner::None && std::find(m_buttons.begin(), m_buttons.end(), m_gesture) == m_buttons.end())
			m_gesture = InputOwner::None;
	}
	static InputOwner press(InputOwner &owner, InputOwner candidate)
	{
		if (owner != InputOwner::None)
			return InputOwner::Blocked;
		return owner = candidate;
	}
	static InputOwner release(InputOwner &owner)
	{
		const auto previous = owner;
		owner = InputOwner::None;
		return previous == InputOwner::None ? InputOwner::Blocked : previous;
	}
	void cancel(InputOwner owner)
	{
		cancelKeyboard(owner);
		cancelPointer(owner);
	}
	std::array<InputOwner, kKeys> m_keys{};
	std::array<InputOwner, 32> m_buttons{};
	InputOwner m_gesture{InputOwner::None};
};
} // namespace gameui
