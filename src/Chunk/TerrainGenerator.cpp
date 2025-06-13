#include <Chunk/TerrainGenerator.hpp>
#include <mutex>

TerrainGenerator::TerrainGenerator(int seed)
	: m_heightScale(30.0f), m_mountainScale(50.0f), m_baseHeight(SEA_LEVEL), m_seed(seed)
{
	setupNoiseGenerators();
}

std::array<Voxel, CHUNK_VOLUME> TerrainGenerator::generateChunk(int chunkX, int chunkZ)
{
	std::array<Voxel, CHUNK_VOLUME> voxels;
	voxels.fill({TextureType::AIR});

	// Generate terrain for each column in the chunk
	for (int localX = 0; localX < CHUNK_SIZE; ++localX)
	{
		for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ)
		{
			int worldX = chunkX + localX;
			int worldZ = chunkZ + localZ;

			generateColumn(voxels, localX, localZ, worldX, worldZ);
		}
	}

	return voxels;
}

void TerrainGenerator::setupNoiseGenerators()
{
	std::cout << "Setting up noise generators with seed: " << m_seed << std::endl;
	// Height map noise
	m_heightNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	m_heightNoise.SetSeed(m_seed);
	m_heightNoise.SetFrequency(0.02f); // Increased frequency significantly
	m_heightNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
	m_heightNoise.SetFractalOctaves(4);
	m_heightNoise.SetFractalLacunarity(2.0f);
	m_heightNoise.SetFractalGain(0.5f);

	// Mountain noise
	m_mountainNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	m_mountainNoise.SetSeed(m_seed + 500);
	m_mountainNoise.SetFrequency(0.01f); // Increased frequency

	// Biome noise
	m_biomeNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	m_biomeNoise.SetSeed(m_seed + 1000);
	m_biomeNoise.SetFrequency(0.005f); // Increased frequency

	// Temperature noise
	m_temperatureNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	m_temperatureNoise.SetSeed(m_seed + 2000);
	m_temperatureNoise.SetFrequency(0.008f); // Increased frequency

	// Humidity noise
	m_humidityNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	m_humidityNoise.SetSeed(m_seed + 3000);
	m_humidityNoise.SetFrequency(0.007f); // Increased frequency

	// Cave noise
	m_caveNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	m_caveNoise.SetSeed(m_seed + 4000);
	m_caveNoise.SetFrequency(0.04f);
	m_caveNoise.SetFractalType(FastNoiseLite::FractalType_Ridged);
	m_caveNoise.SetFractalOctaves(2);

	// Ore noise
	m_oreNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	m_oreNoise.SetSeed(m_seed + 5000);
	m_oreNoise.SetFrequency(0.08f);
	// Test noise generation
	float testNoise = m_heightNoise.GetNoise(100.0f, 100.0f);
	std::cout << "Test noise value at (100,100): " << testNoise << std::endl;
}

void TerrainGenerator::generateColumn(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int worldX, int worldZ)
{
	// Add offset to avoid potential issues around coordinate 0
	float noiseX = static_cast<float>(worldX + 10000);
	float noiseZ = static_cast<float>(worldZ + 10000);

	// Calculate height using multiple noise layers
	float heightValue = m_heightNoise.GetNoise(noiseX, noiseZ);

	// Normalize heightValue to a reasonable range (FastNoiseLite returns values between -1 and 1)
	heightValue = std::max(-1.0f, std::min(1.0f, heightValue));

	// Add mountain peaks with proper normalization
	float mountainValue = m_mountainNoise.GetNoise(noiseX, noiseZ);
	mountainValue = std::max(-1.0f, std::min(1.0f, mountainValue));

	// Base terrain height (oscillates around sea level)
	int terrainHeight = static_cast<int>(m_baseHeight + heightValue * 20.0f); // Base variation of ±20 blocks

	// Add mountain modifier only if mountain value is high (much higher threshold)
	if (mountainValue > 0.7f)
	{
		float mountainBonus = (mountainValue - 0.7f) / 0.3f;	  // Normalize to 0-1
		terrainHeight += static_cast<int>(mountainBonus * 30.0f); // Add up to 30 blocks for mountains
	}
	// Clamp terrain height to reasonable bounds
	terrainHeight = std::max(10, std::min(terrainHeight, 100)); // Lower max height
	// Determine biome
	BiomeType biome = getBiome(worldX, worldZ);
	// Debug output for the first few chunks (remove this later)
	static int debugCounter = 0;
	if (debugCounter < 5)
	{
		// Test various coordinates to understand the pattern
		float testNoise1 = m_heightNoise.GetNoise(noiseX, noiseZ);
		float testNoise2 = m_heightNoise.GetNoise(100.0f, 100.0f);
		float testNoise3 = m_heightNoise.GetNoise(0.0f, 0.0f);

		std::cout << "=== DEBUG #" << debugCounter << " ===" << std::endl;
		std::cout << "World coords: (" << worldX << "," << worldZ << ")" << std::endl;
		std::cout << "Noise coords: (" << noiseX << "," << noiseZ << ")" << std::endl;
		std::cout << "Noise at offset coords: " << testNoise1 << std::endl;
		std::cout << "Noise at (100,100): " << testNoise2 << std::endl;
		std::cout << "Noise at (0,0): " << testNoise3 << std::endl;
		std::cout << "Final values: heightValue=" << heightValue << ", mountainValue=" << mountainValue << std::endl;
		std::cout << "Result: terrainHeight=" << terrainHeight << ", biome=" << biome << std::endl;
		std::cout << "========================" << std::endl;
		debugCounter++;
	}

	// Generate the column
	for (int y = 0; y < CHUNK_HEIGHT; ++y)
	{
		int index = getVoxelIndex(localX, y, localZ);

		if (y <= BEDROCK_LEVEL)
		{
			voxels[index].type = TextureType::BEDROCK;
		}
		else if (y <= terrainHeight)
		{
			// Check for caves
			if (y > SEA_LEVEL && isCave(worldX, y, worldZ))
			{
				voxels[index].type = TextureType::AIR;
			}
			else
			{
				// Generate terrain based on depth and biome
				TextureType blockType = getTerrainBlockType(y, terrainHeight, biome, worldX, worldZ);
				voxels[index].type = blockType;
			}
		}
		// else if (y <= SEA_LEVEL)
		//{
		//	voxels[index].type = TextureType::WATER;
		// }
		//  else remains AIR (default)
	}

	// Generate vegetation
	if (terrainHeight > SEA_LEVEL)
	{
		generateVegetation(voxels, localX, localZ, worldX, worldZ, terrainHeight, biome);
	}
}

BiomeType TerrainGenerator::getBiome(int worldX, int worldZ)
{
	// Use same offset as in generateColumn
	float noiseX = static_cast<float>(worldX + 10000);
	float noiseZ = static_cast<float>(worldZ + 10000);

	float temperature = m_temperatureNoise.GetNoise(noiseX, noiseZ);
	float humidity = m_humidityNoise.GetNoise(noiseX, noiseZ);
	float elevation = m_biomeNoise.GetNoise(noiseX, noiseZ);

	temperature = std::max(-1.0f, std::min(1.0f, temperature));
	humidity = std::max(-1.0f, std::min(1.0f, humidity));
	elevation = std::max(-1.0f, std::min(1.0f, elevation));

	// Debug: Add some variation to ensure biomes are being calculated differently
	// Determine biome based on temperature, humidity, and elevation
	if (elevation > 0.5f) // Higher threshold for mountains
	{
		return temperature < -0.3f ? BiomeType::SNOWY : BiomeType::MOUNTAINS;
	}
	else if (temperature < -0.3f) // Adjusted threshold
	{
		return BiomeType::SNOWY;
	}
	else if (temperature > 0.3f && humidity < -0.2f) // Adjusted thresholds
	{
		return BiomeType::DESERT;
	}
	else if (humidity > 0.2f && temperature > -0.1f) // Adjusted thresholds
	{
		return BiomeType::FOREST;
	}
	else
	{
		return BiomeType::PLAINS;
	}
}

TextureType TerrainGenerator::getTerrainBlockType(int y, int surfaceHeight, BiomeType biome, int worldX, int worldZ)
{
	int depthFromSurface = surfaceHeight - y;

	// Check for ores first
	if (depthFromSurface > 3)
	{
		TextureType ore = generateOre(worldX, y, worldZ);
		if (ore != TextureType::AIR)
		{
			return ore;
		}
	}

	// Surface and subsurface blocks based on biome
	if (depthFromSurface == 0)
	{
		// Surface block
		switch (biome)
		{
		case BiomeType::DESERT:
			return TextureType::SAND;
		case BiomeType::SNOWY:
			return TextureType::SNOW;
		case BiomeType::FOREST:
		case BiomeType::PLAINS:
			return TextureType::GRASS_TOP;
		case BiomeType::MOUNTAINS:
			return y > 120 ? TextureType::STONE : TextureType::GRASS_TOP;
		}
	}
	else if (depthFromSurface <= 3)
	{
		// Subsurface blocks
		switch (biome)
		{
		case BiomeType::DESERT:
			return depthFromSurface <= 5 ? TextureType::SAND : TextureType::STONE;
		case BiomeType::SNOWY:
			return depthFromSurface == 1 ? TextureType::DIRT : TextureType::STONE;
		case BiomeType::FOREST:
		case BiomeType::PLAINS:
			return depthFromSurface <= 3 ? TextureType::DIRT : TextureType::STONE;
		case BiomeType::MOUNTAINS:
			return depthFromSurface <= 2 && y <= 120 ? TextureType::DIRT : TextureType::STONE;
		}
	}

	return TextureType::STONE;
}

TextureType TerrainGenerator::generateOre(int worldX, int y, int worldZ)
{
	// Use same offset for consistency
	float oreValue = m_oreNoise.GetNoise(static_cast<float>(worldX + 10000), static_cast<float>(y), static_cast<float>(worldZ + 10000));

	// Different ores at different depths with different rarities
	if (oreValue > 0.85f)
	{
		if (y < 20)
		{
			if (oreValue > 0.95f)
				return TextureType::DIAMOND_ORE;
			if (oreValue > 0.92f)
				return TextureType::EMERALD_ORE;
			if (oreValue > 0.89f)
				return TextureType::GOLD_ORE;
			return TextureType::IRON_ORE;
		}
		else if (y < 40)
		{
			if (oreValue > 0.92f)
				return TextureType::GOLD_ORE;
			if (oreValue > 0.89f)
				return TextureType::REDSTONE_ORE;
			if (oreValue > 0.87f)
				return TextureType::LAPIS_ORE;
			return TextureType::IRON_ORE;
		}
		else if (y < 80)
		{
			if (oreValue > 0.90f)
				return TextureType::COPPER_ORE;
			if (oreValue > 0.87f)
				return TextureType::COAL_ORE;
			return TextureType::IRON_ORE;
		}
		else
		{
			return TextureType::COAL_ORE;
		}
	}

	return TextureType::AIR; // No ore
}

bool TerrainGenerator::isCave(int worldX, int y, int worldZ)
{
	if (y <= BEDROCK_LEVEL + 5 || y >= CHUNK_HEIGHT - 10)
		return false;

	// Use same offset for consistency
	float caveValue = m_caveNoise.GetNoise(static_cast<float>(worldX + 10000), static_cast<float>(y), static_cast<float>(worldZ + 10000));
	return caveValue > 0.3f; // Lower threshold for more caves
}

void TerrainGenerator::generateVegetation(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ,
										  int worldX, int worldZ, int groundHeight, BiomeType biome)
{

	std::mt19937 rng(worldX * 374761393 + worldZ * 668265263); // Deterministic random
	std::uniform_real_distribution<float> dist(0.0f, 1.0f);

	float vegetationChance = 0.0f;
	switch (biome)
	{
	case BiomeType::FOREST:
		vegetationChance = 0.15f;
		break;
	case BiomeType::PLAINS:
		vegetationChance = 0.05f;
		break;
	case BiomeType::DESERT:
	case BiomeType::MOUNTAINS:
	case BiomeType::SNOWY:
		vegetationChance = 0.01f;
		break;
	}

	if (dist(rng) < vegetationChance && groundHeight + MAX_TREE_HEIGHT < CHUNK_HEIGHT)
	{
		generateTree(voxels, localX, localZ, groundHeight + 1, rng);
	}
}

void TerrainGenerator::generateTree(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ,
									int startY, std::mt19937 &rng)
{
	std::uniform_int_distribution<int> heightDist(MIN_TREE_HEIGHT, MAX_TREE_HEIGHT);
	int treeHeight = heightDist(rng);

	// Generate trunk
	for (int i = 0; i < treeHeight; ++i)
	{
		int y = startY + i;
		if (y >= CHUNK_HEIGHT)
			break;

		int index = getVoxelIndex(localX, y, localZ);
		voxels[index].type = TextureType::OAK_LOG;
	}

	// Generate leaves (simple sphere)
	int leavesY = startY + treeHeight;
	for (int dx = -2; dx <= 2; ++dx)
	{
		for (int dz = -2; dz <= 2; ++dz)
		{
			for (int dy = -1; dy <= 2; ++dy)
			{
				int leafX = localX + dx;
				int leafZ = localZ + dz;
				int leafY = leavesY + dy;

				// Check bounds
				if (leafX < 0 || leafX >= CHUNK_SIZE ||
					leafZ < 0 || leafZ >= CHUNK_SIZE ||
					leafY >= CHUNK_HEIGHT)
					continue;

				// Simple distance check for leaf placement
				float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (distance <= 2.5f && distance >= 0.5f)
				{
					int index = getVoxelIndex(leafX, leafY, leafZ);
					if (voxels[index].type == TextureType::AIR)
					{
						voxels[index].type = TextureType::OAK_LEAVES;
					}
				}
			}
		}
	}
}