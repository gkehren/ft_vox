#pragma once

#include <iostream>
#include <vector>
#include <array>
#include <tuple>
#include <map>
#include <glm/glm.hpp>
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

struct Vertex
{
	glm::vec3 position;
	uint32_t packedData; // 0-2: normal, 3-10: textureIndex, 11: useBiomeColor, 12-13: AO
	glm::vec2 texCoord;
	uint32_t packedBiomeColor; // RGBA8

	bool operator==(const Vertex &other) const
	{
		return position == other.position &&
			   packedData == other.packedData &&
			   texCoord == other.texCoord &&
			   packedBiomeColor == other.packedBiomeColor;
	}
};

inline void hash_combine(std::size_t &seed, uint32_t v)
{
	seed ^= std::hash<uint32_t>{}(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

struct VertexHasher
{
	std::size_t operator()(const Vertex &vertex) const
	{
		size_t seed = 0;
		uint32_t px, py, pz, tx, ty;
		std::memcpy(&px, &vertex.position.x, 4);
		std::memcpy(&py, &vertex.position.y, 4);
		std::memcpy(&pz, &vertex.position.z, 4);
		std::memcpy(&tx, &vertex.texCoord.x, 4);
		std::memcpy(&ty, &vertex.texCoord.y, 4);

		hash_combine(seed, px);
		hash_combine(seed, py);
		hash_combine(seed, pz);
		hash_combine(seed, vertex.packedData);
		hash_combine(seed, tx);
		hash_combine(seed, ty);
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

static const std::map<TextureType, std::string> textureTypeString = {
	{BEDROCK, "Bedrock"},
	{BRICKS, "Bricks"},
	{COBBLESTONE, "Cobblestone"},
	{DIRT, "Dirt"},
	{GLASS, "Glass"},
	{GRASS_TOP, "Grass Top"},
	{GRASS_SIDE, "Grass Side"},
	{GRAVEL, "Gravel"},
	{OAK_LEAVES, "Oak Leaves"},
	{OAK_LOG_TOP, "Oak Log Top"},
	{OAK_LOG, "Oak Log"},
	{OAK_PLANKS, "Oak Planks"},
	{SAND, "Sand"},
	{SNOW, "Snow"},
	{STONE_BRICKS, "Stone Bricks"},
	{STONE, "Stone"},
	{COAL_ORE, "Coal Ore"},
	{COPPER_ORE, "Copper Ore"},
	{DIAMOND_ORE, "Diamond Ore"},
	{EMERALD_ORE, "Emerald Ore"},
	{GOLD_ORE, "Gold Ore"},
	{IRON_ORE, "Iron Ore"},
	{LAPIS_ORE, "Lapis Ore"},
	{REDSTONE_ORE, "Redstone Ore"},
	{WATER, "Water"},
	{BIRCH_LOG, "Birch Log"},
	{BIRCH_LOG_TOP, "Birch Log Top"},
	{BIRCH_LEAVES, "Birch Leaves"},
	{SPRUCE_LOG, "Spruce Log"},
	{SPRUCE_LOG_TOP, "Spruce Log Top"},
	{SPRUCE_LEAVES, "Spruce Leaves"},
	{JUNGLE_LOG, "Jungle Log"},
	{JUNGLE_LOG_TOP, "Jungle Log Top"},
	{JUNGLE_LEAVES, "Jungle Leaves"},
	{ACACIA_LOG, "Acacia Log"},
	{ACACIA_LOG_TOP, "Acacia Log Top"},
	{ACACIA_LEAVES, "Acacia Leaves"},
	{DARK_OAK_LOG, "Dark Oak Log"},
	{DARK_OAK_LOG_TOP, "Dark Oak Log Top"},
	{DARK_OAK_LEAVES, "Dark Oak Leaves"},
	{CACTUS, "Cactus"},
	{CACTUS_TOP, "Cactus Top"},
	{RED_SAND, "Red Sand"},
	{SANDSTONE, "Sandstone"},
	{RED_SANDSTONE, "Red Sandstone"},
	{TERRACOTTA, "Terracotta"},
	{ORANGE_TERRACOTTA, "Orange Terracotta"},
	{RED_TERRACOTTA, "Red Terracotta"},
	{YELLOW_TERRACOTTA, "Yellow Terracotta"},
	{WHITE_TERRACOTTA, "White Terracotta"},
	{BROWN_TERRACOTTA, "Brown Terracotta"},
	{COARSE_DIRT, "Coarse Dirt"},
	{PODZOL, "Podzol"},
	{CLAY, "Clay"},
	{MUD, "Mud"},
	{MOSS_BLOCK, "Moss Block"},
	{ICE, "Ice"},
	{PACKED_ICE, "Packed Ice"},
	{BLUE_ICE, "Blue Ice"},
	{ANDESITE, "Andesite"},
	{DIORITE, "Diorite"},
	{GRANITE, "Granite"},
	{DEEPSLATE, "Deepslate"},
	{DEEPSLATE_TOP, "Deepslate Top"},
	{TUFF, "Tuff"},
	{MOSSY_COBBLESTONE, "Mossy Cobblestone"},
	{CHERRY_LOG, "Cherry Log"},
	{CHERRY_LOG_TOP, "Cherry Log Top"},
	{CHERRY_LEAVES, "Cherry Leaves"},
	{MANGROVE_LOG, "Mangrove Log"},
	{MANGROVE_LOG_TOP, "Mangrove Log Top"},
	{MANGROVE_ROOTS, "Mangrove Roots"},
	{MANGROVE_ROOTS_TOP, "Mangrove Roots Top"},
	{MANGROVE_LEAVES, "Mangrove Leaves"},
	{BAMBOO_BLOCK, "Bamboo Block"},
	{BAMBOO_BLOCK_TOP, "Bamboo Block Top"},
	{BAMBOO_STALK, "Bamboo Stalk"},
	{RED_MUSHROOM_BLOCK, "Red Mushroom Block"},
	{BROWN_MUSHROOM_BLOCK, "Brown Mushroom Block"},
	{MUSHROOM_STEM, "Mushroom Stem"},
	{BASALT, "Basalt"},
	{BASALT_TOP, "Basalt Top"},
	{BLACKSTONE, "Blackstone"},
	{MAGMA, "Magma"},
	{TUBE_CORAL_BLOCK, "Tube Coral Block"},
	{BRAIN_CORAL_BLOCK, "Brain Coral Block"},
	{BUBBLE_CORAL_BLOCK, "Bubble Coral Block"},
	{FIRE_CORAL_BLOCK, "Fire Coral Block"},
	{HORN_CORAL_BLOCK, "Horn Coral Block"},
	{LAVA, "Lava"},
	{DEEPSLATE_COAL_ORE, "Deepslate Coal Ore"},
	{DEEPSLATE_COPPER_ORE, "Deepslate Copper Ore"},
	{DEEPSLATE_DIAMOND_ORE, "Deepslate Diamond Ore"},
	{DEEPSLATE_EMERALD_ORE, "Deepslate Emerald Ore"},
	{DEEPSLATE_GOLD_ORE, "Deepslate Gold Ore"},
	{DEEPSLATE_IRON_ORE, "Deepslate Iron Ore"},
	{DEEPSLATE_LAPIS_ORE, "Deepslate Lapis Ore"},
	{DEEPSLATE_REDSTONE_ORE, "Deepslate Redstone Ore"},
	{DRIPSTONE_BLOCK, "Dripstone Block"},
	{KELP, "Kelp"},
	{KELP_TOP, "Kelp Top"},
};

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
