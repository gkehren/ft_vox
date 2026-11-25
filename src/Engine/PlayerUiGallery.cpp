#include <Engine/PlayerUiGallery.hpp>

#include <Engine/PlayerUiComponents.hpp>

#include <algorithm>

namespace gameui
{
std::span<const GallerySection> gallerySections()
{
	// Heights include the header band (galleryHeaderHeight) plus the content the
	// screen paints; the tests assert each section is tall enough for its own
	// title so a gallery can never overlap itself.
	static constexpr GallerySection sections[] = {
		{"Design tokens", 92.f},
		{"Typography", 124.f},
		{"Buttons", 62.f},
		{"List rows", 148.f},
		{"Form and validation", 148.f},
		{"Status and progress", 112.f},
		{"Notifications and tooltip", 116.f},
		{"Confirmation", 216.f},
		{"Inventory slots", 120.f},
		{"Player UI scale", 104.f},
	};
	return sections;
}

float galleryHeaderHeight(const style::Theme &theme, float scale)
{
	const float s = scale > 0.f ? scale : 1.f;
	return style::lineHeight(style::TextRole::Heading) * s + theme.space.md * s;
}

GalleryLayout galleryLayout(Clip view, const style::Theme &theme, float scale)
{
	const float s = scale > 0.f ? scale : 1.f;
	const float spacing = theme.space.lg * s;
	const auto sections = gallerySections();
	const float width = std::min(1200.f * s, size(view).x);
	// Two columns once the authored card is wide enough for two readable ones;
	// a small window keeps a single scrolling column.
	const int columns = width >= 900.f * s ? 2 : 1;
	const size_t perColumn = (sections.size() + size_t(columns) - 1) / size_t(columns);
	float tallest = 0.f;
	for (size_t column = 0; column < size_t(columns); ++column)
	{
		float height = 0.f;
		for (size_t index = column * perColumn; index < std::min(sections.size(), (column + 1) * perColumn); ++index)
			height += sections[index].height * s + spacing;
		tallest = std::max(tallest, height - (height > 0.f ? spacing : 0.f));
	}
	const glm::vec2 content{width, tallest + 2.f * theme.space.xl * s};
	const auto panel = anchor(view, content, {0.5f, 0.5f});
	GalleryLayout layout{};
	layout.view = view;
	layout.panel = panel;
	layout.body = inset(panel, theme.space.xl * s);
	layout.scale = s;
	layout.contentHeight = tallest;
	layout.sectionSpacing = spacing;
	layout.columns = columns;
	layout.perColumn = perColumn;
	layout.columnWidth = columns > 1 ? (size(layout.body).x - spacing) * 0.5f : size(layout.body).x;
	return layout;
}

Clip gallerySectionRect(const GalleryLayout &layout, const style::Theme &, size_t index)
{
	const auto sections = gallerySections();
	if (index >= sections.size() || layout.perColumn == 0)
		return {};
	const float s = layout.scale > 0.f ? layout.scale : 1.f;
	const size_t column = index / layout.perColumn;
	const size_t first = column * layout.perColumn;
	float y = layout.body.rect.y;
	for (size_t i = first; i < index; ++i)
		y += sections[i].height * s + layout.sectionSpacing;
	const float x = layout.body.rect.x + float(column) * (layout.columnWidth + layout.sectionSpacing);
	return {{x, y, x + layout.columnWidth, y + sections[index].height * s}};
}

HudMockLayout hudMockLayout(Clip view, const style::Theme &theme, float scale, int hotbarSlots)
{
	const float s = scale > 0.f ? scale : 1.f;
	const float pad = theme.space.lg * s;
	const float gap = theme.space.xs * s;
	HudMockLayout layout{};
	layout.view = view;
	layout.hotbarSlots = std::max(1, hotbarSlots);
	layout.hotbarGap = theme.size.slotGap * s;
	layout.hotbarCell = centeredSlotSize(view, layout.hotbarSlots, layout.hotbarGap,
		theme.size.slot * s);
	layout.hotbar = centeredSlotStrip({{view.rect.x, view.rect.y + pad, view.rect.z,
		view.rect.w - theme.size.hotbarBottom * s}}, layout.hotbarSlots, layout.hotbarCell,
		layout.hotbarGap);

	// Half the usable width each (plus the centre gap), so the two top stacks
	// can never collide, from a small window to 21:9.
	const float halfWidth = std::max(0.f, (size(view).x - 3.f * pad) * 0.5f);
	const float stackWidth = std::min(280.f * s, halfWidth);
	const float stackHeight = style::lineHeight(style::TextRole::Body) * s * 2.f + pad;
	layout.status = {{view.rect.x + pad, view.rect.y + pad, view.rect.x + pad + stackWidth,
		view.rect.y + pad + stackHeight}};
	layout.toast = {{view.rect.z - pad - stackWidth, view.rect.y + pad, view.rect.z - pad,
		view.rect.y + pad + stackHeight}};

	const float promptWidth = std::min(360.f * s, std::max(0.f, size(view).x - 2.f * pad));
	const float promptHeight = style::lineHeight(style::TextRole::Body) * s + 2.f * theme.space.xxs * s;
	const float promptBottom = std::max(view.rect.y + pad + stackHeight + pad,
		layout.hotbar.rect.y - gap);
	layout.target = {{view.rect.x + (size(view).x - promptWidth) * 0.5f, promptBottom - promptHeight,
		view.rect.x + (size(view).x + promptWidth) * 0.5f, promptBottom}};

	const float crosshair = theme.size.crosshair * s * 2.f;
	const glm::vec2 center = glm::vec2(view.rect) + size(view) * 0.5f;
	layout.crosshair = rectangle(center - glm::vec2(crosshair) * 0.5f, {crosshair, crosshair});

	const auto inside = [&](Clip rect) {
		return rect.rect.x >= view.rect.x - 0.01f && rect.rect.y >= view.rect.y - 0.01f &&
			rect.rect.z <= view.rect.z + 0.01f && rect.rect.w <= view.rect.w + 0.01f &&
			rect.rect.x < rect.rect.z && rect.rect.y < rect.rect.w;
	};
	layout.fits = inside(layout.status) && inside(layout.toast) && inside(layout.target) &&
		inside(layout.hotbar) && inside(layout.crosshair);
	return layout;
}
} // namespace gameui
