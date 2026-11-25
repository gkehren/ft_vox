#include <Engine/PlayerUiWidgets.hpp>

#include <cstdio>

namespace gameui
{
namespace
{
size_t scalarLength(std::string_view text, size_t offset)
{
	const auto lead = static_cast<unsigned char>(text[offset]);
	if (lead < 0x80)
		return 1;
	const size_t length = lead >= 0xc2 && lead <= 0xdf ? 2 :
		(lead >= 0xe0 && lead <= 0xef ? 3 : (lead >= 0xf0 && lead <= 0xf4 ? 4 : 0));
	if (!length || length > text.size() - offset)
		return 0;
	const auto second = static_cast<unsigned char>(text[offset + 1]);
	if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0) ||
		(lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90))
		return 0;
	for (size_t i = 1; i < length; ++i)
		if ((static_cast<unsigned char>(text[offset + i]) & 0xc0) != 0x80)
			return 0;
	return length;
}

size_t previousUtf8(std::string_view text, size_t offset)
{
	while (offset && (static_cast<unsigned char>(text[--offset]) & 0xc0) == 0x80)
	{
	}
	return offset;
}

std::string singleLine(std::string_view input, size_t capacity)
{
	std::string result;
	for (size_t i = 0; i < input.size() && result.size() < capacity;)
	{
		const size_t length = scalarLength(input, i);
		if (length > capacity - result.size())
			break;
		const auto lead = static_cast<unsigned char>(input[i]);
		if (length && lead >= 32 && lead != 127)
			result.append(input.substr(i, length));
		i += std::max(size_t{1}, length);
	}
	return result;
}

size_t scalarOffset(std::string_view text, size_t count)
{
	size_t offset = 0;
	while (count && offset < text.size())
	{
		offset = nextUtf8(text, offset);
		--count;
	}
	return offset;
}

template<class T>
T *binding(const Widget &widget)
{
	const auto *value = std::get_if<T *>(&widget.control);
	return value ? *value : nullptr;
}

const Glyph *getGlyph(FontView font, std::string_view text, size_t offset)
{
	const auto code = static_cast<unsigned char>(text[offset]);
	const auto index = code >= 32 && code < font.glyphs.size() ? code : static_cast<unsigned char>('?');
	return index < font.glyphs.size() ? &font.glyphs[index] : nullptr;
}

Clip listRow(Clip bounds, int row, float scale)
{
	return rectangle(glm::vec2(bounds.rect) + glm::vec2(6.f, 19.f + 24.f * row) * scale,
		{std::max(0.f, size(bounds).x - 12.f * scale), 22.f * scale});
}

int listSize(const ListBox &list)
{
	return static_cast<int>(std::min(list.items.size(), size_t(INT32_MAX)));
}

int visibleRows(const ListBox &list)
{
	return std::clamp(list.visibleRows, 1, 16);
}

TextGeometry fieldGeometry(const TextField &field, Clip bounds, Clip viewport, FontView font, float scale, bool focused)
{
	const auto clip = viewport.intersect(inset(bounds, 5.f * scale));
	const bool composing = focused && !field.composition().empty();
	const auto pos = composing ? field.selectionStart() : field.caret();
	const float advance = font.measure(std::string_view(field.value()).substr(0, pos), scale) +
		(composing ? font.measure(std::string_view(field.composition()).substr(0, field.compositionCursor()), scale) : 0.f);
	glm::vec2 origin = glm::vec2(bounds.rect) + glm::vec2(8.f, 18.f) * scale;
	if (clip.empty())
		return {clip, origin, origin.x};
	const float right = std::max(clip.rect.x, clip.rect.z - scale);
	if (focused)
		origin.x -= std::max(0.f, origin.x + advance - right);
	return {clip, origin, std::clamp(origin.x + advance, clip.rect.x, right)};
}
} // namespace

void WidgetSet::reset(Clip clip, float height, float offset)
{
	m_count = 0;
	m_viewport = {clip, height, 0.f};
	m_viewport.setOffset(offset);
}

void WidgetSet::add(Widget widget)
{
	if (m_count < m_items.size() && (!widget.id || !find(widget.id)))
		m_items[m_count++] = widget;
}

void WidgetSet::setEnabled(ControlId id, bool enabled, ControlId &focus)
{
	for (size_t i = 0; i < m_count; ++i)
		if (id && m_items[i].id == id)
			m_items[i].enabled = enabled;
	revalidate(focus);
}

const Widget *WidgetSet::find(ControlId id) const
{
	const auto items = widgets();
	const auto it = std::find_if(items.begin(), items.end(),
		[id](const Widget &item) { return item.id == id; });
	return it != items.end() && id ? &*it : nullptr;
}

const Widget *WidgetSet::at(glm::vec2 point) const
{
	if (!hit(m_viewport.clip, point))
		return nullptr;
	auto items = widgets();
	const auto it = std::find_if(items.rbegin(), items.rend(), [&](const Widget &item) {
		return item.id && hit(bounds(item), point);
	});
	return it == items.rend() ? nullptr : &*it;
}

const Widget *WidgetSet::step(ControlId id, bool reverse) const
{
	const auto *current = find(id);
	const int count = static_cast<int>(m_count);
	int index = current ? static_cast<int>(current - m_items.data()) : (reverse ? 0 : count - 1);
	for (int i = 0; i < count; ++i)
	{
		index = (index + (reverse ? count - 1 : 1)) % count;
		const auto &item = m_items[static_cast<size_t>(index)];
		if (canFocus(&item))
			return &item;
	}
	return nullptr;
}

bool WidgetSet::canFocus(const Widget *widget) const
{
	return widget && widget->id && widget->enabled && !widget->bounds.empty() && !m_viewport.clip.empty() &&
		!std::holds_alternative<Label>(widget->control) &&
		widget->bounds.rect.x < m_viewport.clip.rect.z && widget->bounds.rect.z > m_viewport.clip.rect.x;
}

bool WidgetSet::eligible(const Widget *widget) const
{
	return canFocus(widget) && !bounds(*widget).intersect(m_viewport.clip).empty();
}

void WidgetSet::focus(ControlId &current, ControlId next, bool reveal)
{
	const auto *widget = find(next);
	if (reveal && canFocus(widget))
		m_viewport.reveal(widget->bounds);
	current = next;
	revalidate(current);
}

void WidgetSet::revalidate(ControlId &current)
{
	if (!eligible(find(current)))
		current = 0;
	if (!eligible(find(m_pressed)))
		cancelPress();
	auto *next = editor(current);
	if (m_editor != next)
	{
		cancelEditing();
		m_editor = next;
	}
}

TextField *WidgetSet::editor(ControlId focus) const
{
	const auto *widget = find(focus);
	return eligible(widget) ? binding<TextField>(*widget) : nullptr;
}

TextGeometry WidgetSet::editorGeometry(ControlId focus, FontView font, float scale) const
{
	const auto *widget = find(focus);
	const auto *field = editor(focus);
	return field ? fieldGeometry(*field, bounds(*widget), m_viewport.clip, font, scale, true) : TextGeometry{};
}

void WidgetSet::cancelEditing()
{
	if (m_editor)
		m_editor->cancelComposition();
	m_editor = nullptr;
}

WidgetEvent WidgetSet::press(glm::vec2 point, ControlId &current, FontView font, float scale, bool extend)
{
	const auto *widget = at(point);
	if (!eligible(widget))
	{
		if (!widget)
			focus(current, 0, false);
		cancelPress();
		return {};
	}
	const bool hadFocus = current == widget->id;
	focus(current, widget->id, false);
	m_pressed = widget->id;
	if (auto *field = editor(current))
	{
		const auto geometry = fieldGeometry(*field, bounds(*widget), m_viewport.clip, font, scale, hadFocus);
		const auto value = std::string_view(field->value());
		float x = geometry.origin.x;
		size_t byte = 0;
		while (byte < value.size())
		{
			const auto next = nextUtf8(value, byte);
			const float advance = font.measure(value.substr(byte, next - byte), scale);
			if (point.x < x + advance * 0.5f)
				break;
			x += advance;
			byte = next;
		}
		field->cancelComposition();
		field->placeCaret(byte, extend);
	}
	return drag(point, scale);
}

WidgetEvent WidgetSet::drag(glm::vec2 point, float scale)
{
	const auto *widget = find(m_pressed);
	if (!eligible(widget))
	{
		cancelPress();
		return {};
	}
	if (auto *slider = binding<Slider>(*widget))
	{
		const auto bar = inset(bounds(*widget), 8.f * scale);
		if (size(bar).x > 0.f)
		{
			const float value = std::clamp((point.x - bar.rect.x) / size(bar).x, 0.f, 1.f);
			if (slider->value != value)
			{
				slider->value = value;
				return {widget->id, WidgetAction::Change};
			}
		}
	}
	return {};
}

WidgetEvent WidgetSet::release(glm::vec2 point, float scale)
{
	const auto *widget = at(point);
	const auto pressed = m_pressed;
	cancelPress();
	return eligible(widget) && widget->id == pressed ? activate(*widget, &point, scale) : WidgetEvent{};
}

WidgetEvent WidgetSet::activate(const Widget &widget, const glm::vec2 *point, float scale)
{
	if (std::holds_alternative<Button>(widget.control))
		return {widget.id, WidgetAction::Activate};
	if (auto *toggle = binding<Toggle>(widget))
	{
		toggle->value = !toggle->value;
		return {widget.id, WidgetAction::Change};
	}
	if (auto *list = binding<ListBox>(widget); list && point)
	{
		const int first = std::clamp(list->selection.first, 0, std::max(0, listSize(*list) - visibleRows(*list)));
		for (int i = 0; i < visibleRows(*list) && first + i < listSize(*list); ++i)
			if (hit(listRow(bounds(widget), i, scale).intersect(bounds(widget)), *point))
			{
				list->selection.select(first + i, listSize(*list), visibleRows(*list));
				return {widget.id, WidgetAction::Change};
			}
	}
	return {};
}

void WidgetSet::scroll(glm::vec2 point, float delta, float scale, ControlId &focus)
{
	if (!hit(m_viewport.clip, point) || delta == 0.f)
		return;
	cancelPress();
	const auto *widget = at(point);
	if (eligible(widget))
		if (auto *list = binding<ListBox>(*widget))
		{
			list->selection.scroll(delta > 0.f ? -1 : 1, listSize(*list), visibleRows(*list));
			return;
		}
	m_viewport.setOffset(m_viewport.offset - delta * 36.f * scale);
	revalidate(focus);
}

WidgetEvent WidgetSet::key(ControlId &current, NavKey key, bool shift, bool shortcut, bool repeat)
{
	revalidate(current);
	const auto *widget = find(current);
	const bool vertical = key == NavKey::Up || key == NavKey::Down;
	if (key == NavKey::Tab || (vertical && (!widget || (!editor(current) && !binding<ListBox>(*widget)))))
	{
		const auto *next = step(current, key == NavKey::Up || (key == NavKey::Tab && shift));
		focus(current, next ? next->id : 0, true);
		return {};
	}
	if (!widget)
		return {};
	if (auto *field = editor(current))
	{
		if (shortcut)
		{
			switch (key)
			{
			case NavKey::A: field->selectAll(); break;
			case NavKey::C: return {current, WidgetAction::Copy};
			case NavKey::X: return {current, WidgetAction::Cut};
			case NavKey::V: return {current, WidgetAction::Paste};
			default: break;
			}
		}
		else if (field->composition().empty())
		{
			if (key == NavKey::Backspace || key == NavKey::Delete)
			{
				field->erase(key);
				return {current, WidgetAction::Change};
			}
			field->move(key, shift);
		}
		return {};
	}
	if (auto *slider = binding<Slider>(*widget); slider && (key == NavKey::Left || key == NavKey::Right))
	{
		slider->value = std::clamp(slider->value + (key == NavKey::Left ? -0.05f : 0.05f), 0.f, 1.f);
		return {current, WidgetAction::Change};
	}
	if (auto *list = binding<ListBox>(*widget); list && vertical)
	{
		list->selection.select(list->selection.selected + (key == NavKey::Up ? -1 : 1), listSize(*list), visibleRows(*list));
		return {current, WidgetAction::Change};
	}
	return !repeat && (key == NavKey::Enter || key == NavKey::Space) ? activate(*widget, nullptr, 0.f) : WidgetEvent{};
}

float FontView::measure(std::string_view text, float scale) const
{
	float width = 0.f;
	for (size_t i = 0; i < text.size(); i = nextUtf8(text, i))
		if (const auto *glyph = getGlyph(*this, text, i))
			width += glyph->advance;
	return width * scale;
}

void FontView::paint(DrawList &out, glm::vec2 origin, std::string_view text, glm::vec4 color, float scale, Clip clip) const
{
	for (size_t i = 0; i < text.size(); i = nextUtf8(text, i))
		if (const auto *glyph = getGlyph(*this, text, i))
		{
			if (glyph->visible)
				out.glyph(glyph->rect * scale + glm::vec4(origin, origin), glyph->uv, color, clip);
			origin.x += glyph->advance * scale;
		}
}

void WidgetSet::paint(DrawList &out, FontView font, float s, ControlId focus, glm::vec2 pointer) const
{
	constexpr glm::vec4 textColor{0.94f, 0.97f, 1.f, 1.f};
	constexpr glm::vec4 muted{0.70f, 0.77f, 0.81f, 1.f};
	constexpr glm::vec4 accent{0.44f, 0.94f, 0.78f, 1.f};
	constexpr glm::vec4 dark{0.052f, 0.083f, 0.103f, 0.97f};
	constexpr glm::vec4 fill{0.13f, 0.23f, 0.27f, 1.f};
	constexpr glm::vec4 hoverFill{0.19f, 0.34f, 0.37f, 1.f};
	constexpr glm::vec4 disabled{0.086f, 0.13f, 0.15f, 1.f};
	const auto *hover = at(pointer);
	for (const auto &widget : widgets())
	{
		const auto rect = bounds(widget);
		const auto clip = rect.intersect(m_viewport.clip);
		if (clip.empty())
			continue;
		const glm::vec2 origin(rect.rect);
		const auto label = [&](std::string_view text, float x, float y, glm::vec4 color) {
			font.paint(out, origin + glm::vec2(x, y) * s, text, color, s, clip);
		};
		if (std::holds_alternative<Label>(widget.control))
		{
			label(widget.label, 3, 4, textColor);
			continue;
		}
		const bool focused = eligible(&widget) && widget.id == focus;
		if (focused)
			out.panel(rect.rect, accent, clip);
		const auto face = focused ? inset(rect, 1.f) : rect;
		out.panel(face.rect, !widget.enabled ? disabled : (hover == &widget || m_pressed == widget.id ? hoverFill : fill), clip);
		const auto fg = widget.enabled ? textColor : muted;
		if (const auto *field = binding<TextField>(widget))
		{
			label(widget.label, 8, 2, muted);
			const auto fieldLayout = fieldGeometry(*field, rect, m_viewport.clip, font, s, focused);
			const auto live = std::string_view(field->value());
			const auto preedit = focused ? std::string_view(field->composition()) : std::string_view{};
			const auto start = fieldLayout.origin;
			const auto highlight = [&](std::string_view text, glm::vec2 pos, size_t first, size_t last) {
				if (focused && first < last)
				{
					const float x = font.measure(text.substr(0, first), s);
					const float width = font.measure(text.substr(first, last - first), s);
					out.panel(rectangle(pos + glm::vec2(x, -s), {width, 13.f * s}).rect, hoverFill, fieldLayout.clip);
				}
			};
			if (preedit.empty())
			{
				highlight(live, start, field->selectionStart(), field->selectionEnd());
				font.paint(out, start, live, fg, s, fieldLayout.clip);
			}
			else
			{
				const auto before = live.substr(0, field->selectionStart());
				const auto composingAt = start + glm::vec2(font.measure(before, s), 0.f);
				highlight(preedit, composingAt, field->compositionCursor(), field->compositionEnd());
				font.paint(out, start, before, fg, s, fieldLayout.clip);
				font.paint(out, composingAt, preedit, accent, s, fieldLayout.clip);
				font.paint(out, composingAt + glm::vec2(font.measure(preedit, s), 0.f),
					live.substr(field->selectionEnd()), fg, s, fieldLayout.clip);
			}
			if (focused)
				out.panel(rectangle({fieldLayout.caretX, start.y}, {s, 13.f * s}).rect, accent, fieldLayout.clip);
		}
		else if (const auto *toggle = binding<Toggle>(widget))
		{
			label(widget.label, 8, 7, fg);
			label(toggle->value ? "On" : "Off", std::max(8.f, size(rect).x / s - 40.f), 7,
				widget.enabled ? accent : muted);
		}
		else if (const auto *slider = binding<Slider>(widget))
		{
			label(widget.label, 8, 2, fg);
			char value[12];
			std::snprintf(value, sizeof(value), "%.0f%%", std::clamp(slider->value, 0.f, 1.f) * 100.f);
			label(value, std::max(8.f, size(rect).x / s - 40.f), 2, muted);
			const auto bar = rectangle(origin + glm::vec2(8.f, 20.f) * s,
				{std::max(0.f, size(rect).x - 16.f * s), 5.f * s});
			out.panel(bar.rect, dark, clip);
			out.panel(rectangle(glm::vec2(bar.rect), {size(bar).x * std::clamp(slider->value, 0.f, 1.f), 5.f * s}).rect,
				widget.enabled ? accent : muted, clip);
		}
		else if (const auto *list = binding<ListBox>(widget))
		{
			label(widget.label, 8, 3, muted);
			const int first = std::clamp(list->selection.first, 0, std::max(0, listSize(*list) - visibleRows(*list)));
			for (int i = 0; i < visibleRows(*list) && first + i < listSize(*list); ++i)
			{
				const auto row = listRow(rect, i, s);
				if (widget.enabled && (first + i == list->selection.selected || hit(row.intersect(clip), pointer)))
					out.panel(row.rect, hoverFill, clip);
				label(list->items[static_cast<size_t>(first + i)], 12, 24.f + 24.f * i, fg);
			}
		}
		else
			label(widget.label, 8, 8, fg);
	}
	if (hover && !hover->tooltip.empty())
	{
		const auto clip = m_viewport.clip;
		const float height = 21.f * s;
		const auto extent = glm::min(glm::vec2{font.measure(hover->tooltip, s) + 12.f * s, height}, size(clip));
		const auto desired = pointer + glm::vec2{12.f * s, -height - 4.f * s};
		const auto pos = glm::clamp(desired, glm::vec2(clip.rect), glm::vec2(clip.rect.z, clip.rect.w) - extent);
		const auto tip = rectangle(pos, extent).intersect(clip);
		out.panel(tip.rect, dark, tip);
		font.paint(out, pos + glm::vec2(6.f, 4.f) * s, hover->tooltip, textColor, s, tip);
	}
}

size_t nextUtf8(std::string_view text, size_t offset)
{
	return offset >= text.size() ? text.size() : offset + std::max(size_t{1}, scalarLength(text, offset));
}

std::string_view TextField::selection() const
{
	return std::string_view(m_value).substr(selectionStart(), selectionEnd() - selectionStart());
}

void TextField::placeCaret(size_t byte, bool extend)
{
	m_caret = std::min(byte, m_value.size());
	if (m_caret < m_value.size() && (static_cast<unsigned char>(m_value[m_caret]) & 0xc0) == 0x80)
		m_caret = previousUtf8(m_value, m_caret);
	if (!extend)
		m_anchor = m_caret;
}

void TextField::move(NavKey key, bool extend)
{
	switch (key)
	{
	case NavKey::Left:
		placeCaret(!extend && m_caret != m_anchor ? selectionStart() : previousUtf8(m_value, m_caret), extend);
		break;
	case NavKey::Right:
		placeCaret(!extend && m_caret != m_anchor ? selectionEnd() : nextUtf8(m_value, m_caret), extend);
		break;
	case NavKey::Home: placeCaret(0, extend); break;
	case NavKey::End: placeCaret(m_value.size(), extend); break;
	default: break;
	}
}

void TextField::removeSelection()
{
	const auto first = selectionStart();
	m_value.erase(first, selectionEnd() - first);
	placeCaret(first);
}

void TextField::erase(NavKey key)
{
	if (m_caret == m_anchor)
		m_anchor = key == NavKey::Backspace ? previousUtf8(m_value, m_caret) : nextUtf8(m_value, m_caret);
	removeSelection();
}

void TextField::insert(std::string_view text)
{
	const auto clean = singleLine(text, kMaxBytes - m_value.size() + selection().size());
	cancelComposition();
	if (clean.empty())
		return;
	removeSelection();
	m_value.insert(m_caret, clean);
	placeCaret(m_caret + clean.size());
}

void TextField::preedit(std::string_view text, int start, int length)
{
	m_composition = singleLine(text, kMaxBytes);
	if (start < 0)
	{
		m_compositionCursor = m_compositionEnd = m_composition.size();
		return;
	}
	const size_t end = static_cast<size_t>(start) + static_cast<size_t>(std::max(0, length));
	const auto cursorByte = scalarOffset(text, static_cast<size_t>(start));
	const auto endByte = scalarOffset(text, end);
	m_compositionCursor = singleLine(text.substr(0, cursorByte), m_composition.size()).size();
	m_compositionEnd = singleLine(text.substr(0, endByte), m_composition.size()).size();
}

void ListSelection::scroll(int rows, int count, int visible)
{
	first = std::clamp(first + rows, 0, std::max(0, count - visible));
}

void ListSelection::select(int index, int count, int visible)
{
	selected = std::clamp(index, 0, std::max(0, count - 1));
	if (selected < first)
		first = selected;
	else if (selected >= first + visible)
		first = selected - visible + 1;
	scroll(0, count, visible);
}
} // namespace gameui
