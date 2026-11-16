#pragma once

// =============================================================================
// Inventory — the fixed 36-slot authoritative container (issue #224, Epic
// #223 Phase A). Hotbar = slots 0..8, main inventory = slots 9..35, plus the
// selected hotbar index.
//
// Deterministic operations with explicit ItemResult codes (the future
// command/rejection vocabulary, epic #223 §3/§6):
//   * addStack    — hotbar-first MERGE into same-item stacks, then hotbar-
//                   first fill of EMPTY slots; partial adds return the
//                   remainder (the pickup path leaves it in the world).
//                   Oversized input counts are distributed across slots.
//   * removeCount — take up to N from a slot (clamped, canonical empty at 0);
//   * moveStack   — whole-stack move/merge into another slot;
//   * swapStacks  — unconditional exchange (any items, incl. empty);
//   * splitStack  — move the LARGER half (ceil, odd counts defined) of a
//                   slot into another slot with transfer semantics;
//   * selectedStack / setSelectedSlot — hotbar selection, bounds-checked.
//
// Pure value semantics: a std::array of 4-byte stacks + one int, trivially
// copyable, NO heap churn per operation, no UI/sim/render types. The
// simulation facade owns it from #225; the UI drag cursor is presentation-
// local (#229) — authoritative moves happen only through these operations.
// =============================================================================

#include <Items/ItemTypes.hpp>

#include <array>
#include <cstdint>

namespace items
{

class Inventory
{
  public:
    // --- Read access (bounded, plain values) ---

    const std::array<ItemStack, kInventorySize> &slots() const { return m_slots; }

    /// Slot contents (bounds-checked in debug; use slots() for iteration).
    const ItemStack &stack(int slot) const;

    int selectedSlot() const { return m_selectedSlot; }
    ItemStack selectedStack() const { return m_slots[static_cast<size_t>(m_selectedSlot)]; }

    /// Total count of `item` across all 36 slots (the future place-from-
    /// hotbar availability query; uint32_t so bulk grants cannot wrap).
    uint32_t totalCount(ItemId item) const;

    // --- Deterministic mutations (explicit results) ---

    /// Add a stack: merge into same-item slots hotbar-first (0..8, then
    /// 9..35), then fill empty slots in the same order. Ok = everything
    /// added; Partial = remainder returned; Full = nothing added (remainder
    /// == input); InvalidItem = unknown item (nothing added). An empty input
    /// is an Ok no-op. Input counts above maxStack are distributed.
    AddResult addStack(ItemStack stack);

    /// Take up to `count` items from `slot` (clamped to what holds; taking
    /// more than available is Ok). The slot normalizes to the canonical
    /// empty stack when drained. count 0 is an Ok no-op.
    RemoveResult removeCount(int slot, uint16_t count);

    /// Move the whole `from` stack into `to`: empty destination receives it,
    /// same item merges (Partial keeps the remainder in `from`, Full moves
    /// nothing), different items -> ItemMismatch. from == to is an Ok no-op.
    ItemResult moveStack(int from, int to);

    /// Exchange two slots unconditionally (different items and empty slots
    /// included). a == b is an Ok no-op.
    ItemResult swapStacks(int a, int b);

    /// Move the larger half (ceil(count / 2), odd counts defined) of `from`
    /// into `to` with transfer semantics (empty/same-item destination;
    /// ItemMismatch / Full / Partial as in moveStack). from == to is an Ok
    /// no-op; an empty source answers EmptyStack.
    ItemResult splitStack(int from, int to);

    /// Select a hotbar slot (0..kHotbarSize-1); out of range -> InvalidSlot,
    /// selection unchanged.
    ItemResult setSelectedSlot(int slot);

    /// Reset to the fresh state: 36 canonical empty stacks, selection 0.
    void clear();

  private:
    std::array<ItemStack, kInventorySize> m_slots{};
    int m_selectedSlot{0};
};

static_assert(std::is_trivially_copyable_v<Inventory>,
              "Inventory must stay a plain value (no heap, no hidden state)");

} // namespace items
