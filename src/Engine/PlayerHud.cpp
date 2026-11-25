#include <Engine/PlayerHud.hpp>
#include <Items/ItemRegistry.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

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

HudSession hudSession(HudSessionState state, bool ready, std::string_view label,
	std::string_view detail)
{
	using enum HudSessionState;
	HudSession out;
	// Blocked gameplay always gets the same synchronization copy, whichever
	// transport row caused it (F07): no bar, no reticle, no stale inventory.
	const auto synchronizing = [&] {
		out.status = "Synchronizing session";
		out.kind = style::Status::Pending;
		out.activity = "Waiting for the authoritative player, inventory and terrain.";
		out.recovery = "The hotbar returns when the session is ready.";
	};
	switch (state)
	{
	case Connected:
		if (!ready)
		{
			synchronizing();
			break;
		}
		// Normal HUD: no headline competes with the bar or the reticle.
		out.showHotbar = out.allowAim = true;
		break;
	case Pending:
		out.status = label;
		out.kind = style::Status::Pending;
		out.activity = detail;
		break;
	case Degraded:
		if (!ready)
		{
			synchronizing();
			break;
		}
		// Playable but unhealthy: normal HUD, one warning line.
		out.status = label;
		out.kind = style::Status::Warning;
		out.activity = detail;
		out.showHotbar = out.allowAim = true;
		break;
	case Blocked:
		out.status = label;
		out.kind = style::Status::Error;
		out.activity = detail.empty() ? std::string_view{"The session ended."} : detail;
		out.recovery = "Reconnect: Esc, Multiplayer, Reconnect.";
		break;
	}
	return out;
}

HudLayout hudLayout(Clip scene, Clip safeArea, const style::Theme &theme, float scale)
{
	HudLayout layout;
	layout.scene = scene;
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
	const float detailLine = (style::lineHeight(style::TextRole::Body) + theme.space.xxs) * s;
	const float width = std::min(size(body).x, 360.f * s);
	const auto top = glm::vec2{body.rect.x + (size(body).x - width) * 0.5f, body.rect.y};
	if (!(width > 0.f) || !(line > 0.f))
		return layout;

	layout.status = rectangle(top, {width, line});
	// Selection and hints stack up from the bar, exactly as before.
	const auto selection = glm::vec2{top.x, layout.hotbar.bounds.rect.y - gap - line};
	layout.selection = rectangle(selection, {width, line});
	layout.hint = rectangle(selection - glm::vec2{0.f, line + gap}, {width, line});

	// Bounded feedback block between the headline and the optional hint line: the
	// detail shrinks to one line before the recovery instruction is dropped, so
	// the essential information survives a compact resolution while the feedback
	// can never reach the bar.
	const float below = layout.status.rect.w + gap;
	const float ceiling = layout.hint.rect.y - gap;
	const int fits = int(std::floor((ceiling - below - gap - line) / std::max(detailLine, 0.001f)));
	layout.detailLines = std::clamp(fits, 0, kHudDetailLines);
	if (layout.detailLines > 0)
		layout.activity = rectangle({top.x, below}, {width, float(layout.detailLines) * detailLine});
	const float recoveryTop = (layout.detailLines > 0 ? layout.activity.rect.w : below) + gap;
	const Clip recovery = rectangle({top.x, recoveryTop}, {width, line});
	if (recovery.rect.w <= layout.hint.rect.y)
		layout.recovery = recovery;

	// The reticle is the full-window camera projection's exact centre, whatever
	// the docks do. Resolving whether it is painted (and what yields to it) is
	// content-dependent and lives in resolveHud.
	if (!scene.empty())
	{
		const glm::vec2 aim = glm::vec2(scene.rect) + size(scene) * 0.5f;
		const float arm = (theme.size.crosshair + theme.size.borderWidth) * s;
		layout.reticle = rectangle(aim - glm::vec2(arm), glm::vec2(2.f * arm));
	}
	return layout;
}

HudPaint resolveHud(const HudLayout &layout, const HudView &view)
{
	HudPaint paint;
	if (layout.scale <= 0.f)
		return paint;

	// The reticle only disappears when the aim point itself is unavailable:
	// outside the unobstructed area (a dock covers it) or under the mandatory
	// hotbar. Empty bands never enter this decision.
	const bool aimable = layout.reticle.intersect(layout.view) == layout.reticle;
	const bool clearOfBar = !view.hotbar.available || layout.reticle.intersect(layout.hotbar.bounds).empty();
	paint.reticle = view.aim && !layout.reticle.empty() && aimable && clearOfBar;

	const auto yields = [&](Clip band) {
		return paint.reticle && !(layout.reticle.intersect(band).empty());
	};
	paint.status = !view.status.empty() && !yields(layout.status);
	paint.activity = !view.activity.empty() && layout.detailLines > 0 && !yields(layout.activity);
	paint.recovery = !view.recovery.empty() && !yields(layout.recovery);
	paint.selection = view.hotbar.available && !yields(layout.selection);
	paint.hint = view.hints && !yields(layout.hint);
	return paint;
}

std::string_view creativeLabel(std::string_view block, char (&buffer)[48])
{
	constexpr std::string_view prefix = "Creative: ";
	constexpr std::string_view bare = "Creative";
	const size_t capacity = sizeof(buffer) - 1;
	const std::string_view head = block.empty() ? bare : prefix;
	size_t written = 0;
	const auto append = [&](std::string_view text) {
		const size_t count = std::min(text.size(), capacity - written);
		std::memcpy(buffer + written, text.data(), count);
		written += count;
	};
	append(head);
	append(block);
	buffer[written] = '\0';
	return {buffer, written};
}

void paintHud(const Surface &surface, const HudLayout &layout, const HudView &view)
{
	using enum style::TextRole;
	if (layout.scale <= 0.f || layout.view.empty())
		return;
	const HudPaint paint = resolveHud(layout, view);
	const auto &theme = surface.theme();
	const float insetX = surface.px(theme.space.sm);
	// One contrast plate for every piece of copy (issue #303 F03): the hudScrim
	// token keeps text readable against midday sky and dark caves instead of
	// relying on the scene behind it.
	const glm::vec4 plateFace = theme.palette.hudScrim;
	const auto plate = [&](Clip band) {
		if (!band.empty())
			surface.fill(band, plateFace, surface.px(theme.size.radiusSmall));
	};
	// Horizontal padding only: the band already owns exactly the height its
	// role's line (or reserved lines) needs, and a vertical inset would clip the
	// glyphs instead of the backing.
	const auto padded = [](Clip band, float amount) {
		return Clip{{band.rect.x + amount, band.rect.y, band.rect.z - amount, band.rect.w}};
	};
	const auto caption = [&](Clip band, std::string_view text, style::TextRole role) {
		if (text.empty() || band.empty())
			return;
		const float width = std::min(size(band).x,
			surface.font().measure(text, surface.glyphScale(role)) + 2.f * insetX);
		const auto backing = anchor(band, {width, size(band).y}, {0.5f, 0.5f});
		plate(backing);
		surface.fittedLabel(inset(backing, surface.px(theme.space.xxs)), text, role,
			theme.palette.ink, 0.5f);
	};

	if (paint.reticle)
		surface.crosshair(layout.scene, theme.palette.ink, theme.palette.surfaceSunken);

	if (paint.status && !layout.status.empty())
	{
		// The plate follows the headline, so a short status does not paint a wide
		// bar over the scene; the marker and its gap are part of the budget, which
		// leaves the fitted label exactly the text it measured.
		const float marker = std::min(surface.px(theme.space.sm), size(layout.status).y);
		const float width = marker + surface.px(theme.space.sm) + 2.f * insetX +
			surface.font().measure(view.status, surface.glyphScale(Body));
		const auto band = anchor(layout.status, {width, size(layout.status).y}, {0.5f, 0.5f});
		plate(band);
		surface.statusIndicator(padded(band, insetX), view.statusKind, view.status);
	}
	if (paint.activity && !layout.activity.empty())
	{
		plate(layout.activity);
		surface.wrappedLabel(padded(layout.activity, insetX), view.activity, Body,
			theme.palette.ink, layout.detailLines);
	}
	if (paint.recovery)
		caption(layout.recovery, view.recovery, Caption);

	if (!view.hotbar.available)
		return;

	surface.slotGrid(layout.hotbar, view.hotbar.slots);
	if (paint.selection && !layout.selection.empty())
	{
		char buffer[48];
		const auto label = view.creative ? creativeLabel(view.creativeBlock, buffer)
										 : view.hotbar.selectedName;
		caption(layout.selection, label, Body);
	}
	if (paint.hint && !layout.hint.empty())
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
