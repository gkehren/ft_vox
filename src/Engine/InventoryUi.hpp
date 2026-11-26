#pragma once

// =============================================================================
// Pure inventory slot presentation and click planning, unit-testable without
// SDL or Vulkan. The screen sends plans through the host's command adapters.
//
// STRICT boundary (epic #223 §5): nothing here mutates authoritative state.
// The drag cursor is a view of the source slot. Closing the panel discards it
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
#include <optional>

namespace inventoryui
{

inline constexpr int kColumns = 9;
inline constexpr int kMainRows = 3; // 27 main slots above the hotbar row
inline constexpr int kSlotCount = items::kInventorySize;

using SlotArray = std::array<items::ItemStack, kSlotCount>;

struct SlotId
{
    int index{-1};
    constexpr bool valid() const { return index >= 0 && index < kSlotCount; }
    constexpr bool operator==(const SlotId &) const = default;
};

// The visual grid starts at the first main slot and ends with the hotbar.
constexpr SlotId slotAtCell(int cell)
{
    return SlotId{cell >= 0 && cell < kSlotCount ? (cell + items::kHotbarSize) % kSlotCount : -1};
}

constexpr int cellOfSlot(SlotId slot)
{
    return slot.valid() ? (slot.index + kColumns * kMainRows) % kSlotCount : -1;
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
    SlotId sourceSlot;
    DragMode mode{DragMode::None};
    bool active() const { return sourceSlot.valid() && mode != DragMode::None; }
};

/// The stack the cursor DISPLAYS: the source stack (Whole) or its larger
/// half (Half — the same ceil half the splitStack command moves). Reads the
/// snapshot; copies nothing authoritative.
inline items::ItemStack cursorStack(const DragState &drag, const SlotArray &slots)
{
    if (!drag.active())
        return {};
    const items::ItemStack src = slots[static_cast<size_t>(drag.sourceSlot.index)];
    if (drag.mode == DragMode::Whole)
        return src;
    return items::splitHalf(src).taken;
}

/// The stack a slot DISPLAYS while a drag is active: the source shows what
/// would remain after the drop (Whole -> empty, Half -> floor half); every
/// other slot is unchanged. Authoritative state is untouched until a command.
inline items::ItemStack displayedSlotStack(SlotId slot, const DragState &drag, const SlotArray &slots)
{
    if (!slot.valid())
        return {};
    const items::ItemStack src = slots[static_cast<size_t>(slot.index)];
    if (!drag.active() || slot != drag.sourceSlot)
        return src;
    if (drag.mode == DragMode::Whole)
        return {};
    return items::splitHalf(src).remainder;
}

// --- Click planning (pure; the panel issues the planned command) --------------

enum class Command : uint8_t
{
    None,
    Move,  // moveStack(from, to)
    Swap,  // swapStacks(from, to)
    Split, // splitStack(from, to) — moves the ceil half
    Select,
    DropOne,
    DropStack
};

struct Action
{
    Command command{Command::None};
    SlotId from;
    SlotId to;
    bool affects(SlotId slot) const { return slot == from || slot == to; }
};

/// Pickup: clicking an occupied slot with no active drag starts a display
/// drag (left = whole, right = half). NO command — the stack stays
/// authoritative in its slot until a drop target is clicked.
inline DragState planPickup(SlotId slot, bool rightButton, const SlotArray &slots)
{
    if (!slot.valid() || slots[static_cast<size_t>(slot.index)].isEmpty())
        return {};
    return {slot, rightButton ? DragMode::Half : DragMode::Whole};
}

/// Drop: clicking a target with an active drag (see the model above).
/// Right-click, re-clicking the source, out-of-range targets and empty
/// sources cancel without a command.
inline Action planDrop(const DragState &drag, SlotId target, bool rightButton, const SlotArray &slots)
{
    if (!drag.active() || !target.valid() || rightButton || target == drag.sourceSlot)
        return {};
    const items::ItemStack src = slots[static_cast<size_t>(drag.sourceSlot.index)];
    const items::ItemStack dst = slots[static_cast<size_t>(target.index)];
    if (src.isEmpty())
        return {};
    const bool compatible = dst.isEmpty() || dst.item == src.item;
    if (drag.mode == DragMode::Whole)
        return {compatible ? Command::Move : Command::Swap, drag.sourceSlot, target};
    if (compatible)
        return {Command::Split, drag.sourceSlot, target};
    return {};
}

struct Snapshot
{
    SlotArray slots{};
    SlotId selected;
    uint64_t revision{}; // ordered private revision on the wire, zero for solo
    bool operator==(const Snapshot &) const = default;
};

// One screen owns this state. Every input edge uses the host's current view;
// remote snapshots are already scoped and monotonic at the transport boundary.
class Interaction
{
public:
    bool observe(const Snapshot *snapshot, uint64_t epoch, bool enabled)
    {
        const std::optional<Snapshot> next = snapshot ? std::optional{*snapshot} : std::nullopt;
        enabled = enabled && snapshot;
        const bool changed = m_epoch != epoch || m_snapshot != next || m_enabled != enabled;
        if (changed)
            cancel();
        m_epoch = epoch;
        m_snapshot = next;
        m_enabled = enabled;
        return changed;
    }

    Action activate(SlotId slot, bool half)
    {
        if (!m_enabled)
            return {};
        if (!m_drag.active())
        {
            m_drag = planPickup(slot, half, m_snapshot->slots);
            return {};
        }
        const auto action = planDrop(m_drag, slot, half, m_snapshot->slots);
        cancel();
        return action;
    }

    void cancel() { m_drag = {}; }
    const DragState &drag() const { return m_drag; }

private:
    std::optional<Snapshot> m_snapshot;
    uint64_t m_epoch{};
    bool m_enabled{};
    DragState m_drag;
};

} // namespace inventoryui
