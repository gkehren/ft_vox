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

enum Face {
	FRONT,
	BACK,
	LEFT,
	RIGHT,
	TOP,
	BOTTOM
};

enum ChunkState {
	UNLOADED,
	GENERATED,
	MESHED
};

const static std::vector<std::tuple<int, int, int, Face>> directions {
	{-1, 0, 0, Face::LEFT},
	{1, 0, 0, Face::RIGHT},
	{0, -1, 0, Face::BOTTOM},
	{0, 1, 0, Face::TOP},
	{0, 0, -1, Face::BACK},
	{0, 0, 1, Face::FRONT}
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

const static std::array<std::array<int, 3>, 6> faceOffsets = { {
	{1, 0, 0}, {-1, 0, 0},  // Right, Left
	{0, 1, 0}, {0, -1, 0},  // Top, Bottom
	{0, 0, 1}, {0, 0, -1}   // Front, Back
} };

const static std::array<std::array<std::array<float, 3>, 6>, 6> faceVertices = { {
	// Left face (-X)
	{{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f},
	{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}}},
	// Right face (+X)
	{{{1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f},
	{1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}}},
	// Top face (+Y)
	{{{0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 0.0f},
	{0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}},
	// Bottom face (-Y)
	{{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 1.0f},
	{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}}},
	// Front face (+Z)
	{{{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f},
	{0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 1.0f}}},
	// Back face (-Z)
	{{{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
	{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}}}
} };

const static std::array<std::array<float, 3>, 6> faceNormals = { {
	{-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},   // Left, Right
	{0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f},   // Top, Bottom
	{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}    // Front, Back
} };