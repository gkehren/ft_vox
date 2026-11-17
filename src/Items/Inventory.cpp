// Inventory — fixed 36-slot authoritative container (issue #224, Epic #223
// Phase A). See Inventory.hpp for the operation contracts. Every mutation is
// deterministic integer arithmetic over the inline slot array: no heap, no
// RNG, no wall clock — repeated sequences produce byte-equal state.

#include <Items/Inventory.hpp>
#include <Items/ItemRegistry.hpp>
#include <Items/ItemStack.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <utility>

namespace items
{

ItemStack Inventory::stack(int slot) const
{
    if (slot < 0 || slot >= kInventorySize)
        return {};
    return m_slots[static_cast<std::size_t>(slot)];
}

uint32_t Inventory::totalCount(ItemId item) const
{
    uint32_t total = 0;
    for (const ItemStack &slot : m_slots)
    {
        if (slot.item == item)
            total += slot.count;
    }
    return total;
}

AddResult Inventory::addStack(ItemStack stack)
{
    if (stack.isEmpty())
        return {ItemResult::Ok, {}};
    const uint32_t capacity = maxStackOf(stack.item);
    if (capacity == 0)
        return {ItemResult::InvalidItem, stack};

    uint32_t remaining = stack.count;

    // Phase 1 — hotbar-first MERGE into same-item stacks. Ascending slot
    // order is the hotbar-first order: hotbar 0..8 precedes main 9..35.
    for (ItemStack &slot : m_slots)
    {
        if (remaining == 0)
            break;
        if (slot.isEmpty() || slot.item != stack.item || slot.count >= capacity)
            continue;
        const uint32_t moved = std::min<uint32_t>(remaining, capacity - slot.count);
        slot.count = static_cast<uint16_t>(slot.count + moved);
        remaining -= moved;
    }

    // Phase 2 — hotbar-first fill of EMPTY slots with the remainder.
    for (ItemStack &slot : m_slots)
    {
        if (remaining == 0)
            break;
        if (!slot.isEmpty())
            continue;
        const uint32_t placed = std::min<uint32_t>(remaining, capacity);
        slot = makeStack(stack.item, static_cast<uint16_t>(placed));
        remaining -= placed;
    }

    const ItemStack remainder = makeStack(stack.item, static_cast<uint16_t>(remaining));
    if (remaining == 0)
        return {ItemResult::Ok, remainder};
    if (remaining == stack.count)
        return {ItemResult::Full, remainder};
    return {ItemResult::Partial, remainder};
}

RemoveResult Inventory::removeCount(int slot, uint16_t count)
{
    if (slot < 0 || slot >= kInventorySize)
        return {ItemResult::InvalidSlot, {}};
    ItemStack &held = m_slots[static_cast<std::size_t>(slot)];
    if (held.isEmpty())
        return {ItemResult::EmptyStack, {}};
    const uint16_t taken = count < held.count ? count : held.count;
    const ItemStack removed = makeStack(held.item, taken);
    held = makeStack(held.item, static_cast<uint16_t>(held.count - taken));
    return {ItemResult::Ok, removed};
}

ItemResult Inventory::moveStack(int from, int to)
{
    if (from < 0 || from >= kInventorySize || to < 0 || to >= kInventorySize)
        return ItemResult::InvalidSlot;
    if (from == to)
        return ItemResult::Ok;
    const TransferResult transferred =
        transferStack(m_slots[static_cast<std::size_t>(from)], m_slots[static_cast<std::size_t>(to)],
                      m_slots[static_cast<std::size_t>(from)].count);
    if (transferred.result == ItemResult::Ok || transferred.result == ItemResult::Partial)
    {
        m_slots[static_cast<std::size_t>(from)] = transferred.source;
        m_slots[static_cast<std::size_t>(to)] = transferred.destination;
    }
    return transferred.result;
}

ItemResult Inventory::swapStacks(int a, int b)
{
    if (a < 0 || a >= kInventorySize || b < 0 || b >= kInventorySize)
        return ItemResult::InvalidSlot;
    if (a == b)
        return ItemResult::Ok;
    std::swap(m_slots[static_cast<std::size_t>(a)], m_slots[static_cast<std::size_t>(b)]);
    return ItemResult::Ok;
}

ItemResult Inventory::splitStack(int from, int to)
{
    if (from < 0 || from >= kInventorySize || to < 0 || to >= kInventorySize)
        return ItemResult::InvalidSlot;
    if (from == to)
        return ItemResult::Ok;
    const ItemStack &held = m_slots[static_cast<std::size_t>(from)];
    if (held.isEmpty())
        return ItemResult::EmptyStack;
    // Odd counts: the moved half is the larger (ceil), matching splitHalf.
    const uint16_t half = static_cast<uint16_t>((static_cast<uint32_t>(held.count) + 1u) / 2u);
    const TransferResult transferred = transferStack(held, m_slots[static_cast<std::size_t>(to)], half);
    if (transferred.result == ItemResult::Ok || transferred.result == ItemResult::Partial)
    {
        m_slots[static_cast<std::size_t>(from)] = transferred.source;
        m_slots[static_cast<std::size_t>(to)] = transferred.destination;
    }
    return transferred.result;
}

ItemResult Inventory::setSelectedSlot(int slot)
{
    if (slot < 0 || slot >= kHotbarSize)
        return ItemResult::InvalidSlot;
    m_selectedSlot = slot;
    return ItemResult::Ok;
}

void Inventory::clear()
{
    m_slots = {};
    m_selectedSlot = 0;
}

void Inventory::restore(const std::array<ItemStack, kInventorySize> &slots, int selectedSlot)
{
    assert(selectedSlot >= 0 && selectedSlot < kHotbarSize);
    for ([[maybe_unused]] const ItemStack &slot : slots)
    {
        assert(slot.isEmpty() ? slot == ItemStack{}
                              : maxStackOf(slot.item) != 0 && slot.count <= maxStackOf(slot.item));
    }
    m_slots = slots;
    m_selectedSlot = selectedSlot;
}

} // namespace items
