#include <Engine/PlayerHud.hpp>
#include <Items/ItemRegistry.hpp>

#include <algorithm>

namespace gameui
{
HudHotbar makeHudHotbar(std::span<const items::ItemStack> stacks, int selected,
	uint64_t atlasGeneration, bool pending)
{
	HudHotbar view;
	if (stacks.size() < view.slots.size())
		return view;

	view.available = true;
	for (int i = 0; i < items::kHotbarSize; ++i)
	{
		const auto stack = stacks[size_t(i)];
		auto &slot = view.slots[size_t(i)];
		const auto *item = items::definition(stack.item);
		const bool occupied = item && stack.count > 0 && stack.count <= item->maxStack;
		if (occupied)
		{
			slot.count = stack.count;
			slot.label = item->name;
			if (item->placeableBlock)
				slot.icon = AtlasIcon{atlasGeneration, uint32_t(*item->placeableBlock)};
		}
		if (i == selected)
		{
			slot.state |= style::State::Selected;
			if (pending)
				slot.state |= style::State::Pending;
			view.selectedName = occupied ? slot.label : "Empty slot";
			view.canPlace = occupied && item->placeableBlock.has_value();
		}
	}
	return view;
}

HudLayout hudLayout(Clip scene, Clip safeArea, const style::Theme &theme, float scale)
{
	HudLayout layout;
	layout.view = scene.intersect(safeArea);
	const auto extent = size(layout.view);
	constexpr int count = items::kHotbarSize;
	const float stripWidth = count * theme.size.slot + (count - 1) * theme.size.slotGap;
	const float padding = theme.space.sm;
	layout.scale = std::min({scale, extent.x / (stripWidth + 2.f * padding), extent.y / 180.f});
	if (layout.scale <= 0.f)
		return layout;

	const float s = layout.scale;
	const float gap = theme.size.slotGap * s;
	const Clip body = inset(layout.view, padding * s);
	Clip slots = body;
	slots.rect.w = layout.view.rect.w - theme.size.hotbarBottom * s;
	layout.hotbar = {centeredSlotStrip(slots, count, theme.size.slot * s, gap),
		count, 1, theme.size.slot * s, gap};

	const float line = (style::lineHeight(style::TextRole::Body) + 2.f * theme.space.xxs) * s;
	const float width = std::min(size(body).x, 360.f * s);
	const auto top = glm::vec2{body.rect.x + (size(body).x - width) * 0.5f, body.rect.y};
	layout.status = rectangle(top, {width, line});
	layout.activity = rectangle(top + glm::vec2{0.f, line + gap}, {width, line});
	const auto selection = glm::vec2{top.x, layout.hotbar.bounds.rect.y - gap - line};
	layout.selection = rectangle(selection, {width, line});
	layout.hint = rectangle(selection - glm::vec2{0.f, line + gap}, {width, line});

	const glm::vec2 aim = glm::vec2(scene.rect) + size(scene) * 0.5f;
	const float arm = (theme.size.crosshair + theme.size.borderWidth) * s;
	const auto reticle = rectangle(aim - glm::vec2(arm), glm::vec2(2.f * arm));
	if (reticle.intersect(body) == reticle &&
		reticle.intersect(layout.status).empty() && reticle.intersect(layout.activity).empty() &&
		reticle.intersect(layout.selection).empty() && reticle.intersect(layout.hotbar.bounds).empty())
		layout.reticle = reticle;
	return layout;
}

void paintHud(const Surface &surface, const HudLayout &layout, Clip scene, const HudView &view)
{
	using enum style::TextRole;
	const auto &theme = surface.theme();
	const auto caption = [&](Clip band, std::string_view text, style::TextRole role) {
		if (text.empty())
			return;
		const float insetX = surface.px(theme.space.sm);
		const float width = surface.font().measure(text, surface.glyphScale(role)) + 2.f * insetX;
		const auto plate = anchor(band, {width, size(band).y}, {0.5f, 0.5f});
		auto backing = theme.palette.surfaceSunken;
		backing.a = theme.overlay.card;
		surface.fill(plate, backing, surface.px(theme.size.radiusSmall));
		surface.fittedLabel(inset(plate, surface.px(theme.space.xxs)), text, role, theme.palette.ink, 0.5f);
	};
	if (!view.status.empty())
		surface.statusIndicator(layout.status, view.statusKind, view.status);
	caption(view.status.empty() ? layout.status : layout.activity, view.activity, Caption);
	const bool showAim = view.aim && !layout.reticle.empty();
	if (showAim)
		surface.crosshair(scene, theme.palette.ink, theme.palette.surfaceSunken);
	if (!view.hotbar.available)
		return;

	surface.slotGrid(layout.hotbar, view.hotbar.slots);
	caption(layout.selection, view.creative ? view.creativeBlock : view.hotbar.selectedName, Body);
	if (view.hints && (!showAim || layout.hint.intersect(layout.reticle).empty()))
	{
		std::string_view hint = "C capture mouse";
		if (view.captured)
		{
			if (view.creative)
				hint = "Creative flight  |  T cycle  |  V walk";
			else if (view.target)
				hint = view.hotbar.canPlace ? "LMB break  |  RMB place  |  Q drop" : "LMB break  |  E inventory";
			else
				hint = "1-9 / wheel select  |  E inventory";
		}
		caption(layout.hint, hint, Caption);
	}
}
} // namespace gameui
