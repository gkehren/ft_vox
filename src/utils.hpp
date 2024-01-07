#pragma once

#include <iostream>

#define WINDOW_WIDTH	1920
#define WINDOW_HEIGHT	1080

#ifdef __linux__	// linux
#define RES_PATH		"/home/gkehren/Documents/ft_vox/ressources/"
#elif __APPLE__		// Mac
#define RES_PATH		"/Users/gkehren/Documents/ft_vox/ressources/"
#endif

enum TextureType {
	TEXTURE_AIR,
	TEXTURE_GRASS,
	TEXTURE_DIRT,
	TEXTURE_STONE,
	TEXTURE_COBBLESTONE,
	TEXTURE_COUNT // Keep last
};

enum Face {
	LEFT,
	RIGHT,
	TOP,
	BOTTOM,
	FRONT,
	BACK
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
