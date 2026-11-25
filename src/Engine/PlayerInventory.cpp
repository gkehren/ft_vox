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
	const float padding = theme.space.md;
	const float mainHeight = kMainRows * cell + (kMainRows - 1) * gap;
	const float panelWidth = gridWidth + 2.f * padding;
	const float panelHeight = 2.f * padding + mainHeight + cell + 188.f;
	InventoryLayout out;
	const auto space = size(bounds);
	out.scale = std::min({scale, space.x / (panelWidth + 2.f * padding),
		space.y / (panelHeight + 2.f * padding)});
	if (out.scale <= 0.f)
		return out;

	const float s = out.scale;
	const glm::vec2 extent{panelWidth * s, panelHeight * s};
	const glm::vec2 top = glm::vec2(bounds.rect) + (space - extent) * 0.5f;
	const glm::vec2 body = top + glm::vec2(padding * s);
	out.panel = rectangle(top, extent);
	const auto row = [&](float y, float height, float x = 0.f, float width = 0.f) {
		return rectangle(body + glm::vec2(x, y) * s, glm::vec2(width > 0.f ? width : gridWidth, height) * s);
	};
	out.title = row(0.f, 30.f, 0.f, gridWidth - 80.f);
	out.close = row(0.f, 30.f, gridWidth - 76.f, 76.f);
	out.instructions = row(36.f, 36.f);
	out.main = {row(78.f, mainHeight), kColumns, kMainRows, cell * s, gap * s};
	const float hotbarTop = 78.f + mainHeight + 29.f;
	out.hotbarLabel = row(hotbarTop - 25.f, 21.f);
	out.hotbar = {row(hotbarTop, cell), kColumns, 1, cell * s, gap * s};
	out.detail = row(hotbarTop + cell + 5.f, 20.f, 0.f, gridWidth - 168.f);
	out.drop = row(hotbarTop + cell + 5.f, 30.f, gridWidth - 164.f, 164.f);
	out.status = row(hotbarTop + cell + 37.f, 42.f);
	return out;
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
