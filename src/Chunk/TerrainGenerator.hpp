#pragma once

#include <FastNoise/FastNoise.h>
#include <array>
#include <random>
#include <algorithm>
#include <iostream>

#include <utils.hpp>

class TerrainGenerator
{
public:
	// Terrain generation constants
	static constexpr int SEA_LEVEL = 64;
	static constexpr int BEDROCK_LEVEL = 5;
	static constexpr int MAX_TREE_HEIGHT = 8;
	static constexpr int MIN_TREE_HEIGHT = 4;

	explicit TerrainGenerator(int seed = 1337);
	std::array<Voxel, CHUNK_VOLUME> generateChunk(int chunkX, int chunkZ);

	// Getter for seed to enable thread-safe generation
	int getSeed() const { return m_seed; }

private:
	// Noise generators
	FastNoise::SmartNode<FastNoise::DomainScale> m_heightNoise;
	FastNoise::SmartNode<FastNoise::DomainScale> m_biomeNoise;
	FastNoise::SmartNode<FastNoise::DomainScale> m_temperatureNoise;
	FastNoise::SmartNode<FastNoise::DomainScale> m_humidityNoise;
	FastNoise::SmartNode<FastNoise::DomainScale> m_caveNoise;
	FastNoise::SmartNode<FastNoise::DomainScale> m_oreNoise;

	// Generation parameters
	float m_heightScale;
	float m_mountainScale;
	int m_baseHeight;
	int m_seed;

	// Helper functions
	void setupNoiseGenerators();
	std::vector<int> generateHeightMap(int chunkX, int chunkZ);
	std::vector<BiomeType> generateBiomeMap(int chunkX, int chunkZ);
	BiomeType getBiomeFromValues(float temperature, float humidity, float elevation);
	void generateColumn(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int worldX, int worldZ, int terrainHeight, BiomeType biome);
	TextureType getTerrainBlockType(int y, int surfaceHeight, BiomeType biome, int worldX, int worldZ);
	TextureType generateOre(int worldX, int y, int worldZ);
	bool isCave(int worldX, int y, int worldZ);
	void generateVegetation(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int worldX, int worldZ, int groundHeight, BiomeType biome);
	void generateTree(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int startY, std::mt19937 &rng);

	inline int getVoxelIndex(int x, int y, int z) const
	{
		return y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x;
	}
};
