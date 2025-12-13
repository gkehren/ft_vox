#include <Chunk/TerrainGenerator.hpp>
#include <algorithm>
#include <cmath>

TerrainGenerator::TerrainGenerator(int seed)
	: m_seed(seed)
{
	setupNoiseGenerators();
}

void TerrainGenerator::setupNoiseGenerators()
{
	// Continental noise - Large scale continent shapes
	// Controls overall terrain elevation (oceans vs land vs mountains)
	auto continentalBase = FastNoise::New<FastNoise::OpenSimplex2>();
	auto continentalFractal = FastNoise::New<FastNoise::FractalFBm>();
	continentalFractal->SetSource(continentalBase);
	continentalFractal->SetOctaveCount(5);
	continentalFractal->SetLacunarity(2.0f);
	continentalFractal->SetGain(0.45f);

	auto continentalScale = FastNoise::New<FastNoise::DomainScale>();
	continentalScale->SetSource(continentalFractal);
	continentalScale->SetScale(0.0008f); // Very large scale for smooth continent-sized features
	m_continentalNoise = continentalScale;

	// Erosion noise - Controls terrain smoothness vs. roughness
	// Low values = rough/sharp, High values = smooth/eroded
	auto erosionBase = FastNoise::New<FastNoise::Perlin>();
	auto erosionFractal = FastNoise::New<FastNoise::FractalFBm>();
	erosionFractal->SetSource(erosionBase);
	erosionFractal->SetOctaveCount(4);
	erosionFractal->SetLacunarity(2.0f);
	erosionFractal->SetGain(0.5f);

	auto erosionScale = FastNoise::New<FastNoise::DomainScale>();
	erosionScale->SetSource(erosionFractal);
	erosionScale->SetScale(0.002f); // Large scale for smooth erosion zones
	m_erosionNoise = erosionScale;

	// Peaks and Valleys noise - Local height variation
	auto pvBase = FastNoise::New<FastNoise::OpenSimplex2>();
	auto pvFractal = FastNoise::New<FastNoise::FractalFBm>();
	pvFractal->SetSource(pvBase);
	pvFractal->SetOctaveCount(6);
	pvFractal->SetLacunarity(2.0f);
	pvFractal->SetGain(0.5f);

	auto pvScale = FastNoise::New<FastNoise::DomainScale>();
	pvScale->SetSource(pvFractal);
	pvScale->SetScale(0.006f); // Medium scale for terrain details
	m_peaksValleysNoise = pvScale;

	// Ridge noise - For sharp mountain peaks and dramatic formations
	// Low frequency for few, large peaks
	auto ridgeBase = FastNoise::New<FastNoise::OpenSimplex2>();
	auto ridgeFractal = FastNoise::New<FastNoise::FractalRidged>();
	ridgeFractal->SetSource(ridgeBase);
	ridgeFractal->SetOctaveCount(3); // Fewer octaves for broader peaks
	ridgeFractal->SetLacunarity(2.0f);
	ridgeFractal->SetGain(0.5f);

	auto ridgeScale = FastNoise::New<FastNoise::DomainScale>();
	ridgeScale->SetSource(ridgeFractal);
	ridgeScale->SetScale(0.002f); // Large scale for few, dramatic peaks
	m_ridgeNoise = ridgeScale;
}

ChunkData TerrainGenerator::generateChunk(int chunkX, int chunkZ)
{
	ChunkData chunkData;
	chunkData.voxels.fill({TextureType::AIR});

	// Generate the main chunk data using batch noise
	generateChunkBatch(chunkData, chunkX, chunkZ);

	// Generate border voxels for mesh optimization
	generateChunkBorders(chunkData, chunkX, chunkZ);

	return chunkData;
}

void TerrainGenerator::generateChunkBatch(ChunkData &chunkData, int chunkX, int chunkZ)
{
	constexpr int totalPoints = CHUNK_SIZE * CHUNK_SIZE;

	// Allocate noise output buffers
	std::vector<float> continentalResults(totalPoints);
	std::vector<float> erosionResults(totalPoints);
	std::vector<float> peaksValleysResults(totalPoints);
	std::vector<float> ridgeResults(totalPoints);
	std::vector<int> heightMap(totalPoints);

	// Generate noise using GenUniformGrid2D for efficient batch processing
	m_continentalNoise->GenUniformGrid2D(
		continentalResults.data(),
		chunkX, chunkZ,
		CHUNK_SIZE, CHUNK_SIZE,
		1.0f,
		m_seed);

	m_erosionNoise->GenUniformGrid2D(
		erosionResults.data(),
		chunkX, chunkZ,
		CHUNK_SIZE, CHUNK_SIZE,
		1.0f,
		m_seed + 1000);

	m_peaksValleysNoise->GenUniformGrid2D(
		peaksValleysResults.data(),
		chunkX, chunkZ,
		CHUNK_SIZE, CHUNK_SIZE,
		1.0f,
		m_seed + 2000);

	m_ridgeNoise->GenUniformGrid2D(
		ridgeResults.data(),
		chunkX, chunkZ,
		CHUNK_SIZE, CHUNK_SIZE,
		1.0f,
		m_seed + 3000);

	// Calculate height map from noise values
	for (int i = 0; i < totalPoints; ++i)
	{
		heightMap[i] = calculateHeight(
			continentalResults[i],
			erosionResults[i],
			peaksValleysResults[i],
			ridgeResults[i]);
	}

	// Generate voxel columns
	for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ)
	{
		for (int localX = 0; localX < CHUNK_SIZE; ++localX)
		{
			int index = localZ * CHUNK_SIZE + localX;
			int terrainHeight = heightMap[index];
			generateColumn(chunkData.voxels, localX, localZ, terrainHeight);
		}
	}
}

int TerrainGenerator::calculateHeight(float continental, float erosion, float peaksValleys, float ridge) const
{
	// Clamp noise values to [-1, 1] range
	continental = std::clamp(continental, -1.0f, 1.0f);
	erosion = std::clamp(erosion, -1.0f, 1.0f);
	peaksValleys = std::clamp(peaksValleys, -1.0f, 1.0f);
	ridge = std::clamp(ridge, -1.0f, 1.0f);

	// Normalize to [0, 1] range
	float continentalFactor = (continental + 1.0f) * 0.5f;
	float erosionFactor = (erosion + 1.0f) * 0.5f;

	// ============================================
	// TERRAIN HEIGHT CALCULATION
	// Target heights:
	//   Ocean: 35-64 (below sea level)
	//   Beach: 64-70
	//   Plains: 70-120 (gradual variation)
	//   Hills: 90-150 (gentle elevated terrain)
	//   Mountains: 120-240 (dramatic peaks)
	// ============================================

	float baseHeight = static_cast<float>(SEA_LEVEL); // 64

	// Define terrain type weights using smooth transitions
	// Ocean weight: continental < 0.3
	float oceanWeight = 1.0f - smoothstep(0.25f, 0.32f, continentalFactor);

	// Beach/coastal weight: continental 0.3-0.4
	float beachWeight = smoothstep(0.25f, 0.32f, continentalFactor) * (1.0f - smoothstep(0.35f, 0.42f, continentalFactor));

	// Plains weight: continental 0.4-0.6 (wide zone)
	float plainsWeight = smoothstep(0.35f, 0.42f, continentalFactor) * (1.0f - smoothstep(0.55f, 0.65f, continentalFactor));

	// Hills weight: continental 0.6-0.8
	float hillsWeight = smoothstep(0.55f, 0.65f, continentalFactor) * (1.0f - smoothstep(0.75f, 0.85f, continentalFactor));

	// Mountain weight: continental > 0.8
	float mountainWeight = smoothstep(0.75f, 0.85f, continentalFactor);

	// Normalize weights
	float totalWeight = oceanWeight + beachWeight + plainsWeight + hillsWeight + mountainWeight;
	if (totalWeight > 0.001f)
	{
		oceanWeight /= totalWeight;
		beachWeight /= totalWeight;
		plainsWeight /= totalWeight;
		hillsWeight /= totalWeight;
		mountainWeight /= totalWeight;
	}

	// ============================================
	// Calculate height contribution from each terrain type
	// Heights are ABOVE sea level (64)
	// ============================================

	// Ocean: -29 to -6 above sea level (height 35-58)
	float oceanHeight = -20.0f + peaksValleys * 8.0f;

	// Beach/Coastal: 0 to 6 above sea level (height 64-70)
	float beachHeight = 3.0f + peaksValleys * 3.0f * erosionFactor;

	// Plains: 6 to 56 above sea level (height 70-120)
	// GRADUAL variation with smooth rolling terrain
	float plainsBase = 25.0f; // Center around height 89
	float plainsVariation = peaksValleys * 25.0f * (0.5f + erosionFactor * 0.5f); // ±25 gradual
	float plainsHeight = plainsBase + plainsVariation;

	// Hills: 26 to 86 above sea level (height 90-150)
	// Gentle elevated terrain, smooth rounded hills
	float hillsBase = 55.0f; // Center around height 119
	float hillsVariation = peaksValleys * 30.0f * (0.6f + erosionFactor * 0.4f);
	float hillsHeight = hillsBase + hillsVariation;

	// Mountains: 56 to 176 above sea level (height 120-240)
	// Dramatic peaks with steep slopes forming mountain ranges
	float mountainBase = 80.0f; // Start at height ~144
	
	// Ridge creates dramatic peaks - only strong ridge values form peaks
	float peakContribution = 0.0f;
	if (ridge > 0.2f)
	{
		// Quadratic growth for dramatic peaks
		float peakFactor = (ridge - 0.2f) / 0.8f; // 0 to 1 for ridge 0.2 to 1.0
		peakFactor = peakFactor * peakFactor; // Squared for steep peaks
		peakContribution = peakFactor * 95.0f; // Up to 95 extra height (total ~240)
	}
	
	// General mountain terrain with valleys between peaks
	float mountainVariation = peaksValleys * 35.0f;
	
	// Combine mountain elements
	float mountainHeight = mountainBase + mountainVariation + peakContribution;

	// ============================================
	// Blend all terrain types together
	// ============================================

	float heightVariation = oceanWeight * oceanHeight +
							beachWeight * beachHeight +
							plainsWeight * plainsHeight +
							hillsWeight * hillsHeight +
							mountainWeight * mountainHeight;

	// Apply final height calculation
	float finalHeight = baseHeight + heightVariation;

	// Clamp to valid range (1 to 250)
	return std::clamp(static_cast<int>(finalHeight), 1, CHUNK_HEIGHT - 6);
}

void TerrainGenerator::generateColumn(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int terrainHeight)
{
	for (int y = 0; y < CHUNK_HEIGHT; ++y)
	{
		int index = getVoxelIndex(localX, y, localZ);
		TextureType type = getVoxelTypeAt(y, terrainHeight);
		voxels[index].type = type;
	}
}

TextureType TerrainGenerator::getVoxelTypeAt(int worldY, int terrainHeight) const
{
	// Bedrock layer at the bottom
	if (worldY <= BEDROCK_LEVEL)
	{
		return TextureType::BEDROCK;
	}

	// Below terrain height
	if (worldY <= terrainHeight)
	{
		int depthFromSurface = terrainHeight - worldY;

		if (depthFromSurface == 0)
		{
			// Surface block
			if (terrainHeight <= SEA_LEVEL + 2)
			{
				return TextureType::SAND; // Beach/shoreline
			}
			return TextureType::GRASS_TOP;
		}
		else if (depthFromSurface <= 3)
		{
			// Subsurface (3 blocks of dirt)
			if (terrainHeight <= SEA_LEVEL + 2)
			{
				return TextureType::SAND;
			}
			return TextureType::DIRT;
		}
		else
		{
			// Deep underground
			return TextureType::STONE;
		}
	}

	// Water between terrain and sea level
	if (worldY <= SEA_LEVEL)
	{
		return TextureType::WATER;
	}

	// Air above terrain
	return TextureType::AIR;
}

void TerrainGenerator::generateChunkBorders(ChunkData &chunkData, int chunkX, int chunkZ)
{
	constexpr int borderSize = 1;

	// Helper lambda to get height at world position
	auto getHeightAt = [this](int worldX, int worldZ) -> int
	{
		std::vector<float> continental(1), erosion(1), pv(1), ridge(1);

		m_continentalNoise->GenUniformGrid2D(continental.data(), worldX, worldZ, 1, 1, 1.0f, m_seed);
		m_erosionNoise->GenUniformGrid2D(erosion.data(), worldX, worldZ, 1, 1, 1.0f, m_seed + 1000);
		m_peaksValleysNoise->GenUniformGrid2D(pv.data(), worldX, worldZ, 1, 1, 1.0f, m_seed + 2000);
		m_ridgeNoise->GenUniformGrid2D(ridge.data(), worldX, worldZ, 1, 1, 1.0f, m_seed + 3000);

		return calculateHeight(continental[0], erosion[0], pv[0], ridge[0]);
	};

	// North border (Z = -1)
	for (int x = 0; x < CHUNK_SIZE; ++x)
	{
		int worldX = chunkX + x;
		int worldZ = chunkZ - borderSize;
		int height = getHeightAt(worldX, worldZ);

		for (int y = 0; y < CHUNK_HEIGHT; ++y)
		{
			TextureType voxelType = getVoxelTypeAt(y, height);
			if (voxelType != TextureType::AIR)
			{
				chunkData.borderVoxels[glm::ivec3(x, y, -borderSize)] = voxelType;
			}
		}
	}

	// South border (Z = CHUNK_SIZE)
	for (int x = 0; x < CHUNK_SIZE; ++x)
	{
		int worldX = chunkX + x;
		int worldZ = chunkZ + CHUNK_SIZE;
		int height = getHeightAt(worldX, worldZ);

		for (int y = 0; y < CHUNK_HEIGHT; ++y)
		{
			TextureType voxelType = getVoxelTypeAt(y, height);
			if (voxelType != TextureType::AIR)
			{
				chunkData.borderVoxels[glm::ivec3(x, y, CHUNK_SIZE)] = voxelType;
			}
		}
	}

	// West border (X = -1)
	for (int z = 0; z < CHUNK_SIZE; ++z)
	{
		int worldX = chunkX - borderSize;
		int worldZ = chunkZ + z;
		int height = getHeightAt(worldX, worldZ);

		for (int y = 0; y < CHUNK_HEIGHT; ++y)
		{
			TextureType voxelType = getVoxelTypeAt(y, height);
			if (voxelType != TextureType::AIR)
			{
				chunkData.borderVoxels[glm::ivec3(-borderSize, y, z)] = voxelType;
			}
		}
	}

	// East border (X = CHUNK_SIZE)
	for (int z = 0; z < CHUNK_SIZE; ++z)
	{
		int worldX = chunkX + CHUNK_SIZE;
		int worldZ = chunkZ + z;
		int height = getHeightAt(worldX, worldZ);

		for (int y = 0; y < CHUNK_HEIGHT; ++y)
		{
			TextureType voxelType = getVoxelTypeAt(y, height);
			if (voxelType != TextureType::AIR)
			{
				chunkData.borderVoxels[glm::ivec3(CHUNK_SIZE, y, z)] = voxelType;
			}
		}
	}
}

float TerrainGenerator::smoothstep(float edge0, float edge1, float x) const
{
	float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}
