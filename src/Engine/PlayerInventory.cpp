#include <Engine/PlayerInventory.hpp>
#include <Items/ItemRegistry.hpp>

namespace gameui
{
using namespace inventoryui;

InventoryLayout inventoryLayout(Clip bounds, const style::Theme &theme, float scale)
{
	const float cell = theme.size.slot;
	const float gap = theme.size.slotGap;
	const float gridWidth = kColumns * cell + (kColumns - 1) * gap;
	const float padding = theme.space.xl;
	const float mainHeight = kMainRows * cell + (kMainRows - 1) * gap;
	const float panelWidth = gridWidth + 2.f * padding;
	const auto space = size(bounds);
	const float margin = theme.space.md;
	const bool compact = space.y < (340.f + 2.f * margin) * scale;
	const float panelHeight = compact ? 254.f : 340.f;
	InventoryLayout out;
	out.compact = compact;
	out.scale = std::min({scale, space.x / (panelWidth + 2.f * margin),
		space.y / (panelHeight + 2.f * margin)});
	if (out.scale <= 0.f)
		return out;

	const float s = out.scale;
	const glm::vec2 extent{panelWidth * s, panelHeight * s};
	const glm::vec2 top = glm::vec2(bounds.rect) + (space - extent) * 0.5f;
	out.panel = rectangle(top, extent);
	const auto row = [&](float y, float height, float x, float width) {
		return rectangle(top + glm::vec2(padding + x, y) * s, glm::vec2(width, height) * s);
	};
	const float closeWidth = 58.f, dropWidth = 156.f;
	const float titleTop = compact ? 4.f : 12.f;
	const float feedbackWidth = 100.f;
	out.title = row(titleTop, style::lineHeight(style::TextRole::Title), 0.f,
		gridWidth - closeWidth - feedbackWidth - 2.f * gap);
	out.close = row(titleTop, theme.size.buttonHeight, gridWidth - closeWidth, closeWidth);
	out.feedback = row(titleTop, theme.size.buttonHeight, gridWidth - closeWidth - feedbackWidth - gap,
		feedbackWidth);
	const float mainTop = compact ? 34.f : 60.f;
	out.main = {row(mainTop, mainHeight, 0.f, gridWidth), kColumns, kMainRows, cell * s, gap * s};
	const float caption = style::lineHeight(style::TextRole::Caption);
	const float hotbarTop = mainTop + mainHeight + caption + (compact ? 2.f : 12.f);
	out.hotbarLabel = row(hotbarTop - caption, caption, 0.f, 66.f);
	out.hotbar = {row(hotbarTop, cell, 0.f, gridWidth), kColumns, 1, cell * s, gap * s};
	const float pageTop = hotbarTop + cell - theme.size.buttonHeight;
	const float pageWidth = 88.f;
	out.reader = row(mainTop, pageTop - mainTop - gap, 0.f, gridWidth);
	out.previousPage = row(pageTop, theme.size.buttonHeight, 0.f, pageWidth);
	out.nextPage = row(pageTop, theme.size.buttonHeight, gridWidth - pageWidth, pageWidth);
	const float actions = hotbarTop + cell + (compact ? gap : theme.space.md);
	out.detail = row(actions, theme.size.buttonHeight, 0.f, gridWidth - dropWidth - theme.space.md);
	out.drop = row(actions, theme.size.buttonHeight, gridWidth - dropWidth, dropWidth);
	if (compact)
	{
		out.status = row(actions, 2.f * caption + theme.space.xxs, 0.f, gridWidth - dropWidth - theme.space.md);
		out.recovery = row(hotbarTop - caption, caption, 66.f, gridWidth - 66.f);
	}
	else
	{
		const float footerTop = actions + theme.size.buttonHeight + gap;
		out.instructions = out.status = row(footerTop, 2.f * caption + theme.space.xxs, 0.f, gridWidth);
		out.recovery = row(footerTop + 2.f * caption + theme.space.xxs, caption, 0.f, gridWidth);
	}
	return out;
}

style::State inventorySlotState(const InventoryView &view, SlotId id, bool hovered, bool pressed, bool focused)
{
	const bool selected = view.snapshot && view.snapshot->selected == id;
	if (view.snapshot && view.availability == InventoryAvailability::Ready &&
		view.actionPending && view.pendingSlots && view.pendingSlots->affects(id))
		return style::State::Pending | style::State::Unavailable |
			(selected ? style::State::Selected : style::State::Rest);
	return style::interactiveState(view.enabled(), hovered, pressed, focused, selected);
}

Clip InventoryLayout::slot(SlotId id) const
{
	if (!id.valid())
		return {};
	const int cell = cellOfSlot(id);
	const int row = cell / kColumns;
	return row < kMainRows ? slotCell(main, cell % kColumns, row)
		: slotCell(hotbar, cell % kColumns, 0);
}

SlotId InventoryLayout::hitSlot(glm::vec2 point) const
{
	const int mainIndex = slotIndexAt(main, point);
	if (mainIndex >= 0)
		return slotAtCell(mainIndex);
	const int hotbarIndex = slotIndexAt(hotbar, point);
	return hotbarIndex >= 0 ? slotAtCell(kColumns * kMainRows + hotbarIndex) : SlotId{};
}

SlotView makeItemSlot(items::ItemStack stack, std::optional<uint64_t> atlasGeneration)
{
	SlotView slot;
	const auto *item = items::definition(stack.item);
	if (item && stack.count > 0 && stack.count <= item->maxStack)
	{
		slot.count = stack.count;
		slot.label = item->name;
		if (atlasGeneration && item->placeableBlock)
			slot.icon = AtlasIcon{*atlasGeneration, uint32_t(*item->placeableBlock)};
	}
	return slot;
}
} // namespace gameui
