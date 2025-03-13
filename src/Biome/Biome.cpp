#include "Biome.hpp"
#include "NoiseGenerator.hpp"

static float smootherStep(float edge0, float edge1, float x)
{
	x = (x - edge0) / (edge1 - edge0);
	x = std::clamp(x, 0.0f, 1.0f);
	return x * x * (3 - 2 * x);
}

int Biome::generateHeight(int x, int z, NoiseGenerator &noise) const
{
	// Base terrain is the sea level
	int baseHeight = properties.baseHeight;

	// Get base terrain noise
	float baseNoise = noise.terrainBaseNoise(x, z);
	baseNoise = smootherStep(0.0f, 1.0f, baseNoise);

	// Special handling for different biome types
	if (properties.name == "Mountains")
	{
		// Mountains need dramatic terrain
		float mountainNoise = noise.mountainNoise(x, z);
		mountainNoise = smootherStep(0.0f, 1.0f, mountainNoise);

		// Create steeper mountains with clear peaks
		if (mountainNoise > 0.5f)
		{
			// Exponential curve for dramatic peaks
			float factor = (mountainNoise - 0.5f) / 0.5f;
			// Use a higher power curve for more dramatic mountains
			factor = std::pow(factor, 1.5f);

			// Calculate mountain height with more dramatic scaling
			int mountainHeight = static_cast<int>(factor * properties.heightVariation);

			// Add some ridges and variation to mountain tops
			float ridgeNoise = noise.noise2D(x, z, 30.0f) * 10.0f;

			return baseHeight + mountainHeight + static_cast<int>(ridgeNoise);
		}
		else
		{
			// Foothills and lower mountain areas
			return baseHeight + static_cast<int>(mountainNoise * properties.heightVariation * 0.7f);
		}
	}
	else if (properties.name == "SnowyCaps")
	{
		// Similar to mountains but with even more dramatic peaks
		float snowPeakNoise = noise.mountainNoise(x, z);
		snowPeakNoise = smootherStep(0.0f, 1.0f, snowPeakNoise);

		if (snowPeakNoise > 0.6f)
		{
			// Exponential curve for dramatic snowy peaks
			float factor = (snowPeakNoise - 0.6f) / 0.4f;
			factor = std::pow(factor, 1.2f); // Higher power for more dramatic peaks
			int peakHeight = static_cast<int>(factor * properties.heightVariation * 1.3f);
			return baseHeight + peakHeight;
		}
		else
		{
			// Lower slopes of snowy mountains
			return baseHeight + static_cast<int>(snowPeakNoise * properties.heightVariation * 0.8f);
		}
	}
	else if (properties.name == "Hills")
	{
		// Hills have moderate variation with some valleys
		float hillFactor = noise.noise2D(x, z, 70.0f);
		hillFactor = smootherStep(0.0f, 1.0f, hillFactor);

		// Create occasional valleys with sharper transitions
		float valleyFactor = noise.valleyNoise(x, z);
		if (valleyFactor < 0.25f)
		{
			// Create a valley
			float depth = (0.25f - valleyFactor) / 0.25f;
			return baseHeight - static_cast<int>(depth * 10.0f);
		}

		return baseHeight + static_cast<int>(hillFactor * properties.heightVariation);
	}
	else if (properties.name == "Plains" || properties.name == "Forest")
	{
		float plainNoise = noise.noise2D(x, z, 120.0f);
		plainNoise = smootherStep(0.0f, 1.0f, plainNoise);
		float detailNoise = noise.noise2D(x, z, 20.0f) * 2.5f;
		detailNoise = smootherStep(0.0f, 1.0f, detailNoise);

		return baseHeight + static_cast<int>(plainNoise * 4.0f + detailNoise);
	}
	else if (properties.name == "Desert")
	{
		// Gentle dunes with occasional flat areas
		float duneNoise = noise.noise2D(x, z, 50.0f);
		duneNoise = smootherStep(0.0f, 1.0f, duneNoise);

		// Add occasional mesas (desert plateaus)
		float mesaNoise = noise.noise2D(x, z, 180.0f);
		mesaNoise = smootherStep(0.0f, 1.0f, mesaNoise);
		if (mesaNoise > 0.75f)
		{
			float mesaHeight = (mesaNoise - 0.75f) / 0.25f;
			return baseHeight + static_cast<int>(mesaHeight * 35.0f);
		}

		return baseHeight + static_cast<int>(duneNoise * properties.heightVariation);
	}
	else if (properties.name == "Valley")
	{
		// Create dramatic valleys with ridges
		float valleyNoise = noise.valleyNoise(x, z);
		valleyNoise = smootherStep(0.0f, 1.0f, valleyNoise);

		// Sharper ridges along valley edges
		if (valleyNoise > 0.65f)
		{
			float ridgeFactor = (valleyNoise - 0.65f) / 0.35f;
			return baseHeight + static_cast<int>(ridgeFactor * properties.heightVariation * 1.4f);
		}
		// Deep valley floors
		else if (valleyNoise < 0.25f)
		{
			float depth = (0.25f - valleyNoise) / 0.25f;
			return baseHeight - static_cast<int>(depth * 30.0f);
		}
		// Valley slopes
		else
		{
			float normalizedNoise = (valleyNoise - 0.25f) / 0.4f;
			return baseHeight - 25 + static_cast<int>(normalizedNoise * 35.0f);
		}
	}
	else if (properties.name == "Swamp")
	{
		// Very flat, slightly below sea level, with occasional deep pools
		float swampNoise = noise.noise2D(x, z, 60.0f);
		swampNoise = smootherStep(0.0f, 1.0f, swampNoise);

		// Create occasional deeper pools in swamps
		if (swampNoise < 0.25f)
		{
			float poolDepth = (0.25f - swampNoise) / 0.25f;
			return baseHeight - static_cast<int>(poolDepth * 8.0f);
		}

		return baseHeight - 2 + static_cast<int>(swampNoise * 3.0f);
	}

	// Default terrain generation with more variation
	return baseHeight + static_cast<int>(baseNoise * properties.heightVariation * 0.5f);
}

TextureType Biome::getBlockAt(int x, int y, int z, int surfaceHeight, NoiseGenerator &noise) const
{
	// Surface layer
	if (y == surfaceHeight)
	{
		return properties.surfaceBlock;
	}

	// Subsurface layers (usually 3-4 blocks of dirt)
	if (y >= surfaceHeight - 3 && y < surfaceHeight)
	{
		return properties.subsurfaceBlock;
	}

	// Bedrock at bottom
	if (y == 0)
	{
		return TEXTURE_STONE; // Assuming this is your bedrock texture
	}

	// Underground ore generation with depth-based distribution
	float oreNoise = noise.noise3D(x, y, z, 10.0f);

	// Add different ore types based on depth
	if (y < 16 && oreNoise > 0.85f)
	{
		// Deep rare ores (like diamonds)
		return TEXTURE_BRICK;
	}
	else if (y < 32 && oreNoise > 0.82f)
	{
		// Mid-level ores (like gold, redstone)
		return TEXTURE_SMOOTH_STONE;
	}
	else if (y < 64 && oreNoise > 0.78f)
	{
		// Common higher ores (like iron, coal)
		return TEXTURE_SLAB;
	}

	// Regular stone
	return TEXTURE_STONE;
}

bool Biome::shouldBeCave(int x, int y, int z, float caveNoise) const
{
	// Basic cave generation
	if (y == 0)
		return false; // Bedrock level is solid

	return caveNoise <= 0.40;
}
