#pragma once

#include <iostream>
#include <vector>
#include <array>
#include <string_view>
#include <tuple>
#include <glm/glm.hpp>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define WORLD_SIZE 16384
#define WORLD_HEIGHT 256

#define RES_PATH "./ressources/"

struct IVec3Hash
{
	std::size_t operator()(const glm::ivec3 &v) const
	{
		std::size_t h1 = std::hash<int>()(v.x);
		std::size_t h2 = std::hash<int>()(v.y);
		std::size_t h3 = std::hash<int>()(v.z);
		return h1 ^ (h2 << 1) ^ (h3 << 2);
	}
};

struct Voxel
{
	uint8_t type; // Supports up to 256 block types (0-255)
};

struct VoxelDrawData
{
	glm::ivec3 worldOrigin;
	uint32_t flags;
};
static_assert(sizeof(VoxelDrawData) == 16, "VoxelDrawData must be 16 bytes");
static_assert(alignof(VoxelDrawData) >= 4, "VoxelDrawData must be at least 4-byte aligned");

struct Vertex
{
	// Packed position layout: chunk-local quantized at kPositionScale.
	// X/Z cover chunk-local 0..16 including greedy endpoints at boundaries;
	// Y covers the full chunk height (0..256). Out-of-range coordinates are
	// a meshing bug: packPosition asserts in Debug instead of wrapping.
	static constexpr uint32_t kXBits = 9;
	static constexpr uint32_t kYBits = 14;
	static constexpr uint32_t kZBits = 9;
	static constexpr uint32_t kXMask = (1u << kXBits) - 1u;
	static constexpr uint32_t kYMask = (1u << kYBits) - 1u;
	static constexpr uint32_t kZMask = (1u << kZBits) - 1u;
	static constexpr uint32_t kXMax = kXMask;	   // 511 -> 31.9375 at 1/16
	static constexpr uint32_t kYMax = kYMask;	   // 16383 -> 1023.9375
	static constexpr uint32_t kZMax = kZMask;

	uint32_t packedPos;        // 9 bits X (scale 16), 14 bits Y (scale 16), 9 bits Z (scale 16)
	uint32_t packedData;       // 0-2: normal, 3-10: textureIndex, 11: useBiomeColor, 12-13: AO, 14-17: skyLight, 18-21: blockLight
	uint16_t texCoordU;
	uint16_t texCoordV;
	uint32_t packedBiomeColor; // RGBA8

	static constexpr float kPositionScale = 16.0f;
	static constexpr float kInvPositionScale = 1.0f / 16.0f;

	static uint32_t packPosition(const glm::vec3 &localPos)
	{
		const long qx = std::lround(localPos.x * kPositionScale);
		const long qy = std::lround(localPos.y * kPositionScale);
		const long qz = std::lround(localPos.z * kPositionScale);
		assert(qx >= 0 && qx <= static_cast<long>(kXMax) && "chunk-local X outside packed range");
		assert(qy >= 0 && qy <= static_cast<long>(kYMax) && "chunk-local Y outside packed range");
		assert(qz >= 0 && qz <= static_cast<long>(kZMax) && "chunk-local Z outside packed range");

		const uint32_t px = static_cast<uint32_t>(qx) & kXMask;
		const uint32_t py = static_cast<uint32_t>(qy) & kYMask;
		const uint32_t pz = static_cast<uint32_t>(qz) & kZMask;
		return px | (py << kXBits) | (pz << (kXBits + kYBits));
	}

	static glm::vec3 unpackPosition(uint32_t p)
	{
		const float x = static_cast<float>(p & kXMask) * kInvPositionScale;
		const float y = static_cast<float>((p >> kXBits) & kYMask) * kInvPositionScale;
		const float z = static_cast<float>((p >> (kXBits + kYBits)) & kZMask) * kInvPositionScale;
		return {x, y, z};
	}

	glm::vec3 decodePosition(const glm::vec3 &origin = {0.f, 0.f, 0.f}) const
	{
		return origin + unpackPosition(packedPos);
	}

	glm::vec2 decodeTexCoord() const
	{
		return {static_cast<float>(texCoordU), static_cast<float>(texCoordV)};
	}

	bool operator==(const Vertex &other) const
	{
		return packedPos == other.packedPos &&
		       packedData == other.packedData &&
		       texCoordU == other.texCoordU &&
		       texCoordV == other.texCoordV &&
		       packedBiomeColor == other.packedBiomeColor;
	}
};
static_assert(sizeof(Vertex) == 16, "Vertex must be 16 bytes");
// Vertex-input attribute offsets must match makeVertexInput (WorldRenderer):
// R32_UINT @0, R32_UINT @4, R16G16_UINT @8, R32_UINT @12.
static_assert(offsetof(Vertex, packedPos) == 0, "vertex attribute 0 offset");
static_assert(offsetof(Vertex, packedData) == 4, "vertex attribute 1 offset");
static_assert(offsetof(Vertex, texCoordU) == 8, "vertex attribute 2 offset");
static_assert(offsetof(Vertex, packedBiomeColor) == 12, "vertex attribute 3 offset");


inline void hash_combine(std::size_t &seed, uint32_t v)
{
	seed ^= std::hash<uint32_t>{}(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

struct VertexHasher
{
	std::size_t operator()(const Vertex &vertex) const
	{
		size_t seed = 0;
		hash_combine(seed, vertex.packedPos);
		hash_combine(seed, vertex.packedData);
		hash_combine(seed, static_cast<uint32_t>(vertex.texCoordU) | (static_cast<uint32_t>(vertex.texCoordV) << 16u));
		hash_combine(seed, vertex.packedBiomeColor);
		return seed;
	}
};

static constexpr int CHUNK_SIZE = 16;										// Size of a chunk in voxels
static constexpr int CHUNK_HEIGHT = 256;									// Height of a chunk in voxels
static constexpr int CHUNK_VOLUME = CHUNK_SIZE * CHUNK_HEIGHT * CHUNK_SIZE; // Total number of voxels in a chunk
static constexpr int CHUNK_RADIUS = 16;										// Radius of a chunk in world units

// Biome types based on temperature and humidity
enum BiomeType
{
	// Cold biomes
	BIOME_FROZEN_OCEAN, // Very cold, any humidity, below sea level
	BIOME_SNOWY_TUNDRA, // Very cold, low humidity
	BIOME_SNOWY_TAIGA,	// Cold, medium humidity (snowy forest)
	BIOME_ICE_SPIKES,	// Very cold, high humidity

	// Temperate biomes
	BIOME_OCEAN,		// Any temp, below sea level
	BIOME_BEACH,		// Coastal areas
	BIOME_PLAINS,		// Moderate temp, low humidity
	BIOME_FOREST,		// Moderate temp, medium humidity
	BIOME_BIRCH_FOREST, // Moderate temp, medium-high humidity
	BIOME_DARK_FOREST,	// Moderate temp, high humidity
	BIOME_SWAMP,		// Moderate temp, very high humidity
	BIOME_RIVER,		// River biome

	// Hot biomes
	BIOME_DESERT,	// Hot, very low humidity
	BIOME_SAVANNA,	// Hot, low humidity
	BIOME_JUNGLE,	// Hot, high humidity
	BIOME_BADLANDS, // Hot, medium humidity (mesa)

	// Elevation-based biomes
	BIOME_MOUNTAINS,	   // High elevation
	BIOME_SNOWY_MOUNTAINS, // Very high elevation

	// Phase 3 biome expansion — append-only to preserve existing ordinals
	BIOME_FLOWER_MEADOW,
	BIOME_CHERRY_GROVE,
	BIOME_AUTUMN_FOREST,
	BIOME_REDWOOD_FOREST,
	BIOME_MANGROVE_SWAMP,
	BIOME_BAMBOO_JUNGLE,
	BIOME_MOOR,
	BIOME_GLACIER,
	BIOME_FROZEN_RIVER,
	BIOME_VOLCANIC,
	BIOME_OASIS,
	BIOME_MUSHROOM_FIELDS,
	BIOME_CORAL_REEF,

	BIOME_COUNT
};

static_assert(BIOME_SNOWY_MOUNTAINS == 17,
			  "Existing BiomeType ordinals must remain stable");
static_assert(BIOME_FLOWER_MEADOW == 18,
			  "Phase 3 biomes must be appended after legacy biomes");
static_assert(BIOME_COUNT <= 256, "BiomeType values exceed uint8_t-compatible range");

static const char *const biomeTypeString[BIOME_COUNT] = {
	"Frozen Ocean",     // BIOME_FROZEN_OCEAN
	"Snowy Tundra",     // BIOME_SNOWY_TUNDRA
	"Snowy Taiga",      // BIOME_SNOWY_TAIGA
	"Ice Spikes",       // BIOME_ICE_SPIKES
	"Ocean",            // BIOME_OCEAN
	"Beach",            // BIOME_BEACH
	"Plains",           // BIOME_PLAINS
	"Forest",           // BIOME_FOREST
	"Birch Forest",     // BIOME_BIRCH_FOREST
	"Dark Forest",      // BIOME_DARK_FOREST
	"Swamp",            // BIOME_SWAMP
	"River",            // BIOME_RIVER
	"Desert",           // BIOME_DESERT
	"Savanna",          // BIOME_SAVANNA
	"Jungle",           // BIOME_JUNGLE
	"Badlands",         // BIOME_BADLANDS
	"Mountains",        // BIOME_MOUNTAINS
	"Snowy Mountains",  // BIOME_SNOWY_MOUNTAINS
	"Flower Meadow",    // BIOME_FLOWER_MEADOW
	"Cherry Grove",     // BIOME_CHERRY_GROVE
	"Autumn Forest",    // BIOME_AUTUMN_FOREST
	"Redwood Forest",   // BIOME_REDWOOD_FOREST
	"Mangrove Swamp",   // BIOME_MANGROVE_SWAMP
	"Bamboo Jungle",    // BIOME_BAMBOO_JUNGLE
	"Moor",             // BIOME_MOOR
	"Glacier",          // BIOME_GLACIER
	"Frozen River",     // BIOME_FROZEN_RIVER
	"Volcanic",         // BIOME_VOLCANIC
	"Oasis",            // BIOME_OASIS
	"Mushroom Fields",  // BIOME_MUSHROOM_FIELDS
	"Coral Reef",       // BIOME_CORAL_REEF
};

static_assert(sizeof(biomeTypeString) / sizeof(biomeTypeString[0]) == BIOME_COUNT,
			  "Every BiomeType needs a display name");

struct TextureInfo
{
	unsigned int id;
	bool hasTransparency;
	bool hasBiomeColoring;
	glm::vec3 defaultColor;
};

enum TextureType
{
	BEDROCK,
	BRICKS,
	COBBLESTONE,
	DIRT,
	GLASS,
	GRASS_TOP,
	GRASS_SIDE,
	GRAVEL,
	OAK_LEAVES,
	OAK_LOG_TOP,
	OAK_LOG,
	OAK_PLANKS,
	SAND,
	SNOW,
	STONE_BRICKS,
	STONE,
	// ORES
	COAL_ORE,
	COPPER_ORE,
	DIAMOND_ORE,
	EMERALD_ORE,
	GOLD_ORE,
	IRON_ORE,
	LAPIS_ORE,
	REDSTONE_ORE,
	WATER,
	// Append-only expansions (keep 0..WATER ordinals stable)
	// Wood / vegetation
	BIRCH_LOG,
	BIRCH_LOG_TOP,
	BIRCH_LEAVES,
	SPRUCE_LOG,
	SPRUCE_LOG_TOP,
	SPRUCE_LEAVES,
	JUNGLE_LOG,
	JUNGLE_LOG_TOP,
	JUNGLE_LEAVES,
	ACACIA_LOG,
	ACACIA_LOG_TOP,
	ACACIA_LEAVES,
	DARK_OAK_LOG,
	DARK_OAK_LOG_TOP,
	DARK_OAK_LEAVES,
	CACTUS,
	CACTUS_TOP,
	// Climate surfaces
	RED_SAND,
	SANDSTONE,
	RED_SANDSTONE,
	TERRACOTTA,
	ORANGE_TERRACOTTA,
	RED_TERRACOTTA,
	YELLOW_TERRACOTTA,
	WHITE_TERRACOTTA,
	BROWN_TERRACOTTA,
	COARSE_DIRT,
	PODZOL,
	CLAY,
	MUD,
	MOSS_BLOCK,
	// Ice / stone variants
	ICE,
	PACKED_ICE,
	BLUE_ICE,
	ANDESITE,
	DIORITE,
	GRANITE,
	DEEPSLATE,
	DEEPSLATE_TOP,
	TUFF,
	MOSSY_COBBLESTONE,
	// Phase 3 biome blocks (append-only)
	CHERRY_LOG,
	CHERRY_LOG_TOP,
	CHERRY_LEAVES,
	MANGROVE_LOG,
	MANGROVE_LOG_TOP,
	MANGROVE_ROOTS,
	MANGROVE_ROOTS_TOP,
	MANGROVE_LEAVES,
	BAMBOO_BLOCK,
	BAMBOO_BLOCK_TOP,
	BAMBOO_STALK,
	RED_MUSHROOM_BLOCK,
	BROWN_MUSHROOM_BLOCK,
	MUSHROOM_STEM,
	BASALT,
	BASALT_TOP,
	BLACKSTONE,
	MAGMA,
	TUBE_CORAL_BLOCK,
	BRAIN_CORAL_BLOCK,
	BUBBLE_CORAL_BLOCK,
	FIRE_CORAL_BLOCK,
	HORN_CORAL_BLOCK,
	// Phase 4 underground blocks (append-only)
	LAVA,
	DEEPSLATE_COAL_ORE,
	DEEPSLATE_COPPER_ORE,
	DEEPSLATE_DIAMOND_ORE,
	DEEPSLATE_EMERALD_ORE,
	DEEPSLATE_GOLD_ORE,
	DEEPSLATE_IRON_ORE,
	DEEPSLATE_LAPIS_ORE,
	DEEPSLATE_REDSTONE_ORE,
	DRIPSTONE_BLOCK,
	// Phase 5 aquatic blocks (append-only)
	KELP,
	KELP_TOP,
	SHORT_GRASS,
	FERN,
	WILDFLOWER,
	DRY_SHRUB,
	SEAGRASS,
	LILY_PAD,
	COUNT, // Keep last
	AIR	   // Keep after count beacuse AIR is not a texture
};

static_assert(CHERRY_LOG == MOSSY_COBBLESTONE + 1,
			  "Phase 3 TextureType entries must stay append-only");
static_assert(LAVA == HORN_CORAL_BLOCK + 1,
			  "Phase 4 TextureType entries must stay append-only");
static_assert(KELP == DRIPSTONE_BLOCK + 1,
			  "Phase 5 TextureType entries must stay append-only");
// Ensure TextureType fits in Voxel::type (uint8_t)
static_assert(static_cast<int>(AIR) <= 255, "TextureType values exceed uint8_t range for Voxel::type");

inline constexpr std::array<std::string_view, COUNT> textureTypeString = [] {
	std::array<std::string_view, COUNT> names{};
	names[BEDROCK] = "Bedrock";
	names[BRICKS] = "Bricks";
	names[COBBLESTONE] = "Cobblestone";
	names[DIRT] = "Dirt";
	names[GLASS] = "Glass";
	names[GRASS_TOP] = "Grass Top";
	names[GRASS_SIDE] = "Grass Side";
	names[GRAVEL] = "Gravel";
	names[OAK_LEAVES] = "Oak Leaves";
	names[OAK_LOG_TOP] = "Oak Log Top";
	names[OAK_LOG] = "Oak Log";
	names[OAK_PLANKS] = "Oak Planks";
	names[SAND] = "Sand";
	names[SNOW] = "Snow";
	names[STONE_BRICKS] = "Stone Bricks";
	names[STONE] = "Stone";
	names[COAL_ORE] = "Coal Ore";
	names[COPPER_ORE] = "Copper Ore";
	names[DIAMOND_ORE] = "Diamond Ore";
	names[EMERALD_ORE] = "Emerald Ore";
	names[GOLD_ORE] = "Gold Ore";
	names[IRON_ORE] = "Iron Ore";
	names[LAPIS_ORE] = "Lapis Ore";
	names[REDSTONE_ORE] = "Redstone Ore";
	names[WATER] = "Water";
	names[BIRCH_LOG] = "Birch Log";
	names[BIRCH_LOG_TOP] = "Birch Log Top";
	names[BIRCH_LEAVES] = "Birch Leaves";
	names[SPRUCE_LOG] = "Spruce Log";
	names[SPRUCE_LOG_TOP] = "Spruce Log Top";
	names[SPRUCE_LEAVES] = "Spruce Leaves";
	names[JUNGLE_LOG] = "Jungle Log";
	names[JUNGLE_LOG_TOP] = "Jungle Log Top";
	names[JUNGLE_LEAVES] = "Jungle Leaves";
	names[ACACIA_LOG] = "Acacia Log";
	names[ACACIA_LOG_TOP] = "Acacia Log Top";
	names[ACACIA_LEAVES] = "Acacia Leaves";
	names[DARK_OAK_LOG] = "Dark Oak Log";
	names[DARK_OAK_LOG_TOP] = "Dark Oak Log Top";
	names[DARK_OAK_LEAVES] = "Dark Oak Leaves";
	names[CACTUS] = "Cactus";
	names[CACTUS_TOP] = "Cactus Top";
	names[RED_SAND] = "Red Sand";
	names[SANDSTONE] = "Sandstone";
	names[RED_SANDSTONE] = "Red Sandstone";
	names[TERRACOTTA] = "Terracotta";
	names[ORANGE_TERRACOTTA] = "Orange Terracotta";
	names[RED_TERRACOTTA] = "Red Terracotta";
	names[YELLOW_TERRACOTTA] = "Yellow Terracotta";
	names[WHITE_TERRACOTTA] = "White Terracotta";
	names[BROWN_TERRACOTTA] = "Brown Terracotta";
	names[COARSE_DIRT] = "Coarse Dirt";
	names[PODZOL] = "Podzol";
	names[CLAY] = "Clay";
	names[MUD] = "Mud";
	names[MOSS_BLOCK] = "Moss Block";
	names[ICE] = "Ice";
	names[PACKED_ICE] = "Packed Ice";
	names[BLUE_ICE] = "Blue Ice";
	names[ANDESITE] = "Andesite";
	names[DIORITE] = "Diorite";
	names[GRANITE] = "Granite";
	names[DEEPSLATE] = "Deepslate";
	names[DEEPSLATE_TOP] = "Deepslate Top";
	names[TUFF] = "Tuff";
	names[MOSSY_COBBLESTONE] = "Mossy Cobblestone";
	names[CHERRY_LOG] = "Cherry Log";
	names[CHERRY_LOG_TOP] = "Cherry Log Top";
	names[CHERRY_LEAVES] = "Cherry Leaves";
	names[MANGROVE_LOG] = "Mangrove Log";
	names[MANGROVE_LOG_TOP] = "Mangrove Log Top";
	names[MANGROVE_ROOTS] = "Mangrove Roots";
	names[MANGROVE_ROOTS_TOP] = "Mangrove Roots Top";
	names[MANGROVE_LEAVES] = "Mangrove Leaves";
	names[BAMBOO_BLOCK] = "Bamboo Block";
	names[BAMBOO_BLOCK_TOP] = "Bamboo Block Top";
	names[BAMBOO_STALK] = "Bamboo Stalk";
	names[RED_MUSHROOM_BLOCK] = "Red Mushroom Block";
	names[BROWN_MUSHROOM_BLOCK] = "Brown Mushroom Block";
	names[MUSHROOM_STEM] = "Mushroom Stem";
	names[BASALT] = "Basalt";
	names[BASALT_TOP] = "Basalt Top";
	names[BLACKSTONE] = "Blackstone";
	names[MAGMA] = "Magma";
	names[TUBE_CORAL_BLOCK] = "Tube Coral Block";
	names[BRAIN_CORAL_BLOCK] = "Brain Coral Block";
	names[BUBBLE_CORAL_BLOCK] = "Bubble Coral Block";
	names[FIRE_CORAL_BLOCK] = "Fire Coral Block";
	names[HORN_CORAL_BLOCK] = "Horn Coral Block";
	names[LAVA] = "Lava";
	names[DEEPSLATE_COAL_ORE] = "Deepslate Coal Ore";
	names[DEEPSLATE_COPPER_ORE] = "Deepslate Copper Ore";
	names[DEEPSLATE_DIAMOND_ORE] = "Deepslate Diamond Ore";
	names[DEEPSLATE_EMERALD_ORE] = "Deepslate Emerald Ore";
	names[DEEPSLATE_GOLD_ORE] = "Deepslate Gold Ore";
	names[DEEPSLATE_IRON_ORE] = "Deepslate Iron Ore";
	names[DEEPSLATE_LAPIS_ORE] = "Deepslate Lapis Ore";
	names[DEEPSLATE_REDSTONE_ORE] = "Deepslate Redstone Ore";
	names[DRIPSTONE_BLOCK] = "Dripstone Block";
	names[KELP] = "Kelp";
	names[KELP_TOP] = "Kelp Top";
	names[SHORT_GRASS] = "Short Grass";
	names[FERN] = "Fern";
	names[WILDFLOWER] = "Wildflower";
	names[DRY_SHRUB] = "Dry Shrub";
	names[SEAGRASS] = "Seagrass";
	names[LILY_PAD] = "Lily Pad";
	return names;
}();

constexpr bool allTextureTypeNamesDefined()
{
	for (const std::string_view name : textureTypeString)
	{
		if (name.empty())
			return false;
	}
	return true;
}

static_assert(textureTypeString.size() == static_cast<std::size_t>(COUNT));
static_assert(allTextureTypeNamesDefined(),
			  "Every TextureType before COUNT must have a display name");

enum ChunkState
{
	UNLOADED,
	GENERATED,
	MESHED
};

const static std::vector<std::string> skyboxFaces{
	"skybox/right.jpg",
	"skybox/left.jpg",
	"skybox/top.jpg",
	"skybox/bottom.jpg",
	"skybox/front.jpg",
	"skybox/back.jpg"};

const static float skyboxVertices[] = {
	-1.0f, 1.0f, -1.0f,
	-1.0f, -1.0f, -1.0f,
	1.0f, -1.0f, -1.0f,
	1.0f, -1.0f, -1.0f,
	1.0f, 1.0f, -1.0f,
	-1.0f, 1.0f, -1.0f,

	-1.0f, -1.0f, 1.0f,
	-1.0f, -1.0f, -1.0f,
	-1.0f, 1.0f, -1.0f,
	-1.0f, 1.0f, -1.0f,
	-1.0f, 1.0f, 1.0f,
	-1.0f, -1.0f, 1.0f,

	1.0f, -1.0f, -1.0f,
	1.0f, -1.0f, 1.0f,
	1.0f, 1.0f, 1.0f,
	1.0f, 1.0f, 1.0f,
	1.0f, 1.0f, -1.0f,
	1.0f, -1.0f, -1.0f,

	-1.0f, -1.0f, 1.0f,
	-1.0f, 1.0f, 1.0f,
	1.0f, 1.0f, 1.0f,
	1.0f, 1.0f, 1.0f,
	1.0f, -1.0f, 1.0f,
	-1.0f, -1.0f, 1.0f,

	-1.0f, 1.0f, -1.0f,
	1.0f, 1.0f, -1.0f,
	1.0f, 1.0f, 1.0f,
	1.0f, 1.0f, 1.0f,
	-1.0f, 1.0f, 1.0f,
	-1.0f, 1.0f, -1.0f,

	-1.0f, -1.0f, -1.0f,
	-1.0f, -1.0f, 1.0f,
	1.0f, -1.0f, -1.0f,
	1.0f, -1.0f, -1.0f,
	-1.0f, -1.0f, 1.0f,
	1.0f, -1.0f, 1.0f};

const static unsigned int indicesBoundingbox[] = {
	0, 1, 1, 2, 2, 3, 3, 0,
	4, 5, 5, 6, 6, 7, 7, 4,
	0, 4, 1, 5, 2, 6, 3, 7};
