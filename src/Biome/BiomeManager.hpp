#pragma once
#include "../utils.hpp"
#include "../PerlinNoise/PerlinNoise.hpp"
#include <unordered_map>

class BiomeManager
{
public:
	BiomeManager();

	// Get biome type at world coordinates
	BiomeType getBiomeTypeAt(int worldX, int worldY, siv::PerlinNoise *noise);

	// Get Biome parameters
	const BiomeParameters &getBiomeParameters(BiomeType biomeType) const;

	// Get terrain height at world coordinates
	float getTerrainHeightAt(int worldX, int worldY, BiomeType biomeType, siv::PerlinNoise *noise);

	// Blending utilities
	float blendBiomes(float biomeNoise, float value1, float value2, float threshold, float blendRange);

private:
	std::unordered_map<BiomeType, BiomeParameters>
		biomeParams;

	// Height generation for each biome type
	float generateDesertHeight(int worldX, int worldY, siv::PerlinNoise *noise);
	float generateForestHeight(int worldX, int worldY, siv::PerlinNoise *noise);
	float generatePlainHeight(int worldX, int worldY, siv::PerlinNoise *noise);
	float generateMountainHeight(int worldX, int worldY, siv::PerlinNoise *noise);

	// Helper function for smoothstep
	float smoothstep(float edge0, float edge1, float x);
};