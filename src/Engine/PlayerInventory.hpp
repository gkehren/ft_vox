#pragma once

#include <Engine/InventoryUi.hpp>
#include <Engine/PlayerUiComponents.hpp>

namespace gameui
{
enum class InventoryAvailability { Ready, Joining, Resyncing, Disconnected };

/// The last meaningful interaction device the player used on the inventory
/// screen (PR #316 F05): the held-stack preview follows it. Presentation-only
/// state owned by the screen; reset to Pointer by every gesture cancellation
/// (screen close, session reset, focus loss, workspace handoff). Passive
/// pointer events — motion, hover — never change it.
enum class InventoryModality : uint8_t { Pointer, Keyboard };

/// Held-stack placement (PR #316 F03): pure geometry, unit-testable. The
/// stack prefers the usual anchor + offset presentation; approaching the
/// right or bottom edge of `view` flips the offset, and the final rectangle
/// is clamped fully inside `view`. A view smaller than the cell collapses
/// onto the clamped edge, so compact layouts keep the icon inside the game
/// view and the central Developer UI dock area (which bounds `view`).
inline Clip heldStackPlacement(glm::vec2 anchor, float cell, float offset, Clip view)
{
	if (!(cell > 0.f) || view.empty())
		return {};
	glm::vec2 position{anchor.x + offset, anchor.y + offset};
	const glm::vec2 flipped{anchor.x - offset - cell, anchor.y - offset - cell};
	const glm::vec2 lowest{view.rect.x, view.rect.y};
	const glm::vec2 highest{view.rect.z - cell, view.rect.w - cell};
	if (position.x > highest.x && flipped.x >= lowest.x)
		position.x = flipped.x;
	if (position.y > highest.y && flipped.y >= lowest.y)
		position.y = flipped.y;
	position = glm::clamp(position, lowest, glm::max(lowest, highest));
	return rectangle(position, {cell, cell});
}

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
