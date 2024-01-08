#pragma once

#include <iostream>
#include <vector>
#include <tuple>

#define WINDOW_WIDTH	1920
#define WINDOW_HEIGHT	1080

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

const static std::vector<std::tuple<int, int, int, Face>> directions {
	{-1, 0, 0, Face::LEFT},
	{1, 0, 0, Face::RIGHT},
	{0, -1, 0, Face::BOTTOM},
	{0, 1, 0, Face::TOP},
	{0, 0, -1, Face::BACK},
	{0, 0, 1, Face::FRONT}
};

//// FRONT FACE
//this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, 0.5f));
//this->mesh.addVertex(glm::vec3(0.5f, -0.5f, 0.5f));
//this->mesh.addVertex(glm::vec3(0.5f, 0.5f, 0.5f));
//this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, 0.5f));
//this->mesh.addVertex(glm::vec3(0.5f, 0.5f, 0.5f));
//this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, 0.5f));
//this->mesh.addNormal(glm::vec3(0.0f, 0.0f, 1.0f));
//this->mesh.addNormal(glm::vec3(0.0f, 0.0f, 1.0f));
//this->mesh.addNormal(glm::vec3(0.0f, 0.0f, 1.0f));
//this->mesh.addNormal(glm::vec3(0.0f, 0.0f, 1.0f));
//this->mesh.addNormal(glm::vec3(0.0f, 0.0f, 1.0f));
//this->mesh.addNormal(glm::vec3(0.0f, 0.0f, 1.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 1.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 1.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 1.0f));

//// BACK FACE
//this->mesh.addVertex(glm::vec3(0.5f, -0.5f, -0.5f));
//this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, -0.5f));
//this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, -0.5f));
//this->mesh.addVertex(glm::vec3(0.5f, -0.5f, -0.5f));
//this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, -0.5f));
//this->mesh.addVertex(glm::vec3(0.5f, 0.5f, -0.5f));
//this->mesh.addNormal(glm::vec3(0.0f, 0.0f, -1.0f));
//this->mesh.addNormal(glm::vec3(0.0f, 0.0f, -1.0f));
//this->mesh.addNormal(glm::vec3(0.0f, 0.0f, -1.0f));
//this->mesh.addNormal(glm::vec3(0.0f, 0.0f, -1.0f));
//this->mesh.addNormal(glm::vec3(0.0f, 0.0f, -1.0f));
//this->mesh.addNormal(glm::vec3(0.0f, 0.0f, -1.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 1.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 1.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 1.0f));

//// LEFT FACE
//this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, -0.5f));
//this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, 0.5f));
//this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, 0.5f));
//this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, -0.5f));
//this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, 0.5f));
//this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, -0.5f));
//this->mesh.addNormal(glm::vec3(-1.0f, 0.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(-1.0f, 0.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(-1.0f, 0.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(-1.0f, 0.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(-1.0f, 0.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(-1.0f, 0.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 1.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 1.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 1.0f));

//// RIGHT FACE
//this->mesh.addVertex(glm::vec3(0.5f, -0.5f, 0.5f));
//this->mesh.addVertex(glm::vec3(0.5f, -0.5f, -0.5f));
//this->mesh.addVertex(glm::vec3(0.5f, 0.5f, -0.5f));
//this->mesh.addVertex(glm::vec3(0.5f, -0.5f, 0.5f));
//this->mesh.addVertex(glm::vec3(0.5f, 0.5f, -0.5f));
//this->mesh.addVertex(glm::vec3(0.5f, 0.5f, 0.5f));
//this->mesh.addNormal(glm::vec3(1.0f, 0.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(1.0f, 0.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(1.0f, 0.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(1.0f, 0.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(1.0f, 0.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(1.0f, 0.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 1.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 1.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 1.0f));

//// TOP FACE
//this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, 0.5f));
//this->mesh.addVertex(glm::vec3(0.5f, 0.5f, 0.5f));
//this->mesh.addVertex(glm::vec3(0.5f, 0.5f, -0.5f));
//this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, 0.5f));
//this->mesh.addVertex(glm::vec3(0.5f, 0.5f, -0.5f));
//this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, -0.5f));
//this->mesh.addNormal(glm::vec3(0.0f, 1.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(0.0f, 1.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(0.0f, 1.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(0.0f, 1.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(0.0f, 1.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(0.0f, 1.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 1.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 1.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 1.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 0.0f));

//// BOTTOM FACE
//this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, -0.5f));
//this->mesh.addVertex(glm::vec3(0.5f, -0.5f, -0.5f));
//this->mesh.addVertex(glm::vec3(0.5f, -0.5f, 0.5f));
//this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, -0.5f));
//this->mesh.addVertex(glm::vec3(0.5f, -0.5f, 0.5f));
//this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, 0.5f));
//this->mesh.addNormal(glm::vec3(0.0f, -1.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(0.0f, -1.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(0.0f, -1.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(0.0f, -1.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(0.0f, -1.0f, 0.0f));
//this->mesh.addNormal(glm::vec3(0.0f, -1.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 1.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 0.0f));
//this->mesh.addTexture(glm::vec2(1.0f, 1.0f));
//this->mesh.addTexture(glm::vec2(0.0f, 1.0f));

const unsigned int indicesBoundingbox[] = {
	0, 1, 1, 2, 2, 3, 3, 0,
	4, 5, 5, 6, 6, 7, 7, 4,
	0, 4, 1, 5, 2, 6, 3, 7
};
