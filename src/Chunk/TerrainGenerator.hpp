#pragma once

#include <FastNoise/FastNoise.h>
#include <array>
#include <random>
#include <algorithm>
#include <iostream>

#include <utils.hpp>

struct ChunkData
{
	std::array<Voxel, CHUNK_VOLUME> voxels;
	std::array<BiomeType, CHUNK_SIZE * CHUNK_SIZE> biomeMap;
};

class TerrainGenerator
{
public:
	// Terrain generation constants
	static constexpr int SEA_LEVEL = 64;
	static constexpr int BEDROCK_LEVEL = 5;

	explicit TerrainGenerator(int seed = 1337);
	ChunkData generateChunk(int chunkX, int chunkZ);

	// Getter for seed to enable thread-safe generation
	int getSeed() const { return m_seed; }

private:
	// Noise generators - style Minecraft
	FastNoise::SmartNode<FastNoise::DomainScale> m_continentalNoise;
	FastNoise::SmartNode<FastNoise::DomainScale> m_erosionNoise;
	FastNoise::SmartNode<FastNoise::DomainScale> m_peaksValleysNoise;
	FastNoise::SmartNode<FastNoise::DomainScale> m_temperatureNoise;
	FastNoise::SmartNode<FastNoise::DomainScale> m_humidityNoise;
	FastNoise::SmartNode<FastNoise::DomainScale> m_ridgeNoise;

	// Generation parameters
	int m_baseHeight;
	int m_seed;

	// Helper functions
	void setupNoiseGenerators();
	int generateHeightAt(int worldX, int worldZ);
	void generateColumn(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int worldX, int worldZ, int terrainHeight);

	// Utility functions
	float spline(float val);
	float smoothstep(float edge0, float edge1, float x);

	inline int getVoxelIndex(int x, int y, int z) const
	{
		return y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x;
	}
};
