#pragma once

#include <iostream>
#include <vector>
#include <array>
#include <tuple>
#include <map>
#include <glm/glm.hpp>

#define WORLD_SIZE		16384
#define WORLD_HEIGHT	256

#define RES_PATH		"./ressources/"

struct ivec3_hash {
	std::size_t operator()(const glm::ivec3& k) const {
		return std::hash<int>()(k.x) ^ std::hash<int>()(k.y) ^ std::hash<int>()(k.z);
	}
};

struct Voxel {
	uint8_t type : 4; // 16 types (2^4 = 16)
	uint8_t active : 1; // Is solid/air
};

struct Vertex {
	glm::vec3 position;
	glm::vec3 normal;
	glm::vec2 texCoord;

	bool operator==(const Vertex& other) const {
		return position == other.position &&
			normal == other.normal &&
			texCoord == other.texCoord;
	}
};

inline void hash_combine(std::size_t& seed, float v) {
	std::hash<float> hasher;
	seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

struct VertexHasher {
	std::size_t operator()(const Vertex& vertex) const {
		size_t seed = 0;
		hash_combine(seed, vertex.position.x);
		hash_combine(seed, vertex.position.y);
		hash_combine(seed, vertex.position.z);
		hash_combine(seed, vertex.normal.x);
		hash_combine(seed, vertex.normal.y);
		hash_combine(seed, vertex.normal.z);
		hash_combine(seed, vertex.texCoord.x);
		hash_combine(seed, vertex.texCoord.y);
		return seed;
	}
};

enum TextureType {
	TEXTURE_DEFAULT,
	TEXTURE_STONE,
	TEXTURE_DIRT,
	TEXTURE_GRASS,
	TEXTURE_PLANK,
	TEXTURE_SLAB,
	TEXTURE_SMOOTH_STONE,
	TEXTURE_BRICK,
	TEXTURE_SAND,
	TEXTURE_SNOW,
	TEXTURE_NETHER,
	TEXTURE_SOUL,
	TEXTURE_COUNT, // Keep last
	TEXTURE_AIR // Keep after count beacuse AIR is not a texture
};

static const std::map<TextureType, std::string> textureTypeString = {
	{TEXTURE_DEFAULT, "DEFAULT"},
	{TEXTURE_STONE, "Stone"},
	{TEXTURE_DIRT, "Dirt"},
	{TEXTURE_GRASS, "Grass"},
	{TEXTURE_PLANK, "Plank"},
	{TEXTURE_SLAB, "Slab"},
	{TEXTURE_SMOOTH_STONE, "Smooth stone"},
	{TEXTURE_BRICK, "Brick"},
	{TEXTURE_SAND, "Sand"},
	{TEXTURE_SNOW, "Snow"},
	{TEXTURE_NETHER, "Netherrack"},
	{TEXTURE_SOUL, "Soul sand"},
};

enum ChunkState {
	UNLOADED,
	GENERATED,
	MESHED
};

const static std::vector<std::string> skyboxFaces {
	"skybox/right.jpg",
	"skybox/left.jpg",
	"skybox/top.jpg",
	"skybox/bottom.jpg",
	"skybox/front.jpg",
	"skybox/back.jpg"
};

const static float skyboxVertices[] = {
	-1.0f,  1.0f, -1.0f,
	-1.0f, -1.0f, -1.0f,
	 1.0f, -1.0f, -1.0f,
	 1.0f, -1.0f, -1.0f,
	 1.0f,  1.0f, -1.0f,
	-1.0f,  1.0f, -1.0f,

	-1.0f, -1.0f,  1.0f,
	-1.0f, -1.0f, -1.0f,
	-1.0f,  1.0f, -1.0f,
	-1.0f,  1.0f, -1.0f,
	-1.0f,  1.0f,  1.0f,
	-1.0f, -1.0f,  1.0f,

	 1.0f, -1.0f, -1.0f,
	 1.0f, -1.0f,  1.0f,
	 1.0f,  1.0f,  1.0f,
	 1.0f,  1.0f,  1.0f,
	 1.0f,  1.0f, -1.0f,
	 1.0f, -1.0f, -1.0f,

	-1.0f, -1.0f,  1.0f,
	-1.0f,  1.0f,  1.0f,
	 1.0f,  1.0f,  1.0f,
	 1.0f,  1.0f,  1.0f,
	 1.0f, -1.0f,  1.0f,
	-1.0f, -1.0f,  1.0f,

	-1.0f,  1.0f, -1.0f,
	 1.0f,  1.0f, -1.0f,
	 1.0f,  1.0f,  1.0f,
	 1.0f,  1.0f,  1.0f,
	-1.0f,  1.0f,  1.0f,
	-1.0f,  1.0f, -1.0f,

	-1.0f, -1.0f, -1.0f,
	-1.0f, -1.0f,  1.0f,
	 1.0f, -1.0f, -1.0f,
	 1.0f, -1.0f, -1.0f,
	-1.0f, -1.0f,  1.0f,
	 1.0f, -1.0f,  1.0f
};

const static unsigned int indicesBoundingbox[] = {
	0, 1, 1, 2, 2, 3, 3, 0,
	4, 5, 5, 6, 6, 7, 7, 4,
	0, 4, 1, 5, 2, 6, 3, 7
};

enum FaceDirection {
	UP = 0,
	DOWN = 1,
	FRONT = 2,
	BACK = 3,
	LEFT = 4,
	RIGHT = 5
};

static const glm::ivec3 directions[6] = {
	{  0,  1,  0 }, // UP
	{  0, -1,  0 }, // DOWN
	{  0,  0,  1 }, // FRONT
	{  0,  0, -1 }, // BACK
	{ -1,  0,  0 }, // LEFT
	{  1,  0,  0 }  // RIGHT
};

// Offsets pour les sommets de chaque face, ordonnés en CCW
static const glm::vec3 faceVertexOffsets[6][4] = {
	// UP face
	{ {0,1,1}, {1,1,1}, {1,1,0}, {0,1,0} },

	// DOWN face
	{ {0,0,0}, {1,0,0}, {1,0,1}, {0,0,1} },

	// FRONT face
	{ {0,0,1}, {1,0,1}, {1,1,1}, {0,1,1} },

	// BACK face
	{ {0,0,0}, {0,1,0}, {1,1,0}, {1,0,0} },

	// LEFT face
	{ {0,0,0}, {0,0,1}, {0,1,1}, {0,1,0} },

	// RIGHT face
	{ {1,0,0}, {1,1,0}, {1,1,1}, {1,0,1} }
};

// Normales pour chaque face
static const glm::vec3 faceNormals[6] = {
	{  0,  1,  0 }, // UP
	{  0, -1,  0 }, // DOWN
	{  0,  0,  1 }, // FRONT
	{  0,  0, -1 }, // BACK
	{ -1,  0,  0 }, // LEFT
	{  1,  0,  0 }  // RIGHT
};

// Coordonnées de texture pour une face
static const glm::vec2 texCoords[4] = {
	{ 0.0f, 0.0f },
	{ 1.0f, 0.0f },
	{ 1.0f, 1.0f },
	{ 0.0f, 1.0f }
};

static const float TEXTURE_ATLAS_SIZE = 512.0f;
static const float TEXTURE_SIZE = 32.0f / TEXTURE_ATLAS_SIZE;
