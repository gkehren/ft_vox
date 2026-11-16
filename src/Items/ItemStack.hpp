#pragma once

// =============================================================================
// Pure stack math (issue #224, Epic #223 Phase A).
//
// Merge / split / transfer on ItemStack as PURE FUNCTIONS with explicit
// result codes and plain-value answers — no container, no simulation, no
// heap. The inventory (Inventory.hpp) composes them, and the future
// pickup/drop paths (#226) reuse the exact same arithmetic, so "what fits"
// has ONE definition everywhere (epic #223 §1/§6: deterministic rules,
// explicit results, replication-ready values).
//
// Conventions (pinned by tests/test_items.cpp):
//   * canonical empty: any stack whose count reaches 0 becomes {None, 0};
//   * capacity: maxStackOf(item) from the registry (0 for unknown items);
//   * clamped availability is Ok, not an error: taking/transferring more
//     than the source holds moves all available items;
//   * capacity-limited moves are Partial (remainder returned) or Full
//     (nothing moved);
//   * odd half-split: the TAKEN half is the larger — ceil(count / 2) leaves
//     the source, floor(count / 2) stays (a 5-stack splits 3 + 2).
// =============================================================================

#include <Items/ItemTypes.hpp>

namespace items
{

/// Canonical stack constructor: count 0 (or the None sentinel) collapses to
/// the canonical empty stack {None, 0}. Counts above maxStack are legal as
/// transient operation inputs (addStack distributes them); stored slots never
/// exceed maxStack.
ItemStack makeStack(ItemId item, uint16_t count);

/// Merge `source` into `destination` up to the item's maxStack.
/// Same item required (ItemMismatch otherwise); an empty destination accepts
/// any item. Answers with the updated destination (`merged`) and the source
/// remainder. Ok = source drained; Partial = capacity-limited; Full = the
/// destination is already at capacity; EmptyStack = nothing to merge.
MergeResult mergeStacks(ItemStack destination, ItemStack source);

/// Take up to `count` items out of `stack` (clamped to the available count —
/// requesting more than holds is Ok and takes everything). `taken` is the
/// removed part, `remainder` what stays. count 0 is an Ok no-op.
SplitResult splitStack(ItemStack stack, uint16_t count);

/// Split `stack` in half: taken = ceil(count / 2) (the LARGER half on odd
/// counts), remainder = floor(count / 2). Empty source -> EmptyStack.
SplitResult splitHalf(ItemStack stack);

/// Move up to `count` items from `source` into `destination` (merge
/// semantics: empty destination accepts any item, same item merges up to
/// maxStack, different items -> ItemMismatch). Ok = the requested amount (or
/// all available items, whichever is smaller) moved; Partial = the
/// destination filled first; Full = destination at capacity, nothing moved.
TransferResult transferStack(ItemStack source, ItemStack destination, uint16_t count);

} // namespace items
