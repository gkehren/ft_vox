#include "BiomeManager.hpp"
#include <algorithm>
#include <cmath>

BiomeManager::BiomeManager()
{
	// Initialize desert parameters
	biomeParams[BIOME_DESERT] = {
		/* baseHeight */ 64.0f,
		/* heightVariation */ 15.0f,
		/* surfaceBlock */ SAND,
		/* subSurfaceBlock */ SAND,
		/* subSurfaceDepth */ 3,
		/* noiseScale */ 160.0f,
		/* octaves */ 3,
		/* persistence */ 0.4f,
		// Mountain params not used for desert
	};

	// Initialize forest parameters
	biomeParams[BIOME_FOREST] = {
		/* baseHeight */ 64.0f,
		/* heightVariation */ 40.0f,
		/* surfaceBlock */ DIRT,
		/* subSurfaceBlock */ DIRT,
		/* subSurfaceDepth */ 3,
		/* noiseScale */ 128.0f,
		/* octaves */ 3,
		/* persistence */ 0.5f,
		// Mountain params not used for forest
	};

	// Initialize plain parameters
	biomeParams[BIOME_PLAIN] = {
		/* baseHeight */ 64.0f,
		/* heightVariation */ 80.0f,
		/* surfaceBlock */ GRASS_TOP,
		/* subSurfaceBlock */ DIRT,
		/* subSurfaceDepth */ 3,
		/* noiseScale */ 128.0f,
		/* octaves */ 3,
		/* persistence */ 0.5f,
		// Mountain params not used for plains
	};

	// Initialize mountain parameters
	biomeParams[BIOME_MOUNTAIN] = {
		/* baseHeight */ 64.0f,
		/* heightVariation */ 80.0f,
		/* surfaceBlock */ STONE,
		/* subSurfaceBlock */ STONE,
		/* subSurfaceDepth */ 3,
		/* noiseScale */ 128.0f,
		/* octaves */ 4,
		/* persistence */ 0.5f,
		/* mountainNoiseScale */ 128.0f,
		/* mountainOctaves */ 4,
		/* mountainPersistence */ 0.5f,
		/* mountainDetailScale */ 40.0f, // Increased from 24.0f for smoother detail
		/* mountainDetailInfluence */ 0.2f,
		/* peakNoiseScale */ 250.0f, // Increased from 180.0f for fewer peaks
		/* peakThreshold */ 0.78f,	 // Increased to make peaks more rare
		/* peakMultiplier */ 200.0f	 // Higher multiplier for more dramatic peaks
	};
}

BiomeType BiomeManager::getBiomeTypeAt(int worldX, int worldY, siv::PerlinNoise *noise)
{
	float biomeNoise = noise->noise2D_01(
		static_cast<float>(worldX) / 256.0f,
		static_cast<float>(worldY) / 256.0f);

	if (biomeNoise < 0.35f)
		return BIOME_DESERT;
	else if (biomeNoise < 0.5f)
		return BIOME_FOREST;
	else if (biomeNoise < 0.65f)
		return BIOME_PLAIN;
	else
		return BIOME_MOUNTAIN;
}

const BiomeParameters &BiomeManager::getBiomeParameters(BiomeType type) const
{
	return biomeParams.at(type);
}

float BiomeManager::getTerrainHeightAt(int worldX, int worldZ, BiomeType biomeType, siv::PerlinNoise *noise)
{
	switch (biomeType)
	{
	case BIOME_DESERT:
		return generateDesertHeight(worldX, worldZ, noise);
	case BIOME_FOREST:
		return generateForestHeight(worldX, worldZ, noise);
	case BIOME_PLAIN:
		return generatePlainHeight(worldX, worldZ, noise);
	case BIOME_MOUNTAIN:
		return generateMountainHeight(worldX, worldZ, noise);
	default:
		return biomeParams.at(BIOME_PLAIN).baseHeight; // Default fallback
	}
}

float BiomeManager::generateDesertHeight(int worldX, int worldZ, siv::PerlinNoise *noise)
{
	const BiomeParameters &params = biomeParams.at(BIOME_DESERT);

	float noiseValue = noise->noise2D_01(
		static_cast<float>(worldX) / params.noiseScale,
		static_cast<float>(worldZ) / params.noiseScale);

	return params.baseHeight + (noiseValue - 0.5f) * params.heightVariation;
}

float BiomeManager::generateForestHeight(int worldX, int worldZ, siv::PerlinNoise *noise)
{
	const BiomeParameters &params = biomeParams.at(BIOME_FOREST);

	float noiseValue = noise->noise2D_01(
		static_cast<float>(worldX) / params.noiseScale,
		static_cast<float>(worldZ) / params.noiseScale);

	return params.baseHeight + (noiseValue - 0.5f) * params.heightVariation;
}

float BiomeManager::generatePlainHeight(int worldX, int worldZ, siv::PerlinNoise *noise)
{
	const BiomeParameters &params = biomeParams.at(BIOME_PLAIN);

	float noiseValue = noise->noise2D_01(
		static_cast<float>(worldX) / params.noiseScale,
		static_cast<float>(worldZ) / params.noiseScale);

	return params.baseHeight + (noiseValue - 0.5f) * params.heightVariation;
}

float BiomeManager::generateMountainHeight(int worldX, int worldZ, siv::PerlinNoise *noise)
{
	const BiomeParameters &params = biomeParams.at(BIOME_MOUNTAIN);

	// Base mountain terrain
	float mountainNoiseValue = noise->octave2D_01(
		static_cast<float>(worldX) / params.mountainNoiseScale,
		static_cast<float>(worldZ) / params.mountainNoiseScale,
		params.mountainOctaves, params.mountainPersistence);

	// Detailed variations
	float mountainDetail = noise->octave2D_01(
		static_cast<float>(worldX) / params.mountainDetailScale,
		static_cast<float>(worldZ) / params.mountainDetailScale,
		3, 0.4f);

	// Peaks (larger scale, fewer occurrences)
	float peakNoise = noise->octave2D_01(
		static_cast<float>(worldX) / params.peakNoiseScale,
		static_cast<float>(worldZ) / params.peakNoiseScale,
		2, 0.7f);

	// Combine base and detail
	float mountainBase = mountainNoiseValue * (1.0f - params.mountainDetailInfluence) +
						 mountainDetail * params.mountainDetailInfluence;

	// Apply peak influence
	float peakValue = (peakNoise > params.peakThreshold) ? std::pow((peakNoise - params.peakThreshold) / (1.0f - params.peakThreshold), 2.0f) : 0.0f;
	float peakInfluence = std::pow(mountainBase, 2.0f) * peakValue * 0.75f;

	// Combine everything
	float mountainCombined = mountainBase + peakInfluence;

	return params.baseHeight + (mountainCombined - 0.5f) * params.heightVariation +
		   peakInfluence * params.peakMultiplier;
}

float BiomeManager::blendBiomes(float biomeNoise, float value1, float value2, float threshold, float blendRange)
{
	if (biomeNoise < threshold - blendRange)
	{
		return value1;
	}
	else if (biomeNoise > threshold + blendRange)
	{
		return value2;
	}
	else
	{
		float t = smoothstep(threshold - blendRange, threshold + blendRange, biomeNoise);
		return (1.0f - t) * value1 + t * value2;
	}
}

float BiomeManager::smoothstep(float edge0, float edge1, float x)
{
	float t = std::max(0.0f, std::min(1.0f, (x - edge0) / (edge1 - edge0)));
	return t * t * (3.0f - 2.0f * t);
}