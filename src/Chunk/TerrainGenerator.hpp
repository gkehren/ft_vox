#pragma once

#include <FastNoise/FastNoise.h>
#include <array>
#include <random>
#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <glm/glm.hpp>

#include <utils.hpp>

struct ChunkData
{
	std::array<Voxel, CHUNK_VOLUME> voxels;
	std::array<BiomeType, CHUNK_SIZE * CHUNK_SIZE> biomeMap;

	// Bordures du chunk pour l'optimisation du rendu
	// Stocke les voxels adjacents dans les chunks voisins
	std::unordered_map<glm::ivec3, TextureType, IVec3Hash> borderVoxels;
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

	// Detail noise generators for structures and features
	FastNoise::SmartNode<FastNoise::DomainScale> m_treeNoise;
	FastNoise::SmartNode<FastNoise::DomainScale> m_rockNoise;
	FastNoise::SmartNode<FastNoise::DomainScale> m_vegetationNoise;
	FastNoise::SmartNode<FastNoise::DomainScale> m_oreNoise;

	// Generation parameters
	int m_baseHeight;
	int m_seed;

	// Helper functions
	void setupNoiseGenerators();

	// Optimized batch generation methods
	void generateChunkBatch(ChunkData &chunkData, int chunkX, int chunkZ);
	void determineBiomesBatch(std::array<BiomeType, CHUNK_SIZE * CHUNK_SIZE> &biomeMap,
							  const std::vector<float> &heightMap, int chunkX, int chunkZ);
	void generateStructuresBatch(ChunkData &chunkData, int chunkX, int chunkZ);

	// Border generation for mesh optimization
	void generateChunkBorders(ChunkData &chunkData, int chunkX, int chunkZ);

	// Biome-specific generation helpers
	BiomeType getBiomeFromFactors(float temperature, float humidity, float continentalness, int height);
	TextureType getBiomeSurfaceBlock(BiomeType biome, int height);
	float getBiomeTreeDensity(BiomeType biome);
	float getBiomeVegetationDensity(BiomeType biome);

	// Core terrain generation
	int generateHeightAt(int worldX, int worldZ);
	void generateColumn(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int worldX, int worldZ, int terrainHeight);

	// Helper method for border generation
	TextureType getVoxelTypeAt(int worldX, int worldY, int worldZ, int terrainHeight);

	// Structure generation functions
	void generateTreeAt(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int surfaceY, int treeHeight);
	void generateRocksAt(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int surfaceY, int worldX, int worldZ);
	void generateVegetationAt(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int surfaceY, BiomeType biome);
	void generateOres(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int worldX, int worldZ, int terrainHeight);

	// Utility functions
	float spline(float val);
	float smoothstep(float edge0, float edge1, float x);

	// Inline utility function for voxel indexing
	inline int getVoxelIndex(int x, int y, int z) const
	{
		return y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x;
	}
};
