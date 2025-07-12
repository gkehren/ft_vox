#include <Chunk/TerrainGenerator.hpp>

TerrainGenerator::TerrainGenerator(int seed)
	: m_baseHeight(SEA_LEVEL), m_seed(seed)
{
	setupNoiseGenerators();
}

ChunkData TerrainGenerator::generateChunk(int chunkX, int chunkZ)
{
	ChunkData chunkData;
	chunkData.voxels.fill({TextureType::AIR});
	chunkData.biomeMap.fill(BiomeType::PLAINS);
	// Generate terrain for each column in the chunk
	for (int localX = 0; localX < CHUNK_SIZE; ++localX)
	{
		for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ)
		{
			int worldX = chunkX + localX; // chunkX are in world coordinates, do not multiply by CHUNK_SIZE
			int worldZ = chunkZ + localZ; // chunkZ are in world coordinates, do not multiply by CHUNK_SIZE

			// Generate height at this position
			int terrainHeight = generateHeightAt(worldX, worldZ);

			// Generate the column
			generateColumn(chunkData.voxels, localX, localZ, worldX, worldZ, terrainHeight);
		}
	}

	return chunkData;
}

void TerrainGenerator::setupNoiseGenerators()
{
	// Continental noise - Large scale continent shapes (like Minecraft's continentalness)
	auto continentalBase = FastNoise::New<FastNoise::OpenSimplex2>();
	auto continentalFractal = FastNoise::New<FastNoise::FractalFBm>();
	continentalFractal->SetSource(continentalBase);
	continentalFractal->SetOctaveCount(4);
	continentalFractal->SetLacunarity(2.0f);
	continentalFractal->SetGain(0.5f);

	m_continentalNoise = FastNoise::New<FastNoise::DomainScale>();
	m_continentalNoise->SetSource(continentalFractal);
	m_continentalNoise->SetScale(0.001f); // Échelle encore plus large pour plus de cohérence

	// Erosion noise - Controls terrain smoothness vs. jaggedness
	auto erosionBase = FastNoise::New<FastNoise::Perlin>();
	auto erosionFractal = FastNoise::New<FastNoise::FractalFBm>();
	erosionFractal->SetSource(erosionBase);
	erosionFractal->SetOctaveCount(3);
	erosionFractal->SetLacunarity(2.0f);
	erosionFractal->SetGain(0.6f);

	m_erosionNoise = FastNoise::New<FastNoise::DomainScale>();
	m_erosionNoise->SetSource(erosionFractal);
	m_erosionNoise->SetScale(0.005f); // Échelle plus large pour transitions plus douces

	// Peaks and Valleys noise - Controls where high and low areas are
	auto pvBase = FastNoise::New<FastNoise::OpenSimplex2>();
	auto pvFractal = FastNoise::New<FastNoise::FractalFBm>();
	pvFractal->SetSource(pvBase);
	pvFractal->SetOctaveCount(4);
	pvFractal->SetLacunarity(2.1f);
	pvFractal->SetGain(0.55f);

	m_peaksValleysNoise = FastNoise::New<FastNoise::DomainScale>();
	m_peaksValleysNoise->SetSource(pvFractal);
	m_peaksValleysNoise->SetScale(0.006f); // Échelle plus large pour éviter les variations abruptes

	// Temperature noise - For biome variation later
	auto tempBase = FastNoise::New<FastNoise::Simplex>();
	m_temperatureNoise = FastNoise::New<FastNoise::DomainScale>();
	m_temperatureNoise->SetSource(tempBase);
	m_temperatureNoise->SetScale(0.006f);

	// Humidity noise - For biome variation later
	auto humidBase = FastNoise::New<FastNoise::Simplex>();
	m_humidityNoise = FastNoise::New<FastNoise::DomainScale>();
	m_humidityNoise->SetSource(humidBase);
	m_humidityNoise->SetScale(0.007f);

	// Ridge noise - For creating sharp mountain ridges
	auto ridgeBase = FastNoise::New<FastNoise::OpenSimplex2>();
	auto ridgeFractal = FastNoise::New<FastNoise::FractalRidged>();
	ridgeFractal->SetSource(ridgeBase);
	ridgeFractal->SetOctaveCount(5);
	ridgeFractal->SetLacunarity(2.5f);
	ridgeFractal->SetGain(0.7f);

	m_ridgeNoise = FastNoise::New<FastNoise::DomainScale>();
	m_ridgeNoise->SetSource(ridgeFractal);
	m_ridgeNoise->SetScale(0.015f);
}

// Fonction spline pour des transitions douces (style Minecraft)
float TerrainGenerator::spline(float val)
{
	// Clamp to avoid extreme values that cause discontinuities
	val = std::clamp(val, -1.0f, 1.0f);

	// Smoother cubic hermite interpolation
	return val * val * (3.0f - 2.0f * std::abs(val));
}

float TerrainGenerator::smoothstep(float edge0, float edge1, float x)
{
	float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

int TerrainGenerator::generateHeightAt(int worldX, int worldZ)
{
	// Sample all noise layers at this position
	std::vector<float> coords = {static_cast<float>(worldX), static_cast<float>(worldZ)};
	std::vector<float> continentalResult(1);
	std::vector<float> erosionResult(1);
	std::vector<float> peaksValleysResult(1);
	std::vector<float> ridgeResult(1);

	m_continentalNoise->GenPositionArray2D(continentalResult.data(), 1,
										   coords.data(), coords.data() + 1,
										   0.0f, 0.0f, m_seed);

	m_erosionNoise->GenPositionArray2D(erosionResult.data(), 1,
									   coords.data(), coords.data() + 1,
									   0.0f, 0.0f, m_seed + 1000);

	m_peaksValleysNoise->GenPositionArray2D(peaksValleysResult.data(), 1,
											coords.data(), coords.data() + 1,
											0.0f, 0.0f, m_seed + 2000);

	m_ridgeNoise->GenPositionArray2D(ridgeResult.data(), 1,
									 coords.data(), coords.data() + 1,
									 0.0f, 0.0f, m_seed + 3000);

	// Process noise values (similar to Minecraft's approach)
	float continental = continentalResult[0];
	float erosion = erosionResult[0];
	float peaksValleys = peaksValleysResult[0];
	float ridge = ridgeResult[0];

	// Apply spline curves for smoother terrain
	continental = spline(continental);
	erosion = spline(erosion);
	peaksValleys = spline(peaksValleys);

	// Calculate base height using continental noise
	float baseHeight = static_cast<float>(SEA_LEVEL);

	// Continental factor affects overall elevation (-1 to 1 range)
	float continentalFactor = (continental + 1.0f) * 0.5f; // 0 to 1

	// Erosion factor affects terrain sharpness (0 = sharp, 1 = smooth)
	float erosionFactor = (erosion + 1.0f) * 0.5f; // 0 to 1

	// Peaks and valleys factor
	float pvFactor = peaksValleys;

	// Calculate height variations based on continental factor
	float heightVariation = 0.0f;

	if (continentalFactor < 0.1f)
	{
		// Deep ocean - transition plus douce
		float oceanDepth = smoothstep(0.0f, 0.1f, continentalFactor);
		heightVariation = -20.0f + oceanDepth * 15.0f; // De -20 à -5
	}
	else if (continentalFactor < 0.25f)
	{
		// Ocean to shore transition - plus progressive
		float shoreTransition = smoothstep(0.1f, 0.25f, continentalFactor);
		heightVariation = -5.0f + shoreTransition * 10.0f; // De -5 à +5
	}
	else if (continentalFactor < 0.4f)
	{
		// Shore and lowlands - variations douces
		float lowlandBase = smoothstep(0.25f, 0.4f, continentalFactor) * 10.0f; // 0 à 10
		float lowlandVariation = pvFactor * 8.0f;								// Petites variations
		heightVariation = 5.0f + lowlandBase + lowlandVariation;
	}
	else if (continentalFactor < 0.55f)
	{
		// Plains and gentle hills - variations modérées
		float plainsTransition = smoothstep(0.4f, 0.55f, continentalFactor);
		float plainsBase = 15.0f + plainsTransition * 10.0f; // 15 à 25
		float hillHeight = pvFactor * 20.0f;				 // -20 to +20 blocks
		heightVariation = plainsBase + hillHeight;

		// Apply erosion - less erosion means slightly sharper terrain
		if (erosionFactor < 0.2f)
		{
			heightVariation += std::abs(ridge) * 10.0f; // Variations plus modérées
		}
	}
	else if (continentalFactor < 0.7f)
	{
		// Transition vers les collines - plus progressive
		float hillTransition = smoothstep(0.55f, 0.7f, continentalFactor);
		float hillBase = 25.0f + hillTransition * 20.0f; // 25 à 45
		float hillVariation = pvFactor * 25.0f;			 // -25 to +25 blocks
		heightVariation = hillBase + hillVariation;

		// Apply ridge noise progressively
		float ridgeInfluence = smoothstep(0.6f, 0.7f, continentalFactor);
		if (erosionFactor < 0.25f)
		{
			heightVariation += std::abs(ridge) * ridgeInfluence * 20.0f;
		}
	}
	else if (continentalFactor < 0.85f)
	{
		// Transition douce vers les montagnes - CRUCIAL pour éviter les murs
		float preMotainTransition = smoothstep(0.7f, 0.85f, continentalFactor);
		float mountainBase = 45.0f + preMotainTransition * 30.0f; // 45 à 75 (transition très douce)
		float mountainVariation = pvFactor * 35.0f;				  // -35 to +35 blocks
		heightVariation = mountainBase + mountainVariation;

		// Ridge influence croissante mais progressive
		float ridgeInfluence = smoothstep(0.75f, 0.85f, continentalFactor);
		if (erosionFactor < 0.3f)
		{
			heightVariation += std::abs(ridge) * ridgeInfluence * 35.0f;
		}
	}
	else
	{
		// Hautes montagnes - avec base progressive depuis la zone précédente
		float highMountainTransition = smoothstep(0.85f, 1.0f, continentalFactor);
		float mountainBase = 75.0f + highMountainTransition * 40.0f; // 75 à 115 (continuation douce)
		float mountainHeight = pvFactor * 50.0f;					 // Large variation mais contrôlée
		heightVariation = mountainBase + mountainHeight;

		// Apply ridge noise for sharp peaks avec transition douce
		if (erosionFactor < 0.4f)
		{
			float ridgeInfluence = 0.5f + highMountainTransition * 0.5f; // 0.5 à 1.0
			heightVariation += std::abs(ridge) * ridgeInfluence * 45.0f;
		}
	}

	// Final height calculation
	float finalHeight = baseHeight + heightVariation;

	// Clamp to valid range
	return std::max(1, std::min(static_cast<int>(finalHeight), CHUNK_HEIGHT - 1));
}

void TerrainGenerator::generateColumn(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ,
									  int worldX, int worldZ, int terrainHeight)
{
	// Generate simple terrain column - focus on height only for now
	for (int y = 0; y < CHUNK_HEIGHT; ++y)
	{
		int index = getVoxelIndex(localX, y, localZ);

		if (y <= BEDROCK_LEVEL)
		{
			voxels[index].type = TextureType::BEDROCK;
		}
		else if (y <= terrainHeight)
		{
			// Simple terrain generation based on depth
			int depthFromSurface = terrainHeight - y;

			if (depthFromSurface == 0)
			{
				// Surface block
				voxels[index].type = TextureType::GRASS_TOP;
			}
			else if (depthFromSurface <= 3)
			{
				// Subsurface blocks
				voxels[index].type = TextureType::DIRT;
			}
			else
			{
				// Deep blocks
				voxels[index].type = TextureType::STONE;
			}
		}
		// else remains AIR (default)
	}
}
