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

int listSize(const ListBox &list)
{
	return static_cast<int>(std::min(list.items.size(), size_t(INT32_MAX)));
}

int visibleRows(const ListBox &list)
{
	return std::clamp(list.visibleRows, 1, 16);
}

// Form-field geometry shared by painting, caret hit testing and the SDL text
// input area. One metric (`TextStyle`) sizes the glyphs, the line and the
// caret, so a click, a caret and the painted glyphs cannot disagree
// (issue #302 F01) and the editor primitives stay inside the well (F02).
TextGeometry fieldGeometry(const TextField &field, Clip bounds, FontView font, float scale,
						   bool focused, const style::Theme &theme, float fontScale)
{
	const float s = scale > 0.f ? scale : 1.f;
	const auto metric = gameui::textStyle(style::TextRole::Body, s, fontScale);
	const float labelHeight = gameui::textStyle(style::TextRole::Caption, s, fontScale).lineHeight();
	// The well is the value row of the field group (see formFieldHeight).
	const Clip well = {{bounds.rect.x, bounds.rect.y + labelHeight, bounds.rect.z, bounds.rect.w}};
	const float insetX = theme.space.sm * s;
	const float insetY = theme.space.xs * s;
	const Clip content = {{well.rect.x + insetX, well.rect.y + insetY,
		std::max(well.rect.x + insetX, well.rect.z - insetX),
		std::max(well.rect.y + insetY, well.rect.w - insetY)}};
	TextGeometry out{};
	out.lineHeight = metric.lineHeight();
	out.glyphScale = metric.glyphScale();
	if (content.empty())
	{
		out.clip = {};
		out.origin = glm::vec2(content.rect);
		out.caretX = out.origin.x;
		return out;
	}
	out.clip = content;
	out.origin = glm::vec2(content.rect.x,
		content.rect.y + std::max(0.f, (size(content).y - out.lineHeight) * 0.5f));
	const bool composing = focused && !field.composition().empty();
	const auto pos = composing ? field.selectionStart() : field.caret();
	const float advance = font.measure(std::string_view(field.value()).substr(0, pos), out.glyphScale) +
		(composing ? font.measure(std::string_view(field.composition()).substr(0, field.compositionCursor()),
			out.glyphScale) : 0.f);
	const float right = std::max(content.rect.x, content.rect.z - s);
	if (focused)
		out.origin.x -= std::max(0.f, out.origin.x + advance - right);
	out.caretX = std::clamp(out.origin.x + advance, content.rect.x, right);
	return out;
}
} // namespace

ListGeometry listGeometry(Clip bounds, const ListBox &list, const style::Theme &theme, float scale)
{
	const float s = scale > 0.f ? scale : 1.f;
	ListGeometry geometry{};
	geometry.rowHeight = theme.size.rowHeight * s;
	geometry.rows = visibleRows(list);
	geometry.label = {{bounds.rect.x, bounds.rect.y, bounds.rect.z,
		std::min(bounds.rect.w, bounds.rect.y + style::lineHeight(style::TextRole::Caption) * s)}};
	const float top = std::min(bounds.rect.w, geometry.label.rect.w + theme.space.xxs * s);
	geometry.viewport = {{bounds.rect.x, top, bounds.rect.z,
		std::min(bounds.rect.w, top + geometry.rowHeight * float(geometry.rows))}};
	return geometry;
}

Clip listRowRect(const ListGeometry &geometry, int index)
{
	if (index < 0 || geometry.rowHeight <= 0.f)
		return {};
	const float top = geometry.viewport.rect.y + float(index) * geometry.rowHeight;
	return {{geometry.viewport.rect.x, top, geometry.viewport.rect.z, top + geometry.rowHeight}};
}

int listRowAt(const ListGeometry &geometry, glm::vec2 point)
{
	if (!hit(geometry.viewport, point))
		return -1;
	for (int index = 0; index < geometry.rows; ++index)
		if (hit(listRowRect(geometry, index).intersect(geometry.viewport), point))
			return index;
	return -1;
}

SliderGeometry sliderGeometry(Clip bounds, const style::Theme &theme, float scale)
{
	const float s = scale > 0.f ? scale : 1.f;
	const auto metric = gameui::textStyle(style::TextRole::Body, s);
	SliderGeometry geometry{};
	geometry.label = {{bounds.rect.x, bounds.rect.y, bounds.rect.z, bounds.rect.y + metric.lineHeight()}};
	const float readout = 48.f * s;
	geometry.value = {{std::max(bounds.rect.x, bounds.rect.z - readout), geometry.label.rect.y,
		bounds.rect.z, geometry.label.rect.w}};
	const float top = std::min(bounds.rect.w, geometry.label.rect.w + theme.space.xs * s);
	geometry.track = {{bounds.rect.x, top, bounds.rect.z,
		std::min(bounds.rect.w, top + std::max(2.f, theme.space.xs * s))}};
	return geometry;
}

float sliderValueAt(const SliderGeometry &geometry, float x)
{
	const float width = size(geometry.track).x;
	if (!(width > 0.f))
		return 0.f;
	return std::clamp((x - geometry.track.rect.x) / width, 0.f, 1.f);
}

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

TextGeometry WidgetSet::editorGeometry(ControlId focus, FontView font, float scale, float fontScale) const
{
	const auto *widget = find(focus);
	const auto *field = editor(focus);
	if (!field || !widget)
		return {};
	auto geometry = fieldGeometry(*field, bounds(*widget), font, scale, true, style::baseline(), fontScale);
	// The editor's own content rect AND the visible part of the screen: the SDL
	// text area, the caret and the candidate window all live inside it.
	geometry.clip = geometry.clip.intersect(m_viewport.clip);
	const float left = geometry.clip.rect.x;
	const float right = std::max(left, geometry.clip.rect.z - 1.f);
	geometry.caretX = std::clamp(geometry.caretX, left, right);
	return geometry;
}

void WidgetSet::cancelEditing()
{
	if (m_editor)
		m_editor->cancelComposition();
	m_editor = nullptr;
}

WidgetEvent WidgetSet::press(glm::vec2 point, ControlId &current, FontView font, float scale, bool extend,
							float fontScale, const style::Theme &theme)
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
		const auto geometry = fieldGeometry(*field, bounds(*widget), font, scale, hadFocus, theme,
			fontScale);
		const auto value = std::string_view(field->value());
		float x = geometry.origin.x;
		size_t byte = 0;
		while (byte < value.size())
		{
			const auto next = nextUtf8(value, byte);
			const float advance = font.measure(value.substr(byte, next - byte), geometry.glyphScale);
			if (point.x < x + advance * 0.5f)
				break;
			x += advance;
			byte = next;
		}
		field->cancelComposition();
		field->placeCaret(byte, extend);
	}
	return drag(point, scale, theme);
}

WidgetEvent WidgetSet::drag(glm::vec2 point, float scale, const style::Theme &theme)
{
	const auto *widget = find(m_pressed);
	if (!eligible(widget))
	{
		cancelPress();
		return {};
	}
	if (auto *slider = binding<Slider>(*widget))
	{
		// The same track the painter draws: no legacy interaction margin can
		// drift away from the visible track (issue #302 F13).
		const float value = sliderValueAt(sliderGeometry(bounds(*widget), theme, scale), point.x);
		if (slider->value != value)
		{
			slider->value = value;
			return {widget->id, WidgetAction::Change};
		}
	}
	return {};
}

WidgetEvent WidgetSet::release(glm::vec2 point, float scale, const style::Theme &theme)
{
	const auto *widget = at(point);
	const auto pressed = m_pressed;
	cancelPress();
	return eligible(widget) && widget->id == pressed ? activate(*widget, &point, scale, theme)
													  : WidgetEvent{};
}

WidgetEvent WidgetSet::activate(const Widget &widget, const glm::vec2 *point, float scale,
								const style::Theme &theme)
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
		// Rendering, hover and activation share listGeometry/listRowAt, so the
		// row a click lands on is exactly the row that was painted (F03).
		const int row = listRowAt(listGeometry(bounds(widget), *list, theme, scale), *point);
		if (row >= 0)
		{
			const int first = std::clamp(list->selection.first, 0,
				std::max(0, listSize(*list) - visibleRows(*list)));
			const int selected = list->selection.selected;
			list->selection.select(first + row, listSize(*list), visibleRows(*list));
			return selected != list->selection.selected ? WidgetEvent{widget.id, WidgetAction::Change}
														: WidgetEvent{};
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
	return !repeat && (key == NavKey::Enter || key == NavKey::Space)
		? activate(*widget, nullptr, 0.f, style::baseline()) : WidgetEvent{};
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
			// One metric for glyphs, line and caret (F01); every editor primitive is
			// clipped by the field's own content rect (F02) so a long value, a full
			// selection or a preedit never bleeds into the well or a neighbour.
			const auto layout = fieldGeometry(*field, rect, font, s, focused, theme, fontScale);
			const auto editorClip = layout.clip.intersect(m_viewport.clip);
			if (editorClip.empty())
				continue;
			const auto live = std::string_view(field->value());
			const auto preedit = focused ? std::string_view(field->composition()) : std::string_view{};
			const auto fg = widget.enabled ? p.ink : p.inkDisabled;
			const auto highlight = [&](std::string_view text, glm::vec2 pos, size_t first, size_t last) {
				if (!focused || first >= last)
					return;
				const float x = font.measure(text.substr(0, first), layout.glyphScale);
				const float width = font.measure(text.substr(first, last - first), layout.glyphScale);
				const auto fillRect = rectangle(pos, {width, layout.lineHeight}).intersect(editorClip);
				if (fillRect.empty())
					return;
				glm::vec4 selection = p.accent;
				selection.a = 0.32f;
				surface.fill(rectangle({pos.x + x, pos.y}, {width, layout.lineHeight}).intersect(editorClip),
					selection, 0.f);
			};
			if (preedit.empty())
			{
				highlight(live, layout.origin, field->selectionStart(), field->selectionEnd());
				font.paint(out, layout.origin, live, fg, layout.glyphScale, editorClip);
			}
			else
			{
				const auto before = live.substr(0, field->selectionStart());
				const auto composingAt = layout.origin +
					glm::vec2(font.measure(before, layout.glyphScale), 0.f);
				highlight(preedit, composingAt, field->compositionCursor(), field->compositionEnd());
				font.paint(out, layout.origin, before, fg, layout.glyphScale, editorClip);
				font.paint(out, composingAt, preedit, p.accent, layout.glyphScale, editorClip);
				font.paint(out, composingAt + glm::vec2(font.measure(preedit, layout.glyphScale), 0.f),
					live.substr(field->selectionEnd()), fg, layout.glyphScale, editorClip);
			}
			if (focused)
			{
				const auto caret = rectangle({layout.caretX, layout.origin.y},
					{std::max(1.f, s), layout.lineHeight}).intersect(editorClip);
				if (!caret.empty())
					surface.fill(caret, p.accent, 0.f);
			}
			continue;
		}
		if (const auto *toggle = binding<Toggle>(widget))
		{
			const float box = theme.size.rowHeight * s * 0.55f;
			const auto indicator = rectangle({rect.rect.z - box - theme.space.md * s,
				rect.rect.y + (size(rect).y - box) * 0.5f}, {box, box});
			surface.fill(indicator, style::toggleFace(theme, state, toggle->value),
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
			// One track geometry for painting and for pointer->value (F13).
			const auto geometry = sliderGeometry(rect, theme, s);
			char value[12];
			std::snprintf(value, sizeof(value), "%.0f%%",
				std::clamp(slider->value, 0.f, 1.f) * 100.f);
			surface.fittedLabel(geometry.label, widget.label, style::TextRole::Body,
				widget.enabled ? p.ink : p.inkDisabled, 0.f);
			surface.fittedLabel(geometry.value, value, style::TextRole::Body, p.inkMuted, 1.f);
			surface.fill(geometry.track, p.surfaceSunken, theme.size.radiusSmall * s);
			surface.fill(progressFill(geometry.track, slider->value),
				style::sliderFill(theme, state), theme.size.radiusSmall * s);
			if (focused)
				surface.ring(rect, p.focus, theme.size.borderWidth * s, theme.size.radiusSmall * s);
			continue;
		}
		if (const auto *list = binding<ListBox>(widget))
		{
			// Painting, hover and activation share one geometry (F03): the rect a
			// row is painted in is the rect that is hit-tested, a partially
			// visible row is clipped to the list window, and a row scrolled out of
			// that window never receives input or overdraws a neighbour.
			const auto geometry = listGeometry(rect, *list, theme, s);
			const auto rowsClip = geometry.viewport.intersect(m_viewport.clip);
			surface.label(geometry.label, widget.label, style::TextRole::Caption, p.inkMuted);
			const int first = std::clamp(list->selection.first, 0,
				std::max(0, listSize(*list) - geometry.rows));
			for (int i = 0; i < geometry.rows && first + i < listSize(*list); ++i)
			{
				const auto row = listRowRect(geometry, i);
				if (row.intersect(rowsClip).empty())
					continue;
				const bool selected = first + i == list->selection.selected;
				const bool rowHovered = hit(row.intersect(rowsClip), pointer);
				surface.listRow(row.intersect(rowsClip), list->items[static_cast<size_t>(first + i)], {},
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

size_t scalarCount(std::string_view text, size_t bytes)
{
	size_t count = 0;
	for (size_t offset = 0; offset < std::min(bytes, text.size()); offset = nextUtf8(text, offset))
		++count;
	return count;
}

size_t compositionByteOffset(std::string_view text, int index, CompositionUnit unit)
{
	if (index <= 0)
		return 0;
	size_t scalar = 0, units = 0, offset = 0;
	while (offset < text.size())
	{
		const size_t length = nextUtf8(text, offset) - offset;
		const size_t cost = unit == CompositionUnit::Utf16 && length == 4 ? 2 : 1;
		// A UTF-16 index inside a surrogate pair rounds DOWN to the pair: the
		// range can never split a supplementary character into invalid bytes.
		const bool reached = unit == CompositionUnit::Utf16
			? units >= size_t(index) || units + cost > size_t(index)
			: scalar >= size_t(index);
		if (reached)
			return offset;
		units += cost;
		++scalar;
		offset += length;
	}
	return text.size();
}

CompositionRange compositionRange(std::string_view text, int start, int length,
								  CompositionUnit unit)
{
	if (start < 0)
		return {};
	const auto startByte = compositionByteOffset(text, start, unit);
	const auto startScalar = int(scalarCount(text, startByte));
	if (length < 0)
		return {startScalar, -1};
	// Bounded in 64-bit: a backend may hand over INT32_MAX for "to the end".
	const auto endUnits = static_cast<int>(
		std::min<long long>(static_cast<long long>(start) + length, INT32_MAX));
	const auto endByte = compositionByteOffset(text, endUnits, unit);
	const auto endScalar = int(scalarCount(text, endByte));
	return {startScalar, std::max(0, endScalar - startScalar)};
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

void TextField::preedit(std::string_view text, int start, int length, CompositionUnit unit)
{
	m_composition = singleLine(text, kMaxBytes);
	// Normalize the platform range (UTF-16 units on Windows, scalars elsewhere)
	// to scalars before mapping it onto the sanitized composition, so a
	// supplementary character never corrupts the preedit selection (F09).
	const auto range = compositionRange(text, start, length, unit);
	if (range.start < 0)
	{
		m_compositionCursor = m_compositionEnd = m_composition.size();
		return;
	}
	const size_t end = static_cast<size_t>(range.start) + static_cast<size_t>(std::max(0, range.length));
	const auto cursorByte = scalarOffset(text, static_cast<size_t>(range.start));
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
