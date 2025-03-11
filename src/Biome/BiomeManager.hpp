#pragma once

#include <vector>
#include <unordered_map>
#include <string>
#include "Biome.hpp"

struct BiomeInfluence
{
	const Biome *biome;
	float weight;
};

class BiomeManager
{
private:
	std::vector<std::unique_ptr<Biome>> biomes;
	std::unordered_map<std::string, int> biomeIndices;

public:
	BiomeManager();
	~BiomeManager() = default;

	// Register a new biome
	void registerBiome(std::unique_ptr<Biome> biome);

	// Get biome at specific coordinates
	const Biome *getBiomeAt(int x, int z, NoiseGenerator &noise) const;

	// Get biome influences at specific coordinates
	std::vector<BiomeInfluence> getBiomeInfluences(int x, int z, NoiseGenerator &noise) const;
};