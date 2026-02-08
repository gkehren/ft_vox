#pragma once

#include <iostream>
#include <vector>
#include <array>
#include <tuple>
#include <map>
#include <glm/glm.hpp>

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
	uint8_t type : 6; // 16 types (2^4 = 16)
};

struct Vertex
{
	glm::vec3 position;
	glm::vec3 normal;
	glm::vec2 texCoord;
	float textureIndex;
	float useBiomeColor;
	glm::vec3 biomeColor;

	bool operator==(const Vertex &other) const
	{
		return position == other.position &&
			   normal == other.normal &&
			   texCoord == other.texCoord &&
			   textureIndex == other.textureIndex &&
			   useBiomeColor == other.useBiomeColor &&
			   biomeColor == other.biomeColor;
	}
};

inline void hash_combine(std::size_t &seed, float v)
{
	std::hash<float> hasher;
	seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

struct VertexHasher
{
	std::size_t operator()(const Vertex &vertex) const
	{
		size_t seed = 0;
		hash_combine(seed, vertex.position.x);
		hash_combine(seed, vertex.position.y);
		hash_combine(seed, vertex.position.z);
		hash_combine(seed, vertex.normal.x);
		hash_combine(seed, vertex.normal.y);
		hash_combine(seed, vertex.normal.z);
		hash_combine(seed, vertex.texCoord.x);
		hash_combine(seed, vertex.texCoord.y);
		hash_combine(seed, vertex.textureIndex);
		hash_combine(seed, vertex.useBiomeColor);
		hash_combine(seed, vertex.biomeColor.x);
		hash_combine(seed, vertex.biomeColor.y);
		hash_combine(seed, vertex.biomeColor.z);
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

	BIOME_COUNT
};

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
	COUNT, // Keep last
	AIR	   // Keep after count beacuse AIR is not a texture
};

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

struct BiomeParameters
{
	// General biome properties
	float baseHeight;
	float heightVariation;

	// Surface properties
	TextureType surfaceBlock;
	TextureType subSurfaceBlock;
	int subSurfaceDepth;

	// Noise parameters
	float noiseScale;
	int octaves;
	float persistence;
	glm::vec3 waterColor;

	// Mountain-specific parameters
	float mountainNoiseScale;
	int mountainOctaves;
	float mountainPersistence;
	float mountainDetailScale;
	float mountainDetailInfluence;
	float peakNoiseScale;
	float peakThreshold;
	float peakMultiplier;
};