#pragma once

#include <Engine/PlayerUiInteraction.hpp>
#include <Engine/PlayerUiStyle.hpp>
#include <Renderer/GameUiDrawList.hpp>

#include <optional>
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

// --- IME composition ranges (issue #302 F09) --------------------------------

/// Unit of `SDL_EVENT_TEXT_EDITING::start/length`. SDL does NOT normalize it:
/// the bundled 3.4.18 Windows backend sends UTF-16 code-unit offsets
/// (`SDL_windowskeyboard.c` — `ime_cursor`, `ime_selected_start/length`,
/// `SDL_wcslen(readingstring)`) while the Linux fcitx/ibus backends send
/// Unicode scalar offsets (`SDL_utf8strnlen`, formatted-preedit positions).
enum class CompositionUnit : uint8_t
{
	Scalar = 0,
	Utf16,
};

/// The unit the bundled platform backend uses for a composition range.
inline constexpr CompositionUnit kPlatformCompositionUnit =
#if defined(_WIN32)
	CompositionUnit::Utf16;
#else
	CompositionUnit::Scalar;
#endif

/// Composition range in Unicode scalars, -1 when the backend did not set it.
struct CompositionRange
{
	int start{-1};
	int length{-1};
};

/// Scalar count of a UTF-8 prefix (the SDL_utf8strnlen equivalent).
size_t scalarCount(std::string_view text, size_t bytes);

/// Byte offset of the scalar at `index` platform units. A UTF-16 index that
/// points into the middle of a surrogate pair rounds DOWN to the pair, so a
/// supplementary character can never be split into an invalid range.
size_t compositionByteOffset(std::string_view text, int index, CompositionUnit unit);

/// Normalize a platform composition range to scalar offsets.
CompositionRange compositionRange(std::string_view text, int start, int length,
								  CompositionUnit unit);

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
	// Platform offsets (see kPlatformCompositionUnit) are normalized to Unicode
	// scalars, then translated to bounded byte offsets of the sanitized text.
	void preedit(std::string_view text, int start = -1, int length = -1,
				 CompositionUnit unit = kPlatformCompositionUnit);
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
	/// `bounds` is the component rectangle the text belongs to: every glyph quad is
	/// geometrically clipped to it, with its UVs remapped, so text can never paint
	/// outside its component even where the surface scissor would only hide it
	/// (issue #302 S01). `nullopt` keeps the previous behavior for standalone text.
	void paint(DrawList &out, glm::vec2 origin, std::string_view text, glm::vec4 color,
			   float scale, Clip clip, std::optional<Clip> bounds = std::nullopt) const;
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
// `variant`/`validation` are presentation tokens (issue #302): the button and
// field components read them so an interactive control and a painted sample
// share one appearance.
struct Widget
{
	ControlId id{};
	Clip bounds{};
	std::string_view label, tooltip;
	Control control;
	bool enabled{true};
	style::ButtonVariant variant{style::ButtonVariant::Secondary};
	style::Validation validation{style::Validation::None};
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
	Clip clip{};        // editable content rect, already intersected with the viewport
	glm::vec2 origin{}; // text origin, horizontally scrolled so the caret stays visible
	float caretX{};     // caret x on screen, inside `clip`
	float lineHeight{}; // one Body line at this player scale
	float glyphScale{}; // the ONE text multiplier (role x player scale x framebuffer)
};

/// Geometry of a list widget shared by painting, hover detection and mouse
/// activation (issue #302 F03): caption label line, then the row window.
struct ListGeometry
{
	Clip label{};
	Clip viewport{}; // visible row window; rows are clipped to it
	float rowHeight{};
	int rows{}; // visible rows that fit the widget
};

ListGeometry listGeometry(Clip bounds, const ListBox &list, const style::Theme &theme, float scale);

/// Rect of the visible row at `index` (0 = first visible). It may extend past
/// `ListGeometry::viewport`; painters intersect, hit tests reject.
Clip listRowRect(const ListGeometry &geometry, int index);

/// Row under `point`, or -1 in a gap, outside the widget or on a clipped row.
int listRowAt(const ListGeometry &geometry, glm::vec2 point);

/// Geometry of a slider shared by painting, pointer-to-value conversion and
/// dragging (issue #302 F13).
struct SliderGeometry
{
	Clip label{};
	Clip value{};
	Clip track{};
};

SliderGeometry sliderGeometry(Clip bounds, const style::Theme &theme, float scale);

/// Pointer x -> 0..1 over the painted track: 0% / 25% / 50% / 75% / 100% of the
/// visible track map onto the same values as the keyboard steps.
float sliderValueAt(const SliderGeometry &geometry, float x);

// Screen composition supplies stable IDs and state. The same set handles
// hit testing, focus, editing and painting for all instances of a control.
class WidgetSet
{
public:
	/// Bounded control count per screen (design-system gallery included).
	static constexpr size_t kMaxWidgets = 24;

	void reset(Clip clip, float height, float offset);
	void add(Widget widget);
	void setEnabled(ControlId id, bool enabled, ControlId &focus);
	std::span<const Widget> widgets() const { return {m_items.data(), m_count}; }
	const ScrollViewport &viewport() const { return m_viewport; }
	void fitContent(float height)
	{
		m_viewport.contentHeight = height;
		m_viewport.setOffset(m_viewport.offset);
	}
	Clip bounds(const Widget &widget) const { return m_viewport.place(widget.bounds); }
	const Widget *find(ControlId id) const;
	const Widget *at(glm::vec2 point) const;
	bool focusableAt(glm::vec2 point) const { return eligible(at(point)); }
	const Widget *step(ControlId id, bool reverse) const;
	void focus(ControlId &current, ControlId next, bool reveal);
	void revalidate(ControlId &current);
	/// `fontScale` is the framebuffer compensation the face was baked with: it is
	/// passed explicitly by the host so text geometry never depends on a paint
	/// that may not have run yet (issue #302 F01).
	WidgetEvent press(glm::vec2 point, ControlId &focus, FontView font, float scale, bool extend = false,
					  float fontScale = 1.f, const style::Theme &theme = style::baseline());
	WidgetEvent drag(glm::vec2 point, float scale, const style::Theme &theme = style::baseline());
	WidgetEvent release(glm::vec2 point, float scale, const style::Theme &theme = style::baseline());
	void scroll(glm::vec2 point, float delta, float scale, ControlId &focus);
	WidgetEvent key(ControlId &focus, NavKey key, bool shift, bool shortcut, bool repeat);
	WidgetEvent commitText(ControlId focus, std::string_view text);
	WidgetEvent completeCut(ControlId focus);
	TextField *editor(ControlId focus) const;
	TextGeometry editorGeometry(ControlId focus, FontView font, float scale, float fontScale = 1.f) const;
	void cancelEditing();
	void cancelPress() { m_pressed = 0; }
	bool hasPress() const { return m_pressed != 0; }
	/// Paint every widget from the design tokens. One Surface per frame keeps
	/// interactive controls and painted components visually identical.
	void paint(DrawList &out, FontView font, float scale, ControlId focus, glm::vec2 pointer,
			   const style::Theme &theme = style::baseline(), uint32_t atlasLayers = 0,
			   float fontScale = 1.f) const;

private:
	bool canFocus(const Widget *widget) const;
	bool eligible(const Widget *widget) const;
	WidgetEvent activate(const Widget &widget, const glm::vec2 *point, float scale,
						 const style::Theme &theme);

	std::array<Widget, kMaxWidgets> m_items{};
	size_t m_count{};
	ScrollViewport m_viewport{};
	ControlId m_pressed{};
	TextField *m_editor{};
};
} // namespace gameui
