#pragma once

#include <iostream>
#include <vector>
#include <tuple>
#include <map>

#define WINDOW_WIDTH	1920
#define WINDOW_HEIGHT	1080

#define WORLD_SIZE		4096 //16384
#define WORLD_HEIGHT	256

#ifdef __linux__	// linux
#define RES_PATH		"/home/gkehren/Documents/ft_vox/ressources/"
#elif __APPLE__		// Mac
#define RES_PATH		"/Users/gkehren/Documents/ft_vox/ressources/"
#endif

enum TextureType {
	TEXTURE_DEFAULT,
	TEXTURE_STONE,
	TEXTURE_DIRT,
	TEXTURE_GRASS,
	TEXTURE_PLANK,
	TEXTURE_SLAB,
	TEXTURE_SMOOTH_STONE,
	TEXTURE_BRICK,
	TEXTURE_COUNT, // Keep last
	TEXTURE_AIR // Keep after count beacuse AIR is not a texture
};

enum Face {
	LEFT,
	RIGHT,
	TOP,
	BOTTOM,
	FRONT,
	BACK
};

enum ChunkState {
	UNLOADED,
	GENERATED,
	MESHED,
	LOADED
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

static const std::map<Face, std::pair<glm::vec3, std::vector<glm::vec3>>> faceData = {
	{Face::FRONT, {glm::vec3(0.0f, 0.0f, 1.0f), {
		glm::vec3(-0.5f, -0.5f, 0.5f),
		glm::vec3(0.5f, -0.5f, 0.5f),
		glm::vec3(0.5f, 0.5f, 0.5f),
		glm::vec3(-0.5f, -0.5f, 0.5f),
		glm::vec3(0.5f, 0.5f, 0.5f),
		glm::vec3(-0.5f, 0.5f, 0.5f)
	}}},
	{Face::BACK, {glm::vec3(0.0f, 0.0f, -1.0f), {
		glm::vec3(0.5f, -0.5f, -0.5f),
		glm::vec3(-0.5f, -0.5f, -0.5f),
		glm::vec3(-0.5f, 0.5f, -0.5f),
		glm::vec3(0.5f, -0.5f, -0.5f),
		glm::vec3(-0.5f, 0.5f, -0.5f),
		glm::vec3(0.5f, 0.5f, -0.5f)
	}}},
	{Face::LEFT, {glm::vec3(-1.0f, 0.0f, 0.0f), {
		glm::vec3(-0.5f, -0.5f, -0.5f),
		glm::vec3(-0.5f, -0.5f, 0.5f),
		glm::vec3(-0.5f, 0.5f, 0.5f),
		glm::vec3(-0.5f, -0.5f, -0.5f),
		glm::vec3(-0.5f, 0.5f, 0.5f),
		glm::vec3(-0.5f, 0.5f, -0.5f)
	}}},
	{Face::RIGHT, {glm::vec3(1.0f, 0.0f, 0.0f), {
		glm::vec3(0.5f, -0.5f, 0.5f),
		glm::vec3(0.5f, -0.5f, -0.5f),
		glm::vec3(0.5f, 0.5f, -0.5f),
		glm::vec3(0.5f, -0.5f, 0.5f),
		glm::vec3(0.5f, 0.5f, -0.5f),
		glm::vec3(0.5f, 0.5f, 0.5f)
	}}},
	{Face::TOP, {glm::vec3(0.0f, 1.0f, 0.0f), {
		glm::vec3(-0.5f, 0.5f, 0.5f),
		glm::vec3(0.5f, 0.5f, 0.5f),
		glm::vec3(0.5f, 0.5f, -0.5f),
		glm::vec3(-0.5f, 0.5f, 0.5f),
		glm::vec3(0.5f, 0.5f, -0.5f),
		glm::vec3(-0.5f, 0.5f, -0.5f)
	}}},
	{Face::BOTTOM, {glm::vec3(0.0f, -1.0f, 0.0f), {
		glm::vec3(-0.5f, -0.5f, -0.5f),
		glm::vec3(0.5f, -0.5f, -0.5f),
		glm::vec3(0.5f, -0.5f, 0.5f),
		glm::vec3(-0.5f, -0.5f, -0.5f),
		glm::vec3(0.5f, -0.5f, 0.5f),
		glm::vec3(-0.5f, -0.5f, 0.5f)
	}}}
};
