// =============================================================================
// ItemRegistry — the single static append-only item catalog (issue #224,
// Epic #223 Phase A). See ItemRegistry.hpp for the contract overview.
//
// This translation unit owns THREE constexpr tables and the alignment
// static_asserts that pin them together (the biomeTypeString / kBlockLayers
// discipline from utils.hpp):
//
//   kDefinitions — one row per ItemId ordinal (the save/wire-stable catalog);
//   kBlockItem   — TextureType -> ItemId, DERIVED from the definitions'
//                  placeableBlock fields (+ face-variant aliasing), so every
//                  placeable item maps back to its block BY CONSTRUCTION;
//   kBlockDrop   — TextureType -> drop ItemId, written EXPLICITLY row by row
//                  (epic #223 §1: the break->drop rule is gameplay data, not
//                  a renderer or simulation heuristic).
//
// v1 drop policy (explicit, deterministic — no RNG, no tools):
//   * STONE -> COBBLESTONE and grass block -> DIRT (the epic's examples);
//   * leaves drop THEMSELVES (no sapling items in v1);
//   * ores drop their own block (no raw-mineral items in v1);
//   * glass/ice drop themselves (no silk-touch distinction in v1);
//   * every other breakable block drops its own block item;
//   * BEDROCK unbreakable, WATER/LAVA not breakable by hand -> no drop.
// =============================================================================

#include <Items/ItemRegistry.hpp>

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace items
{
namespace
{

// --- Canonical voxel blocks ----------------------------------------------------
// The render atlas carries per-face variants that are never stored as voxels
// (the mesher remaps faces through Renderer/MinecraftTextures.hpp). The item
// domain owns its OWN variant->canonical mapping because the CPU core must
// not include renderer headers (dependency rule, epic #204 / #223). The two
// tables must agree on the variant set; test_items pins every row.
constexpr TextureType canonicalBlock(TextureType type)
{
    switch (type)
    {
    case GRASS_SIDE:
        return GRASS_TOP;
    case OAK_LOG_TOP:
        return OAK_LOG;
    case BIRCH_LOG_TOP:
        return BIRCH_LOG;
    case SPRUCE_LOG_TOP:
        return SPRUCE_LOG;
    case JUNGLE_LOG_TOP:
        return JUNGLE_LOG;
    case ACACIA_LOG_TOP:
        return ACACIA_LOG;
    case DARK_OAK_LOG_TOP:
        return DARK_OAK_LOG;
    case CHERRY_LOG_TOP:
        return CHERRY_LOG;
    case MANGROVE_LOG_TOP:
        return MANGROVE_LOG;
    case MANGROVE_ROOTS_TOP:
        return MANGROVE_ROOTS;
    case BAMBOO_BLOCK_TOP:
        return BAMBOO_BLOCK;
    case BASALT_TOP:
        return BASALT;
    case CACTUS_TOP:
        return CACTUS;
    case DEEPSLATE_TOP:
        return DEEPSLATE;
    default:
        return type;
    }
}

constexpr bool isFaceVariant(TextureType type)
{
    return canonicalBlock(type) != type;
}

constexpr bool isFluidBlock(TextureType type)
{
    return type == WATER || type == LAVA;
}

// --- The catalog (append-only; ordinal = index + 1) -----------------------------
constexpr std::array<ItemDefinition, kItemCount> kDefinitions = {{
    {ItemId::Bedrock, "Bedrock", kDefaultMaxStack, BEDROCK},
    {ItemId::Bricks, "Bricks", kDefaultMaxStack, BRICKS},
    {ItemId::Cobblestone, "Cobblestone", kDefaultMaxStack, COBBLESTONE},
    {ItemId::Dirt, "Dirt", kDefaultMaxStack, DIRT},
    {ItemId::Glass, "Glass", kDefaultMaxStack, GLASS},
    {ItemId::GrassBlock, "Grass Block", kDefaultMaxStack, GRASS_TOP},
    {ItemId::Gravel, "Gravel", kDefaultMaxStack, GRAVEL},
    {ItemId::OakLeaves, "Oak Leaves", kDefaultMaxStack, OAK_LEAVES},
    {ItemId::OakLog, "Oak Log", kDefaultMaxStack, OAK_LOG},
    {ItemId::OakPlanks, "Oak Planks", kDefaultMaxStack, OAK_PLANKS},
    {ItemId::Sand, "Sand", kDefaultMaxStack, SAND},
    {ItemId::Snow, "Snow", kDefaultMaxStack, SNOW},
    {ItemId::StoneBricks, "Stone Bricks", kDefaultMaxStack, STONE_BRICKS},
    {ItemId::Stone, "Stone", kDefaultMaxStack, STONE},
    {ItemId::CoalOre, "Coal Ore", kDefaultMaxStack, COAL_ORE},
    {ItemId::CopperOre, "Copper Ore", kDefaultMaxStack, COPPER_ORE},
    {ItemId::DiamondOre, "Diamond Ore", kDefaultMaxStack, DIAMOND_ORE},
    {ItemId::EmeraldOre, "Emerald Ore", kDefaultMaxStack, EMERALD_ORE},
    {ItemId::GoldOre, "Gold Ore", kDefaultMaxStack, GOLD_ORE},
    {ItemId::IronOre, "Iron Ore", kDefaultMaxStack, IRON_ORE},
    {ItemId::LapisOre, "Lapis Ore", kDefaultMaxStack, LAPIS_ORE},
    {ItemId::RedstoneOre, "Redstone Ore", kDefaultMaxStack, REDSTONE_ORE},
    {ItemId::BirchLog, "Birch Log", kDefaultMaxStack, BIRCH_LOG},
    {ItemId::BirchLeaves, "Birch Leaves", kDefaultMaxStack, BIRCH_LEAVES},
    {ItemId::SpruceLog, "Spruce Log", kDefaultMaxStack, SPRUCE_LOG},
    {ItemId::SpruceLeaves, "Spruce Leaves", kDefaultMaxStack, SPRUCE_LEAVES},
    {ItemId::JungleLog, "Jungle Log", kDefaultMaxStack, JUNGLE_LOG},
    {ItemId::JungleLeaves, "Jungle Leaves", kDefaultMaxStack, JUNGLE_LEAVES},
    {ItemId::AcaciaLog, "Acacia Log", kDefaultMaxStack, ACACIA_LOG},
    {ItemId::AcaciaLeaves, "Acacia Leaves", kDefaultMaxStack, ACACIA_LEAVES},
    {ItemId::DarkOakLog, "Dark Oak Log", kDefaultMaxStack, DARK_OAK_LOG},
    {ItemId::DarkOakLeaves, "Dark Oak Leaves", kDefaultMaxStack, DARK_OAK_LEAVES},
    {ItemId::Cactus, "Cactus", kDefaultMaxStack, CACTUS},
    {ItemId::RedSand, "Red Sand", kDefaultMaxStack, RED_SAND},
    {ItemId::Sandstone, "Sandstone", kDefaultMaxStack, SANDSTONE},
    {ItemId::RedSandstone, "Red Sandstone", kDefaultMaxStack, RED_SANDSTONE},
    {ItemId::Terracotta, "Terracotta", kDefaultMaxStack, TERRACOTTA},
    {ItemId::OrangeTerracotta, "Orange Terracotta", kDefaultMaxStack, ORANGE_TERRACOTTA},
    {ItemId::RedTerracotta, "Red Terracotta", kDefaultMaxStack, RED_TERRACOTTA},
    {ItemId::YellowTerracotta, "Yellow Terracotta", kDefaultMaxStack, YELLOW_TERRACOTTA},
    {ItemId::WhiteTerracotta, "White Terracotta", kDefaultMaxStack, WHITE_TERRACOTTA},
    {ItemId::BrownTerracotta, "Brown Terracotta", kDefaultMaxStack, BROWN_TERRACOTTA},
    {ItemId::CoarseDirt, "Coarse Dirt", kDefaultMaxStack, COARSE_DIRT},
    {ItemId::Podzol, "Podzol", kDefaultMaxStack, PODZOL},
    {ItemId::Clay, "Clay", kDefaultMaxStack, CLAY},
    {ItemId::Mud, "Mud", kDefaultMaxStack, MUD},
    {ItemId::MossBlock, "Moss Block", kDefaultMaxStack, MOSS_BLOCK},
    {ItemId::Ice, "Ice", kDefaultMaxStack, ICE},
    {ItemId::PackedIce, "Packed Ice", kDefaultMaxStack, PACKED_ICE},
    {ItemId::BlueIce, "Blue Ice", kDefaultMaxStack, BLUE_ICE},
    {ItemId::Andesite, "Andesite", kDefaultMaxStack, ANDESITE},
    {ItemId::Diorite, "Diorite", kDefaultMaxStack, DIORITE},
    {ItemId::Granite, "Granite", kDefaultMaxStack, GRANITE},
    {ItemId::Deepslate, "Deepslate", kDefaultMaxStack, DEEPSLATE},
    {ItemId::Tuff, "Tuff", kDefaultMaxStack, TUFF},
    {ItemId::MossyCobblestone, "Mossy Cobblestone", kDefaultMaxStack, MOSSY_COBBLESTONE},
    {ItemId::CherryLog, "Cherry Log", kDefaultMaxStack, CHERRY_LOG},
    {ItemId::CherryLeaves, "Cherry Leaves", kDefaultMaxStack, CHERRY_LEAVES},
    {ItemId::MangroveLog, "Mangrove Log", kDefaultMaxStack, MANGROVE_LOG},
    {ItemId::MangroveRoots, "Mangrove Roots", kDefaultMaxStack, MANGROVE_ROOTS},
    {ItemId::MangroveLeaves, "Mangrove Leaves", kDefaultMaxStack, MANGROVE_LEAVES},
    {ItemId::BambooBlock, "Bamboo Block", kDefaultMaxStack, BAMBOO_BLOCK},
    {ItemId::BambooStalk, "Bamboo Stalk", kDefaultMaxStack, BAMBOO_STALK},
    {ItemId::RedMushroomBlock, "Red Mushroom Block", kDefaultMaxStack, RED_MUSHROOM_BLOCK},
    {ItemId::BrownMushroomBlock, "Brown Mushroom Block", kDefaultMaxStack, BROWN_MUSHROOM_BLOCK},
    {ItemId::MushroomStem, "Mushroom Stem", kDefaultMaxStack, MUSHROOM_STEM},
    {ItemId::Basalt, "Basalt", kDefaultMaxStack, BASALT},
    {ItemId::Blackstone, "Blackstone", kDefaultMaxStack, BLACKSTONE},
    {ItemId::Magma, "Magma", kDefaultMaxStack, MAGMA},
    {ItemId::TubeCoralBlock, "Tube Coral Block", kDefaultMaxStack, TUBE_CORAL_BLOCK},
    {ItemId::BrainCoralBlock, "Brain Coral Block", kDefaultMaxStack, BRAIN_CORAL_BLOCK},
    {ItemId::BubbleCoralBlock, "Bubble Coral Block", kDefaultMaxStack, BUBBLE_CORAL_BLOCK},
    {ItemId::FireCoralBlock, "Fire Coral Block", kDefaultMaxStack, FIRE_CORAL_BLOCK},
    {ItemId::HornCoralBlock, "Horn Coral Block", kDefaultMaxStack, HORN_CORAL_BLOCK},
    {ItemId::DeepslateCoalOre, "Deepslate Coal Ore", kDefaultMaxStack, DEEPSLATE_COAL_ORE},
    {ItemId::DeepslateCopperOre, "Deepslate Copper Ore", kDefaultMaxStack, DEEPSLATE_COPPER_ORE},
    {ItemId::DeepslateDiamondOre, "Deepslate Diamond Ore", kDefaultMaxStack, DEEPSLATE_DIAMOND_ORE},
    {ItemId::DeepslateEmeraldOre, "Deepslate Emerald Ore", kDefaultMaxStack, DEEPSLATE_EMERALD_ORE},
    {ItemId::DeepslateGoldOre, "Deepslate Gold Ore", kDefaultMaxStack, DEEPSLATE_GOLD_ORE},
    {ItemId::DeepslateIronOre, "Deepslate Iron Ore", kDefaultMaxStack, DEEPSLATE_IRON_ORE},
    {ItemId::DeepslateLapisOre, "Deepslate Lapis Ore", kDefaultMaxStack, DEEPSLATE_LAPIS_ORE},
    {ItemId::DeepslateRedstoneOre, "Deepslate Redstone Ore", kDefaultMaxStack, DEEPSLATE_REDSTONE_ORE},
    {ItemId::DripstoneBlock, "Dripstone Block", kDefaultMaxStack, DRIPSTONE_BLOCK},
    {ItemId::Kelp, "Kelp", kDefaultMaxStack, KELP},
    {ItemId::KelpTop, "Kelp Top", kDefaultMaxStack, KELP_TOP},
    {ItemId::ShortGrass, "Short Grass", kDefaultMaxStack, SHORT_GRASS},
    {ItemId::Fern, "Fern", kDefaultMaxStack, FERN},
    {ItemId::Wildflower, "Wildflower", kDefaultMaxStack, WILDFLOWER},
    {ItemId::DryShrub, "Dry Shrub", kDefaultMaxStack, DRY_SHRUB},
    {ItemId::Seagrass, "Seagrass", kDefaultMaxStack, SEAGRASS},
    {ItemId::LilyPad, "Lily Pad", kDefaultMaxStack, LILY_PAD},
}};

// --- Block -> item (derived: placeableBlock round-trip by construction) ---------
constexpr std::array<ItemId, COUNT> kBlockItem = [] {
    std::array<ItemId, COUNT> table{}; // ItemId::None everywhere
    for (const ItemDefinition &def : kDefinitions)
    {
        if (def.placeableBlock.has_value())
            table[static_cast<std::size_t>(*def.placeableBlock)] = def.id;
    }
    // Face variants alias their canonical voxel block's item.
    for (std::size_t i = 0; i < static_cast<std::size_t>(COUNT); ++i)
    {
        const TextureType type = static_cast<TextureType>(i);
        const TextureType canonical = canonicalBlock(type);
        if (canonical != type)
            table[i] = table[static_cast<std::size_t>(canonical)];
    }
    return table;
}();

// --- Block -> drop (EXPLICIT table; None = no drop / unbreakable) ----------------
constexpr std::array<ItemId, COUNT> kBlockDrop = [] {
    std::array<ItemId, COUNT> drops{}; // ItemId::None = no drop
    // BEDROCK: unbreakable. WATER / LAVA: not breakable by hand in v1.
    // (Both stay None explicitly — no drop entry may ever be inferred.)
    drops[BRICKS] = ItemId::Bricks;
    drops[COBBLESTONE] = ItemId::Cobblestone;
    drops[DIRT] = ItemId::Dirt;
    drops[GLASS] = ItemId::Glass; // v1: no silk-touch distinction
    drops[GRASS_TOP] = ItemId::Dirt; // grass block -> dirt (epic #223 §1)
    drops[GRAVEL] = ItemId::Gravel;
    drops[OAK_LEAVES] = ItemId::OakLeaves; // leaves -> themselves (v1 explicit)
    drops[OAK_LOG] = ItemId::OakLog;
    drops[OAK_PLANKS] = ItemId::OakPlanks;
    drops[SAND] = ItemId::Sand;
    drops[SNOW] = ItemId::Snow;
    drops[STONE_BRICKS] = ItemId::StoneBricks;
    drops[STONE] = ItemId::Cobblestone; // stone -> cobblestone (epic #223 §1)
    drops[COAL_ORE] = ItemId::CoalOre; // v1: ores drop their own block
    drops[COPPER_ORE] = ItemId::CopperOre;
    drops[DIAMOND_ORE] = ItemId::DiamondOre;
    drops[EMERALD_ORE] = ItemId::EmeraldOre;
    drops[GOLD_ORE] = ItemId::GoldOre;
    drops[IRON_ORE] = ItemId::IronOre;
    drops[LAPIS_ORE] = ItemId::LapisOre;
    drops[REDSTONE_ORE] = ItemId::RedstoneOre;
    drops[BIRCH_LOG] = ItemId::BirchLog;
    drops[BIRCH_LEAVES] = ItemId::BirchLeaves;
    drops[SPRUCE_LOG] = ItemId::SpruceLog;
    drops[SPRUCE_LEAVES] = ItemId::SpruceLeaves;
    drops[JUNGLE_LOG] = ItemId::JungleLog;
    drops[JUNGLE_LEAVES] = ItemId::JungleLeaves;
    drops[ACACIA_LOG] = ItemId::AcaciaLog;
    drops[ACACIA_LEAVES] = ItemId::AcaciaLeaves;
    drops[DARK_OAK_LOG] = ItemId::DarkOakLog;
    drops[DARK_OAK_LEAVES] = ItemId::DarkOakLeaves;
    drops[CACTUS] = ItemId::Cactus;
    drops[RED_SAND] = ItemId::RedSand;
    drops[SANDSTONE] = ItemId::Sandstone;
    drops[RED_SANDSTONE] = ItemId::RedSandstone;
    drops[TERRACOTTA] = ItemId::Terracotta;
    drops[ORANGE_TERRACOTTA] = ItemId::OrangeTerracotta;
    drops[RED_TERRACOTTA] = ItemId::RedTerracotta;
    drops[YELLOW_TERRACOTTA] = ItemId::YellowTerracotta;
    drops[WHITE_TERRACOTTA] = ItemId::WhiteTerracotta;
    drops[BROWN_TERRACOTTA] = ItemId::BrownTerracotta;
    drops[COARSE_DIRT] = ItemId::CoarseDirt;
    drops[PODZOL] = ItemId::Podzol;
    drops[CLAY] = ItemId::Clay;
    drops[MUD] = ItemId::Mud;
    drops[MOSS_BLOCK] = ItemId::MossBlock;
    drops[ICE] = ItemId::Ice; // v1: no silk-touch distinction
    drops[PACKED_ICE] = ItemId::PackedIce;
    drops[BLUE_ICE] = ItemId::BlueIce;
    drops[ANDESITE] = ItemId::Andesite;
    drops[DIORITE] = ItemId::Diorite;
    drops[GRANITE] = ItemId::Granite;
    drops[DEEPSLATE] = ItemId::Deepslate;
    drops[TUFF] = ItemId::Tuff;
    drops[MOSSY_COBBLESTONE] = ItemId::MossyCobblestone;
    drops[CHERRY_LOG] = ItemId::CherryLog;
    drops[CHERRY_LEAVES] = ItemId::CherryLeaves;
    drops[MANGROVE_LOG] = ItemId::MangroveLog;
    drops[MANGROVE_ROOTS] = ItemId::MangroveRoots;
    drops[MANGROVE_LEAVES] = ItemId::MangroveLeaves;
    drops[BAMBOO_BLOCK] = ItemId::BambooBlock;
    drops[BAMBOO_STALK] = ItemId::BambooStalk;
    drops[RED_MUSHROOM_BLOCK] = ItemId::RedMushroomBlock;
    drops[BROWN_MUSHROOM_BLOCK] = ItemId::BrownMushroomBlock;
    drops[MUSHROOM_STEM] = ItemId::MushroomStem;
    drops[BASALT] = ItemId::Basalt;
    drops[BLACKSTONE] = ItemId::Blackstone;
    drops[MAGMA] = ItemId::Magma;
    drops[TUBE_CORAL_BLOCK] = ItemId::TubeCoralBlock;
    drops[BRAIN_CORAL_BLOCK] = ItemId::BrainCoralBlock;
    drops[BUBBLE_CORAL_BLOCK] = ItemId::BubbleCoralBlock;
    drops[FIRE_CORAL_BLOCK] = ItemId::FireCoralBlock;
    drops[HORN_CORAL_BLOCK] = ItemId::HornCoralBlock;
    drops[DEEPSLATE_COAL_ORE] = ItemId::DeepslateCoalOre;
    drops[DEEPSLATE_COPPER_ORE] = ItemId::DeepslateCopperOre;
    drops[DEEPSLATE_DIAMOND_ORE] = ItemId::DeepslateDiamondOre;
    drops[DEEPSLATE_EMERALD_ORE] = ItemId::DeepslateEmeraldOre;
    drops[DEEPSLATE_GOLD_ORE] = ItemId::DeepslateGoldOre;
    drops[DEEPSLATE_IRON_ORE] = ItemId::DeepslateIronOre;
    drops[DEEPSLATE_LAPIS_ORE] = ItemId::DeepslateLapisOre;
    drops[DEEPSLATE_REDSTONE_ORE] = ItemId::DeepslateRedstoneOre;
    drops[DRIPSTONE_BLOCK] = ItemId::DripstoneBlock;
    drops[KELP] = ItemId::Kelp;
    drops[KELP_TOP] = ItemId::KelpTop;
    drops[SHORT_GRASS] = ItemId::ShortGrass;
    drops[FERN] = ItemId::Fern;
    drops[WILDFLOWER] = ItemId::Wildflower;
    drops[DRY_SHRUB] = ItemId::DryShrub;
    drops[SEAGRASS] = ItemId::Seagrass;
    drops[LILY_PAD] = ItemId::LilyPad;
    return drops;
}();

// --- Registry / catalog alignment static_asserts --------------------------------
// Same discipline as biomeTypeString / kBlockLayers: a misaligned row is a
// COMPILE error, not a runtime surprise in a save file.

static_assert(kDefinitions.size() == kItemCount, "one definition per registry ordinal");

constexpr bool definitionsAligned()
{
    for (std::size_t i = 0; i < kDefinitions.size(); ++i)
    {
        const ItemDefinition &def = kDefinitions[i];
        if (static_cast<uint16_t>(def.id) != static_cast<uint16_t>(i + 1))
            return false; // append-only ordinal == table index + 1
        if (def.name == nullptr || def.name[0] == '\0')
            return false;
        if (def.maxStack == 0)
            return false;
        if (!def.placeableBlock.has_value())
            return false; // v1 catalog: every item is a block item
        const TextureType block = *def.placeableBlock;
        if (block < 0 || block >= COUNT)
            return false;
        if (isFaceVariant(block) || isFluidBlock(block))
            return false; // placeableBlock is a canonical voxel type
        if (kBlockItem[static_cast<std::size_t>(block)] != def.id)
            return false; // every placeable item maps back to its block
    }
    return true;
}
static_assert(definitionsAligned(),
              "ItemDefinition ordinals/names/placeableBlock must align with the block catalog");

constexpr bool namesUnique()
{
    for (std::size_t i = 0; i < kDefinitions.size(); ++i)
        for (std::size_t j = i + 1; j < kDefinitions.size(); ++j)
        {
            std::string_view a = kDefinitions[i].name;
            std::string_view b = kDefinitions[j].name;
            if (a == b)
                return false;
        }
    return true;
}
static_assert(namesUnique(), "item names must be unique (name<->id parsing is bijective)");

constexpr bool blockCoverageTotal()
{
    for (std::size_t i = 0; i < static_cast<std::size_t>(COUNT); ++i)
    {
        const TextureType type = static_cast<TextureType>(i);
        if (isFaceVariant(type) || isFluidBlock(type))
            continue;
        if (kBlockItem[i] == ItemId::None)
            return false; // every canonical block (incl. BEDROCK) has an item
        if (kBlockItem[i] != kBlockItem[static_cast<std::size_t>(canonicalBlock(type))])
            return false; // variants alias their canonical block
    }
    return true;
}
static_assert(blockCoverageTotal(),
              "every breakable/placeable block needs exactly one block item");

constexpr bool dropTableTotal()
{
    for (std::size_t i = 0; i < static_cast<std::size_t>(COUNT); ++i)
    {
        const TextureType type = static_cast<TextureType>(i);
        if (isFaceVariant(type))
            continue; // drops are keyed by canonical block
        const bool breakable = type != BEDROCK && !isFluidBlock(type);
        const ItemId drop = kBlockDrop[i];
        if (breakable != (drop != ItemId::None))
            return false; // totality: breakable <-> has a drop
        if (drop != ItemId::None)
        {
            const uint16_t ordinal = static_cast<uint16_t>(drop);
            if (ordinal == 0 || ordinal > kItemCount)
                return false;
            if (!kDefinitions[ordinal - 1].placeableBlock.has_value())
                return false; // v1: every drop is a placeable block item
        }
    }
    return true;
}
static_assert(dropTableTotal(), "drop table must be total over breakable blocks");

// The epic's explicit drop decisions, pinned at compile time.
static_assert(kBlockDrop[STONE] == ItemId::Cobblestone, "STONE drops COBBLESTONE");
static_assert(kBlockDrop[GRASS_TOP] == ItemId::Dirt, "grass block drops DIRT");
static_assert(kBlockDrop[BEDROCK] == ItemId::None, "BEDROCK is unbreakable");
static_assert(kBlockDrop[WATER] == ItemId::None, "WATER is not breakable by hand");
static_assert(kBlockDrop[LAVA] == ItemId::None, "LAVA is not breakable by hand");
static_assert(kBlockDrop[OAK_LEAVES] == ItemId::OakLeaves &&
                  kBlockDrop[BIRCH_LEAVES] == ItemId::BirchLeaves &&
                  kBlockDrop[SPRUCE_LEAVES] == ItemId::SpruceLeaves &&
                  kBlockDrop[JUNGLE_LEAVES] == ItemId::JungleLeaves &&
                  kBlockDrop[ACACIA_LEAVES] == ItemId::AcaciaLeaves &&
                  kBlockDrop[DARK_OAK_LEAVES] == ItemId::DarkOakLeaves &&
                  kBlockDrop[CHERRY_LEAVES] == ItemId::CherryLeaves &&
                  kBlockDrop[MANGROVE_LEAVES] == ItemId::MangroveLeaves,
              "v1 leaves drop themselves (explicit decision, no RNG)");

// --- Token normalization (the textureTypeString / SimScript discipline) ---------
std::string upperUnderscore(std::string_view token)
{
    std::string out;
    out.reserve(token.size());
    for (const char c : token)
    {
        if (c == ' ' || c == '-')
            out.push_back('_');
        else
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

// One-time normalized-name table (thread-safe magic static): parseItemId
// compares against precomputed UPPER_UNDERSCORE tokens instead of
// re-normalizing every catalog name on each call (PR #231 review: keep the
// grammar seam near-allocation-free before #230 wires it into script
// parsing). Per call this leaves only the input copy + normalized token.
const std::array<std::string, kItemCount> &normalizedItemNames()
{
    static const std::array<std::string, kItemCount> table = [] {
        std::array<std::string, kItemCount> names{};
        for (std::size_t i = 0; i < kItemCount; ++i)
            names[i] = upperUnderscore(kDefinitions[i].name);
        return names;
    }();
    return table;
}

} // namespace

// --- Registry access -------------------------------------------------------------

std::span<const ItemDefinition> itemDefinitions()
{
    return kDefinitions;
}

const ItemDefinition *definition(ItemId id)
{
    const uint16_t ordinal = static_cast<uint16_t>(id);
    if (ordinal == 0 || ordinal > kItemCount)
        return nullptr;
    return &kDefinitions[ordinal - 1];
}

std::string_view itemName(ItemId id)
{
    const ItemDefinition *def = definition(id);
    return def != nullptr ? std::string_view(def->name) : std::string_view();
}

uint16_t maxStackOf(ItemId id)
{
    const ItemDefinition *def = definition(id);
    return def != nullptr ? def->maxStack : 0;
}

std::optional<TextureType> placeableBlockOf(ItemId id)
{
    const ItemDefinition *def = definition(id);
    return def != nullptr ? def->placeableBlock : std::nullopt;
}

bool isPlaceable(ItemId id)
{
    return placeableBlockOf(id).has_value();
}

// --- Block mappings ----------------------------------------------------------------

ItemId itemForBlock(TextureType block)
{
    if (block < 0 || block >= COUNT)
        return ItemId::None;
    return kBlockItem[static_cast<std::size_t>(block)];
}

bool isBreakableBlock(TextureType block)
{
    if (block < 0 || block >= COUNT)
        return false;
    const TextureType canonical = canonicalBlock(block);
    return canonical != BEDROCK && !isFluidBlock(canonical);
}

ItemStack dropForBlock(TextureType block)
{
    if (block < 0 || block >= COUNT)
        return {};
    const ItemId drop = kBlockDrop[static_cast<std::size_t>(canonicalBlock(block))];
    if (drop == ItemId::None)
        return {};
    return {drop, 1}; // v1: breaking yields exactly one drop item
}

// --- Name parsing --------------------------------------------------------------------

std::optional<ItemId> parseItemId(std::string_view token)
{
    if (token.empty())
        return std::nullopt;

    // Numeric registry ordinal (append-only ids; 0 / None is not an item).
    const std::string text(token);
    char *end = nullptr;
    const long ordinal = std::strtol(text.c_str(), &end, 10);
    if (end != text.c_str() && *end == '\0')
    {
        if (ordinal >= 1 && ordinal <= static_cast<long>(kItemCount))
            return static_cast<ItemId>(ordinal);
        return std::nullopt;
    }

    const std::string normalized = upperUnderscore(token);
    const std::array<std::string, kItemCount> &names = normalizedItemNames();
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        if (normalized == names[i])
            return kDefinitions[i].id;
    }
    return std::nullopt;
}

} // namespace items
