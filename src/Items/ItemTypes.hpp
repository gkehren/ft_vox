#pragma once

// =============================================================================
// Item domain contracts (issue #224, Epic #223 Phase A).
//
// The gameplay loop (break -> drop -> pickup -> inventory -> place) needs an
// item vocabulary that is STABLE FOR SAVES AND FUTURE NETWORK REPLICATION,
// independent of the render-atlas TextureType enum (which carries per-face
// variants like GRASS_TOP / GRASS_SIDE and is append-only for rendering
// reasons). This header is the plain-value core of that vocabulary:
//
//   * ItemId   — stable uint16_t registry ordinal, append-only. Saves and the
//                future wire protocol reference ItemId, NEVER TextureType.
//   * ItemStack— { ItemId item; uint16_t count; }, empty = count 0. No
//                durability / NBT / metadata in v1 (epic #223 non-goals).
//   * ItemResult — explicit result codes: the deterministic operation
//                vocabulary shared by the inventory, the future pickup/drop
//                paths and (later) the authoritative server's rejection
//                codes. No exceptions, no silent clamping without a code.
//
// Dependency rule (enforced by tests/check_core_includes.py): Items/ is a
// CPU-only core module — std headers only here; ItemRegistry additionally
// includes utils.hpp for the TextureType block catalog. NEVER Renderer/,
// Vulkan/, Camera/, Engine/, SDL3 or ImGui.
//
// Nothing consumes this module yet (issue #224 is contracts-only, exactly
// like #205 created the sim/render boundary before migrating behavior):
// simulation integration is #225, drops/pickup #226, persistence #228.
// =============================================================================

#include <cstdint>
#include <type_traits>

namespace items
{

// --- Item identity -----------------------------------------------------------

/// Stable append-only registry ordinal (epic #223 design decision §1).
/// `None` (0) is a sentinel, NOT a registry entry: it is the item field of the
/// canonical empty stack and the "no item" answer of the block mappings.
/// Registry entries start at 1 and mirror the TextureType block catalog order
/// (skipping render-only face variants and the hand-unbreakable fluids), so
/// the block<->item alignment is auditable row by row. New items are APPENDED
/// before the kItemCount pin; existing ordinals never move (save/wire stable).
enum class ItemId : uint16_t
{
    None = 0, // sentinel: canonical empty stack / no item

    // v1 block-item catalog (one item per breakable/placeable block).
    Bedrock,           // 1  — placeable (creative grant), unbreakable
    Bricks,            // 2
    Cobblestone,       // 3
    Dirt,              // 4
    Glass,             // 5
    GrassBlock,        // 6  — places GRASS_TOP; drops DIRT
    Gravel,            // 7
    OakLeaves,         // 8
    OakLog,            // 9
    OakPlanks,         // 10
    Sand,              // 11
    Snow,              // 12
    StoneBricks,       // 13
    Stone,             // 14 — drops COBBLESTONE
    CoalOre,           // 15
    CopperOre,         // 16
    DiamondOre,        // 17
    EmeraldOre,        // 18
    GoldOre,           // 19
    IronOre,           // 20
    LapisOre,          // 21
    RedstoneOre,       // 22
    // Wood / vegetation (TextureType append-only expansion)
    BirchLog,          // 23
    BirchLeaves,       // 24
    SpruceLog,         // 25
    SpruceLeaves,      // 26
    JungleLog,         // 27
    JungleLeaves,      // 28
    AcaciaLog,         // 29
    AcaciaLeaves,      // 30
    DarkOakLog,        // 31
    DarkOakLeaves,     // 32
    Cactus,            // 33
    // Climate surfaces
    RedSand,           // 34
    Sandstone,         // 35
    RedSandstone,      // 36
    Terracotta,        // 37
    OrangeTerracotta,  // 38
    RedTerracotta,     // 39
    YellowTerracotta,  // 40
    WhiteTerracotta,   // 41
    BrownTerracotta,   // 42
    CoarseDirt,        // 43
    Podzol,            // 44
    Clay,              // 45
    Mud,               // 46
    MossBlock,         // 47
    // Ice / stone variants
    Ice,               // 48
    PackedIce,         // 49
    BlueIce,           // 50
    Andesite,          // 51
    Diorite,           // 52
    Granite,           // 53
    Deepslate,         // 54
    Tuff,              // 55
    MossyCobblestone,  // 56
    // Phase 3 biome blocks
    CherryLog,         // 57
    CherryLeaves,      // 58
    MangroveLog,       // 59
    MangroveRoots,     // 60
    MangroveLeaves,    // 61
    BambooBlock,       // 62
    BambooStalk,       // 63
    RedMushroomBlock,  // 64
    BrownMushroomBlock,// 65
    MushroomStem,      // 66
    Basalt,            // 67
    Blackstone,        // 68
    Magma,             // 69
    TubeCoralBlock,    // 70
    BrainCoralBlock,   // 71
    BubbleCoralBlock,  // 72
    FireCoralBlock,    // 73
    HornCoralBlock,    // 74
    // Phase 4 underground blocks
    DeepslateCoalOre,     // 75
    DeepslateCopperOre,   // 76
    DeepslateDiamondOre,  // 77
    DeepslateEmeraldOre,  // 78
    DeepslateGoldOre,     // 79
    DeepslateIronOre,     // 80
    DeepslateLapisOre,    // 81
    DeepslateRedstoneOre, // 82
    DripstoneBlock,       // 83
    // Phase 5 aquatic blocks / details
    Kelp,              // 84
    KelpTop,           // 85
    ShortGrass,        // 86
    Fern,              // 87
    Wildflower,        // 88
    DryShrub,          // 89
    Seagrass,          // 90
    LilyPad,           // 91
};

/// Number of registry entries (excludes the None sentinel). Append-only pin:
/// extending the catalog bumps this and the LilyPad ordinal assert below.
inline constexpr uint16_t kItemCount = 91;

// Append-only ordinal discipline (same pattern as the BiomeType / TextureType
// phase pins in utils.hpp): existing save/wire ordinals must never move.
static_assert(static_cast<uint16_t>(ItemId::None) == 0, "None sentinel stays 0");
static_assert(static_cast<uint16_t>(ItemId::Bedrock) == 1, "first registry ordinal is stable");
static_assert(static_cast<uint16_t>(ItemId::RedstoneOre) == 22,
              "legacy block-item ordinals must remain stable");
static_assert(static_cast<uint16_t>(ItemId::BirchLog) == 23,
              "wood/vegetation items must stay appended after the legacy catalog");
static_assert(static_cast<uint16_t>(ItemId::CherryLog) == 57,
              "Phase 3 block items must stay append-only");
static_assert(static_cast<uint16_t>(ItemId::DeepslateCoalOre) == 75,
              "Phase 4 block items must stay append-only");
static_assert(static_cast<uint16_t>(ItemId::Kelp) == 84,
              "Phase 5 block items must stay append-only");
static_assert(static_cast<uint16_t>(ItemId::LilyPad) == kItemCount,
              "last registry ordinal must match kItemCount (append-only catalog)");

// --- Inventory geometry -------------------------------------------------------

inline constexpr int kHotbarSize = 9;      // hotbar = inventory slots 0..8
inline constexpr int kInventorySize = 36;  // hotbar 9 + main 27
static_assert(kHotbarSize < kInventorySize, "hotbar is a prefix of the inventory");

// --- Explicit result vocabulary ------------------------------------------------

/// Deterministic operation results (epic #223 §1/§3/§6). Every mutating stack
/// or inventory operation answers with one of these; the future authoritative
/// server reuses the same vocabulary as its rejection codes.
enum class ItemResult : uint8_t
{
    Ok,           // fully applied (a clamped take/transfer of all available
                  // items is still Ok — availability is not an error)
    Partial,      // partially applied; the remainder is returned to the caller
                  // (pickup leaves it in the world, add returns it)
    Full,         // no capacity anywhere; NOTHING was applied
    InvalidSlot,  // slot index outside the container bounds
    EmptyStack,   // source stack/slot is empty; nothing to take
    InvalidItem,  // unknown item id (None or out of registry with count > 0)
    ItemMismatch, // merge/transfer/move between two different items
    NotPlaceable, // item has no placeableBlock (the future place-from-hotbar
                  // rejection code; no v1 inventory operation returns it)
    IdExhausted,  // the entity-id allocator reached its representable bound
                  // (stabilization PR3): a drop that needs a fresh entity id
                  // is refused WITHOUT consuming inventory items. Append-only
                  // vocabulary: existing codes never move (save/wire stable).
};

// --- Plain-value stacks ---------------------------------------------------------

/// A counted item stack. Plain value, no hidden state: empty == count 0.
/// CANONICAL EMPTY is { ItemId::None, 0 } — every operation in this module
/// normalizes a stack whose count reaches 0 to that exact value, so equal
/// gameplay histories always produce byte-equal state (deterministic digests,
/// save round-trips and future replication compare values, not histories).
/// The guarantee applies to WELL-FORMED inputs and to all produced/stored
/// state: a rejected INVALID input (unknown item id with count > 0) is
/// echoed verbatim in the result remainder — it is the caller's error and
/// never enters storage.
/// Well-formed stored stacks satisfy 1 <= count <= maxStackOf(item); counts
/// above maxStack are legal only as transient operation inputs (e.g. a bulk
/// addStack distributes them across slots).
struct ItemStack
{
    ItemId item{ItemId::None};
    uint16_t count{0};

    constexpr bool isEmpty() const { return count == 0; }

    constexpr friend bool operator==(const ItemStack &, const ItemStack &) = default;
};

static_assert(sizeof(ItemStack) == 4, "ItemStack is a compact 4-byte plain value");
static_assert(std::is_trivially_copyable_v<ItemStack>, "ItemStack must stay a plain value");
static_assert(std::is_trivially_copyable_v<ItemId>, "ItemId must stay a plain value");
static_assert(std::is_trivially_copyable_v<ItemResult>, "ItemResult must stay a plain value");

// --- Operation result values ----------------------------------------------------
// Pure stack math answers with the updated VALUES plus the result code (no
// out-parameters, no heap). The field meanings are fixed per operation:

/// mergeStacks(destination, source): `merged` is the updated destination,
/// `remainder` is what is left of the source (canonical empty when drained).
struct MergeResult
{
    ItemResult result{ItemResult::Ok};
    ItemStack merged{};
    ItemStack remainder{};
};

/// splitStack / splitHalf: `taken` is the removed part, `remainder` stays in
/// the source slot (canonical empty when the source was drained).
struct SplitResult
{
    ItemResult result{ItemResult::Ok};
    ItemStack taken{};
    ItemStack remainder{};
};

/// transferStack(source, destination, count): both updated values.
struct TransferResult
{
    ItemResult result{ItemResult::Ok};
    ItemStack source{};
    ItemStack destination{};
};

/// Inventory::addStack: `remainder` is the part that did not fit (canonical
/// empty on Ok; the untouched input on Full / InvalidItem).
struct AddResult
{
    ItemResult result{ItemResult::Ok};
    ItemStack remainder{};
};

/// Inventory::removeCount: `removed` is what was taken out of the slot.
struct RemoveResult
{
    ItemResult result{ItemResult::Ok};
    ItemStack removed{};
};

static_assert(std::is_trivially_copyable_v<MergeResult>);
static_assert(std::is_trivially_copyable_v<SplitResult>);
static_assert(std::is_trivially_copyable_v<TransferResult>);
static_assert(std::is_trivially_copyable_v<AddResult>);
static_assert(std::is_trivially_copyable_v<RemoveResult>);

} // namespace items
