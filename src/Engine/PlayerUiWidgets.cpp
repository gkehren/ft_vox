#include <Engine/PlayerUiWidgets.hpp>

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
} // namespace

void WidgetSet::reset(Clip clip, float height, float offset)
{
	m_count = 0;
	m_viewport = {clip, height, 0.f};
	m_viewport.setOffset(offset);
}

void WidgetSet::add(Widget widget)
{
	if (m_count < m_items.size())
		m_items[m_count++] = widget;
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
		if (item.id && item.enabled && !item.bounds.empty())
			return &item;
	}
	return nullptr;
}

void WidgetSet::press(glm::vec2 point, ScreenStack &stack)
{
	const auto *widget = at(point);
	m_pressed = widget && widget->enabled ? widget->id : 0;
	stack.focus(m_pressed);
}

const Widget *WidgetSet::release(glm::vec2 point)
{
	const auto *widget = at(point);
	const bool activate = widget && widget->enabled && pressed(widget->id);
	cancelPress();
	return activate ? widget : nullptr;
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

void TextField::preedit(std::string_view text)
{
	m_composition = singleLine(text, kMaxBytes);
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
