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

/// Held-stack placement (PR #316 F03, bounds F10): pure geometry,
/// unit-testable. The stack prefers the usual anchor + offset presentation;
/// approaching the right or bottom edge of `view` flips the offset, and the
/// final rectangle is clamped fully inside `view`. The effective cell never
/// exceeds the available view — a view strictly smaller than the requested
/// cell shrinks the square (its aspect ratio preserved) instead of
/// overhanging the clamped edge, and a view with no room at all places
/// nothing. Compact and ultrawide layouts stay inside the game view and the
/// central Developer UI dock area (which bounds `view`).
inline Clip heldStackPlacement(glm::vec2 anchor, float cell, float offset, Clip view)
{
	if (!(cell > 0.f) || view.empty())
		return {};
	const auto available = size(view);
	const float effectiveCell = std::min({cell, available.x, available.y});
	if (!(effectiveCell > 0.f))
		return {};
	glm::vec2 position{anchor.x + offset, anchor.y + offset};
	const glm::vec2 flipped{anchor.x - offset - effectiveCell, anchor.y - offset - effectiveCell};
	const glm::vec2 lowest{view.rect.x, view.rect.y};
	const glm::vec2 highest{view.rect.z - effectiveCell, view.rect.w - effectiveCell};
	if (position.x > highest.x && flipped.x >= lowest.x)
		position.x = flipped.x;
	if (position.y > highest.y && flipped.y >= lowest.y)
		position.y = flipped.y;
	position = glm::clamp(position, lowest, glm::max(lowest, highest));
	return rectangle(position, {effectiveCell, effectiveCell});
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
	Clip panel, title, instructions, status, recovery, hotbarLabel, close, drop, detail;
	Clip feedback, reader, previousPage, nextPage;
	SlotGrid main, hotbar;
	float scale{};
	bool compact{};

	Clip slot(inventoryui::SlotId id) const;
	inventoryui::SlotId hitSlot(glm::vec2 point) const;
};

InventoryLayout inventoryLayout(Clip bounds, const style::Theme &theme, float scale);
style::State inventorySlotState(const InventoryView &view, inventoryui::SlotId id,
	bool hovered, bool pressed, bool focused);
SlotView makeItemSlot(items::ItemStack stack, std::optional<uint64_t> atlasGeneration);
} // namespace gameui
