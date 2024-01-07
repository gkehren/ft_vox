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

//const float vertices[] = {
//	// x, y, z, u, v
//	// Front
//	-0.5f, -0.5f,  0.5f, 0.0f, 0.0f,
//	 0.5f, -0.5f,  0.5f, 1.0f, 0.0f,
//	 0.5f,  0.5f,  0.5f, 1.0f, 1.0f,
//	-0.5f,  0.5f,  0.5f, 0.0f, 1.0f,

//	// Back
//	0.5f, -0.5f, -0.5f, 0.0f, 0.0f,
//   -0.5f, -0.5f, -0.5f, 1.0f, 0.0f,
//   -0.5f,  0.5f, -0.5f, 1.0f, 1.0f,
//	0.5f,  0.5f, -0.5f, 0.0f, 1.0f,

//	// Left
//   -0.5f, -0.5f, -0.5f, 0.0f, 0.0f,
//   -0.5f, -0.5f,  0.5f, 1.0f, 0.0f,
//   -0.5f,  0.5f,  0.5f, 1.0f, 1.0f,
//   -0.5f,  0.5f, -0.5f, 0.0f, 1.0f,

//	// Right
//	0.5f, -0.5f,  0.5f, 0.0f, 0.0f,
//	0.5f, -0.5f, -0.5f, 1.0f, 0.0f,
//	0.5f,  0.5f, -0.5f, 1.0f, 1.0f,
//	0.5f,  0.5f,  0.5f, 0.0f, 1.0f,

//	// Top
//   -0.5f,  0.5f,  0.5f, 0.0f, 0.0f,
//	0.5f,  0.5f,  0.5f, 1.0f, 0.0f,
//	0.5f,  0.5f, -0.5f, 1.0f, 1.0f,
//   -0.5f,  0.5f, -0.5f, 0.0f, 1.0f,

//	// Bottom
//   -0.5f, -0.5f, -0.5f, 0.0f, 0.0f,
//	0.5f, -0.5f, -0.5f, 1.0f, 0.0f,
//	0.5f, -0.5f,  0.5f, 1.0f, 1.0f,
//   -0.5f, -0.5f,  0.5f, 0.0f, 1.0f,
//};

//const unsigned int indices[] = {
//	// Front
//	0, 1, 2, 2, 3, 0,

//	// Back
//	4, 5, 6, 6, 7, 4,

//	// Left
//	8, 9, 10, 10, 11, 8,

//	// Right
//	12, 13, 14, 14, 15, 12,

//	// Top
//	16, 17, 18, 18, 19, 16,

//	// Bottom
//	20, 21, 22, 22, 23, 20
//};

const unsigned int indicesBoundingbox[] = {
	0, 1, 1, 2, 2, 3, 3, 0,
	4, 5, 5, 6, 6, 7, 7, 4,
	0, 4, 1, 5, 2, 6, 3, 7
};
