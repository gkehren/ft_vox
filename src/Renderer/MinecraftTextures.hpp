#pragma once

#include <utils.hpp>

#include <cstddef>
#include <fstream>
#include <string>

/// One atlas layer: Minecraft pack basename + fallback + transparency.
/// Indexed by TextureType 0 .. COUNT-1. Single source for load paths and isTransparent.
struct BlockLayerDesc
{
	const char *file;		  ///< Pack / preferred name, e.g. "birch_leaves.png"
	const char *fallbackFile; ///< Fallback texture name in default pack, e.g. "oak_leaves.png"
	bool transparent;
};

/// Compile-time table in TextureType order (must match enum ordinals).
inline constexpr BlockLayerDesc kBlockLayers[] = {
	// Existing core (fallbackFile == file)
	{"bedrock.png", "bedrock.png", false},					// BEDROCK
	{"bricks.png", "bricks.png", false},					// BRICKS
	{"cobblestone.png", "cobblestone.png", false},			// COBBLESTONE
	{"dirt.png", "dirt.png", false},						// DIRT
	{"glass.png", "glass.png", true},						// GLASS
	{"grass_block_top.png", "grass_block_top.png", false},	// GRASS_TOP
	{"grass_block_side.png", "grass_block_side.png", false}, // GRASS_SIDE
	{"gravel.png", "gravel.png", false},					// GRAVEL
	{"oak_leaves.png", "oak_leaves.png", true},				// OAK_LEAVES
	{"oak_log_top.png", "oak_log_top.png", false},			// OAK_LOG_TOP
	{"oak_log.png", "oak_log.png", false},					// OAK_LOG
	{"oak_planks.png", "oak_planks.png", false},			// OAK_PLANKS
	{"sand.png", "sand.png", false},						// SAND
	{"snow.png", "snow.png", false},						// SNOW
	{"stone_bricks.png", "stone_bricks.png", false},		// STONE_BRICKS
	{"stone.png", "stone.png", false},						// STONE
	{"coal_ore.png", "coal_ore.png", false},				// COAL_ORE
	{"copper_ore.png", "copper_ore.png", false},			// COPPER_ORE
	{"diamond_ore.png", "diamond_ore.png", false},			// DIAMOND_ORE
	{"emerald_ore.png", "emerald_ore.png", false},			// EMERALD_ORE
	{"gold_ore.png", "gold_ore.png", false},				// GOLD_ORE
	{"iron_ore.png", "iron_ore.png", false},				// IRON_ORE
	{"lapis_ore.png", "lapis_ore.png", false},				// LAPIS_ORE
	{"redstone_ore.png", "redstone_ore.png", false},		// REDSTONE_ORE
	{"water_still.png", "water_still.png", true},			// WATER
	// Wood / vegetation (fall back to oak / glass when missing in pack)
	{"birch_log.png", "oak_log.png", false},			 // BIRCH_LOG
	{"birch_log_top.png", "oak_log_top.png", false},	 // BIRCH_LOG_TOP
	{"birch_leaves.png", "oak_leaves.png", true},		 // BIRCH_LEAVES
	{"spruce_log.png", "oak_log.png", false},			 // SPRUCE_LOG
	{"spruce_log_top.png", "oak_log_top.png", false},	 // SPRUCE_LOG_TOP
	{"spruce_leaves.png", "oak_leaves.png", true},		 // SPRUCE_LEAVES
	{"jungle_log.png", "oak_log.png", false},			 // JUNGLE_LOG
	{"jungle_log_top.png", "oak_log_top.png", false},	 // JUNGLE_LOG_TOP
	{"jungle_leaves.png", "oak_leaves.png", true},		 // JUNGLE_LEAVES
	{"acacia_log.png", "oak_log.png", false},			 // ACACIA_LOG
	{"acacia_log_top.png", "oak_log_top.png", false},	 // ACACIA_LOG_TOP
	{"acacia_leaves.png", "oak_leaves.png", true},		 // ACACIA_LEAVES
	{"dark_oak_log.png", "oak_log.png", false},			 // DARK_OAK_LOG
	{"dark_oak_log_top.png", "oak_log_top.png", false},	 // DARK_OAK_LOG_TOP
	{"dark_oak_leaves.png", "oak_leaves.png", true},	 // DARK_OAK_LEAVES
	{"cactus_side.png", "oak_log.png", false},			 // CACTUS
	{"cactus_top.png", "oak_log_top.png", false},		 // CACTUS_TOP
	// Climate surfaces
	{"red_sand.png", "sand.png", false},				 // RED_SAND
	{"sandstone.png", "sand.png", false},				 // SANDSTONE
	{"red_sandstone.png", "bricks.png", false},			 // RED_SANDSTONE
	{"terracotta.png", "bricks.png", false},			 // TERRACOTTA
	{"orange_terracotta.png", "bricks.png", false},		 // ORANGE_TERRACOTTA
	{"red_terracotta.png", "bricks.png", false},		 // RED_TERRACOTTA
	{"yellow_terracotta.png", "bricks.png", false},		 // YELLOW_TERRACOTTA
	{"white_terracotta.png", "snow.png", false},		 // WHITE_TERRACOTTA
	{"brown_terracotta.png", "dirt.png", false},		 // BROWN_TERRACOTTA
	{"coarse_dirt.png", "dirt.png", false},				 // COARSE_DIRT
	{"podzol_top.png", "dirt.png", false},				 // PODZOL
	{"clay.png", "sand.png", false},					 // CLAY
	{"mud.png", "dirt.png", false},						 // MUD
	{"moss_block.png", "oak_leaves.png", false},		 // MOSS_BLOCK
	// Ice / stone variants
	{"ice.png", "glass.png", true},						 // ICE
	{"packed_ice.png", "snow.png", false},				 // PACKED_ICE
	{"blue_ice.png", "glass.png", true},				 // BLUE_ICE
	{"andesite.png", "stone.png", false},				 // ANDESITE
	{"diorite.png", "stone.png", false},				 // DIORITE
	{"granite.png", "stone.png", false},				 // GRANITE
	{"deepslate.png", "cobblestone.png", false},		 // DEEPSLATE
	{"deepslate_top.png", "stone.png", false},			 // DEEPSLATE_TOP
	{"tuff.png", "stone.png", false},					 // TUFF
	{"mossy_cobblestone.png", "cobblestone.png", false}, // MOSSY_COBBLESTONE
	// Phase 3 biome blocks
	{"cherry_log.png", "cherry_log.png", false},					 // CHERRY_LOG
	{"cherry_log_top.png", "cherry_log_top.png", false},			 // CHERRY_LOG_TOP
	{"cherry_leaves.png", "cherry_leaves.png", true},				 // CHERRY_LEAVES
	{"mangrove_log.png", "mangrove_log.png", false},				 // MANGROVE_LOG
	{"mangrove_log_top.png", "mangrove_log_top.png", false},		 // MANGROVE_LOG_TOP
	{"mangrove_roots_side.png", "mangrove_roots_side.png", true},	 // MANGROVE_ROOTS
	{"mangrove_roots_top.png", "mangrove_roots_top.png", true},		 // MANGROVE_ROOTS_TOP
	{"mangrove_leaves.png", "mangrove_leaves.png", true},			 // MANGROVE_LEAVES
	{"bamboo_block.png", "bamboo_block.png", false},				 // BAMBOO_BLOCK
	{"bamboo_block_top.png", "bamboo_block_top.png", false},		 // BAMBOO_BLOCK_TOP
	{"bamboo_stalk.png", "bamboo_stalk.png", true},					 // BAMBOO_STALK
	{"red_mushroom_block.png", "red_mushroom_block.png", false},	 // RED_MUSHROOM_BLOCK
	{"brown_mushroom_block.png", "brown_mushroom_block.png", false}, // BROWN_MUSHROOM_BLOCK
	{"mushroom_stem.png", "mushroom_stem.png", false},				 // MUSHROOM_STEM
	{"basalt_side.png", "basalt_side.png", false},					 // BASALT
	{"basalt_top.png", "basalt_top.png", false},					 // BASALT_TOP
	{"blackstone.png", "blackstone.png", false},					 // BLACKSTONE
	{"magma.png", "magma.png", false},								 // MAGMA
	{"tube_coral_block.png", "tube_coral_block.png", false},		 // TUBE_CORAL_BLOCK
	{"brain_coral_block.png", "brain_coral_block.png", false},		 // BRAIN_CORAL_BLOCK
	{"bubble_coral_block.png", "bubble_coral_block.png", false},	 // BUBBLE_CORAL_BLOCK
	{"fire_coral_block.png", "fire_coral_block.png", false},		 // FIRE_CORAL_BLOCK
	{"horn_coral_block.png", "horn_coral_block.png", false},		 // HORN_CORAL_BLOCK
	// Phase 4 underground blocks
	{"lava_still.png", "lava_still.png", false},						 // LAVA
	{"deepslate_coal_ore.png", "deepslate_coal_ore.png", false},		 // DEEPSLATE_COAL_ORE
	{"deepslate_copper_ore.png", "deepslate_copper_ore.png", false},	 // DEEPSLATE_COPPER_ORE
	{"deepslate_diamond_ore.png", "deepslate_diamond_ore.png", false}, // DEEPSLATE_DIAMOND_ORE
	{"deepslate_emerald_ore.png", "deepslate_emerald_ore.png", false}, // DEEPSLATE_EMERALD_ORE
	{"deepslate_gold_ore.png", "deepslate_gold_ore.png", false},		 // DEEPSLATE_GOLD_ORE
	{"deepslate_iron_ore.png", "deepslate_iron_ore.png", false},		 // DEEPSLATE_IRON_ORE
	{"deepslate_lapis_ore.png", "deepslate_lapis_ore.png", false},	 // DEEPSLATE_LAPIS_ORE
	{"deepslate_redstone_ore.png", "deepslate_redstone_ore.png", false}, // DEEPSLATE_REDSTONE_ORE
	{"dripstone_block.png", "dripstone_block.png", false},			 // DRIPSTONE_BLOCK
	// Phase 5 aquatic blocks
	{"kelp_plant.png", "kelp_plant.png", true}, // KELP
	{"kelp.png", "kelp.png", true},			  // KELP_TOP
};

static_assert(sizeof(kBlockLayers) / sizeof(kBlockLayers[0]) ==
				  static_cast<std::size_t>(TextureType::COUNT),
			  "kBlockLayers must match TextureType::COUNT");

inline bool blockLayerIsTransparent(TextureType t)
{
	const int i = static_cast<int>(t);
	if (i < 0 || i >= static_cast<int>(TextureType::COUNT))
		return false;
	return kBlockLayers[static_cast<std::size_t>(i)].transparent;
}

inline const char *blockLayerFile(TextureType t)
{
	const int i = static_cast<int>(t);
	if (i < 0 || i >= static_cast<int>(TextureType::COUNT))
		return nullptr;
	return kBlockLayers[static_cast<std::size_t>(i)].file;
}

inline const char *blockLayerFallbackFile(TextureType t)
{
	const int i = static_cast<int>(t);
	if (i < 0 || i >= static_cast<int>(TextureType::COUNT))
		return nullptr;
	return kBlockLayers[static_cast<std::size_t>(i)].fallbackFile;
}

// ---------------------------------------------------------------------------
// Block policy helpers (meshing / materials)
// ---------------------------------------------------------------------------

inline bool blockIsLeaves(TextureType t)
{
	switch (t)
	{
	case OAK_LEAVES:
	case BIRCH_LEAVES:
	case SPRUCE_LEAVES:
	case JUNGLE_LEAVES:
	case ACACIA_LEAVES:
	case DARK_OAK_LEAVES:
	case CHERRY_LEAVES:
	case MANGROVE_LEAVES:
		return true;
	default:
		return false;
	}
}

/// Biome foliage tint + wind (all leaf species).
inline bool blockIsFoliage(TextureType t)
{
	return blockIsLeaves(t);
}

inline bool blockIsIce(TextureType t)
{
	return t == ICE || t == PACKED_ICE || t == BLUE_ICE;
}

/// Surfaces that can host trees / cacti / ice spikes.
inline bool blockIsPlantableSurface(TextureType t)
{
	switch (t)
	{
	case GRASS_TOP:
	case DIRT:
	case SAND:
	case SNOW:
	case RED_SAND:
	case PODZOL:
	case COARSE_DIRT:
	case MUD:
	case MOSS_BLOCK:
	case TERRACOTTA:
	case ORANGE_TERRACOTTA:
	case RED_TERRACOTTA:
	case YELLOW_TERRACOTTA:
	case BROWN_TERRACOTTA:
	case WHITE_TERRACOTTA:
	case PACKED_ICE:
		return true;
	default:
		return false;
	}
}

/// Stone-like hosts that ore veins may replace.
inline bool blockIsOreHost(TextureType t)
{
	switch (t)
	{
	case STONE:
	case ANDESITE:
	case DIORITE:
	case GRANITE:
	case TUFF:
	case DEEPSLATE:
		return true;
	default:
		return false;
	}
}

/// Soft ground valid for cactus placement (desert / badlands).
inline bool blockIsCactusGround(TextureType t)
{
	return t == SAND || t == RED_SAND || t == TERRACOTTA || t == ORANGE_TERRACOTTA ||
		   t == RED_TERRACOTTA;
}

/// Sky-light propagates through air and transparent layers (same table as meshing).
inline bool blockTransmitsSkyLight(TextureType t)
{
	return t == AIR || blockLayerIsTransparent(t);
}

/// Top-face atlas layer when the stored voxel type has a distinct +Y texture.
inline TextureType blockTopFace(TextureType t)
{
	switch (t)
	{
	case GRASS_SIDE:
	case GRASS_TOP:
		return GRASS_TOP;
	case OAK_LOG:
		return OAK_LOG_TOP;
	case BIRCH_LOG:
		return BIRCH_LOG_TOP;
	case SPRUCE_LOG:
		return SPRUCE_LOG_TOP;
	case JUNGLE_LOG:
		return JUNGLE_LOG_TOP;
	case ACACIA_LOG:
		return ACACIA_LOG_TOP;
	case DARK_OAK_LOG:
		return DARK_OAK_LOG_TOP;
	case CHERRY_LOG:
		return CHERRY_LOG_TOP;
	case MANGROVE_LOG:
		return MANGROVE_LOG_TOP;
	case MANGROVE_ROOTS:
		return MANGROVE_ROOTS_TOP;
	case BAMBOO_BLOCK:
		return BAMBOO_BLOCK_TOP;
	case BASALT:
		return BASALT_TOP;
	case CACTUS:
		return CACTUS_TOP;
	case DEEPSLATE:
		return DEEPSLATE_TOP;
	default:
		return t;
	}
}

/// Bottom-face atlas layer (−Y).
inline TextureType blockBottomFace(TextureType t)
{
	switch (t)
	{
	case GRASS_SIDE:
		return DIRT;
	case OAK_LOG:
		return OAK_LOG_TOP;
	case BIRCH_LOG:
		return BIRCH_LOG_TOP;
	case SPRUCE_LOG:
		return SPRUCE_LOG_TOP;
	case JUNGLE_LOG:
		return JUNGLE_LOG_TOP;
	case ACACIA_LOG:
		return ACACIA_LOG_TOP;
	case DARK_OAK_LOG:
		return DARK_OAK_LOG_TOP;
	case CHERRY_LOG:
		return CHERRY_LOG_TOP;
	case MANGROVE_LOG:
		return MANGROVE_LOG_TOP;
	case MANGROVE_ROOTS:
		return MANGROVE_ROOTS_TOP;
	case BAMBOO_BLOCK:
		return BAMBOO_BLOCK_TOP;
	case BASALT:
		return BASALT_TOP;
	case CACTUS:
		return CACTUS_TOP;
	case DEEPSLATE:
		return DEEPSLATE_TOP;
	default:
		return t;
	}
}

/// Atlas layer for a face given world-space normal (dominant axis).
inline TextureType blockFaceTexture(TextureType t, float normalY)
{
	if (normalY > 0.9f)
		return blockTopFace(t);
	if (normalY < -0.9f)
		return blockBottomFace(t);
	return t;
}

inline std::string trimTrailingSlashes(std::string path)
{
	while (!path.empty() && (path.back() == '/' || path.back() == '\\'))
		path.pop_back();
	return path;
}

/// Minecraft vertical animation strip: first frame is width×width when height >= 2*width.
inline void blockTextureFrameSize(int imageW, int imageH, int &frameW, int &frameH)
{
	if (imageW > 0 && imageH >= 2 * imageW)
	{
		frameW = imageW;
		frameH = imageW;
		return;
	}
	frameW = imageW;
	frameH = imageH;
}

/// Result of resolving pack vs bundled block textures (CPU side).
struct TextureAtlasLoadReport
{
	int requiredLayers{0}; ///< TextureType::COUNT
	int packHits{0};	   ///< layers taken from the pack
	int packMisses{0};	   ///< layers that fell back to bundled
	bool packRequested{false};

	bool packInvalid() const { return packRequested && packHits == 0; }
	bool packIncomplete() const { return packRequested && packHits > 0 && packMisses > 0; }
	bool packComplete() const { return !packRequested || packMisses == 0; }
};
