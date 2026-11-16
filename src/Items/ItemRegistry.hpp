#pragma once

// =============================================================================
// ItemRegistry — the single static append-only item catalog (issue #224,
// Epic #223 Phase A).
//
// One ItemDefinition table owns the whole v1 item vocabulary: one block item
// per breakable/placeable block, its stable display name, its maxStack and
// the optional block it places. The block->item and block->drop mappings are
// defined HERE (not in the simulation, not in the renderer):
//
//   * STONE drops COBBLESTONE, the grass block drops DIRT (epic #223 §1);
//   * leaves drop themselves in v1 (explicit table decision — no saplings,
//     no RNG); ores drop their own block (v1 has no raw-mineral items);
//   * BEDROCK is unbreakable; WATER / LAVA are not breakable by hand;
//   * render-only face variants (GRASS_SIDE, *_LOG_TOP, ...) resolve to their
//     canonical voxel block's item, so the lookups are total over the atlas.
//
// static_asserts in ItemRegistry.cpp pin the registry/catalog alignment with
// the same discipline as biomeTypeString / kBlockLayers in utils.hpp:
// ordinal alignment, unique names, placeable round-trips, full block coverage
// and drop-table totality over every breakable block.
//
// Dependency rule: utils.hpp (TextureType catalog) + std ONLY. The render-side
// face remapping (Renderer/MinecraftTextures.hpp blockTopFace) is deliberately
// NOT included — the item domain owns its own canonical-block mapping so the
// CPU core never depends on the renderer (tests/check_core_includes.py).
// =============================================================================

#include <Items/ItemTypes.hpp>
#include <utils.hpp>

#include <optional>
#include <span>
#include <string_view>

namespace items
{

/// v1 default / universal stack size (epic #223 §1: maxStack, 64 default).
inline constexpr uint16_t kDefaultMaxStack = 64;

/// One registry entry. `name` is a stable string literal (save display /
/// headless grammar token source); `placeableBlock` is the canonical voxel
/// TextureType the item places, absent for non-block items (none in v1).
/// Plain value: no heap, no render types, no icon/atlas reference (icons are
/// derived presentation, issue #229).
struct ItemDefinition
{
    ItemId id{ItemId::None};
    const char *name{""};
    uint16_t maxStack{kDefaultMaxStack};
    std::optional<TextureType> placeableBlock{};
};

static_assert(std::is_trivially_copyable_v<ItemDefinition>,
              "ItemDefinition must stay a plain value");

// --- Registry access -----------------------------------------------------------

/// The whole catalog, in ordinal order (index i <-> ItemId(i + 1)).
std::span<const ItemDefinition> itemDefinitions();

/// Registry entry for `id`, or nullptr for None / unknown ids. The pointer is
/// into the static table: stable for the process lifetime, never freed.
const ItemDefinition *definition(ItemId id);

/// Stable display name ("" for None / unknown ids).
std::string_view itemName(ItemId id);

/// Stack capacity of `id` (0 for None / unknown ids — an unknown item can
/// never be stored, merged or placed).
uint16_t maxStackOf(ItemId id);

/// The canonical voxel block `id` places, if any.
std::optional<TextureType> placeableBlockOf(ItemId id);
bool isPlaceable(ItemId id);

// --- Block mappings (total over the render-atlas catalog) -----------------------

/// The block item for a voxel block type. Face variants resolve to their
/// canonical block's item (GRASS_SIDE -> GrassBlock, OAK_LOG_TOP -> OakLog).
/// ItemId::None for AIR, the fluids (WATER / LAVA — no hand-breakable item in
/// v1) and out-of-range types.
ItemId itemForBlock(TextureType block);

/// Whether a block can be broken by hand in v1: every catalog block except
/// BEDROCK (unbreakable) and WATER / LAVA (fluids). Face variants inherit
/// their canonical block's answer; AIR / out-of-range are never breakable.
bool isBreakableBlock(TextureType block);

/// The drop yielded by breaking `block` by hand: exactly one of the drop
/// item in v1 (no counts/RNG yet). Canonical empty for unbreakable blocks,
/// fluids, AIR and out-of-range types. STONE -> Cobblestone, grass block ->
/// Dirt, leaves/ores/etc. -> themselves (explicit table in ItemRegistry.cpp).
ItemStack dropForBlock(TextureType block);

// --- Name parsing (headless grammar seam, issue #230) ----------------------------

/// Parse an item token with the textureTypeString token discipline (the
/// headless SimScript parseBlockName pattern): case-insensitive, spaces and
/// hyphens normalize to underscores ("Grass Block" / "grass-block" /
/// "GRASS_BLOCK" all match), or a numeric registry ordinal in [1, kItemCount].
/// std::nullopt for unknown tokens, the None sentinel ("0" / "none") and
/// out-of-range ordinals. Pure lookup — no allocation beyond the normalized
/// token, no simulation state.
std::optional<ItemId> parseItemId(std::string_view token);

} // namespace items
