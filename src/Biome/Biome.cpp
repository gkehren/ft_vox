#include "Biome.hpp"
#include "NoiseGenerator.hpp"

int Biome::generateHeight(int x, int z, NoiseGenerator &noise) const
{
	// Base terrain height
	float baseNoise = noise.terrainBaseNoise(x, z);
	int baseHeight = static_cast<int>(baseNoise * properties.baseHeight);

	// Add mountain influence of this is a moutainous biome
	float mountainFactor = 0.0f;
	float mountainNoise = noise.mountainNoise(x, z);

	if (mountainNoise > 0.6f)
	{
		mountainFactor = (mountainNoise - 0.6f) / 0.4f; // Normalize to range [0, 1]
		mountainFactor *= mountainFactor;				// Squaring to smooth the transition

		float mountainHeight = mountainNoise * properties.heightVariation * 2.0f;
		return static_cast<int>(baseHeight * (1.0f - mountainFactor) + mountainHeight * mountainFactor);
	}

	// Add random variations based on biome properties
	float variation = noise.noise2D(x, z, 30.0f) * properties.heightVariation;

	return static_cast<int>(baseHeight + variation);
}

TextureType Biome::getBlockAt(int x, int y, int z, int surfaceHeight, NoiseGenerator &noise) const
{
	// Surface layer
	if (y == surfaceHeight)
		return properties.surfaceBlock;

	// Subsurface layers (e.g., dirt under grass)
	if (y < surfaceHeight - properties.subsurfaceDepth)
		return properties.subsurfaceBlock;

	// Deep underground - stone and ores
	float oreNoise = noise.noise3D(x, y, z, 10.0f);

	// Example ore distribution
	if (oreNoise > 0.8f)
		return TEXTURE_BRICK;

	return TEXTURE_STONE; // Default underground block
}

bool Biome::shouldBeCave(int x, int y, int z, float caveNoise) const
{
	// Basic cave generation
	if (y == 0)
		return false; // Bedrock level is solid

	return caveNoise <= 0.25;
}
