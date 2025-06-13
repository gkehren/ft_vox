#pragma once

#include <FastNoise/FastNoiseLite.h>
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

private:
	// Noise generators
	FastNoiseLite m_heightNoise;
	FastNoiseLite m_biomeNoise;
	FastNoiseLite m_mountainNoise;
	FastNoiseLite m_caveNoise;
	FastNoiseLite m_oreNoise;
	FastNoiseLite m_temperatureNoise;
	FastNoiseLite m_humidityNoise;

	// Generation parameters
	float m_heightScale;
	float m_mountainScale;
	int m_baseHeight;
	int m_seed;

	// Helper functions
	void setupNoiseGenerators();
	void generateColumn(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int worldX, int worldZ);
	BiomeType getBiome(int worldX, int worldZ);
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
