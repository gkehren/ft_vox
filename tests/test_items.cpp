// CPU-only item domain acceptance (issue #224, Epic #223 Phase A).
//
// Proves the issue #224 acceptance list with NO graphics runtime: this target
// links ONLY ft_vox_core (transitively glm + FastNoise2 + Threads) and includes
// no SDL / Vulkan / ImGui header. It exercises a symbol from every new Items
// translation unit so the linker must resolve them all from the CPU-only
// library:
//
//   ItemRegistry.cpp -> itemDefinitions / definition / itemForBlock /
//                       dropForBlock / isBreakableBlock / parseItemId
//   ItemStack.cpp    -> makeStack / mergeStacks / splitStack / splitHalf /
//                       transferStack
//   Inventory.cpp    -> addStack / removeCount / moveStack / swapStacks /
//                       splitStack / selection / clear
//
// Coverage: registry invariants (unique ids/names, append-only ordinals,
// placeable round-trips, drop-table totality over breakable blocks, name
// parsing round-trips), stack math edges (overflow at maxStack, partial
// merge, odd split, empty stacks, uint16_t boundaries) and inventory edges
// (full-inventory partial add, hotbar-first ordering, selection bounds,
// move/swap/split result matrix, deterministic repeated sequences).

#include <Items/Inventory.hpp>
#include <Items/ItemRegistry.hpp>
#include <Items/ItemStack.hpp>
#include <Items/ItemTypes.hpp>
#include <utils.hpp>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>

namespace
{
int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char *msg)
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        std::printf("  FAIL: %s\n", msg);
    }
}

// --- Plain-value contracts (epic #223 §1/§6: no hidden state, no heap) -------
static_assert(items::kHotbarSize == 9, "hotbar is 9 slots");
static_assert(items::kInventorySize == 36, "inventory is 36 slots (9 + 27)");
static_assert(sizeof(items::ItemStack) == 4, "ItemStack is a 4-byte plain value");
static_assert(std::is_trivially_copyable_v<items::ItemStack>);
static_assert(std::is_trivially_copyable_v<items::ItemDefinition>);
static_assert(std::is_trivially_copyable_v<items::Inventory>);
static_assert(std::is_trivially_copyable_v<items::AddResult>);
static_assert(std::is_trivially_copyable_v<items::RemoveResult>);
static_assert(std::is_trivially_copyable_v<items::MergeResult>);
static_assert(std::is_trivially_copyable_v<items::SplitResult>);
static_assert(std::is_trivially_copyable_v<items::TransferResult>);

using items::ItemId;
using items::ItemResult;
using items::ItemStack;

constexpr ItemStack S(ItemId id, uint16_t count)
{
    return {id, count};
}

constexpr ItemStack kEmpty{}; // canonical empty {None, 0}

// The render-only face variants and their canonical voxel blocks (mirrors the
// ItemRegistry.cpp mapping; pinned row by row so the two cannot drift).
constexpr struct
{
    TextureType variant;
    TextureType canonical;
} kFaceVariants[] = {
    {GRASS_SIDE, GRASS_TOP},
    {OAK_LOG_TOP, OAK_LOG},
    {BIRCH_LOG_TOP, BIRCH_LOG},
    {SPRUCE_LOG_TOP, SPRUCE_LOG},
    {JUNGLE_LOG_TOP, JUNGLE_LOG},
    {ACACIA_LOG_TOP, ACACIA_LOG},
    {DARK_OAK_LOG_TOP, DARK_OAK_LOG},
    {CHERRY_LOG_TOP, CHERRY_LOG},
    {MANGROVE_LOG_TOP, MANGROVE_LOG},
    {MANGROVE_ROOTS_TOP, MANGROVE_ROOTS},
    {BAMBOO_BLOCK_TOP, BAMBOO_BLOCK},
    {BASALT_TOP, BASALT},
    {CACTUS_TOP, CACTUS},
    {DEEPSLATE_TOP, DEEPSLATE},
};

bool isFaceVariant(TextureType type)
{
    for (const auto &row : kFaceVariants)
        if (row.variant == type)
            return true;
    return false;
}

std::string upperUnderscore(const std::string &in)
{
    std::string out;
    for (const char c : in)
    {
        if (c == ' ' || c == '-')
            out.push_back('_');
        else
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

// --- Registry invariants ------------------------------------------------------

void testRegistryOrdinalsAndNames()
{
    const auto defs = items::itemDefinitions();
    check(defs.size() == items::kItemCount, "registry exposes exactly kItemCount definitions");

    bool aligned = true;
    bool named = true;
    bool stacked = true;
    for (std::size_t i = 0; i < defs.size(); ++i)
    {
        aligned = aligned && static_cast<uint16_t>(defs[i].id) == static_cast<uint16_t>(i + 1);
        named = named && defs[i].name != nullptr && defs[i].name[0] != '\0';
        stacked = stacked && defs[i].maxStack == items::kDefaultMaxStack;
    }
    check(aligned, "append-only ordinals: definition index i <-> ItemId(i+1), no gaps");
    check(named, "every definition has a non-empty stable name");
    check(stacked, "v1 catalog: every item stacks to the 64 default");

    // Save/wire-stable ordinal pins (the runtime mirror of the header
    // static_asserts — a reordered catalog breaks these immediately).
    check(static_cast<uint16_t>(ItemId::Bedrock) == 1, "Bedrock ordinal pinned at 1");
    check(static_cast<uint16_t>(ItemId::GrassBlock) == 6, "GrassBlock ordinal pinned at 6");
    check(static_cast<uint16_t>(ItemId::Stone) == 14, "Stone ordinal pinned at 14");
    check(static_cast<uint16_t>(ItemId::LilyPad) == items::kItemCount,
          "last ordinal matches kItemCount (append-only)");

    bool uniqueNames = true;
    bool uniqueNormalized = true;
    for (std::size_t i = 0; i < defs.size(); ++i)
        for (std::size_t j = i + 1; j < defs.size(); ++j)
        {
            const std::string a = defs[i].name;
            const std::string b = defs[j].name;
            uniqueNames = uniqueNames && a != b;
            uniqueNormalized = uniqueNormalized && upperUnderscore(a) != upperUnderscore(b);
        }
    check(uniqueNames, "item names are unique");
    check(uniqueNormalized, "normalized item tokens are unique (bijective name parsing)");
}

void testPlaceableRoundTrips()
{
    bool roundTrip = true;
    bool allPlaceable = true;
    for (const items::ItemDefinition &def : items::itemDefinitions())
    {
        allPlaceable = allPlaceable && def.placeableBlock.has_value();
        if (!def.placeableBlock.has_value())
            continue;
        const TextureType block = *def.placeableBlock;
        roundTrip = roundTrip && items::itemForBlock(block) == def.id;
        roundTrip = roundTrip && items::placeableBlockOf(def.id) == block;
        roundTrip = roundTrip && items::isPlaceable(def.id);
        roundTrip = roundTrip && !isFaceVariant(block); // canonical voxel types only
    }
    check(allPlaceable, "v1 catalog: every item is a placeable block item");
    check(roundTrip, "every placeable item maps back to its block (itemForBlock/placeableBlockOf)");

    check(items::placeableBlockOf(ItemId::GrassBlock) == GRASS_TOP,
          "GrassBlock places the canonical GRASS_TOP voxel type");
    check(!items::isPlaceable(ItemId::None), "None is not placeable");
    check(items::definition(ItemId::None) == nullptr, "definition(None) is null");
    check(items::definition(static_cast<ItemId>(items::kItemCount + 1)) == nullptr,
          "definition(out-of-range) is null");
    check(items::definition(static_cast<ItemId>(0xFFFF)) == nullptr,
          "definition(0xFFFF) is null");
    check(items::itemName(ItemId::None).empty(), "itemName(None) is empty");
    check(items::maxStackOf(ItemId::None) == 0, "maxStackOf(None) is 0");
    check(items::maxStackOf(static_cast<ItemId>(0xFFFF)) == 0, "maxStackOf(unknown) is 0");
    check(items::maxStackOf(ItemId::Stone) == 64, "maxStackOf(Stone) is 64");
}

void testBlockItemCoverage()
{
    bool total = true;
    bool fluidsExcluded = true;
    for (int i = 0; i < static_cast<int>(COUNT); ++i)
    {
        const TextureType block = static_cast<TextureType>(i);
        const ItemId item = items::itemForBlock(block);
        if (block == WATER || block == LAVA)
            fluidsExcluded = fluidsExcluded && item == ItemId::None;
        else
            total = total && item != ItemId::None; // incl. BEDROCK + face variants
    }
    check(total, "every non-fluid atlas block (incl. variants) maps to a block item");
    check(fluidsExcluded, "WATER/LAVA have no hand-breakable item in v1");
    check(items::itemForBlock(AIR) == ItemId::None, "AIR has no item");
    check(items::itemForBlock(COUNT) == ItemId::None, "COUNT ordinal has no item");
    check(items::itemForBlock(static_cast<TextureType>(255)) == ItemId::None,
          "out-of-range block has no item");

    bool aliases = true;
    for (const auto &row : kFaceVariants)
    {
        aliases = aliases && items::itemForBlock(row.variant) == items::itemForBlock(row.canonical);
        aliases = aliases && items::isBreakableBlock(row.variant) == items::isBreakableBlock(row.canonical);
        aliases = aliases && items::dropForBlock(row.variant) == items::dropForBlock(row.canonical);
    }
    check(aliases, "face variants alias their canonical block (item/breakable/drop)");
}

void testDropTable()
{
    // Totality: breakable <-> exactly one valid placeable drop item; the
    // breakable set is every atlas block except BEDROCK/WATER/LAVA themselves
    // (face variants inherit their canonical block's breakability).
    bool total = true;
    bool breakableSet = true;
    for (int i = 0; i < static_cast<int>(COUNT); ++i)
    {
        const TextureType block = static_cast<TextureType>(i);
        const bool expectedBreakable = block != BEDROCK && block != WATER && block != LAVA;
        breakableSet = breakableSet && items::isBreakableBlock(block) == expectedBreakable;

        const ItemStack drop = items::dropForBlock(block);
        if (items::isBreakableBlock(block))
        {
            total = total && !drop.isEmpty() && drop.count == 1;
            total = total && items::definition(drop.item) != nullptr;
            total = total && items::isPlaceable(drop.item);
        }
        else
        {
            total = total && drop.isEmpty();
        }
    }
    check(breakableSet, "breakable set = canonical blocks minus BEDROCK/WATER/LAVA");
    check(total, "drop table is total over breakable blocks (1x placeable item, else empty)");

    // The epic's explicit decisions.
    check(items::dropForBlock(STONE) == S(ItemId::Cobblestone, 1), "STONE drops COBBLESTONE");
    check(items::dropForBlock(GRASS_TOP) == S(ItemId::Dirt, 1), "grass block drops DIRT");
    check(items::dropForBlock(GRASS_SIDE) == S(ItemId::Dirt, 1), "grass side variant drops DIRT");
    check(!items::isBreakableBlock(BEDROCK) && items::dropForBlock(BEDROCK).isEmpty(),
          "BEDROCK is unbreakable with no drop");
    check(!items::isBreakableBlock(WATER) && items::dropForBlock(WATER).isEmpty(),
          "WATER is not breakable by hand");
    check(!items::isBreakableBlock(LAVA) && items::dropForBlock(LAVA).isEmpty(),
          "LAVA is not breakable by hand");
    check(!items::isBreakableBlock(AIR) && items::dropForBlock(AIR).isEmpty(),
          "AIR is not breakable");
    check(items::dropForBlock(OAK_LEAVES) == S(ItemId::OakLeaves, 1) &&
              items::dropForBlock(BIRCH_LEAVES) == S(ItemId::BirchLeaves, 1) &&
              items::dropForBlock(SPRUCE_LEAVES) == S(ItemId::SpruceLeaves, 1) &&
              items::dropForBlock(JUNGLE_LEAVES) == S(ItemId::JungleLeaves, 1) &&
              items::dropForBlock(ACACIA_LEAVES) == S(ItemId::AcaciaLeaves, 1) &&
              items::dropForBlock(DARK_OAK_LEAVES) == S(ItemId::DarkOakLeaves, 1) &&
              items::dropForBlock(CHERRY_LEAVES) == S(ItemId::CherryLeaves, 1) &&
              items::dropForBlock(MANGROVE_LEAVES) == S(ItemId::MangroveLeaves, 1),
          "v1 leaves drop themselves (all 8 species, explicit)");
    check(items::dropForBlock(COAL_ORE) == S(ItemId::CoalOre, 1) &&
              items::dropForBlock(DEEPSLATE_DIAMOND_ORE) == S(ItemId::DeepslateDiamondOre, 1),
          "v1 ores drop their own block (no raw-mineral items)");
    check(items::dropForBlock(GLASS) == S(ItemId::Glass, 1) &&
              items::dropForBlock(ICE) == S(ItemId::Ice, 1),
          "v1 glass/ice drop themselves (no silk-touch distinction)");
    check(items::dropForBlock(LILY_PAD) == S(ItemId::LilyPad, 1) &&
              items::dropForBlock(SHORT_GRASS) == S(ItemId::ShortGrass, 1),
          "detail blocks (lily pad / short grass) are breakable with drops");
}

void testNameParsing()
{
    bool roundTrip = true;
    bool tokenForms = true;
    bool numeric = true;
    for (const items::ItemDefinition &def : items::itemDefinitions())
    {
        const std::string name = def.name;
        roundTrip = roundTrip && items::parseItemId(name) == def.id;

        std::string lower;
        std::string hyphen;
        for (const char c : name)
        {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            hyphen.push_back(c == ' ' ? '-' : c);
        }
        tokenForms = tokenForms && items::parseItemId(upperUnderscore(name)) == def.id;
        tokenForms = tokenForms && items::parseItemId(lower) == def.id;
        tokenForms = tokenForms && items::parseItemId(hyphen) == def.id;

        const std::string ordinal = std::to_string(static_cast<uint16_t>(def.id));
        numeric = numeric && items::parseItemId(ordinal) == def.id;
    }
    check(roundTrip, "every item name parses back to its id");
    check(tokenForms, "UPPER_UNDERSCORE / lowercase / hyphen token forms all parse");
    check(numeric, "numeric registry ordinals parse (headless grammar discipline)");

    check(items::parseItemId("grass_block") == ItemId::GrassBlock, "grass_block token parses");
    check(items::parseItemId("GRASS-BLOCK") == ItemId::GrassBlock, "GRASS-BLOCK token parses");
    check(items::parseItemId("oak log") == ItemId::OakLog, "space-form token parses");

    check(!items::parseItemId("").has_value(), "empty token rejected");
    check(!items::parseItemId("none").has_value(), "None sentinel is not a parseable item");
    check(!items::parseItemId("0").has_value(), "ordinal 0 rejected");
    check(!items::parseItemId("-1").has_value(), "negative ordinal rejected");
    check(!items::parseItemId(std::to_string(items::kItemCount + 1)).has_value(),
          "out-of-range ordinal rejected");
    check(!items::parseItemId("65535").has_value(), "0xFFFF ordinal rejected");
    check(!items::parseItemId("999999999999999999999").has_value(), "overflowing ordinal rejected");
    check(!items::parseItemId("NotAnItem").has_value(), "unknown name rejected");
    check(!items::parseItemId("1.5").has_value(), "non-integer token rejected");
    check(!items::parseItemId("Grass Top").has_value(),
          "render-atlas face names are not item tokens (item vocabulary is independent)");
}

// --- Pure stack math -----------------------------------------------------------

void testMakeStack()
{
    check(items::makeStack(ItemId::Stone, 0) == kEmpty, "count 0 collapses to canonical empty");
    check(items::makeStack(ItemId::None, 5) == kEmpty, "None sentinel collapses to canonical empty");
    check(items::makeStack(ItemId::Stone, 64) == S(ItemId::Stone, 64), "makeStack keeps valid counts");
    check(kEmpty.item == ItemId::None && kEmpty.count == 0, "canonical empty is {None, 0}");
}

void testMergeEdges()
{
    // Overflow at maxStack: 40 + 40 -> 64 merged + 16 remainder.
    const items::MergeResult overflow = items::mergeStacks(S(ItemId::Stone, 40), S(ItemId::Stone, 40));
    check(overflow.result == ItemResult::Partial, "merge overflow is Partial");
    check(overflow.merged == S(ItemId::Stone, 64), "overflow merges up to maxStack");
    check(overflow.remainder == S(ItemId::Stone, 16), "overflow returns the remainder");

    // Exact fit: remainder is the CANONICAL empty (deterministic values).
    const items::MergeResult exact = items::mergeStacks(S(ItemId::Stone, 32), S(ItemId::Stone, 32));
    check(exact.result == ItemResult::Ok, "exact-fit merge is Ok");
    check(exact.merged == S(ItemId::Stone, 64), "exact fit fills the destination");
    check(exact.remainder == kEmpty, "drained source normalizes to canonical empty");

    const items::MergeResult intoEmpty = items::mergeStacks(kEmpty, S(ItemId::Dirt, 10));
    check(intoEmpty.result == ItemResult::Ok && intoEmpty.merged == S(ItemId::Dirt, 10) &&
              intoEmpty.remainder == kEmpty,
          "merge into empty destination accepts any item");

    const items::MergeResult intoFull = items::mergeStacks(S(ItemId::Stone, 64), S(ItemId::Stone, 1));
    check(intoFull.result == ItemResult::Full && intoFull.merged == S(ItemId::Stone, 64) &&
              intoFull.remainder == S(ItemId::Stone, 1),
          "merge into a full stack is Full (nothing moved)");

    const items::MergeResult mismatch = items::mergeStacks(S(ItemId::Dirt, 1), S(ItemId::Stone, 1));
    check(mismatch.result == ItemResult::ItemMismatch && mismatch.merged == S(ItemId::Dirt, 1) &&
              mismatch.remainder == S(ItemId::Stone, 1),
          "merge between different items is ItemMismatch (nothing moved)");

    const items::MergeResult emptySource = items::mergeStacks(S(ItemId::Stone, 10), kEmpty);
    check(emptySource.result == ItemResult::EmptyStack && emptySource.merged == S(ItemId::Stone, 10) &&
              emptySource.remainder == kEmpty,
          "empty source merge answers EmptyStack (nothing to merge, values unchanged)");

    const items::MergeResult oversized = items::mergeStacks(kEmpty, S(ItemId::Stone, 200));
    check(oversized.result == ItemResult::Partial && oversized.merged == S(ItemId::Stone, 64) &&
              oversized.remainder == S(ItemId::Stone, 136),
          "oversized transient source clamps at maxStack with remainder");
}

void testSplitEdges()
{
    const items::SplitResult partial = items::splitStack(S(ItemId::Stone, 5), 2);
    check(partial.result == ItemResult::Ok && partial.taken == S(ItemId::Stone, 2) &&
              partial.remainder == S(ItemId::Stone, 3),
          "splitStack takes the requested count");

    const items::SplitResult clamped = items::splitStack(S(ItemId::Stone, 5), 99);
    check(clamped.result == ItemResult::Ok && clamped.taken == S(ItemId::Stone, 5) &&
              clamped.remainder == kEmpty,
          "splitStack clamps to available (Ok) and canonicalizes the drained source");

    const items::SplitResult zero = items::splitStack(S(ItemId::Stone, 5), 0);
    check(zero.result == ItemResult::Ok && zero.taken == kEmpty && zero.remainder == S(ItemId::Stone, 5),
          "splitStack count 0 is an Ok no-op");

    check(items::splitStack(kEmpty, 1).result == ItemResult::EmptyStack, "split of empty is EmptyStack");
    check(items::splitStack(S(ItemId::Stone, 1), 1).remainder == kEmpty,
          "splitting the last item canonicalizes the remainder");

    // Odd half-split: the TAKEN half is the larger (ceil).
    const items::SplitResult odd = items::splitHalf(S(ItemId::Stone, 5));
    check(odd.result == ItemResult::Ok && odd.taken == S(ItemId::Stone, 3) &&
              odd.remainder == S(ItemId::Stone, 2),
          "odd splitHalf: 5 -> taken 3 + remainder 2");
    const items::SplitResult even = items::splitHalf(S(ItemId::Stone, 6));
    check(even.taken == S(ItemId::Stone, 3) && even.remainder == S(ItemId::Stone, 3),
          "even splitHalf: 6 -> 3 + 3");
    const items::SplitResult one = items::splitHalf(S(ItemId::Stone, 1));
    check(one.taken == S(ItemId::Stone, 1) && one.remainder == kEmpty, "splitHalf of 1 takes the item");
    const items::SplitResult full = items::splitHalf(S(ItemId::Stone, 64));
    check(full.taken == S(ItemId::Stone, 32) && full.remainder == S(ItemId::Stone, 32),
          "splitHalf of a full stack is 32 + 32");
    check(items::splitHalf(kEmpty).result == ItemResult::EmptyStack, "splitHalf of empty is EmptyStack");
}

void testTransferEdges()
{
    const items::TransferResult partial =
        items::transferStack(S(ItemId::Stone, 10), S(ItemId::Stone, 60), 5);
    check(partial.result == ItemResult::Partial && partial.source == S(ItemId::Stone, 6) &&
              partial.destination == S(ItemId::Stone, 64),
          "transfer into a nearly-full slot is Partial (capacity-limited)");

    const items::TransferResult full =
        items::transferStack(S(ItemId::Stone, 10), S(ItemId::Stone, 64), 5);
    check(full.result == ItemResult::Full && full.source == S(ItemId::Stone, 10) &&
              full.destination == S(ItemId::Stone, 64),
          "transfer into a full slot is Full (nothing moved)");

    const items::TransferResult mismatch =
        items::transferStack(S(ItemId::Stone, 10), S(ItemId::Dirt, 1), 5);
    check(mismatch.result == ItemResult::ItemMismatch, "transfer between different items is ItemMismatch");

    const items::TransferResult intoEmpty = items::transferStack(S(ItemId::Stone, 10), kEmpty, 3);
    check(intoEmpty.result == ItemResult::Ok && intoEmpty.source == S(ItemId::Stone, 7) &&
              intoEmpty.destination == S(ItemId::Stone, 3),
          "transfer into empty slot moves the requested count");

    const items::TransferResult clamped = items::transferStack(S(ItemId::Stone, 10), kEmpty, 64);
    check(clamped.result == ItemResult::Ok && clamped.source == kEmpty &&
              clamped.destination == S(ItemId::Stone, 10),
          "transfer clamps to available items (Ok, source canonicalized)");

    check(items::transferStack(kEmpty, kEmpty, 5).result == ItemResult::EmptyStack,
          "transfer from empty is EmptyStack");
    const items::TransferResult zero = items::transferStack(S(ItemId::Stone, 10), kEmpty, 0);
    check(zero.result == ItemResult::Ok && zero.source == S(ItemId::Stone, 10) && zero.destination == kEmpty,
          "transfer count 0 is an Ok no-op");

    // uint16_t boundary: a maximal transient count never wraps.
    const items::TransferResult huge = items::transferStack(S(ItemId::Stone, 65535), kEmpty, 65535);
    check(huge.result == ItemResult::Partial && huge.destination == S(ItemId::Stone, 64) &&
              huge.source == S(ItemId::Stone, 65535 - 64),
          "uint16_t-max transient count transfers exactly maxStack without wrapping");

    const ItemStack invalid{static_cast<ItemId>(0xFFFF), 1};
    check(items::transferStack(invalid, kEmpty, 1).result == ItemResult::InvalidItem,
          "transfer of an unknown item is InvalidItem");
    check(items::transferStack(S(ItemId::Stone, 1), invalid, 1).result == ItemResult::InvalidItem,
          "transfer into an unknown-item stack is InvalidItem");
    check(items::mergeStacks(kEmpty, invalid).result == ItemResult::InvalidItem,
          "merge of an unknown item is InvalidItem");
    check(items::splitStack(invalid, 1).result == ItemResult::InvalidItem,
          "split of an unknown item is InvalidItem");
}

// --- Inventory -------------------------------------------------------------------

void testFreshInventory()
{
    const items::Inventory inv;
    bool allEmpty = true;
    for (const ItemStack &slot : inv.slots())
        allEmpty = allEmpty && slot == kEmpty;
    check(allEmpty, "fresh inventory: 36 canonical empty stacks");
    check(inv.selectedSlot() == 0, "fresh inventory selects hotbar slot 0");
    check(inv.selectedStack() == kEmpty, "fresh selected stack is empty");
    check(inv.totalCount(ItemId::Stone) == 0, "fresh inventory holds nothing");
}

void testAddHotbarFirst()
{
    items::Inventory inv;

    // Empty-slot fill is hotbar-first: the first stack lands in slot 0, a
    // different item takes slot 1 (no merge between different items).
    check(inv.addStack(S(ItemId::Dirt, 5)).result == ItemResult::Ok, "first add is Ok");
    check(inv.stack(0) == S(ItemId::Dirt, 5), "first add lands in hotbar slot 0");
    check(inv.addStack(S(ItemId::Stone, 5)).result == ItemResult::Ok, "second add is Ok");
    check(inv.stack(1) == S(ItemId::Stone, 5), "different item takes the next hotbar slot");
    check(inv.removeCount(0, 5).result == ItemResult::Ok && inv.removeCount(1, 5).result == ItemResult::Ok,
          "reset the fixture");

    // Seed Stone in hotbar slot 5 AND main slot 20. A full stack is parked in
    // slot 5 first so the next add cannot merge into it, then trimmed.
    check(inv.addStack(S(ItemId::Stone, 64)).result == ItemResult::Ok, "park a full Stone stack");
    check(inv.moveStack(0, 5) == ItemResult::Ok, "park it in hotbar slot 5");
    check(inv.addStack(S(ItemId::Stone, 10)).result == ItemResult::Ok, "second Stone stack (slot 5 is full)");
    check(inv.stack(0) == S(ItemId::Stone, 10), "full hotbar stack forced the add into slot 0");
    check(inv.moveStack(0, 20) == ItemResult::Ok, "seed main slot 20");
    check(inv.removeCount(5, 54).removed == S(ItemId::Stone, 54), "trim hotbar slot 5 to 10");

    // Merge is hotbar-first: the hotbar stack absorbs everything while it has
    // room; the main-inventory stack stays untouched.
    const items::AddResult merged = inv.addStack(S(ItemId::Stone, 20));
    check(merged.result == ItemResult::Ok && merged.remainder == kEmpty, "hotbar-first merge is Ok");
    check(inv.stack(5) == S(ItemId::Stone, 30), "merge fills the HOTBAR stack first");
    check(inv.stack(20) == S(ItemId::Stone, 10), "main-inventory stack untouched while hotbar has room");

    // Merge overflow continues hotbar -> main in slot order.
    const items::AddResult spill = inv.addStack(S(ItemId::Stone, 44));
    check(spill.result == ItemResult::Ok && spill.remainder == kEmpty, "merge spill is Ok");
    check(inv.stack(5) == S(ItemId::Stone, 64), "hotbar stack filled to maxStack first");
    check(inv.stack(20) == S(ItemId::Stone, 20), "overflow merged into the main-inventory stack");

    // After merges saturate, the remainder fills EMPTY slots hotbar-first.
    items::Inventory fresh;
    check(fresh.addStack(S(ItemId::Dirt, 5)).result == ItemResult::Ok, "seed slot 0");
    const items::AddResult fill = fresh.addStack(S(ItemId::Dirt, 60));
    check(fill.result == ItemResult::Ok && fill.remainder == kEmpty, "merge + empty-fill is Ok");
    check(fresh.stack(0) == S(ItemId::Dirt, 64), "existing stack merged to maxStack");
    check(fresh.stack(1) == S(ItemId::Dirt, 1), "remainder fills the next EMPTY hotbar slot");

    // The hotbar/main geometry: 9 distinct items fill slots 0..8, the 10th
    // goes to main slot 9.
    items::Inventory geometry;
    for (uint16_t i = 1; i <= 10; ++i)
        check(geometry.addStack(S(static_cast<ItemId>(i), 1)).result == ItemResult::Ok,
              "geometry fill add is Ok");
    bool hotbarPrefix = true;
    for (int i = 0; i < items::kHotbarSize; ++i)
        hotbarPrefix = hotbarPrefix && geometry.stack(i) == S(static_cast<ItemId>(i + 1), 1);
    check(hotbarPrefix, "hotbar is slots 0..8");
    check(geometry.stack(9) == S(static_cast<ItemId>(10), 1), "10th distinct item starts main slot 9");
}

void testAddPartialAndFull()
{
    items::Inventory inv;
    for (int i = 0; i < items::kInventorySize; ++i)
        check(inv.addStack(S(ItemId::Stone, 64)).result == ItemResult::Ok, "fill inventory with full stacks");

    // Full inventory: nothing moves, remainder == input.
    const auto before = inv.slots();
    const items::AddResult full = inv.addStack(S(ItemId::Stone, 10));
    check(full.result == ItemResult::Full, "add to a full inventory is Full");
    check(full.remainder == S(ItemId::Stone, 10), "Full returns the untouched input");
    check(inv.slots() == before, "Full leaves every slot unchanged");

    // One stack with room for 4: partial add merges 4, returns 6.
    check(inv.removeCount(35, 4).removed == S(ItemId::Stone, 4), "make room in the last slot");
    const items::AddResult partial = inv.addStack(S(ItemId::Stone, 10));
    check(partial.result == ItemResult::Partial, "add with limited room is Partial");
    check(partial.remainder == S(ItemId::Stone, 6), "Partial returns the unfit remainder");
    check(inv.stack(35) == S(ItemId::Stone, 64), "the only stack with room absorbed 4");

    // Oversized inputs distribute across slots (bulk grant / pickup seam).
    items::Inventory bulk;
    const items::AddResult exact = bulk.addStack(S(ItemId::Stone, 36 * 64));
    check(exact.result == ItemResult::Ok && exact.remainder == kEmpty,
          "oversized add distributes exactly 36 full stacks");
    bool allFull = true;
    for (const ItemStack &slot : bulk.slots())
        allFull = allFull && slot == S(ItemId::Stone, 64);
    check(allFull, "every slot holds maxStack after the exact bulk add");

    const items::AddResult over = bulk.addStack(S(ItemId::Stone, 1));
    check(over.result == ItemResult::Full && over.remainder == S(ItemId::Stone, 1),
          "bulk-filled inventory rejects further adds");

    items::Inventory huge;
    const items::AddResult max = huge.addStack(S(ItemId::Stone, 65535));
    check(max.result == ItemResult::Partial && max.remainder == S(ItemId::Stone, 65535 - 36 * 64),
          "uint16_t-max add distributes 2304 items without wrapping");

    // Empty / invalid inputs.
    items::Inventory misc;
    const items::AddResult empty = misc.addStack(kEmpty);
    check(empty.result == ItemResult::Ok && empty.remainder == kEmpty, "empty add is an Ok no-op");
    const ItemStack invalid{static_cast<ItemId>(0xFFFF), 5};
    const items::AddResult bad = misc.addStack(invalid);
    check(bad.result == ItemResult::InvalidItem && bad.remainder == invalid,
          "unknown-item add is InvalidItem (nothing stored)");
    check(misc.totalCount(ItemId::Stone) == 0, "rejected adds store nothing");
}

void testRemoveCount()
{
    items::Inventory inv;
    check(inv.addStack(S(ItemId::Stone, 10)).result == ItemResult::Ok, "seed slot 0");

    check(inv.removeCount(-1, 1).result == ItemResult::InvalidSlot, "negative slot rejected");
    check(inv.removeCount(items::kInventorySize, 1).result == ItemResult::InvalidSlot,
          "slot == kInventorySize rejected");
    check(inv.removeCount(5, 1).result == ItemResult::EmptyStack, "empty slot answers EmptyStack");

    const items::RemoveResult zero = inv.removeCount(0, 0);
    check(zero.result == ItemResult::Ok && zero.removed == kEmpty && inv.stack(0) == S(ItemId::Stone, 10),
          "removeCount 0 is an Ok no-op");

    const items::RemoveResult partial = inv.removeCount(0, 3);
    check(partial.result == ItemResult::Ok && partial.removed == S(ItemId::Stone, 3) &&
              inv.stack(0) == S(ItemId::Stone, 7),
          "removeCount takes the requested count");

    const items::RemoveResult exact = inv.removeCount(0, 7);
    check(exact.result == ItemResult::Ok && exact.removed == S(ItemId::Stone, 7) && inv.stack(0) == kEmpty,
          "draining a slot canonicalizes it to {None, 0}");

    check(inv.addStack(S(ItemId::Dirt, 4)).result == ItemResult::Ok, "reseed slot 0");
    const items::RemoveResult clamped = inv.removeCount(0, 99);
    check(clamped.result == ItemResult::Ok && clamped.removed == S(ItemId::Dirt, 4) && inv.stack(0) == kEmpty,
          "removeCount clamps to available (Ok)");
}

void testMoveSwapSplitMatrix()
{
    items::Inventory inv;
    inv.addStack(S(ItemId::Stone, 10)); // slot 0
    inv.addStack(S(ItemId::Dirt, 5));   // slot 1

    // moveStack into an empty slot: whole move, source canonicalized.
    check(inv.moveStack(0, 20) == ItemResult::Ok, "move into empty slot is Ok");
    check(inv.stack(0) == kEmpty && inv.stack(20) == S(ItemId::Stone, 10), "move transfers the whole stack");

    // addStack merges into slot 20 first (room 54), the overflow lands in
    // slot 0: fixture slot 0 {Stone,10}, slot 20 {Stone,64}.
    check(inv.addStack(S(ItemId::Stone, 64)).result == ItemResult::Ok, "merge-fill fixture add");
    check(inv.stack(0) == S(ItemId::Stone, 10) && inv.stack(20) == S(ItemId::Stone, 64),
          "merge-first add overflow fixture");

    // moveStack merge: Full / ItemMismatch / Partial / Ok.
    check(inv.moveStack(0, 20) == ItemResult::Full, "merge into full destination is Full");
    check(inv.stack(0) == S(ItemId::Stone, 10), "Full moves nothing");
    check(inv.moveStack(0, 1) == ItemResult::ItemMismatch, "move onto a different item is ItemMismatch");
    check(inv.stack(1) == S(ItemId::Dirt, 5), "ItemMismatch moves nothing");
    check(inv.removeCount(20, 4).removed == S(ItemId::Stone, 4), "make room for a partial merge");
    check(inv.moveStack(0, 20) == ItemResult::Partial, "capacity-limited move is Partial");
    check(inv.stack(0) == S(ItemId::Stone, 6) && inv.stack(20) == S(ItemId::Stone, 64),
          "Partial keeps the remainder in the source");
    check(inv.removeCount(20, 64).removed == S(ItemId::Stone, 64), "drain slot 20");
    check(inv.moveStack(0, 20) == ItemResult::Ok, "whole move into the drained slot is Ok");
    check(inv.stack(0) == kEmpty && inv.stack(20) == S(ItemId::Stone, 6), "Ok move canonicalizes the source");

    // moveStack bounds / no-ops.
    check(inv.moveStack(-1, 0) == ItemResult::InvalidSlot, "move from negative slot rejected");
    check(inv.moveStack(0, items::kInventorySize) == ItemResult::InvalidSlot, "move to out-of-range rejected");
    check(inv.moveStack(3, 3) == ItemResult::Ok, "move onto itself is an Ok no-op");
    check(inv.moveStack(10, 11) == ItemResult::EmptyStack, "move from empty slot is EmptyStack");

    // swapStacks: unconditional exchange (with empty / different items).
    check(inv.swapStacks(0, 1) == ItemResult::Ok, "swap with an empty slot is Ok");
    check(inv.stack(0) == S(ItemId::Dirt, 5) && inv.stack(1) == kEmpty, "swap moves into the empty slot");
    check(inv.swapStacks(20, 1) == ItemResult::Ok, "park Stone in slot 1");
    check(inv.swapStacks(0, 1) == ItemResult::Ok, "swap different items is Ok");
    check(inv.stack(0) == S(ItemId::Stone, 6) && inv.stack(1) == S(ItemId::Dirt, 5), "swap exchanges contents");
    check(inv.swapStacks(2, 2) == ItemResult::Ok, "swap onto itself is an Ok no-op");
    check(inv.swapStacks(-1, 0) == ItemResult::InvalidSlot, "swap with negative slot rejected");
    check(inv.swapStacks(0, 36) == ItemResult::InvalidSlot, "swap with out-of-range rejected");

    // splitStack(from, to): the LARGER half moves (odd counts defined).
    items::Inventory split;
    split.addStack(S(ItemId::Stone, 5)); // slot 0
    check(split.splitStack(0, 1) == ItemResult::Ok, "odd split into empty slot is Ok");
    check(split.stack(0) == S(ItemId::Stone, 2) && split.stack(1) == S(ItemId::Stone, 3),
          "odd split: 5 -> source keeps 2, destination takes 3");
    check(split.splitStack(1, 2) == ItemResult::Ok, "split of 3 is Ok");
    check(split.stack(1) == S(ItemId::Stone, 1) && split.stack(2) == S(ItemId::Stone, 2),
          "odd split: 3 -> source keeps 1, destination takes 2");
    check(split.splitStack(2, 3) == ItemResult::Ok, "even split is Ok");
    check(split.stack(2) == S(ItemId::Stone, 1) && split.stack(3) == S(ItemId::Stone, 1),
          "even split: 2 -> 1 + 1");
    check(split.splitStack(3, 4) == ItemResult::Ok, "split of a single item is Ok");
    check(split.stack(3) == kEmpty && split.stack(4) == S(ItemId::Stone, 1),
          "splitting 1 drains the source canonically");

    // splitStack transfer semantics: Partial / Full / ItemMismatch / bounds.
    // Fixture: slot 0 {Stone,5}, slot 1 {Stone,62} (park a full stack first so
    // the second add cannot merge, then trim slot 0).
    items::Inventory edges;
    edges.addStack(S(ItemId::Stone, 64)); // slot 0 full
    edges.addStack(S(ItemId::Stone, 62)); // slot 1 (slot 0 has no room)
    check(edges.removeCount(0, 59).removed == S(ItemId::Stone, 59), "trim slot 0 to 5");
    check(edges.stack(0) == S(ItemId::Stone, 5) && edges.stack(1) == S(ItemId::Stone, 62),
          "split fixture");
    check(edges.splitStack(0, 1) == ItemResult::Partial, "split into a nearly-full slot is Partial");
    check(edges.stack(0) == S(ItemId::Stone, 3) && edges.stack(1) == S(ItemId::Stone, 64),
          "Partial split moves only what fits (half 3, room 2)");
    check(edges.splitStack(0, 1) == ItemResult::Full, "split into a full slot is Full");
    edges.addStack(S(ItemId::Dirt, 1)); // slot 2 (first empty)
    check(edges.stack(2) == S(ItemId::Dirt, 1), "Dirt fixture lands in slot 2");
    check(edges.splitStack(0, 2) == ItemResult::ItemMismatch, "split onto a different item is ItemMismatch");
    check(edges.splitStack(10, 11) == ItemResult::EmptyStack, "split from empty slot is EmptyStack");
    check(edges.splitStack(-1, 0) == ItemResult::InvalidSlot, "split from negative slot rejected");
    check(edges.splitStack(0, 36) == ItemResult::InvalidSlot, "split to out-of-range rejected");
    check(edges.splitStack(0, 0) == ItemResult::Ok, "split onto itself is an Ok no-op");
}

void testSelectionBounds()
{
    items::Inventory inv;
    for (int i = 0; i < items::kHotbarSize; ++i)
        inv.addStack(S(static_cast<ItemId>(i + 1), i + 1));

    bool selectable = true;
    for (int i = 0; i < items::kHotbarSize; ++i)
    {
        selectable = selectable && inv.setSelectedSlot(i) == ItemResult::Ok;
        selectable = selectable && inv.selectedSlot() == i;
        selectable = selectable && inv.selectedStack() == S(static_cast<ItemId>(i + 1), i + 1);
    }
    check(selectable, "every hotbar slot 0..8 is selectable and selectedStack follows");

    check(inv.setSelectedSlot(-1) == ItemResult::InvalidSlot, "negative selection rejected");
    check(inv.setSelectedSlot(items::kHotbarSize) == ItemResult::InvalidSlot,
          "selection == kHotbarSize rejected (main inventory is not selectable)");
    check(inv.setSelectedSlot(items::kInventorySize) == ItemResult::InvalidSlot,
          "selection == kInventorySize rejected");
    check(inv.selectedSlot() == 8, "rejected selections leave the selection unchanged");
}

void testValueSemanticsAndTotals()
{
    items::Inventory inv;
    inv.addStack(S(ItemId::Stone, 64));
    inv.addStack(S(ItemId::Stone, 30));
    inv.addStack(S(ItemId::Dirt, 7));
    inv.setSelectedSlot(2);

    check(inv.totalCount(ItemId::Stone) == 94, "totalCount sums across slots");
    check(inv.totalCount(ItemId::Dirt) == 7, "totalCount per item");
    check(inv.totalCount(ItemId::None) == 0, "totalCount(None) is 0");
    check(inv.totalCount(ItemId::Glass) == 0, "totalCount of an absent item is 0");

    items::Inventory copy = inv; // plain-value copy, independent state
    copy.addStack(S(ItemId::Stone, 100));
    copy.setSelectedSlot(0);
    check(inv.totalCount(ItemId::Stone) == 94 && inv.selectedSlot() == 2,
          "mutating a copy never touches the original (value semantics)");
    check(copy.totalCount(ItemId::Stone) == 194, "copy mutates independently");

    inv.clear();
    bool allEmpty = true;
    for (const ItemStack &slot : inv.slots())
        allEmpty = allEmpty && slot == kEmpty;
    check(allEmpty && inv.selectedSlot() == 0, "clear resets slots and selection");
}

// --- Deterministic repeated sequences ---------------------------------------------

// Knuth LCG: fixed seed, defined uint64_t wrap — identical on every platform.
struct Lcg
{
    uint64_t state;
    uint32_t next()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(state >> 32);
    }
};

bool sameState(const items::Inventory &a, const items::Inventory &b)
{
    return a.slots() == b.slots() && a.selectedSlot() == b.selectedSlot();
}

bool wellFormed(const items::Inventory &inv)
{
    for (const ItemStack &slot : inv.slots())
    {
        if (slot.isEmpty())
        {
            if (slot != kEmpty)
                return false; // empties must be canonical
        }
        else if (items::maxStackOf(slot.item) == 0 || slot.count > items::maxStackOf(slot.item))
        {
            return false; // stored stacks never exceed maxStack / hold unknown items
        }
    }
    return true;
}

void testDeterministicSequences()
{
    constexpr int kSteps = 4000;
    items::Inventory a;
    items::Inventory b;
    Lcg rng{0x224E223ULL}; // issue-flavored fixed seed

    bool identical = true;
    bool invariants = true;
    for (int step = 0; step < kSteps; ++step)
    {
        const uint32_t op = rng.next() % 6;
        const int slotA = static_cast<int>(rng.next() % items::kInventorySize);
        const int slotB = static_cast<int>(rng.next() % items::kInventorySize);
        const ItemId item = static_cast<ItemId>(1 + rng.next() % items::kItemCount);
        const uint16_t count = static_cast<uint16_t>(rng.next() % 200);

        ItemResult ra = ItemResult::Ok;
        ItemResult rb = ItemResult::Ok;
        switch (op)
        {
        case 0:
        {
            const items::AddResult xa = a.addStack(S(item, count));
            const items::AddResult xb = b.addStack(S(item, count));
            ra = xa.result;
            rb = xb.result;
            identical = identical && xa.remainder == xb.remainder;
            break;
        }
        case 1:
        {
            const items::RemoveResult xa = a.removeCount(slotA, count);
            const items::RemoveResult xb = b.removeCount(slotA, count);
            ra = xa.result;
            rb = xb.result;
            identical = identical && xa.removed == xb.removed;
            break;
        }
        case 2:
            ra = a.moveStack(slotA, slotB);
            rb = b.moveStack(slotA, slotB);
            break;
        case 3:
            ra = a.swapStacks(slotA, slotB);
            rb = b.swapStacks(slotA, slotB);
            break;
        case 4:
            ra = a.splitStack(slotA, slotB);
            rb = b.splitStack(slotA, slotB);
            break;
        default:
        {
            const int hotbar = static_cast<int>(rng.next() % (items::kHotbarSize + 2));
            ra = a.setSelectedSlot(hotbar);
            rb = b.setSelectedSlot(hotbar);
            break;
        }
        }
        identical = identical && ra == rb && sameState(a, b);
        invariants = invariants && wellFormed(a);
    }
    check(identical, "4000-step scripted sequence: two inventories stay byte-identical (results + state)");
    check(invariants, "every intermediate state keeps canonical empties and maxStack bounds");

    // Replay the SAME sequence from a fresh seed on a third inventory: the
    // final state must match (determinism is seed-driven, not history-driven).
    items::Inventory c;
    Lcg replay{0x224E223ULL};
    for (int step = 0; step < kSteps; ++step)
    {
        const uint32_t op = replay.next() % 6;
        const int slotA = static_cast<int>(replay.next() % items::kInventorySize);
        const int slotB = static_cast<int>(replay.next() % items::kInventorySize);
        const ItemId item = static_cast<ItemId>(1 + replay.next() % items::kItemCount);
        const uint16_t count = static_cast<uint16_t>(replay.next() % 200);
        switch (op)
        {
        case 0:
            c.addStack(S(item, count));
            break;
        case 1:
            c.removeCount(slotA, count);
            break;
        case 2:
            c.moveStack(slotA, slotB);
            break;
        case 3:
            c.swapStacks(slotA, slotB);
            break;
        case 4:
            c.splitStack(slotA, slotB);
            break;
        default:
            c.setSelectedSlot(static_cast<int>(replay.next() % (items::kHotbarSize + 2)));
            break;
        }
    }
    check(sameState(a, c), "replaying the same seed reproduces the exact final state");
}

} // namespace

int main()
{
    std::printf("test_items: CPU-only item domain (no GPU, no SDL)\n");
    testRegistryOrdinalsAndNames();
    testPlaceableRoundTrips();
    testBlockItemCoverage();
    testDropTable();
    testNameParsing();
    testMakeStack();
    testMergeEdges();
    testSplitEdges();
    testTransferEdges();
    testFreshInventory();
    testAddHotbarFirst();
    testAddPartialAndFull();
    testRemoveCount();
    testMoveSwapSplitMatrix();
    testSelectionBounds();
    testValueSemanticsAndTotals();
    testDeterministicSequences();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("PASS\n");
    return g_failures == 0 ? 0 : 1;
}
