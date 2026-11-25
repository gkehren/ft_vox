#pragma once

#include <Engine/PlayerUiInteraction.hpp>
#include <Renderer/GameUiGeometry.hpp>

#include <span>
#include <string>
#include <string_view>

namespace gameui
{
enum class WidgetKind { Label, Button, TextField, Toggle, Slider, List };
enum class NavKey { Other, Tab, Enter, Space, Left, Right, Up, Down, Home, End, Backspace, Delete, A, C, V, X };

struct Widget
{
	ControlId id{};
	WidgetKind kind{};
	Clip bounds{};
	std::string_view label, tooltip;
	bool enabled{true};
};

// A control ID is stable within its screen. Geometry is built in paint order
// and shared by pointer hit testing, keyboard traversal and rendering.
class WidgetSet
{
public:
	void reset(Clip clip, float height, float offset);
	void add(Widget widget);
	std::span<const Widget> widgets() const { return {m_items.data(), m_count}; }
	ScrollViewport &viewport() { return m_viewport; }
	const ScrollViewport &viewport() const { return m_viewport; }
	Clip bounds(const Widget &widget) const { return m_viewport.place(widget.bounds); }
	const Widget *find(ControlId id) const;
	const Widget *at(glm::vec2 point) const;
	const Widget *step(ControlId id, bool reverse) const;
	void press(glm::vec2 point, ScreenStack &stack);
	const Widget *release(glm::vec2 point);
	void cancelPress() { m_pressed = 0; }
	bool hasPress() const { return m_pressed != 0; }
	bool pressed(ControlId id) const { return id && id == m_pressed; }

private:
	std::array<Widget, 16> m_items{};
	size_t m_count{};
	ScrollViewport m_viewport{};
	ControlId m_pressed{};
};

// This editor operates at Unicode scalar boundaries. Shaping, bidi and
// grapheme segmentation belong to the text service selected for #302.
size_t nextUtf8(std::string_view text, size_t offset);

class TextField
{
public:
	static constexpr size_t kMaxBytes = 96;
	const std::string &value() const { return m_value; }
	const std::string &composition() const { return m_composition; }
	size_t caret() const { return m_caret; }
	size_t selectionStart() const { return std::min(m_anchor, m_caret); }
	size_t selectionEnd() const { return std::max(m_anchor, m_caret); }
	std::string_view selection() const;
	void placeCaret(size_t byte, bool extend = false);
	void move(NavKey key, bool extend);
	void selectAll() { m_anchor = 0; m_caret = m_value.size(); }
	void erase(NavKey key);
	void insert(std::string_view text);
	void preedit(std::string_view text);
	void cancelComposition() { m_composition.clear(); }

private:
	void removeSelection();
	std::string m_value, m_composition;
	size_t m_caret{}, m_anchor{};
};

struct ListSelection
{
	int first{}, selected{};
	void scroll(int rows, int count, int visible);
	void select(int index, int count, int visible);
};
} // namespace gameui
