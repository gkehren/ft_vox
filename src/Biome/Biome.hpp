#pragma once

#include <string>
#include <utils.hpp>

class NoiseGenerator;

struct BiomeProperties
{
	std::string name;

	// Surface blocks
	TextureType surfaceBlock;
	TextureType subsurfaceBlock;
	TextureType underwaterSurfaceBlock;
	int surfaceDepth = 1;
	int subsurfaceDepth = 3;

	// Height parameters
	float baseHeight = 64.0f;
	float heightVariation = 8.0f;

	// Climate parameters (range 0.0 to 1.0)
	float temperature = 0.5f; // 0.0 (cold) to 1.0 (hot)
	float humidity = 0.5f;	  // 0.0 (dry) to 1.0 (wet)
};

class Biome
{
private:
	BiomeProperties properties;

public:
	Biome(const BiomeProperties &props) : properties(props) {}

	// Getters
	const BiomeProperties &getProperties() const { return properties; }
	const std::string &getName() const { return properties.name; }

	// Height generation for this biome
	int generateHeight(int x, int z, NoiseGenerator &noise) const;

	// Get appropriate lock for a given position
	TextureType getBlockAt(int x, int y, int z, int surfaceHeight, NoiseGenerator &noise) const;

	// Check if block should be replaced by a cave
	bool shouldBeCave(int x, int y, int z, float caveNoise) const;
};