#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace gameui
{
enum class Screen { Hud, Inventory, Pause, Showcase, ConfirmExit, MainMenu, Loading };
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
	bool suppressDeveloper() const { return top() == Screen::ConfirmExit || top() == Screen::Loading; }
	bool nativeScreen() const { return top() != Screen::Hud && top() != Screen::Inventory; }
	bool blocksGameplay() const { return top() != Screen::Hud; }
	bool pausesWorld(bool connected) const { return !connected && contains(Screen::Pause); }

private:
	struct Entry { Screen screen{}; ControlId focus{}; };
	std::array<Entry, 5> m_entries{};
	size_t m_size{1};
	uint64_t m_revision{};
};

enum class InputOwner : uint8_t { None, Blocked, Global, PlayerUi, DeveloperUi, Gameplay };
enum class InputKind { Key, Pointer, Text };

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
	bool devDialog{};
	bool devText{};
	bool nativePointer{};
	bool transition{};

	InputOwner choose(InputKind kind, bool global = false, bool back = false, bool inventoryBack = false) const
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
		if (kind == InputKind::Key && back)
		{
			if (devText || (!nativeScreen && devKeyboard && !inventory))
				return InputOwner::DeveloperUi;
			if (nativeScreen || inventory)
				return InputOwner::PlayerUi;
		}
		if (kind == InputKind::Pointer)
		{
			if (devPointer)
				return InputOwner::DeveloperUi;
			if (nativePointer)
				return InputOwner::PlayerUi;
		}
		else if (devText)
			return InputOwner::DeveloperUi;
		if (nativeScreen)
			return InputOwner::PlayerUi;
		if (inventory && inventoryBack)
			return InputOwner::PlayerUi;
		if (kind != InputKind::Pointer && devKeyboard)
			return InputOwner::DeveloperUi;
		if (inventory || transition || kind == InputKind::Text)
			return InputOwner::Blocked;
		return InputOwner::Gameplay;
	}
	bool allowsMovement() const
	{
		return interactive && !locked && !nativeScreen && !inventory && !paused &&
			!devKeyboard && !devDialog && !transition;
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
		return down ? press(owner, candidate) : release(owner);
	}
	InputOwner pointerOwner() const
	{
		const auto first = std::find_if(m_buttons.begin(), m_buttons.end(),
			[](InputOwner owner) { return owner != InputOwner::None; });
		return first == m_buttons.end() ? InputOwner::None : *first;
	}
	bool gameplayKey(size_t code) const { return code < m_keys.size() && m_keys[code] == InputOwner::Gameplay; }
	void cancelGameplay() { cancel(InputOwner::Gameplay); }
	void cancelDeveloper() { cancel(InputOwner::DeveloperUi); }
	void cancelPlayerUi() { cancel(InputOwner::PlayerUi); }
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
			m_buttons[button] = held ? InputOwner::Blocked : InputOwner::None;
	}

private:
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
		for (auto &key : m_keys)
			if (key == owner)
				key = InputOwner::Blocked;
		for (auto &button : m_buttons)
			if (button == owner)
				button = InputOwner::Blocked;
	}
	std::array<InputOwner, kKeys> m_keys{};
	std::array<InputOwner, 32> m_buttons{};
};
} // namespace gameui
