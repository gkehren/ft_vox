#pragma once

#include <Engine/InventoryUi.hpp>
#include <Engine/PlayerUiComponents.hpp>

namespace gameui
{
enum class InventoryAvailability { Ready, Joining, Resyncing, Disconnected };

struct InventoryView
{
	std::optional<inventoryui::Snapshot> snapshot;
	uint64_t epoch{};
	InventoryAvailability availability{InventoryAvailability::Joining};
	bool actionPending{};
	bool canDrop{};
	std::optional<inventoryui::Action> pendingSlots;
	std::string_view feedback;
	style::Status feedbackKind{style::Status::Info};
	std::string_view recovery;

	bool enabled() const
	{
		return snapshot && availability == InventoryAvailability::Ready && !actionPending;
	}
};

struct InventoryLayout
{
	Clip panel, title, instructions, status, hotbarLabel, close, drop, detail;
	SlotGrid main, hotbar;
	float scale{};

	Clip slot(inventoryui::SlotId id) const;
	inventoryui::SlotId hitSlot(glm::vec2 point) const;
};

InventoryLayout inventoryLayout(Clip bounds, const style::Theme &theme, float scale);
SlotView makeItemSlot(items::ItemStack stack, std::optional<uint64_t> atlasGeneration);
} // namespace gameui
