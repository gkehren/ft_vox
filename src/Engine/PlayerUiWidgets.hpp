#pragma once

#include <Engine/PlayerUiInteraction.hpp>
#include <Renderer/GameUiDrawList.hpp>

#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace gameui
{
enum class NavKey { Other, Tab, Enter, Space, Left, Right, Up, Down, Home, End, Backspace, Delete, A, C, V, X };

// This editor operates at Unicode scalar boundaries. Shaping, bidi and
// grapheme segmentation belong to the text service selected for #302.
size_t nextUtf8(std::string_view text, size_t offset);

class TextField
{
public:
	static constexpr size_t kMaxBytes = 96;
	const std::string &value() const { return m_value; }
	const std::string &composition() const { return m_composition; }
	size_t compositionCursor() const { return m_compositionCursor; }
	size_t compositionEnd() const { return m_compositionEnd; }
	size_t caret() const { return m_caret; }
	size_t selectionStart() const { return std::min(m_anchor, m_caret); }
	size_t selectionEnd() const { return std::max(m_anchor, m_caret); }
	std::string_view selection() const;
	void placeCaret(size_t byte, bool extend = false);
	void move(NavKey key, bool extend);
	void selectAll() { m_anchor = 0; m_caret = m_value.size(); }
	bool erase(NavKey key);
	bool insert(std::string_view text);
	// Platform offsets are scalar indices, translated to bounded byte offsets.
	void preedit(std::string_view text, int start = -1, int length = -1);
	void cancelComposition()
	{
		m_composition.clear();
		m_compositionCursor = m_compositionEnd = 0;
	}

private:
	bool removeSelection();
	std::string m_value, m_composition;
	size_t m_caret{}, m_anchor{};
	size_t m_compositionCursor{}, m_compositionEnd{};
};

struct ListSelection
{
	int first{}, selected{};
	void scroll(int rows, int count, int visible);
	void select(int index, int count, int visible);
};

struct Glyph
{
	glm::vec4 rect{}, uv{};
	float advance{};
	bool visible{};
};

struct FontView
{
	std::span<const Glyph> glyphs;
	float measure(std::string_view text, float scale) const;
	void paint(DrawList &out, glm::vec2 origin, std::string_view text, glm::vec4 color,
			   float scale, Clip clip) const;
};

struct Label {};
struct Button {};
struct Toggle { bool value{}; };
struct Slider { float value{}; };
struct ListBox
{
	std::span<const std::string_view> items;
	int visibleRows{3};
	ListSelection selection{};
};
using Control = std::variant<Label, Button, TextField *, Toggle *, Slider *, ListBox *>;

// Screen-owned bindings must outlive this set's composition and input.
struct Widget
{
	ControlId id{};
	Clip bounds{};
	std::string_view label, tooltip;
	Control control;
	bool enabled{true};
};

// Change means the bound value effectively changed: committed text, deletion,
// cut and paste each report it once, tagged with the control that owns the
// binding. Unchanged values, IME preedit, caret movement and selection report
// nothing. Copy and Paste name the selection only; the host SDL adapter owns
// the clipboard and completes a paste through commitText.
enum class WidgetAction { None, Activate, Change, Copy, Cut, Paste };
struct WidgetEvent
{
	ControlId id{};
	WidgetAction action{};
};

struct TextGeometry
{
	Clip clip{};
	glm::vec2 origin{};
	float caretX{};
};

// Screen composition supplies stable IDs and state. The same set handles
// hit testing, focus, editing and painting for all instances of a control.
class WidgetSet
{
public:
	void reset(Clip clip, float height, float offset);
	void add(Widget widget);
	void setEnabled(ControlId id, bool enabled, ControlId &focus);
	std::span<const Widget> widgets() const { return {m_items.data(), m_count}; }
	const ScrollViewport &viewport() const { return m_viewport; }
	Clip bounds(const Widget &widget) const { return m_viewport.place(widget.bounds); }
	const Widget *find(ControlId id) const;
	const Widget *at(glm::vec2 point) const;
	bool focusableAt(glm::vec2 point) const { return eligible(at(point)); }
	const Widget *step(ControlId id, bool reverse) const;
	void focus(ControlId &current, ControlId next, bool reveal);
	void revalidate(ControlId &current);
	WidgetEvent press(glm::vec2 point, ControlId &focus, FontView font, float scale, bool extend = false);
	WidgetEvent drag(glm::vec2 point, float scale);
	WidgetEvent release(glm::vec2 point, float scale);
	void scroll(glm::vec2 point, float delta, float scale, ControlId &focus);
	WidgetEvent key(ControlId &focus, NavKey key, bool shift, bool shortcut, bool repeat);
	WidgetEvent commitText(ControlId focus, std::string_view text);
	WidgetEvent completeCut(ControlId focus);
	TextField *editor(ControlId focus) const;
	TextGeometry editorGeometry(ControlId focus, FontView font, float scale) const;
	void cancelEditing();
	void cancelPress() { m_pressed = 0; }
	bool hasPress() const { return m_pressed != 0; }
	void paint(DrawList &out, FontView font, float scale, ControlId focus, glm::vec2 pointer) const;

private:
	bool canFocus(const Widget *widget) const;
	bool eligible(const Widget *widget) const;
	WidgetEvent activate(const Widget &widget, const glm::vec2 *point, float scale);

	std::array<Widget, 16> m_items{};
	size_t m_count{};
	ScrollViewport m_viewport{};
	ControlId m_pressed{};
	TextField *m_editor{};
};
} // namespace gameui
