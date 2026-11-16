// Pure stack math (issue #224, Epic #223 Phase A). See ItemStack.hpp for the
// conventions; transferStack is the single capacity arithmetic that merge
// and the inventory compose, so "what fits" cannot drift between paths.

#include <Items/ItemStack.hpp>
#include <Items/ItemRegistry.hpp>

namespace items
{

ItemStack makeStack(ItemId item, uint16_t count)
{
    if (count == 0 || item == ItemId::None)
        return {};
    return {item, count};
}

TransferResult transferStack(ItemStack source, ItemStack destination, uint16_t count)
{
    if (source.isEmpty())
        return {ItemResult::EmptyStack, source, destination};
    const uint32_t capacity = maxStackOf(source.item);
    if (capacity == 0)
        return {ItemResult::InvalidItem, source, destination};
    if (!destination.isEmpty() && maxStackOf(destination.item) == 0)
        return {ItemResult::InvalidItem, source, destination};
    if (count == 0)
        return {ItemResult::Ok, source, destination};
    if (!destination.isEmpty() && destination.item != source.item)
        return {ItemResult::ItemMismatch, source, destination};

    // uint32_t intermediates: count may be a transient oversized input, and
    // destination.count + moved must never wrap uint16_t.
    const uint32_t destCount = destination.count;
    const uint32_t room = destCount >= capacity ? 0u : capacity - destCount;
    const uint32_t want = count < source.count ? count : source.count;
    const uint32_t moved = want < room ? want : room;
    if (moved == 0)
        return {ItemResult::Full, source, destination};

    const ItemResult result = moved == want ? ItemResult::Ok : ItemResult::Partial;
    return {result,
            makeStack(source.item, static_cast<uint16_t>(source.count - moved)),
            makeStack(source.item, static_cast<uint16_t>(destCount + moved))};
}

MergeResult mergeStacks(ItemStack destination, ItemStack source)
{
    // A merge is a whole-source transfer; reuse the one capacity arithmetic.
    const TransferResult transferred = transferStack(source, destination, source.count);
    return {transferred.result, transferred.destination, transferred.source};
}

SplitResult splitStack(ItemStack stack, uint16_t count)
{
    if (stack.isEmpty())
        return {ItemResult::EmptyStack, {}, {}};
    if (maxStackOf(stack.item) == 0)
        return {ItemResult::InvalidItem, {}, {}};
    const uint16_t taken = count < stack.count ? count : stack.count;
    return {ItemResult::Ok,
            makeStack(stack.item, taken),
            makeStack(stack.item, static_cast<uint16_t>(stack.count - taken))};
}

SplitResult splitHalf(ItemStack stack)
{
    if (stack.isEmpty())
        return {ItemResult::EmptyStack, {}, {}};
    // Odd counts: the TAKEN half is the larger (ceil), the source keeps floor.
    const uint16_t half = static_cast<uint16_t>((static_cast<uint32_t>(stack.count) + 1u) / 2u);
    return splitStack(stack, half);
}

} // namespace items
