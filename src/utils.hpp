#pragma once

#include <iostream>

#define WINDOW_WIDTH	1920
#define WINDOW_HEIGHT	1080

#define WORLD_SIZE		5
#define WORLD_HEIGHT	1

#ifdef __linux__	// linux
#define BASE_PATH		"/home/gkehren/Documents/ft_vox/ressources/"
#elif __APPLE__		// Mac
#define BASE_PATH		"/Users/gkehren/Documents/ft_vox/ressources/"
#endif

enum TextureType {
	TEXTURE_GRASS,
	TEXTURE_DIRT,
	TEXTURE_STONE,
	TEXTURE_COBBLESTONE,
	TEXTURE_COUNT // Keep last
};
