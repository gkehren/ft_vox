#include <Chunk/TerrainGenerator.hpp>

TerrainGenerator::TerrainGenerator(int seed)
	: m_heightScale(50.0f), m_mountainScale(50.0f), m_baseHeight(SEA_LEVEL), m_seed(seed)
{
	setupNoiseGenerators();
}

std::array<Voxel, CHUNK_VOLUME> TerrainGenerator::generateChunk(int chunkX, int chunkZ)
{
	std::array<Voxel, CHUNK_VOLUME> voxels;
	voxels.fill({TextureType::AIR});

	// Pre-generate height map for the entire chunk using batch processing
	std::vector<int> heightMap = generateHeightMap(chunkX, chunkZ);
	std::vector<BiomeType> biomeMap = generateBiomeMap(chunkX, chunkZ);

	// Generate terrain for each column in the chunk
	for (int localX = 0; localX < CHUNK_SIZE; ++localX)
	{
		for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ)
		{
			int worldX = chunkX + localX;
			int worldZ = chunkZ + localZ;
			int columnIndex = localZ * CHUNK_SIZE + localX;

			generateColumn(voxels, localX, localZ, worldX, worldZ, heightMap[columnIndex], biomeMap[columnIndex]);
		}
	}

	return voxels;
}

void TerrainGenerator::setupNoiseGenerators()
{
	// Height map noise - Complex FBm with ridged noise for mountains
	auto heightBase = FastNoise::New<FastNoise::Simplex>();

	auto heightFBm = FastNoise::New<FastNoise::FractalFBm>();
	heightFBm->SetSource(heightBase);
	heightFBm->SetOctaveCount(6);
	heightFBm->SetLacunarity(2.0f);
	heightFBm->SetGain(0.5f);
	heightFBm->SetWeightedStrength(0.0f);

	// Add ridged noise for mountain peaks
	auto ridgedBase = FastNoise::New<FastNoise::Simplex>();

	auto ridgedNoise = FastNoise::New<FastNoise::FractalRidged>();
	ridgedNoise->SetSource(ridgedBase);
	ridgedNoise->SetOctaveCount(4);
	ridgedNoise->SetLacunarity(2.0f);
	ridgedNoise->SetGain(0.6f);
	// Combine FBm and Ridged for varied terrain - scale ridged noise down
	auto ridgedScale = FastNoise::New<FastNoise::DomainScale>();
	ridgedScale->SetSource(ridgedNoise);
	ridgedScale->SetScale(0.5f); // Scale ridged noise to be less dominant

	auto heightAdd = FastNoise::New<FastNoise::Add>();
	heightAdd->SetLHS(heightFBm);
	heightAdd->SetRHS(ridgedScale);
	auto heightScale = FastNoise::New<FastNoise::DomainScale>();
	heightScale->SetSource(heightAdd);
	heightScale->SetScale(0.005f); // Increase frequency for more variation

	m_heightNoise = heightScale;

	// Biome noise - Simple Perlin for smooth biome transitions
	auto biomeBase = FastNoise::New<FastNoise::Perlin>();

	auto biomeScale = FastNoise::New<FastNoise::DomainScale>();
	biomeScale->SetSource(biomeBase);
	biomeScale->SetScale(0.005f);

	m_biomeNoise = biomeScale;

	// Temperature noise
	auto tempBase = FastNoise::New<FastNoise::Simplex>();

	auto tempScale = FastNoise::New<FastNoise::DomainScale>();
	tempScale->SetSource(tempBase);
	tempScale->SetScale(0.008f);

	m_temperatureNoise = tempScale;

	// Humidity noise
	auto humidBase = FastNoise::New<FastNoise::Simplex>();

	auto humidScale = FastNoise::New<FastNoise::DomainScale>();
	humidScale->SetSource(humidBase);
	humidScale->SetScale(0.007f);

	m_humidityNoise = humidScale;

	// Cave noise - 3D ridged fractal for interesting cave systems
	auto caveBase = FastNoise::New<FastNoise::OpenSimplex2>();

	auto caveFractal = FastNoise::New<FastNoise::FractalRidged>();
	caveFractal->SetSource(caveBase);
	caveFractal->SetOctaveCount(3);
	caveFractal->SetLacunarity(2.0f);
	caveFractal->SetGain(0.5f);

	auto caveScale = FastNoise::New<FastNoise::DomainScale>();
	caveScale->SetSource(caveFractal);
	caveScale->SetScale(0.02f);

	m_caveNoise = caveScale;

	// Ore noise - High frequency for ore pockets
	auto oreBase = FastNoise::New<FastNoise::Value>();

	auto oreFractal = FastNoise::New<FastNoise::FractalFBm>();
	oreFractal->SetSource(oreBase);
	oreFractal->SetOctaveCount(2);
	oreFractal->SetLacunarity(2.5f);
	oreFractal->SetGain(0.4f);

	auto oreScale = FastNoise::New<FastNoise::DomainScale>();
	oreScale->SetSource(oreFractal);
	oreScale->SetScale(0.05f);

	m_oreNoise = oreScale;
}

std::vector<int> TerrainGenerator::generateHeightMap(int chunkX, int chunkZ)
{
	std::vector<int> heightMap(CHUNK_SIZE * CHUNK_SIZE);
	std::vector<float> noiseOutput(CHUNK_SIZE * CHUNK_SIZE);

	// Prepare coordinates arrays for batch processing
	// int index = 0;
	// for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ)
	//{
	//	for (int localX = 0; localX < CHUNK_SIZE; ++localX)
	//	{
	//		m_xCoords[index] = static_cast<float>(chunkX + localX);
	//		m_yCoords[index] = static_cast<float>(chunkZ + localZ);
	//		++index;
	//	}
	//}	// Generate height values in batch
	m_heightNoise->GenUniformGrid2D(noiseOutput.data(), chunkX, chunkZ, CHUNK_SIZE, CHUNK_SIZE, 0.005f, m_seed);

	// Convert noise values to height
	for (int i = 0; i < CHUNK_SIZE * CHUNK_SIZE; ++i)
	{
		int terrainHeight = static_cast<int>(m_baseHeight + noiseOutput[i] * m_heightScale);
		heightMap[i] = std::max(1, std::min(terrainHeight, CHUNK_HEIGHT - 1)); // Clamp height to valid range
	}

	return heightMap;
}

std::vector<BiomeType> TerrainGenerator::generateBiomeMap(int chunkX, int chunkZ)
{
	std::vector<BiomeType> biomeMap(CHUNK_SIZE * CHUNK_SIZE);
	std::vector<float> tempOutput(CHUNK_SIZE * CHUNK_SIZE);
	std::vector<float> humidOutput(CHUNK_SIZE * CHUNK_SIZE);
	std::vector<float> elevationOutput(CHUNK_SIZE * CHUNK_SIZE);

	// Generate temperature, humidity, and elevation maps
	m_temperatureNoise->GenUniformGrid2D(tempOutput.data(), chunkX, chunkZ, CHUNK_SIZE, CHUNK_SIZE, 0.008f, m_seed + 2000);

	m_humidityNoise->GenUniformGrid2D(humidOutput.data(), chunkX, chunkZ, CHUNK_SIZE, CHUNK_SIZE, 0.007f, m_seed + 3000);

	m_biomeNoise->GenUniformGrid2D(elevationOutput.data(), chunkX, chunkZ, CHUNK_SIZE, CHUNK_SIZE, 0.005f, m_seed + 1000);

	// Determine biomes
	for (int i = 0; i < CHUNK_SIZE * CHUNK_SIZE; ++i)
	{
		float temperature = tempOutput[i];
		float humidity = humidOutput[i];
		float elevation = elevationOutput[i];

		biomeMap[i] = getBiomeFromValues(temperature, humidity, elevation);
	}

	return biomeMap;
}

BiomeType TerrainGenerator::getBiomeFromValues(float temperature, float humidity, float elevation)
{
	// Determine biome based on temperature, humidity, and elevation
	if (elevation > 0.4f)
		return temperature < -0.2f ? BiomeType::SNOWY : BiomeType::MOUNTAINS;
	else if (temperature < -0.3f)
		return BiomeType::SNOWY;
	else if (temperature > 0.3f && humidity < -0.2f)
		return BiomeType::DESERT;
	else if (humidity > 0.2f && temperature > -0.1f)
		return BiomeType::FOREST;
	else
		return BiomeType::PLAINS;
}

void TerrainGenerator::generateColumn(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int worldX, int worldZ, int terrainHeight, BiomeType biome)
{
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
	// Use 3D coordinates for ore generation
	std::vector<float> oreCoordsX = {static_cast<float>(worldX)};
	std::vector<float> oreCoordsY = {static_cast<float>(y)};
	std::vector<float> oreCoordsZ = {static_cast<float>(worldZ)};
	std::vector<float> oreResult(1);

	m_oreNoise->GenPositionArray3D(oreResult.data(), 1, oreCoordsX.data(), oreCoordsY.data(), oreCoordsZ.data(), 0.0f, 0.0f, 0.0f, m_seed + 5000);
	float oreValue = oreResult[0];

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

	std::vector<float> caveCoordsX = {static_cast<float>(worldX)};
	std::vector<float> caveCoordsY = {static_cast<float>(y)};
	std::vector<float> caveCoordsZ = {static_cast<float>(worldZ)};
	std::vector<float> caveResult(1);

	m_caveNoise->GenPositionArray3D(caveResult.data(), 1, caveCoordsX.data(), caveCoordsY.data(), caveCoordsZ.data(), 0.0f, 0.0f, 0.0f, m_seed + 4000);
	return caveResult[0] > 0.6f; // Cave threshold
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