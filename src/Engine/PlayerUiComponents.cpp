#include <Engine/PlayerUiComponents.hpp>

#include <cstdio>

namespace gameui
{
namespace
{
Clip lineBelow(Clip rect, float height)
{
	return {{rect.rect.x, rect.rect.y, rect.rect.z, std::min(rect.rect.w, rect.rect.y + height)}};
}
} // namespace

// --- Slot contract ---------------------------------------------------------

SlotGrid slotGrid(Clip bounds, int columns, int rows, float gap)
{
	columns = std::max(1, columns);
	rows = std::max(1, rows);
	gap = std::max(0.f, gap);
	const glm::vec2 extent = size(bounds);
	const float cell = std::min((extent.x - gap * float(columns - 1)) / float(columns),
								(extent.y - gap * float(rows - 1)) / float(rows));
	return {bounds, columns, rows, std::max(0.f, cell), gap};
}

float centeredSlotSize(Clip bounds, int count, float gap, float maxCell)
{
	count = std::max(1, count);
	gap = std::max(0.f, gap);
	const float fit = (size(bounds).x - gap * float(count - 1)) / float(count);
	const float limit = maxCell > 0.f ? std::min(fit, maxCell) : fit;
	return std::max(0.f, limit);
}

Clip slotCell(const SlotGrid &grid, int column, int row)
{
	if (column < 0 || row < 0 || column >= grid.columns || row >= grid.rows)
		return {};
	const glm::vec2 origin = glm::vec2(grid.bounds.rect) +
		glm::vec2(float(column), float(row)) * (grid.cell + grid.gap);
	return rectangle(origin, {grid.cell, grid.cell});
}

int slotIndexAt(const SlotGrid &grid, glm::vec2 point)
{
	if (grid.cell <= 0.f || grid.columns < 1 || grid.rows < 1)
		return -1;
	const glm::vec2 local = point - glm::vec2(grid.bounds.rect);
	if (local.x < 0.f || local.y < 0.f)
		return -1;
	const float pitch = grid.cell + grid.gap;
	const int column = int(std::floor(local.x / pitch));
	const int row = int(std::floor(local.y / pitch));
	if (column < 0 || row < 0 || column >= grid.columns || row >= grid.rows)
		return -1;
	// Reject the gap between cells so a press in the gutter hits nothing.
	if (!hit(slotCell(grid, column, row), point))
		return -1;
	return row * grid.columns + column;
}

Clip centeredSlotStrip(Clip bounds, int count, float cell, float gap)
{
	count = std::max(1, count);
	const float width = float(count) * cell + float(count - 1) * std::max(0.f, gap);
	const glm::vec2 origin{bounds.rect.x + (size(bounds).x - width) * 0.5f,
		bounds.rect.y + size(bounds).y - cell};
	return rectangle(origin, {std::max(0.f, width), cell});
}

int centeredSlotIndexAt(Clip bounds, int count, float cell, float gap, glm::vec2 point)
{
	const auto strip = centeredSlotStrip(bounds, count, cell, gap);
	return slotIndexAt(slotGrid(strip, count, 1, gap), point);
}

std::string_view formatCount(uint32_t count, char (&buffer)[8])
{
	if (count <= 1)
		return {};
	if (count > 99)
	{
		std::snprintf(buffer, sizeof(buffer), "99+");
		return buffer;
	}
	std::snprintf(buffer, sizeof(buffer), "%u", count);
	return buffer;
}

// --- Tooltip ---------------------------------------------------------------

TooltipLayout tooltipLayout(Clip bounds, glm::vec2 pointer, glm::vec2 extent, float gap)
{
	const glm::vec2 available = size(bounds);
	const glm::vec2 box = glm::min(glm::max(extent, glm::vec2(0.f)), available);
	if (available.x <= 0.f || available.y <= 0.f)
		return {{}, false};
	float x = pointer.x + gap;
	if (x + box.x > bounds.rect.z)
		x = pointer.x - gap - box.x;
	float y = pointer.y - gap - box.y;
	bool below = false;
	if (y < bounds.rect.y)
	{
		y = pointer.y + gap;
		below = true;
	}
	x = std::clamp(x, bounds.rect.x, std::max(bounds.rect.x, bounds.rect.z - box.x));
	y = std::clamp(y, bounds.rect.y, std::max(bounds.rect.y, bounds.rect.w - box.y));
	return {rectangle({x, y}, box), below};
}

glm::vec2 tooltipExtent(FontView font, std::string_view text, const style::Theme &theme, float scale,
						float fontScale)
{
	const float s = scale > 0.f ? scale : 1.f;
	const float glyphs = fontScale > 0.f ? fontScale : 1.f;
	const float width = font.measure(text, style::typeScale(style::TextRole::Caption) * s * glyphs) +
		2.f * theme.space.sm * s;
	return {width, style::lineHeight(style::TextRole::Caption) * s + 2.f * theme.space.xs * s};
}

// --- Toasts ----------------------------------------------------------------

namespace
{
/// Bound a toast field without splitting a UTF-8 sequence.
std::string boundedText(std::string_view text)
{
	if (text.size() <= Toast::kMaxText)
		return std::string(text);
	size_t size = Toast::kMaxText;
	while (size && (static_cast<unsigned char>(text[size]) & 0xc0) == 0x80)
		--size;
	return std::string(text.substr(0, size));
}
} // namespace

bool ToastQueue::push(Toast toast)
{
	toast.title = boundedText(toast.title);
	toast.body = boundedText(toast.body);
	const bool full = m_count == kCapacity;
	const size_t keep = std::min(m_count, kCapacity - 1);
	for (size_t i = keep; i > 0; --i)
		m_toasts[i] = m_toasts[i - 1];
	m_toasts[0] = toast;
	m_toasts[0].age = 0.f;
	m_count = std::min(m_count + 1, size_t(kCapacity));
	return !full;
}

void ToastQueue::advance(float dt)
{
	if (!(dt > 0.f))
		return;
	// R08: age in place. Nothing is copied or moved while no entry expires, and
	// an expiring entry is compacted with moves only (never a self-move).
	size_t write = 0;
	for (size_t read = 0; read < m_count; ++read)
	{
		Toast &entry = m_toasts[read];
		entry.age += dt;
		if (entry.lifetime > 0.f && entry.age >= entry.lifetime)
			continue;
		if (write != read)
			m_toasts[write] = std::move(entry);
		++write;
	}
	// Release the retired slots so their text does not outlive the notification.
	for (size_t index = write; index < m_count; ++index)
		m_toasts[index] = {};
	m_count = write;
}

void ToastQueue::clear()
{
	m_toasts.fill({});
	m_count = 0;
}

bool ToastQueue::dismiss(uint32_t id)
{
	size_t write = 0;
	bool removed = false;
	for (size_t read = 0; read < m_count; ++read)
	{
		if (m_toasts[read].id == id)
		{
			removed = true;
			continue;
		}
		m_toasts[write++] = m_toasts[read];
	}
	m_count = removed ? write : m_count;
	return removed;
}

// --- Confirmation ----------------------------------------------------------

ConfirmationLayout confirmationLayout(Clip bounds, glm::vec2 preferred, bool destructive,
									  const style::Theme &theme, float scale)
{
	(void)destructive; // geometry is identical; only the default focus differs
	const float s = scale > 0.f ? scale : 1.f;
	const auto panel = anchor(bounds, preferred, {0.5f, 0.5f});
	ConfirmationLayout layout{};
	layout.panel = panel;
	const auto body = inset(panel, theme.space.xl * s);
	const float titleHeight = style::lineHeight(style::TextRole::Heading) * s;
	const float buttonHeight = theme.size.buttonHeight * s;
	const float gap = theme.space.md * s;
	layout.title = lineBelow(body, titleHeight);
	layout.confirm = {{body.rect.z - (size(body).x - gap) * 0.5f, body.rect.w - buttonHeight,
		body.rect.z, body.rect.w}};
	layout.cancel = {{body.rect.x, body.rect.w - buttonHeight,
		layout.confirm.rect.x - gap, body.rect.w}};
	layout.body = {{body.rect.x, layout.title.rect.w + theme.space.md * s, body.rect.z,
		layout.cancel.rect.y - theme.space.md * s}};
	return layout;
}

// --- Progress --------------------------------------------------------------

Clip progressFill(Clip track, float value)
{
	return {{track.rect.x, track.rect.y, track.rect.x + size(track).x * progressFraction(value), track.rect.w}};
}

// --- Surface ---------------------------------------------------------------

void Surface::fill(Clip rect, glm::vec4 color, float radius) const
{
	if (color.a <= 0.f || rect.empty())
		return;
	// DrawList has no rounded primitive; a two-step octagon approximates the
	// corner token (see docs/game-ui-design-system.md, renderer follow-up).
	const float r = std::min({radius, size(rect).x * 0.5f, size(rect).y * 0.5f});
	if (r < 1.f)
	{
		m_out.panel(rect.rect, color, m_clip);
		return;
	}
	const float half = std::max(1.f, std::floor(r * 0.5f));
	const auto band = [&](float y0, float y1, float insetX) {
		const float x0 = rect.rect.x + insetX;
		const float x1 = rect.rect.z - insetX;
		// R05: a fractional or very narrow rect must not produce an inverted band.
		if (y1 <= y0 || x1 <= x0)
			return;
		m_out.panel({x0, y0, x1, y1}, color, m_clip);
	};
	band(rect.rect.y, rect.rect.y + half, r);
	band(rect.rect.y + half, rect.rect.y + r, half);
	band(rect.rect.y + r, rect.rect.w - r, 0.f);
	band(rect.rect.w - r, rect.rect.w - half, half);
	band(rect.rect.w - half, rect.rect.w, r);
}

void Surface::ring(Clip rect, glm::vec4 color, float width, float radius) const
{
	(void)radius;
	if (size(rect).x <= 0.f || size(rect).y <= 0.f)
		return;
	// R05: a border can never be wider than the geometry it frames — including a
	// fractional rectangle, where a 1 px minimum would otherwise overflow.
	const float w = std::min({std::max(1.f, width), size(rect).x * 0.5f, size(rect).y * 0.5f});
	if (!(w > 0.f))
		return;
	m_out.panel({rect.rect.x, rect.rect.y, rect.rect.z, rect.rect.y + w}, color, m_clip);
	m_out.panel({rect.rect.x, rect.rect.w - w, rect.rect.z, rect.rect.w}, color, m_clip);
	m_out.panel({rect.rect.x, rect.rect.y + w, rect.rect.x + w, rect.rect.w - w}, color, m_clip);
	m_out.panel({rect.rect.z - w, rect.rect.y + w, rect.rect.z, rect.rect.w - w}, color, m_clip);
}

void Surface::frame(Clip rect, glm::vec4 face, glm::vec4 border, float width, float radius) const
{
	fill(rect, face, radius);
	ring(rect, border, width, radius);
}

void Surface::label(Clip rect, std::string_view text, style::TextRole role, glm::vec4 color) const
{
	// A label is fitted into its own component rect: variable-length text can
	// never overdraw a neighbouring control (issue #302 F12).
	fittedLabel(rect, text, role, color, 0.f);
}

float Surface::fittedWidth(std::string_view text, style::TextRole role, float available,
						   FittedText &fitted) const
{
	const float roleScale = glyphScale(role);
	fitted = fitText(m_font, text, role, m_scale * m_fontScale, available);
	float width = m_font.measure(text.substr(0, fitted.bytes), roleScale);
	if (fitted.elided)
		width += m_font.measure("...", roleScale);
	return width;
}

void Surface::labelAligned(Clip rect, std::string_view text, style::TextRole role, glm::vec4 color,
						   float alignX) const
{
	// R05: an aligned label is fitted into its own rectangle as well, so a
	// right-aligned readout can never spill to the left over its neighbour.
	fittedLabel(rect, text, role, color, alignX);
}

void Surface::fittedLabel(Clip rect, std::string_view text, style::TextRole role, glm::vec4 color,
						  float alignX) const
{
	// R05: a rectangle that cannot show even half a line paints no text at all,
	// instead of a glyph band spilling over the neighbouring component. Half a
	// line is the floor: a badge squeezed below its line height still renders
	// (a quantity is worth reading), a sub-pixel band does not.
	const float line = style::lineHeight(role) * m_scale;
	if (text.empty() || size(rect).x <= 0.f || size(rect).y + 0.001f < line * 0.5f)
		return;
	FittedText fitted{};
	const float width = fittedWidth(text, role, size(rect).x, fitted);
	const float roleScale = glyphScale(role);
	const float x = rect.rect.x + (size(rect).x - width) * std::clamp(alignX, 0.f, 1.f);
	m_font.paint(m_out, {x, centeredLineTop(rect, role, m_scale)}, text.substr(0, fitted.bytes), color,
		roleScale, m_clip);
	if (fitted.elided)
	{
		const float advance = m_font.measure(text.substr(0, fitted.bytes), roleScale);
		m_font.paint(m_out, {x + advance, centeredLineTop(rect, role, m_scale)}, "...", color, roleScale,
			m_clip);
	}
}

void Surface::backdrop(Clip bounds, glm::vec4 color) const
{
	fill(bounds, color, 0.f);
}

void Surface::card(Clip rect, bool raised) const
{
	const auto &p = m_theme.palette;
	glm::vec4 face = raised ? p.surfaceRaised : p.surface;
	face.a = raised ? m_theme.overlay.raised : m_theme.overlay.card;
	frame(rect, face, p.border, px(m_theme.size.borderWidth), px(m_theme.size.radiusMedium));
}

Clip Surface::cardContent(Clip rect) const
{
	return inset(rect, px(m_theme.space.xl));
}

void Surface::sectionHeader(Clip rect, std::string_view title) const
{
	if (size(rect).y < px(m_theme.space.xs))
		return;
	Clip line = {{rect.rect.x, rect.rect.y, rect.rect.z,
		std::min(rect.rect.w, rect.rect.y + style::lineHeight(style::TextRole::Heading) * m_scale)}};
	fittedLabel(line, title, style::TextRole::Heading, m_theme.palette.ink, 0.f);
	const float ruleWidth = px(m_theme.size.borderWidth);
	const float rule = std::min(rect.rect.w - ruleWidth, line.rect.w + px(m_theme.space.xs));
	if (rule >= rect.rect.y)
		fill({{rect.rect.x, rule, rect.rect.z, rule + ruleWidth}}, m_theme.palette.border, 0.f);
}

void Surface::divider(Clip rect) const
{
	const float thickness = px(m_theme.size.borderWidth);
	if (size(rect).y < thickness)
		return;
	const float y = std::min(rect.rect.y + size(rect).y * 0.5f, rect.rect.w - thickness);
	fill({{rect.rect.x, y, rect.rect.z, y + thickness}}, m_theme.palette.border, 0.f);
}

void Surface::button(Clip rect, std::string_view text, style::ButtonVariant variant,
					 style::State state) const
{
	const auto tokens = style::buttonTokens(m_theme, variant, state);
	const float radius = px(m_theme.size.radiusMedium);
	if (tokens.face.a > 0.f)
		fill(rect, tokens.face, radius);
	glm::vec4 border = tokens.border;
	float borderWidth = px(m_theme.size.borderWidth);
	if (style::has(state, style::State::Focused) && !style::disabled(state))
	{
		border = m_theme.palette.focus;
		borderWidth = px(m_theme.size.focusRing);
	}
	if (border.a > 0.f)
		ring(rect, border, borderWidth, radius);
	const float pad = std::min(px(m_theme.space.sm), size(rect).x * 0.25f);
	fittedLabel({{rect.rect.x + pad, rect.rect.y, std::max(rect.rect.x + pad, rect.rect.z - pad),
			rect.rect.w}}, text, style::TextRole::Body, tokens.text, 0.5f);
}

void Surface::listRow(Clip rect, std::string_view title, std::string_view trailing,
					  style::State state) const
{
	const auto tokens = style::rowTokens(m_theme, state);
	if (tokens.face.a > 0.f)
		fill(rect, tokens.face, px(m_theme.size.radiusSmall));
	if (style::has(state, style::State::Selected))
		fill({{rect.rect.x, rect.rect.y, rect.rect.x + px(m_theme.size.borderStrongWidth), rect.rect.w}},
			m_theme.palette.accent, 0.f);
	const float leader = px(m_theme.size.borderStrongWidth) + px(m_theme.space.xs);
	Clip text = {{rect.rect.x + leader, rect.rect.y, rect.rect.z - px(m_theme.space.sm), rect.rect.w}};
	if (!trailing.empty())
	{
		const float width = m_font.measure(trailing, glyphScale(style::TextRole::Caption));
		Clip value = {{std::max(text.rect.x, text.rect.z - width), text.rect.y, text.rect.z, text.rect.w}};
		text.rect.z = std::max(text.rect.x, value.rect.x - px(m_theme.space.sm));
		fittedLabel(value, trailing, style::TextRole::Caption, tokens.trailing, 1.f);
	}
	fittedLabel(text, title, style::TextRole::Body, tokens.text, 0.f);
}

float formFieldHeight(const style::Theme &theme, float scale, bool hasMessage)
{
	const float s = scale > 0.f ? scale : 1.f;
	const float line = style::lineHeight(style::TextRole::Caption) * s;
	return line + theme.size.controlHeight * s + (hasMessage ? line + theme.space.xxs * s : 0.f);
}

void Surface::formField(Clip rect, std::string_view label, std::string_view value,
						style::Validation validation, style::State state,
						std::string_view message) const
{
	const float s = m_scale;
	const float labelHeight = style::lineHeight(style::TextRole::Caption) * s;
	const float messageHeight = labelHeight + m_theme.space.xxs * s;
	const float wellTop = rect.rect.y + labelHeight;
	// Feedback is painted only when the well keeps its control height, so the
	// caption can never land on the field border: size the group with
	// formFieldHeight(..., true) and both always fit.
	const bool showMessage = !message.empty() &&
		rect.rect.w - messageHeight - wellTop >= m_theme.size.controlHeight * s;
	Clip labelLine = lineBelow(rect, labelHeight);
	Clip messageLine = {{rect.rect.x, rect.rect.w - labelHeight, rect.rect.z, rect.rect.w}};
	Clip well = {{rect.rect.x, wellTop, rect.rect.z,
		std::max(wellTop, showMessage ? rect.rect.w - messageHeight : rect.rect.w)}};
	const auto tokens = style::fieldTokens(m_theme, state, validation);
	if (labelLine.rect.w > labelLine.rect.y)
		Surface::label(labelLine, label, style::TextRole::Caption, m_theme.palette.inkMuted);
	fill(well, tokens.face, px(m_theme.size.radiusSmall));
	const float borderWidth = validation == style::Validation::None ? px(m_theme.size.borderWidth)
																   : px(m_theme.size.borderStrongWidth);
	ring(well, tokens.border, borderWidth, px(m_theme.size.radiusSmall));
	Clip inner = inset(well, px(m_theme.space.sm));
	fittedLabel(inner, value, style::TextRole::Body, tokens.text, 0.f);
	if (showMessage && messageLine.rect.y < messageLine.rect.w)
		fittedLabel(messageLine, message, style::TextRole::Caption,
			style::validationColor(m_theme, validation), 0.f);
}

void Surface::statusIndicator(Clip rect, style::Status status, std::string_view text) const
{
	const float dot = px(m_theme.space.sm);
	const float y = rect.rect.y + (size(rect).y - dot) * 0.5f;
	fill({{rect.rect.x, y, rect.rect.x + dot, y + dot}}, style::statusColor(m_theme, status),
		px(m_theme.size.radiusSmall));
	Clip textRect = {{rect.rect.x + dot + px(m_theme.space.sm), rect.rect.y, rect.rect.z, rect.rect.w}};
	fittedLabel(textRect, text, style::TextRole::Body, m_theme.palette.ink, 0.f);
}

void Surface::progress(Clip rect, float value, bool pending, float phase) const
{
	const float radius = px(m_theme.size.radiusSmall);
	fill(rect, m_theme.palette.surfaceSunken, radius);
	const Clip inner = inset(rect, px(m_theme.size.borderWidth));
	if (inner.empty())
		return;
	if (!pending)
	{
		fill(progressFill(inner, value), m_theme.palette.accent, radius);
		return;
	}
	// Indeterminate: one band sweeping left to right on the caller's phase, so
	// the animation stays deterministic and testable. The band is clamped to the
	// track, never painted past it on a very small progress bar (F12).
	const float width = std::min(size(inner).x, std::max(px(8.f), size(inner).x * 0.28f));
	const float travel = std::max(0.f, size(inner).x - width);
	const float x = inner.rect.x + travel * std::clamp(phase, 0.f, 1.f);
	fill({{x, inner.rect.y, x + width, inner.rect.w}}, m_theme.palette.pending, radius);
}

void Surface::toast(Clip rect, const Toast &entry) const
{
	glm::vec4 face = m_theme.palette.surface;
	face.a = m_theme.overlay.toast;
	frame(rect, face, m_theme.palette.border, px(m_theme.size.borderWidth), px(m_theme.size.radiusLarge));
	const float accent = px(m_theme.size.borderStrongWidth) + px(m_theme.space.xxs);
	fill({{rect.rect.x, rect.rect.y, rect.rect.x + accent, rect.rect.w}},
		style::statusColor(m_theme, entry.status), px(m_theme.size.radiusSmall));
	Clip body = inset(rect, px(m_theme.space.md));
	body.rect.x += accent;
	const float titleHeight = style::lineHeight(style::TextRole::Body) * m_scale;
	Clip title = lineBelow(body, titleHeight);
	fittedLabel(title, entry.title, style::TextRole::Body, m_theme.palette.ink, 0.f);
	if (!entry.body.empty() && title.rect.w < body.rect.w)
	{
		Clip detail = {{body.rect.x, title.rect.w, body.rect.z, body.rect.w}};
		fittedLabel(detail, entry.body, style::TextRole::Caption, m_theme.palette.inkMuted, 0.f);
	}
}

void Surface::tooltip(Clip bounds, glm::vec2 pointer, std::string_view text) const
{
	const auto extent = tooltipExtent(m_font, text, m_theme, m_scale, m_fontScale);
	const auto layout = tooltipLayout(bounds, pointer, extent, px(m_theme.space.xs));
	glm::vec4 face = m_theme.palette.surfaceSunken;
	face.a = m_theme.overlay.card;
	frame(layout.rect, face, m_theme.palette.border, px(m_theme.size.borderWidth),
		px(m_theme.size.radiusSmall));
	fittedLabel(inset(layout.rect, px(m_theme.space.xs)), text, style::TextRole::Caption,
		m_theme.palette.ink, 0.f);
}

void Surface::confirmation(const ConfirmationLayout &layout, std::string_view title,
						   std::string_view body) const
{
	glm::vec4 face = m_theme.palette.surface;
	face.a = m_theme.overlay.card;
	frame(layout.panel, face, m_theme.palette.borderStrong, px(m_theme.size.borderStrongWidth),
		px(m_theme.size.radiusLarge));
	fittedLabel(layout.title, title, style::TextRole::Heading, m_theme.palette.ink, 0.f);
	fittedLabel(layout.body, body, style::TextRole::Body, m_theme.palette.inkMuted, 0.f);
}

void Surface::slot(Clip rect, const SlotView &entry) const
{
	const auto &p = m_theme.palette;
	// One resolver for combined states (R06): disabled never paints a focus ring,
	// validation always wins the border color, focus keeps its width.
	const auto tokens = style::slotTokens(m_theme, entry.state);
	frame(rect, tokens.face, tokens.border, px(tokens.borderWidth), px(m_theme.size.radiusSmall));
	const Clip iconRect = inset(rect, px(m_theme.space.xs));
	const uint32_t layers = m_atlasLayers ? m_atlasLayers : UINT32_MAX;
	// Occupancy is `count != 0`; an icon is an optional, generation-tagged
	// handle (F08). Layer 0 is a real texture, a stale handle is not sampled and
	// an occupied slot without a drawable icon keeps its count and its frame.
	const bool occupied = !entry.empty();
	const auto status = entry.icon ? iconStatus(*entry.icon, m_out.atlasGeneration(), layers)
								   : IconStatus::MissingLayer;
	if (occupied && entry.icon && drawable(status))
		m_out.icon(iconRect.rect, *entry.icon, tokens.tint, m_clip);
	else if (occupied)
	{
		// Missing/stale icon fallback: keep the slot, mark it, never drop it.
		fill(iconRect, p.surfaceRaised, px(m_theme.size.radiusSmall));
		labelAligned(iconRect, std::string_view(&kFallbackGlyph, 1), style::TextRole::Body,
			tokens.indicator, 0.5f);
	}

	// Durability/progress owns a reserved band at the bottom; the quantity badge
	// sits above it and always paints a contrast backing (F14).
	const float barInset = px(m_theme.space.xxs);
	const float barHeight = std::max(px(2.f), px(m_theme.size.borderWidth) * 2.f);
	const Clip track = {{rect.rect.x + barInset, rect.rect.w - barHeight - barInset,
		rect.rect.z - barInset, rect.rect.w - barInset}};
	const auto bar = entry.progress ? entry.progress : entry.durability;
	const float bandBottom = (bar ? track.rect.y : rect.rect.w) - barInset;
	char badge[8]{};
	const auto count = formatCount(entry.count, badge);
	if (!count.empty())
	{
		const float roleScale = glyphScale(style::TextRole::Caption);
		const float lineHeight = textStyle(style::TextRole::Caption, m_scale, m_fontScale).lineHeight();
		const float width = std::min(size(rect).x, m_font.measure(count, roleScale) +
			2.f * px(m_theme.space.xxs));
		const float top = std::max(iconRect.rect.y, bandBottom - lineHeight);
		const Clip badgeRect = {{std::max(rect.rect.x, rect.rect.z - width - barInset), top,
			rect.rect.z - barInset, std::max(top, bandBottom)}};
		if (badgeRect.rect.y < badgeRect.rect.w)
		{
			glm::vec4 backing = p.surface;
			backing.a = m_theme.overlay.card;
			fill(badgeRect, backing, px(m_theme.size.radiusSmall));
			fittedLabel(inset(badgeRect, px(m_theme.space.xxs)), count, style::TextRole::Caption,
				tokens.indicator, 1.f);
		}
	}
	if (bar)
	{
		const float value = progressFraction(*bar);
		const glm::vec4 color = entry.progress ? p.accent
			: (value > 0.5f ? p.ok : (value > 0.2f ? p.warning : p.error));
		fill(track, p.surface, 0.f);
		fill(progressFill(track, value), color, 0.f);
	}
}

void Surface::slotGrid(const SlotGrid &grid, std::span<const SlotView> entries, int focused) const
{
	for (int row = 0; row < grid.rows; ++row)
		for (int column = 0; column < grid.columns; ++column)
		{
			const int index = row * grid.columns + column;
			const auto cell = slotCell(grid, column, row);
			if (cell.empty())
				continue;
			SlotView view = index < int(entries.size()) ? entries[size_t(index)] : SlotView{};
			if (index == focused)
				view.state |= style::State::Focused;
			slot(cell, view);
		}
}

void Surface::crosshair(Clip viewport, glm::vec4 color) const
{
	const glm::vec2 center = glm::vec2(viewport.rect) + size(viewport) * 0.5f;
	const float arm = px(m_theme.size.crosshair);
	const float thickness = std::max(1.f, px(m_theme.size.borderWidth));
	fill({{center.x - arm, center.y - thickness * 0.5f, center.x + arm, center.y + thickness * 0.5f}},
		color, 0.f);
	fill({{center.x - thickness * 0.5f, center.y - arm, center.x + thickness * 0.5f, center.y + arm}},
		color, 0.f);
}
} // namespace gameui
