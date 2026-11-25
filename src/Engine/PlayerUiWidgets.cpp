#include <Engine/PlayerUiWidgets.hpp>

#include <Engine/PlayerUiComponents.hpp>

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

// Form-field geometry shared by painting, caret hit testing and the SDL text
// input area, derived from the same tokens the field component paints with
// (label line, then the sunken well with the value line inside it).
TextGeometry fieldGeometry(const TextField &field, Clip bounds, Clip viewport, FontView font, float scale,
						   bool focused, const style::Theme &theme = style::baseline(), float fontScale = 1.f)
{
	const float s = scale > 0.f ? scale : 1.f;
	const float glyphs = fontScale > 0.f ? fontScale : 1.f;
	const float labelHeight = style::lineHeight(style::TextRole::Caption) * s;
	const Clip well = {{bounds.rect.x, bounds.rect.y + labelHeight, bounds.rect.z, bounds.rect.w}};
	const auto clip = viewport.intersect(inset(well, theme.space.xs * s));
	const bool composing = focused && !field.composition().empty();
	const auto pos = composing ? field.selectionStart() : field.caret();
	const float advance = font.measure(std::string_view(field.value()).substr(0, pos), glyphs) +
		(composing ? font.measure(std::string_view(field.composition()).substr(0, field.compositionCursor()), glyphs) : 0.f);
	const float lineHeight = style::lineHeight(style::TextRole::Body) * s;
	glm::vec2 origin = glm::vec2(well.rect) +
		glm::vec2(theme.space.sm * s, std::max(0.f, (size(well).y - lineHeight) * 0.5f));
	if (clip.empty())
		return {clip, origin, origin.x};
	const float right = std::max(clip.rect.x, clip.rect.z - s);
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
	return field ? fieldGeometry(*field, bounds(*widget), m_viewport.clip, font, scale, true,
		style::baseline(), m_fontScale) : TextGeometry{};
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
		const auto geometry = fieldGeometry(*field, bounds(*widget), m_viewport.clip, font, scale, hadFocus,
			style::baseline(), m_fontScale);
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
				const int selected = list->selection.selected;
				list->selection.select(first + i, listSize(*list), visibleRows(*list));
				return selected != list->selection.selected ? WidgetEvent{widget.id, WidgetAction::Change} : WidgetEvent{};
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
				return field->erase(key) ? WidgetEvent{current, WidgetAction::Change} : WidgetEvent{};
			field->move(key, shift);
		}
		return {};
	}
	if (auto *slider = binding<Slider>(*widget); slider && (key == NavKey::Left || key == NavKey::Right))
	{
		const float value = std::clamp(slider->value + (key == NavKey::Left ? -0.05f : 0.05f), 0.f, 1.f);
		if (value == slider->value)
			return {};
		slider->value = value;
		return {current, WidgetAction::Change};
	}
	if (auto *list = binding<ListBox>(*widget); list && vertical)
	{
		const int selected = list->selection.selected;
		list->selection.select(list->selection.selected + (key == NavKey::Up ? -1 : 1), listSize(*list), visibleRows(*list));
		return selected != list->selection.selected ? WidgetEvent{current, WidgetAction::Change} : WidgetEvent{};
	}
	return !repeat && (key == NavKey::Enter || key == NavKey::Space) ? activate(*widget, nullptr, 0.f) : WidgetEvent{};
}

WidgetEvent WidgetSet::commitText(ControlId focus, std::string_view text)
{
	auto *field = editor(focus);
	return field && field->insert(text) ? WidgetEvent{focus, WidgetAction::Change} : WidgetEvent{};
}

WidgetEvent WidgetSet::completeCut(ControlId focus)
{
	auto *field = editor(focus);
	return field && !field->selection().empty() && field->erase(NavKey::Delete)
		? WidgetEvent{focus, WidgetAction::Change} : WidgetEvent{};
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

// One paint path for every control: the same Surface the painted components use,
// so an interactive button, field, row or toggle is indistinguishable from its
// gallery sample. No color, radius or spacing literal lives in this function.
void WidgetSet::paint(DrawList &out, FontView font, float scale, ControlId focus, glm::vec2 pointer,
					  const style::Theme &theme, uint32_t atlasLayers, float fontScale) const
{
	Surface surface(out, font, theme, scale, m_viewport.clip, atlasLayers, fontScale);
	const auto &p = theme.palette;
	const float s = scale > 0.f ? scale : 1.f;
	m_fontScale = fontScale > 0.f ? fontScale : 1.f;
	const auto *hover = at(pointer);
	for (const auto &widget : widgets())
	{
		const auto rect = bounds(widget);
		if (rect.intersect(m_viewport.clip).empty())
			continue;
		const bool focused = eligible(&widget) && widget.id == focus;
		const auto state = style::interactiveState(widget.enabled, hover == &widget,
			m_pressed == widget.id, focused, false);
		if (std::holds_alternative<Label>(widget.control))
		{
			surface.fittedLabel(rect, widget.label, style::TextRole::Heading, p.ink, 0.f);
			continue;
		}
		if (std::holds_alternative<Button>(widget.control))
		{
			surface.button(rect, widget.label, widget.variant, state);
			continue;
		}
		if (const auto *field = binding<TextField>(widget))
		{
			surface.formField(rect, widget.label, {}, widget.validation, state);
			const auto layout = fieldGeometry(*field, rect, m_viewport.clip, font, s, focused, theme,
				fontScale);
			const auto live = std::string_view(field->value());
			const auto preedit = focused ? std::string_view(field->composition()) : std::string_view{};
			const float glyphs = style::typeScale(style::TextRole::Body) * s *
				(fontScale > 0.f ? fontScale : 1.f);
			const auto fg = widget.enabled ? p.ink : p.inkDisabled;
			const auto highlight = [&](std::string_view text, glm::vec2 pos, size_t first, size_t last) {
				if (!focused || first >= last)
					return;
				const float x = font.measure(text.substr(0, first), glyphs);
				const float width = font.measure(text.substr(first, last - first), glyphs);
				const auto fillRect = rectangle(pos + glm::vec2(x, -s),
					{width, style::lineHeight(style::TextRole::Body) * s});
				glm::vec4 selection = p.accent;
				selection.a = 0.32f;
				surface.fill(fillRect, selection, 0.f);
			};
			if (preedit.empty())
			{
				highlight(live, layout.origin, field->selectionStart(), field->selectionEnd());
				font.paint(out, layout.origin, live, fg, glyphs, layout.clip);
			}
			else
			{
				const auto before = live.substr(0, field->selectionStart());
				const auto composingAt = layout.origin +
					glm::vec2(font.measure(before, glyphs), 0.f);
				highlight(preedit, composingAt, field->compositionCursor(), field->compositionEnd());
				font.paint(out, layout.origin, before, fg, glyphs, layout.clip);
				font.paint(out, composingAt, preedit, p.accent, glyphs, layout.clip);
				font.paint(out, composingAt + glm::vec2(font.measure(preedit, glyphs), 0.f),
					live.substr(field->selectionEnd()), fg, glyphs, layout.clip);
			}
			if (focused)
			{
				const auto caret = rectangle({layout.caretX, layout.origin.y},
					{std::max(1.f, s), style::lineHeight(style::TextRole::Body) * s});
				surface.fill(caret, p.accent, 0.f);
			}
			continue;
		}
		if (const auto *toggle = binding<Toggle>(widget))
		{
			const float box = theme.size.rowHeight * s * 0.55f;
			const auto indicator = rectangle({rect.rect.z - box - theme.space.md * s,
				rect.rect.y + (size(rect).y - box) * 0.5f}, {box, box});
			surface.fill(indicator, toggle->value && widget.enabled ? p.accent : p.surfaceSunken,
				theme.size.radiusSmall * s);
			surface.ring(indicator, focused ? p.focus : p.border,
				theme.size.borderWidth * s, theme.size.radiusSmall * s);
			surface.fittedLabel({{rect.rect.x, rect.rect.y, indicator.rect.x - theme.space.sm * s,
					rect.rect.w}}, widget.label, style::TextRole::Body,
				widget.enabled ? p.ink : p.inkDisabled, 0.f);
			continue;
		}
		if (const auto *slider = binding<Slider>(widget))
		{
			const float lineHeight = style::lineHeight(style::TextRole::Body) * s;
			char value[12];
			std::snprintf(value, sizeof(value), "%.0f%%",
				std::clamp(slider->value, 0.f, 1.f) * 100.f);
			surface.fittedLabel({{rect.rect.x, rect.rect.y, rect.rect.z - theme.space.xl * s,
					rect.rect.y + lineHeight}}, widget.label, style::TextRole::Body,
				widget.enabled ? p.ink : p.inkDisabled, 0.f);
			surface.labelAligned({{rect.rect.z - theme.space.xl * s, rect.rect.y, rect.rect.z,
					rect.rect.y + lineHeight}}, value, style::TextRole::Body, p.inkMuted, 1.f);
			const auto track = rectangle(glm::vec2(rect.rect.x, rect.rect.y + lineHeight * 1.15f),
				{size(rect).x, std::max(2.f, theme.space.xs * s)});
			surface.fill(track, p.surfaceSunken, theme.size.radiusSmall * s);
			surface.fill(progressFill(track, slider->value), widget.enabled ? p.accent : p.inkDisabled,
				theme.size.radiusSmall * s);
			if (focused)
				surface.ring(rect, p.focus, theme.size.borderWidth * s, theme.size.radiusSmall * s);
			continue;
		}
		if (const auto *list = binding<ListBox>(widget))
		{
			surface.label({{rect.rect.x, rect.rect.y, rect.rect.z,
					rect.rect.y + style::lineHeight(style::TextRole::Caption) * s}}, widget.label,
				style::TextRole::Caption, p.inkMuted);
			const int first = std::clamp(list->selection.first, 0,
				std::max(0, listSize(*list) - visibleRows(*list)));
			const float top = rect.rect.y + style::lineHeight(style::TextRole::Caption) * s +
				theme.space.xxs * s;
			for (int i = 0; i < visibleRows(*list) && first + i < listSize(*list); ++i)
			{
				const auto row = rectangle({rect.rect.x, top + float(i) * theme.size.rowHeight * s},
					{size(rect).x, theme.size.rowHeight * s});
				if (row.rect.w > m_viewport.clip.rect.w)
					break;
				const bool selected = first + i == list->selection.selected;
				const bool rowHovered = hit(row.intersect(m_viewport.clip), pointer);
				surface.listRow(row, list->items[static_cast<size_t>(first + i)], {},
					style::interactiveState(widget.enabled, rowHovered, false, false, selected));
			}
			if (focused)
				surface.ring(rect, p.focus, theme.size.borderWidth * s, theme.size.radiusSmall * s);
			continue;
		}
		surface.fittedLabel(rect, widget.label, style::TextRole::Body, p.ink, 0.f);
	}
	if (hover && !hover->tooltip.empty())
		surface.tooltip(m_viewport.clip, pointer, hover->tooltip);
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

bool TextField::removeSelection()
{
	const auto first = selectionStart();
	const auto count = selectionEnd() - first;
	if (!count)
		return false;
	m_value.erase(first, count);
	placeCaret(first);
	return true;
}

bool TextField::erase(NavKey key)
{
	if (m_caret == m_anchor)
		m_anchor = key == NavKey::Backspace ? previousUtf8(m_value, m_caret) : nextUtf8(m_value, m_caret);
	return removeSelection();
}

bool TextField::insert(std::string_view text)
{
	const auto clean = singleLine(text, kMaxBytes - m_value.size() + selection().size());
	cancelComposition();
	if (clean.empty())
		return false;
	const auto first = selectionStart();
	const bool changed = clean != selection();
	if (changed)
		m_value.replace(first, selection().size(), clean);
	placeCaret(first + clean.size());
	return changed;
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
