#pragma once

#include <utils.hpp>

#include <cstddef>
#include <fstream>
#include <string>

/// One atlas layer: Minecraft block texture basename + meshing transparency.
/// Indexed by TextureType 0 .. COUNT-1. Single source for load paths and isTransparent.
struct BlockLayerDesc
{
	const char *file; ///< e.g. "stone.png" under textures/block/ or ressources/textures/
	bool transparent;
};

/// Compile-time table in TextureType order (must match enum ordinals).
inline constexpr BlockLayerDesc kBlockLayers[] = {
	{"bedrock.png", false},			 // BEDROCK
	{"bricks.png", false},			 // BRICKS
	{"cobblestone.png", false},		 // COBBLESTONE
	{"dirt.png", false},			 // DIRT
	{"glass.png", true},			 // GLASS
	{"grass_block_top.png", false},	 // GRASS_TOP
	{"grass_block_side.png", false}, // GRASS_SIDE
	{"gravel.png", false},			 // GRAVEL
	{"oak_leaves.png", true},		 // OAK_LEAVES
	{"oak_log_top.png", false},		 // OAK_LOG_TOP
	{"oak_log.png", false},			 // OAK_LOG
	{"oak_planks.png", false},		 // OAK_PLANKS
	{"sand.png", false},			 // SAND
	{"snow.png", false},			 // SNOW
	{"stone_bricks.png", false},	 // STONE_BRICKS
	{"stone.png", false},			 // STONE
	{"coal_ore.png", false},		 // COAL_ORE
	{"copper_ore.png", false},		 // COPPER_ORE
	{"diamond_ore.png", false},		 // DIAMOND_ORE
	{"emerald_ore.png", false},		 // EMERALD_ORE
	{"gold_ore.png", false},		 // GOLD_ORE
	{"iron_ore.png", false},		 // IRON_ORE
	{"lapis_ore.png", false},		 // LAPIS_ORE
	{"redstone_ore.png", false},	 // REDSTONE_ORE
	{"water_still.png", true},		 // WATER
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

inline std::string trimTrailingSlashes(std::string path)
{
	while (!path.empty() && (path.back() == '/' || path.back() == '\\'))
		path.pop_back();
	return path;
}

inline bool blockTextureFileReadable(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	return static_cast<bool>(f);
}

/// Resolve block PNG path: pack wins when present; if pack root is set but file is missing,
/// falls back to bundled and sets *fellBackFromPack = true (caller must report).
inline std::string resolveBlockTexturePath(const std::string &packRoot, const char *basename,
										   bool *fellBackFromPack = nullptr)
{
	if (fellBackFromPack)
		*fellBackFromPack = false;

	const std::string bundled = std::string(RES_PATH) + "textures/" + basename;
	if (packRoot.empty())
		return bundled;

	const std::string packPath =
		packRoot + "/assets/minecraft/textures/block/" + basename;
	if (blockTextureFileReadable(packPath))
		return packPath;

	if (fellBackFromPack)
		*fellBackFromPack = true;
	return bundled;
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
