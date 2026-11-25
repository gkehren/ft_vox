#pragma once

// =============================================================================
// Pure inventory presentation logic (issue #229, Epic #223 Phase F): slot
// layout math, drag-cursor display and click planning. Header-only and
// ImGui-free so the policy is unit-testable headlessly (the InputRouting.hpp
// / UiShortcuts.hpp pattern; covered by test_ui_shell).
//
// STRICT boundary (epic #223 §5): nothing here mutates authoritative state.
// The panel issues simulation commands through GameUIFrame callbacks; the
// drag cursor is UI-LOCAL PRESENTATION — a VIEW of the source slot, never a
// copy of authoritative data. Closing the panel mid-drag discards the cursor
// and the source slot still shows its stack: no duplication, no loss.
//
// Interaction model (v1, documented):
//   * left-click an occupied slot  -> pick up the WHOLE stack (display);
//   * right-click an occupied slot -> pick up HALF (ceil, the splitStack half);
//   * left-click a target while holding:
//       - empty / same item -> moveStack (Whole) or splitStack (Half);
//       - different item    -> swapStacks (Whole) or cancel (Half);
//   * right-click / re-click source / close panel -> cancel (no command).
// Every authoritative effect is exactly one of the exposed sim commands.
// =============================================================================

#include <Items/ItemStack.hpp>
#include <Items/ItemTypes.hpp>

#include <array>
#include <cstdint>

namespace inventoryui
{

inline constexpr int kColumns = 9;
inline constexpr int kMainRows = 3; // 27 main slots above the hotbar row
inline constexpr int kSlotCount = items::kInventorySize;

using SlotArray = std::array<items::ItemStack, kSlotCount>;

// --- Layout math (pixel space; the caller applies UI scale) ------------------

struct SlotRect
{
    float x{0}, y{0}, w{0}, h{0};
};

/// Inventory-panel grid: main slots 9..35 as three rows of nine on top,
/// hotbar slots 0..8 as the bottom row (extra gap above it, MC-style).
inline SlotRect panelSlotRect(int slot, float originX, float originY, float slotSize, float spacing)
{
    const float pitch = slotSize + spacing;
    int row = 0, col = 0;
    float extraGap = 0.0f;
    if (slot < items::kHotbarSize)
    {
        row = kMainRows;
        col = slot;
        extraGap = spacing * 2.0f;
    }
    else
    {
        const int main = slot - items::kHotbarSize;
        row = main / kColumns;
        col = main % kColumns;
    }
    return {originX + float(col) * pitch, originY + float(row) * pitch + extraGap, slotSize, slotSize};
}

/// Panel content size for the 3+1 rows (used to size the child region).
inline float panelWidth(float slotSize, float spacing)
{
    return float(kColumns) * slotSize + float(kColumns - 1) * spacing;
}
inline float panelHeight(float slotSize, float spacing)
{
    return float(kMainRows + 1) * slotSize + float(kMainRows) * spacing + spacing * 2.0f;
}

// --- Drag cursor (UI-local presentation state) --------------------------------

enum class DragMode : uint8_t
{
    None,
    Whole,
    Half
};

struct DragState
{
    int sourceSlot{-1};
    DragMode mode{DragMode::None};
    bool active() const { return sourceSlot >= 0 && mode != DragMode::None; }
};

/// The stack the cursor DISPLAYS: the source stack (Whole) or its larger
/// half (Half — the same ceil half the splitStack command moves). Reads the
/// snapshot; copies nothing authoritative.
inline items::ItemStack cursorStack(const DragState &drag, const SlotArray &slots)
{
    if (!drag.active() || drag.sourceSlot >= kSlotCount)
        return {};
    const items::ItemStack src = slots[static_cast<size_t>(drag.sourceSlot)];
    if (drag.mode == DragMode::Whole)
        return src;
    return items::splitHalf(src).taken;
}

/// The stack a slot DISPLAYS while a drag is active: the source shows what
/// would remain after the drop (Whole -> empty, Half -> floor half); every
/// other slot is unchanged. Authoritative state is untouched until a command.
inline items::ItemStack displayedSlotStack(int slot, const DragState &drag, const SlotArray &slots)
{
    if (slot < 0 || slot >= kSlotCount)
        return {};
    const items::ItemStack src = slots[static_cast<size_t>(slot)];
    if (!drag.active() || slot != drag.sourceSlot)
        return src;
    if (drag.mode == DragMode::Whole)
        return {};
    return items::splitHalf(src).remainder;
}

// --- Click planning (pure; the panel issues the planned command) --------------

enum class ClickCommand : uint8_t
{
    None,
    Move,  // moveStack(from, to)
    Swap,  // swapStacks(from, to)
    Split  // splitStack(from, to) — moves the ceil half
};

struct ClickPlan
{
    ClickCommand command{ClickCommand::None};
    int from{-1};
    int to{-1};
};

/// Pickup: clicking an occupied slot with no active drag starts a display
/// drag (left = whole, right = half). NO command — the stack stays
/// authoritative in its slot until a drop target is clicked.
inline DragState planPickup(int slot, bool rightButton, const SlotArray &slots)
{
    if (slot < 0 || slot >= kSlotCount || slots[static_cast<size_t>(slot)].isEmpty())
        return {};
    return {slot, rightButton ? DragMode::Half : DragMode::Whole};
}

/// Drop: clicking a target with an active drag (see the model above).
/// Right-click, re-clicking the source, out-of-range targets and empty
/// sources cancel without a command.
inline ClickPlan planDrop(const DragState &drag, int target, bool rightButton, const SlotArray &slots)
{
    if (!drag.active() || drag.sourceSlot >= kSlotCount || target < 0 || target >= kSlotCount ||
        rightButton || target == drag.sourceSlot)
        return {};
    const items::ItemStack src = slots[static_cast<size_t>(drag.sourceSlot)];
    const items::ItemStack dst = slots[static_cast<size_t>(target)];
    if (src.isEmpty())
        return {};
    const bool compatible = dst.isEmpty() || dst.item == src.item;
    if (drag.mode == DragMode::Whole)
        return {compatible ? ClickCommand::Move : ClickCommand::Swap, drag.sourceSlot, target};
    if (compatible)
        return {ClickCommand::Split, drag.sourceSlot, target};
    return {};
}

} // namespace inventoryui
