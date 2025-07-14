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

	// Use optimized batch generation
	generateChunkBatch(chunkData, chunkX, chunkZ);

	// Generate chunk borders for mesh optimization
	generateChunkBorders(chunkData, chunkX, chunkZ);

	return chunkData;
}

void TerrainGenerator::generateChunkBatch(ChunkData &chunkData, int chunkX, int chunkZ)
{
	constexpr int totalPoints = CHUNK_SIZE * CHUNK_SIZE;

	// Prepare coordinate arrays for batch processing
	std::vector<float> xCoords(totalPoints);
	std::vector<float> zCoords(totalPoints);
	std::vector<float> heightMap(totalPoints);

	// Fill coordinate arrays
	for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ)
	{
		for (int localX = 0; localX < CHUNK_SIZE; ++localX)
		{
			int index = localZ * CHUNK_SIZE + localX;
			xCoords[index] = static_cast<float>(chunkX + localX);
			zCoords[index] = static_cast<float>(chunkZ + localZ);
		}
	}

	// Batch generate all noise values for terrain height
	std::vector<float> continentalResults(totalPoints);
	std::vector<float> erosionResults(totalPoints);
	std::vector<float> peaksValleysResults(totalPoints);
	std::vector<float> ridgeResults(totalPoints);

	m_continentalNoise->GenPositionArray2D(continentalResults.data(), totalPoints,
										   xCoords.data(), zCoords.data(),
										   0.0f, 0.0f, m_seed);

	m_erosionNoise->GenPositionArray2D(erosionResults.data(), totalPoints,
									   xCoords.data(), zCoords.data(),
									   0.0f, 0.0f, m_seed + 1000);

	m_peaksValleysNoise->GenPositionArray2D(peaksValleysResults.data(), totalPoints,
											xCoords.data(), zCoords.data(),
											0.0f, 0.0f, m_seed + 2000);

	m_ridgeNoise->GenPositionArray2D(ridgeResults.data(), totalPoints,
									 xCoords.data(), zCoords.data(),
									 0.0f, 0.0f, m_seed + 3000);

	// Calculate height map using batch results
	for (int i = 0; i < totalPoints; ++i)
	{
		float continental = spline(continentalResults[i]);
		float erosion = spline(erosionResults[i]);
		float peaksValleys = spline(peaksValleysResults[i]);
		float ridge = ridgeResults[i];

		// Apply the same height calculation logic as before
		float baseHeight = static_cast<float>(SEA_LEVEL);
		float continentalFactor = (continental + 1.0f) * 0.5f;
		float erosionFactor = (erosion + 1.0f) * 0.5f;
		float pvFactor = peaksValleys;

		float heightVariation = 0.0f;

		if (continentalFactor < 0.1f)
		{
			float oceanDepth = smoothstep(0.0f, 0.1f, continentalFactor);
			heightVariation = -20.0f + oceanDepth * 15.0f;
		}
		else if (continentalFactor < 0.25f)
		{
			float shoreTransition = smoothstep(0.1f, 0.25f, continentalFactor);
			heightVariation = -5.0f + shoreTransition * 10.0f;
		}
		else if (continentalFactor < 0.4f)
		{
			float lowlandBase = smoothstep(0.25f, 0.4f, continentalFactor) * 10.0f;
			float lowlandVariation = pvFactor * 8.0f;
			heightVariation = 5.0f + lowlandBase + lowlandVariation;
		}
		else if (continentalFactor < 0.55f)
		{
			float plainsTransition = smoothstep(0.4f, 0.55f, continentalFactor);
			float plainsBase = 15.0f + plainsTransition * 10.0f;
			float hillHeight = pvFactor * 20.0f;
			heightVariation = plainsBase + hillHeight;

			if (erosionFactor < 0.2f)
			{
				heightVariation += std::abs(ridge) * 10.0f;
			}
		}
		else if (continentalFactor < 0.7f)
		{
			float hillTransition = smoothstep(0.55f, 0.7f, continentalFactor);
			float hillBase = 25.0f + hillTransition * 20.0f;
			float hillVariation = pvFactor * 25.0f;
			heightVariation = hillBase + hillVariation;

			float ridgeInfluence = smoothstep(0.6f, 0.7f, continentalFactor);
			if (erosionFactor < 0.25f)
			{
				heightVariation += std::abs(ridge) * ridgeInfluence * 20.0f;
			}
		}
		else if (continentalFactor < 0.85f)
		{
			float preMotainTransition = smoothstep(0.7f, 0.85f, continentalFactor);
			float mountainBase = 45.0f + preMotainTransition * 20.0f;
			float mountainVariation = pvFactor * 25.0f;
			heightVariation = mountainBase + mountainVariation;

			float ridgeInfluence = smoothstep(0.75f, 0.85f, continentalFactor);
			if (erosionFactor < 0.3f)
			{
				heightVariation += std::abs(ridge) * ridgeInfluence * 20.0f;
			}
		}
		else
		{
			float highMountainTransition = smoothstep(0.85f, 1.0f, continentalFactor);
			float mountainBase = 65.0f + highMountainTransition * 45.0f;
			float mountainHeight = pvFactor * 35.0f;
			heightVariation = mountainBase + mountainHeight;

			if (erosionFactor < 0.4f)
			{
				float ridgeInfluence = 0.3f + highMountainTransition * 0.7f;
				heightVariation += std::abs(ridge) * ridgeInfluence * 35.0f;
			}
		}

		float finalHeight = baseHeight + heightVariation;
		heightMap[i] = std::max(1.0f, std::min(finalHeight, static_cast<float>(CHUNK_HEIGHT - 1)));
	}

	// Generate biomes in batch
	determineBiomesBatch(chunkData.biomeMap, heightMap, chunkX, chunkZ);

	// Generate basic terrain columns
	for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ)
	{
		for (int localX = 0; localX < CHUNK_SIZE; ++localX)
		{
			int index = localZ * CHUNK_SIZE + localX;
			int terrainHeight = static_cast<int>(heightMap[index]);
			int worldX = chunkX + localX;
			int worldZ = chunkZ + localZ;
			BiomeType biome = chunkData.biomeMap[index];

			// Generate basic column
			generateColumn(chunkData.voxels, localX, localZ, worldX, worldZ, terrainHeight);

			// Apply biome-specific surface blocks
			int surfaceIndex = getVoxelIndex(localX, terrainHeight, localZ);
			if (terrainHeight < CHUNK_HEIGHT && chunkData.voxels[surfaceIndex].type != TextureType::AIR)
			{
				chunkData.voxels[surfaceIndex].type = getBiomeSurfaceBlock(biome, terrainHeight);
			}

			// Apply biome-specific subsurface modifications
			for (int y = terrainHeight - 1; y >= std::max(terrainHeight - 6, BEDROCK_LEVEL + 1); --y)
			{
				int subIndex = getVoxelIndex(localX, y, localZ);
				if (y < CHUNK_HEIGHT && chunkData.voxels[subIndex].type == TextureType::DIRT)
				{
					if (biome == BiomeType::DESERT && (terrainHeight - y) <= 6)
					{
						chunkData.voxels[subIndex].type = TextureType::SAND;
					}
					// Other biomes keep dirt as is
				}
			}
		}
	}

	// Generate structures in batch
	generateStructuresBatch(chunkData, chunkX, chunkZ);
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

	// Tree noise - Pour espacement naturel des arbres
	auto treeBase = FastNoise::New<FastNoise::Perlin>();
	auto treeFractal = FastNoise::New<FastNoise::FractalFBm>();
	treeFractal->SetSource(treeBase);
	treeFractal->SetOctaveCount(2);	  // Moins d'octaves pour patterns plus larges
	treeFractal->SetLacunarity(3.0f); // Plus de lacunarité pour espacement
	treeFractal->SetGain(0.4f);		  // Moins de gain pour réduire densité

	m_treeNoise = FastNoise::New<FastNoise::DomainScale>();
	m_treeNoise->SetSource(treeFractal);
	m_treeNoise->SetScale(0.03f); // Échelle beaucoup plus large pour espacement naturel

	// Rock noise - For rock formations and boulders
	auto rockBase = FastNoise::New<FastNoise::OpenSimplex2>();
	auto rockFractal = FastNoise::New<FastNoise::FractalFBm>();
	rockFractal->SetSource(rockBase);
	rockFractal->SetOctaveCount(3);
	rockFractal->SetLacunarity(2.0f);
	rockFractal->SetGain(0.6f);

	m_rockNoise = FastNoise::New<FastNoise::DomainScale>();
	m_rockNoise->SetSource(rockFractal);
	m_rockNoise->SetScale(0.08f); // Smaller scale for detailed rock placement

	// Vegetation noise - For bushes and small vegetation
	auto vegBase = FastNoise::New<FastNoise::Simplex>();
	m_vegetationNoise = FastNoise::New<FastNoise::DomainScale>();
	m_vegetationNoise->SetSource(vegBase);
	m_vegetationNoise->SetScale(0.1f); // High frequency for scattered vegetation

	// Ore noise - For mineral deposits
	auto oreBase = FastNoise::New<FastNoise::Perlin>();
	auto oreFractal = FastNoise::New<FastNoise::FractalFBm>();
	oreFractal->SetSource(oreBase);
	oreFractal->SetOctaveCount(4);
	oreFractal->SetLacunarity(2.2f);
	oreFractal->SetGain(0.5f);

	m_oreNoise = FastNoise::New<FastNoise::DomainScale>();
	m_oreNoise->SetSource(oreFractal);
	m_oreNoise->SetScale(0.03f); // Large scale for ore veins
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
		float mountainBase = 45.0f + preMotainTransition * 20.0f; // 45 à 65 (réduit encore)
		float mountainVariation = pvFactor * 25.0f;				  // -25 to +25 blocks (réduit)
		heightVariation = mountainBase + mountainVariation;

		// Ridge influence croissante mais progressive
		float ridgeInfluence = smoothstep(0.75f, 0.85f, continentalFactor);
		if (erosionFactor < 0.3f)
		{
			heightVariation += std::abs(ridge) * ridgeInfluence * 20.0f; // Réduit pour continuité
		}
	}
	else
	{
		// Hautes montagnes - avec base progressive depuis la zone précédente
		float highMountainTransition = smoothstep(0.85f, 1.0f, continentalFactor);
		float mountainBase = 65.0f + highMountainTransition * 45.0f; // 65 à 110 (ajusté pour continuité parfaite)
		float mountainHeight = pvFactor * 35.0f;					 // Large variation mais contrôlée
		heightVariation = mountainBase + mountainHeight;

		// Apply ridge noise for sharp peaks avec transition douce
		if (erosionFactor < 0.4f)
		{
			float ridgeInfluence = 0.3f + highMountainTransition * 0.7f; // 0.3 à 1.0 (encore plus progressif)
			heightVariation += std::abs(ridge) * ridgeInfluence * 35.0f; // Réduit mais toujours impressionnant
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
	// Note: Biome is now determined in batch, so we'll use a placeholder here
	// This method will be called from generateChunkBatch where biome is already known
	BiomeType biome = BiomeType::PLAINS; // Will be overridden by caller

	for (int y = 0; y < CHUNK_HEIGHT; ++y)
	{
		int index = getVoxelIndex(localX, y, localZ);

		if (y <= BEDROCK_LEVEL)
		{
			voxels[index].type = TextureType::BEDROCK;
		}
		else if (y <= terrainHeight)
		{
			int depthFromSurface = terrainHeight - y;

			if (depthFromSurface == 0)
			{
				// Surface block - depends on biome and height
				// For now use basic logic, will be improved with proper biome from batch generation
				if (terrainHeight <= SEA_LEVEL + 1)
				{
					voxels[index].type = TextureType::SAND;
				}
				else
				{
					voxels[index].type = TextureType::GRASS_TOP;
				}
			}
			else if (depthFromSurface <= 3)
			{
				// Subsurface blocks
				if (terrainHeight <= SEA_LEVEL + 2) // Near water
				{
					voxels[index].type = TextureType::SAND;
				}
				else
				{
					voxels[index].type = TextureType::DIRT;
				}
			}
			else
			{
				// Deep blocks - with potential ore generation
				voxels[index].type = TextureType::STONE;
			}
		}
		else if (y <= SEA_LEVEL)
		{
			// Water generation
			voxels[index].type = TextureType::WATER;
		}
		// else remains AIR (default)
	}
}

void TerrainGenerator::generateTreeAt(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int surfaceY, int treeHeight)
{
	// Arbre style Minecraft avec tronc d'un seul voxel
	if (surfaceY + treeHeight + 3 >= CHUNK_HEIGHT) // +3 pour les feuilles au-dessus
		return;

	// Tronc - un seul voxel de large
	for (int y = 1; y <= treeHeight; ++y)
	{
		int trunkY = surfaceY + y;
		if (trunkY < CHUNK_HEIGHT)
		{
			int index = getVoxelIndex(localX, trunkY, localZ);
			if (voxels[index].type == TextureType::AIR)
			{
				voxels[index].type = TextureType::OAK_LOG;
			}
		}
	}

	// Feuillage style Minecraft - 4 couches
	int leavesTopY = surfaceY + treeHeight; // Sommet du tronc

	// Couche 1 (sommet) - 1 bloc de feuilles au-dessus du tronc
	{
		int leafY = leavesTopY + 1;
		if (leafY < CHUNK_HEIGHT)
		{
			int index = getVoxelIndex(localX, leafY, localZ);
			if (voxels[index].type == TextureType::AIR)
			{
				voxels[index].type = TextureType::OAK_LEAVES;
			}
		}
	}

	// Couche 2 (niveau du sommet du tronc) - croix 3x3 sans coins
	for (int dx = -1; dx <= 1; ++dx)
	{
		for (int dz = -1; dz <= 1; ++dz)
		{
			// Éviter les coins pour la forme en croix
			if (std::abs(dx) == 1 && std::abs(dz) == 1)
				continue;
			// Éviter le centre (position du tronc)
			if (dx == 0 && dz == 0)
				continue;

			int leafX = localX + dx;
			int leafZ = localZ + dz;
			int leafY = leavesTopY;

			if (leafX >= 0 && leafX < CHUNK_SIZE &&
				leafZ >= 0 && leafZ < CHUNK_SIZE &&
				leafY < CHUNK_HEIGHT)
			{
				int index = getVoxelIndex(leafX, leafY, leafZ);
				if (voxels[index].type == TextureType::AIR)
				{
					voxels[index].type = TextureType::OAK_LEAVES;
				}
			}
		}
	}

	// Couche 3 - carré 3x3 complet (1 bloc en dessous du sommet)
	for (int dx = -1; dx <= 1; ++dx)
	{
		for (int dz = -1; dz <= 1; ++dz)
		{
			// Éviter le centre (position du tronc)
			if (dx == 0 && dz == 0)
				continue;

			int leafX = localX + dx;
			int leafZ = localZ + dz;
			int leafY = leavesTopY - 1;

			if (leafX >= 0 && leafX < CHUNK_SIZE &&
				leafZ >= 0 && leafZ < CHUNK_SIZE &&
				leafY < CHUNK_HEIGHT && leafY > surfaceY)
			{
				int index = getVoxelIndex(leafX, leafY, leafZ);
				if (voxels[index].type == TextureType::AIR)
				{
					voxels[index].type = TextureType::OAK_LEAVES;
				}
			}
		}
	}

	// Couche 4 (base) - carré 5x5 sans coins (2 blocs en dessous du sommet)
	for (int dx = -2; dx <= 2; ++dx)
	{
		for (int dz = -2; dz <= 2; ++dz)
		{
			// Éviter les coins pour une forme plus ronde
			if (std::abs(dx) == 2 && std::abs(dz) == 2)
				continue;
			// Éviter le centre (position du tronc)
			if (dx == 0 && dz == 0)
				continue;

			int leafX = localX + dx;
			int leafZ = localZ + dz;
			int leafY = leavesTopY - 2;

			if (leafX >= 0 && leafX < CHUNK_SIZE &&
				leafZ >= 0 && leafZ < CHUNK_SIZE &&
				leafY < CHUNK_HEIGHT && leafY > surfaceY)
			{
				int index = getVoxelIndex(leafX, leafY, leafZ);
				if (voxels[index].type == TextureType::AIR)
				{
					voxels[index].type = TextureType::OAK_LEAVES;
				}
			}
		}
	}

	// Variation : pour les arbres plus grands, ajouter une couche supplémentaire
	if (treeHeight >= 6)
	{
		// Couche bonus - carré 5x5 sans coins (3 blocs en dessous du sommet)
		for (int dx = -2; dx <= 2; ++dx)
		{
			for (int dz = -2; dz <= 2; ++dz)
			{
				// Éviter les coins pour une forme plus ronde
				if (std::abs(dx) == 2 && std::abs(dz) == 2)
					continue;
				// Éviter le centre (position du tronc)
				if (dx == 0 && dz == 0)
					continue;
				// Seulement quelques blocs pour ne pas faire un arbre trop touffu
				if ((dx + dz) % 2 == 0)
					continue;

				int leafX = localX + dx;
				int leafZ = localZ + dz;
				int leafY = leavesTopY - 3;

				if (leafX >= 0 && leafX < CHUNK_SIZE &&
					leafZ >= 0 && leafZ < CHUNK_SIZE &&
					leafY < CHUNK_HEIGHT && leafY > surfaceY)
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

void TerrainGenerator::generateRocksAt(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int surfaceY, int worldX, int worldZ)
{
	std::vector<float> coords = {static_cast<float>(worldX), static_cast<float>(worldZ)};
	std::vector<float> rockResult(1);

	m_rockNoise->GenPositionArray2D(rockResult.data(), 1,
									coords.data(), coords.data() + 1,
									0.0f, 0.0f, m_seed + 7000);

	float rockValue = rockResult[0];

	// Generate different types of rock formations - much rarer
	if (rockValue > 0.85f) // Large boulder - très rare
	{
		for (int dx = -1; dx <= 1; ++dx)
		{
			for (int dz = -1; dz <= 1; ++dz)
			{
				int rockX = localX + dx;
				int rockZ = localZ + dz;
				if (rockX >= 0 && rockX < CHUNK_SIZE && rockZ >= 0 && rockZ < CHUNK_SIZE)
				{
					int height = 1 + (std::abs(dx) + std::abs(dz) < 2 ? 1 : 0);
					for (int dy = 1; dy <= height; ++dy)
					{
						int rockY = surfaceY + dy;
						if (rockY < CHUNK_HEIGHT)
						{
							int index = getVoxelIndex(rockX, rockY, rockZ);
							if (voxels[index].type == TextureType::AIR)
							{
								voxels[index].type = TextureType::COBBLESTONE;
							}
						}
					}
				}
			}
		}
	}
	else if (rockValue > 0.8f) // Small rock - rare
	{
		int rockY = surfaceY + 1;
		if (rockY < CHUNK_HEIGHT)
		{
			int index = getVoxelIndex(localX, rockY, localZ);
			if (voxels[index].type == TextureType::AIR)
			{
				voxels[index].type = TextureType::COBBLESTONE;
			}
		}
	}
	else if (rockValue > 0.75f) // Gravel patch - rare
	{
		int gravelY = surfaceY + 1;
		if (gravelY < CHUNK_HEIGHT)
		{
			int index = getVoxelIndex(localX, gravelY, localZ);
			if (voxels[index].type == TextureType::AIR)
			{
				voxels[index].type = TextureType::GRAVEL;
			}
		}
	}
}

void TerrainGenerator::generateVegetationAt(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int surfaceY, BiomeType biome)
{
	std::vector<float> coords = {static_cast<float>(localX * 4), static_cast<float>(localZ * 4)};
	std::vector<float> vegResult(1);

	m_vegetationNoise->GenPositionArray2D(vegResult.data(), 1,
										  coords.data(), coords.data() + 1,
										  0.0f, 0.0f, m_seed + 8000);

	float vegValue = (vegResult[0] + 1.0f) * 0.5f; // 0 to 1

	// Probabilité de buisson selon le biome
	float bushProbability = 0.0f;
	switch (biome)
	{
	case BiomeType::FOREST:
		bushProbability = 0.98f; // 2% chance en forêt
		break;
	case BiomeType::PLAINS:
		bushProbability = 0.985f; // 1.5% chance en plaines
		break;
	case BiomeType::MOUNTAINS:
		bushProbability = 0.995f; // 0.5% chance en montagne (très rare)
		break;
	case BiomeType::DESERT:
		bushProbability = 0.999f; // 0.1% chance en désert (quasi inexistant)
		break;
	case BiomeType::SNOWY:
		return; // Pas de buissons dans la neige
	default:
		bushProbability = 0.99f; // 1% chance par défaut
		break;
	}

	// Buissons plus rares et variés
	if (vegValue > bushProbability)
	{
		// Différents types de buissons selon la valeur de noise
		float bushVariation = (vegValue - bushProbability) / (1.0f - bushProbability); // 0 to 1

		if (bushVariation < 0.3f) // Petit buisson simple (30% des buissons)
		{
			int bushY = surfaceY + 1;
			if (bushY < CHUNK_HEIGHT)
			{
				int index = getVoxelIndex(localX, bushY, localZ);
				if (voxels[index].type == TextureType::AIR)
				{
					voxels[index].type = TextureType::OAK_LEAVES;
				}
			}
		}
		else if (bushVariation < 0.7f) // Buisson moyen en croix (40% des buissons)
		{
			// Forme en croix
			for (int dx = -1; dx <= 1; ++dx)
			{
				for (int dz = -1; dz <= 1; ++dz)
				{
					// Seulement croix (pas diagonales)
					if (std::abs(dx) + std::abs(dz) > 1)
						continue;

					int bushX = localX + dx;
					int bushZ = localZ + dz;

					if (bushX >= 0 && bushX < CHUNK_SIZE && bushZ >= 0 && bushZ < CHUNK_SIZE)
					{
						int bushHeight = (dx == 0 && dz == 0) ? 2 : 1; // Centre plus haut

						for (int dy = 1; dy <= bushHeight; ++dy)
						{
							int bushY = surfaceY + dy;
							if (bushY < CHUNK_HEIGHT)
							{
								int index = getVoxelIndex(bushX, bushY, bushZ);
								if (voxels[index].type == TextureType::AIR)
								{
									voxels[index].type = TextureType::OAK_LEAVES;
								}
							}
						}
					}
				}
			}
		}
		else // Grand buisson dense (30% des buissons)
		{
			// Buisson plus gros et dense
			for (int dx = -1; dx <= 1; ++dx)
			{
				for (int dz = -1; dz <= 1; ++dz)
				{
					int bushX = localX + dx;
					int bushZ = localZ + dz;

					if (bushX >= 0 && bushX < CHUNK_SIZE && bushZ >= 0 && bushZ < CHUNK_SIZE)
					{
						// Hauteur variable pour forme naturelle
						int bushHeight = 1;
						if (dx == 0 && dz == 0) // Centre
						{
							bushHeight = 3;
						}
						else if (std::abs(dx) + std::abs(dz) == 1) // Côtés directs
						{
							bushHeight = 2;
						}
						// Coins restent à 1

						for (int dy = 1; dy <= bushHeight; ++dy)
						{
							int bushY = surfaceY + dy;
							if (bushY < CHUNK_HEIGHT)
							{
								int index = getVoxelIndex(bushX, bushY, bushZ);
								if (voxels[index].type == TextureType::AIR)
								{
									voxels[index].type = TextureType::OAK_LEAVES;
								}
							}
						}
					}
				}
			}
		}
	}
}

void TerrainGenerator::generateOres(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX, int localZ, int worldX, int worldZ, int terrainHeight)
{
	for (int y = BEDROCK_LEVEL + 1; y < terrainHeight - 1; ++y)
	{
		int index = getVoxelIndex(localX, y, localZ);

		// Only replace stone blocks
		if (voxels[index].type != TextureType::STONE)
			continue;

		// Sample 3D ore noise
		std::vector<float> coords = {static_cast<float>(worldX), static_cast<float>(y), static_cast<float>(worldZ)};
		std::vector<float> oreResult(1);

		// Use different noise seeds for different ore types
		int oreType = (worldX + worldZ + y) % 8; // Simple way to get different ore types

		m_oreNoise->GenPositionArray3D(oreResult.data(), 1,
									   coords.data(), coords.data() + 1, coords.data() + 2,
									   0.0f, 0.0f, 0.0f, m_seed + 9000 + oreType * 1000);

		float oreValue = oreResult[0];

		// Different ore generation based on depth and noise value
		if (oreValue > 0.6f) // Rare ores
		{
			if (y < 20) // Deep ores
			{
				if (oreValue > 0.75f)
					voxels[index].type = TextureType::DIAMOND_ORE;
				else if (oreValue > 0.7f)
					voxels[index].type = TextureType::GOLD_ORE;
				else
					voxels[index].type = TextureType::REDSTONE_ORE;
			}
			else if (y < 40) // Mid-depth ores
			{
				if (oreValue > 0.7f)
					voxels[index].type = TextureType::IRON_ORE;
				else
					voxels[index].type = TextureType::COPPER_ORE;
			}
			else // Shallow ores
			{
				if (oreValue > 0.7f)
					voxels[index].type = TextureType::COAL_ORE;
				else
					voxels[index].type = TextureType::COPPER_ORE;
			}
		}
		else if (oreValue > 0.5f && y < 30) // Common deep ores
		{
			if ((worldX + worldZ + y) % 3 == 0)
				voxels[index].type = TextureType::COAL_ORE;
		}
	}
}

void TerrainGenerator::determineBiomesBatch(std::array<BiomeType, CHUNK_SIZE * CHUNK_SIZE> &biomeMap,
											const std::vector<float> &heightMap, int chunkX, int chunkZ)
{
	constexpr int totalPoints = CHUNK_SIZE * CHUNK_SIZE;

	// Prepare coordinate arrays
	std::vector<float> xCoords(totalPoints);
	std::vector<float> zCoords(totalPoints);

	for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ)
	{
		for (int localX = 0; localX < CHUNK_SIZE; ++localX)
		{
			int index = localZ * CHUNK_SIZE + localX;
			xCoords[index] = static_cast<float>(chunkX + localX);
			zCoords[index] = static_cast<float>(chunkZ + localZ);
		}
	}

	// Batch generate climate data
	std::vector<float> temperatureResults(totalPoints);
	std::vector<float> humidityResults(totalPoints);
	std::vector<float> continentalResults(totalPoints);

	m_temperatureNoise->GenPositionArray2D(temperatureResults.data(), totalPoints,
										   xCoords.data(), zCoords.data(),
										   0.0f, 0.0f, m_seed + 5000);

	m_humidityNoise->GenPositionArray2D(humidityResults.data(), totalPoints,
										xCoords.data(), zCoords.data(),
										0.0f, 0.0f, m_seed + 6000);

	m_continentalNoise->GenPositionArray2D(continentalResults.data(), totalPoints,
										   xCoords.data(), zCoords.data(),
										   0.0f, 0.0f, m_seed);

	// Determine biomes using batch results
	for (int i = 0; i < totalPoints; ++i)
	{
		float temperature = temperatureResults[i];
		float humidity = humidityResults[i];
		float continentalness = continentalResults[i];
		int height = static_cast<int>(heightMap[i]);

		biomeMap[i] = getBiomeFromFactors(temperature, humidity, continentalness, height);
	}
}

void TerrainGenerator::generateStructuresBatch(ChunkData &chunkData, int chunkX, int chunkZ)
{
	constexpr int totalPoints = CHUNK_SIZE * CHUNK_SIZE;

	// Prepare coordinate arrays for structure noise
	std::vector<float> xCoordsTree(totalPoints);
	std::vector<float> zCoordsTree(totalPoints);
	std::vector<float> xCoordsRock(totalPoints);
	std::vector<float> zCoordsRock(totalPoints);
	std::vector<float> xCoordsVeg(totalPoints);
	std::vector<float> zCoordsVeg(totalPoints);

	for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ)
	{
		for (int localX = 0; localX < CHUNK_SIZE; ++localX)
		{
			int index = localZ * CHUNK_SIZE + localX;
			int worldX = chunkX + localX;
			int worldZ = chunkZ + localZ;

			// Different scales for different structures
			xCoordsTree[index] = static_cast<float>(worldX * 0.05f);
			zCoordsTree[index] = static_cast<float>(worldZ * 0.05f);
			xCoordsRock[index] = static_cast<float>(worldX);
			zCoordsRock[index] = static_cast<float>(worldZ);
			xCoordsVeg[index] = static_cast<float>(localX * 4);
			zCoordsVeg[index] = static_cast<float>(localZ * 4);
		}
	}

	// Batch generate structure noise
	std::vector<float> treeResults(totalPoints);
	std::vector<float> rockResults(totalPoints);
	std::vector<float> vegResults(totalPoints);

	m_treeNoise->GenPositionArray2D(treeResults.data(), totalPoints,
									xCoordsTree.data(), zCoordsTree.data(),
									0.0f, 0.0f, m_seed + 4000);

	m_rockNoise->GenPositionArray2D(rockResults.data(), totalPoints,
									xCoordsRock.data(), zCoordsRock.data(),
									0.0f, 0.0f, m_seed + 7000);

	m_vegetationNoise->GenPositionArray2D(vegResults.data(), totalPoints,
										  xCoordsVeg.data(), zCoordsVeg.data(),
										  0.0f, 0.0f, m_seed + 8000);

	// Process results and place structures
	for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ)
	{
		for (int localX = 0; localX < CHUNK_SIZE; ++localX)
		{
			int index = localZ * CHUNK_SIZE + localX;
			int worldX = chunkX + localX;
			int worldZ = chunkZ + localZ;
			int terrainHeight = generateHeightAt(worldX, worldZ);
			BiomeType biome = chunkData.biomeMap[index];

			if (terrainHeight > SEA_LEVEL)
			{
				// Tree generation with batch results
				float treeChance = (treeResults[index] + 1.0f) * 0.5f;

				// Espacement minimum entre arbres
				bool canPlaceTree = true;
				int minSpacing = 8;
				if ((worldX % minSpacing < 3) || (worldZ % minSpacing < 3))
				{
					canPlaceTree = false;
				}

				// Variation supplémentaire
				float coordinateVariation = std::sin(worldX * 0.13f) * std::cos(worldZ * 0.19f);
				treeChance = (treeChance + coordinateVariation * 0.2f + 1.0f) * 0.5f;
				treeChance = std::clamp(treeChance, 0.0f, 1.0f);

				float treeDensity = getBiomeTreeDensity(biome);

				if (treeChance > (1.0f - treeDensity) && canPlaceTree)
				{
					// Hauteur d'arbre avec variation
					int treeHeight = 4;
					int heightSeed = (worldX * 1664525 + worldZ * 1013904223 + m_seed) & 0x7FFFFFFF;
					float heightRandom = (heightSeed % 1000) / 1000.0f;

					if (heightRandom > 0.7f)
						treeHeight = 5;
					if (heightRandom > 0.9f)
						treeHeight = 6;
					if (heightRandom > 0.98f)
						treeHeight = 7;

					generateTreeAt(chunkData.voxels, localX, localZ, terrainHeight, treeHeight);
				}

				// Rock generation avec batch results
				float rockValue = rockResults[index];
				if (rockValue > 0.85f)
				{
					// Large boulder
					for (int dx = -1; dx <= 1; ++dx)
					{
						for (int dz = -1; dz <= 1; ++dz)
						{
							int rockX = localX + dx;
							int rockZ = localZ + dz;
							if (rockX >= 0 && rockX < CHUNK_SIZE && rockZ >= 0 && rockZ < CHUNK_SIZE)
							{
								int height = 1 + (std::abs(dx) + std::abs(dz) < 2 ? 1 : 0);
								for (int dy = 1; dy <= height; ++dy)
								{
									int rockY = terrainHeight + dy;
									if (rockY < CHUNK_HEIGHT)
									{
										int voxelIndex = getVoxelIndex(rockX, rockY, rockZ);
										if (chunkData.voxels[voxelIndex].type == TextureType::AIR)
										{
											chunkData.voxels[voxelIndex].type = TextureType::COBBLESTONE;
										}
									}
								}
							}
						}
					}
				}
				else if (rockValue > 0.8f)
				{
					// Small rock
					int rockY = terrainHeight + 1;
					if (rockY < CHUNK_HEIGHT)
					{
						int voxelIndex = getVoxelIndex(localX, rockY, localZ);
						if (chunkData.voxels[voxelIndex].type == TextureType::AIR)
						{
							chunkData.voxels[voxelIndex].type = TextureType::COBBLESTONE;
						}
					}
				}
				else if (rockValue > 0.75f)
				{
					// Gravel patch
					int gravelY = terrainHeight + 1;
					if (gravelY < CHUNK_HEIGHT)
					{
						int voxelIndex = getVoxelIndex(localX, gravelY, localZ);
						if (chunkData.voxels[voxelIndex].type == TextureType::AIR)
						{
							chunkData.voxels[voxelIndex].type = TextureType::GRAVEL;
						}
					}
				}

				// Vegetation generation avec batch results
				if (terrainHeight <= SEA_LEVEL + 60)
				{
					float vegValue = (vegResults[index] + 1.0f) * 0.5f;
					float bushProbability = getBiomeVegetationDensity(biome);

					if (vegValue > bushProbability)
					{
						generateVegetationAt(chunkData.voxels, localX, localZ, terrainHeight, biome);
					}
				}
			}

			// Generate ores
			generateOres(chunkData.voxels, localX, localZ, worldX, worldZ, terrainHeight);
		}
	}
}

BiomeType TerrainGenerator::getBiomeFromFactors(float temperature, float humidity, float continentalness, int height)
{
	// Biomes de montagne basés sur l'altitude
	if (height > SEA_LEVEL + 60)
	{
		if (height > SEA_LEVEL + 90 || temperature < -0.4f)
		{
			return BiomeType::SNOWY; // Hautes montagnes enneigées ou zones très froides
		}
		else
		{
			return BiomeType::MOUNTAINS; // Montagnes tempérées
		}
	}

	// Biomes basés sur température et humidité
	if (temperature > 0.5f && humidity < -0.4f) // Très chaud et sec
	{
		return BiomeType::DESERT;
	}
	else if (temperature > -0.2f && humidity > 0.1f) // Tempéré et humide
	{
		return BiomeType::FOREST;
	}
	else if (temperature < -0.3f) // Très froid
	{
		return BiomeType::SNOWY;
	}
	else // Conditions moyennes
	{
		return BiomeType::PLAINS;
	}
}

TextureType TerrainGenerator::getBiomeSurfaceBlock(BiomeType biome, int height)
{
	switch (biome)
	{
	case BiomeType::DESERT:
		return TextureType::SAND;
	case BiomeType::SNOWY:
		return TextureType::SNOW;
	case BiomeType::MOUNTAINS:
		return height > SEA_LEVEL + 80 ? TextureType::STONE : TextureType::GRASS_TOP;
	case BiomeType::FOREST:
	case BiomeType::PLAINS:
	default:
		return height <= SEA_LEVEL + 1 ? TextureType::SAND : TextureType::GRASS_TOP;
	}
}

float TerrainGenerator::getBiomeTreeDensity(BiomeType biome)
{
	switch (biome)
	{
	case BiomeType::FOREST:
		return 0.05f; // 5% - Forêts denses
	case BiomeType::PLAINS:
		return 0.008f; // 0.8% - Très rares
	case BiomeType::MOUNTAINS:
		return 0.015f; // 1.5% - Rares en montagne
	case BiomeType::DESERT:
	case BiomeType::SNOWY:
	default:
		return 0.001f; // Quasi inexistants
	}
}

float TerrainGenerator::getBiomeVegetationDensity(BiomeType biome)
{
	switch (biome)
	{
	case BiomeType::FOREST:
		return 0.97f; // 3% chance de buissons
	case BiomeType::PLAINS:
		return 0.98f; // 2% chance
	case BiomeType::MOUNTAINS:
		return 0.995f; // 0.5% chance - Très rare
	case BiomeType::DESERT:
	case BiomeType::SNOWY:
	default:
		return 1.0f; // Pas de végétation
	}
}

void TerrainGenerator::generateChunkBorders(ChunkData &chunkData, int chunkX, int chunkZ)
{
	// Generate voxels for chunk borders to optimize mesh generation
	// This includes voxels from adjacent chunks that are needed for face culling

	// Define the border ranges (1 voxel outside chunk boundaries)
	const int borderSize = 1;
	const int extendedSize = CHUNK_SIZE + 2 * borderSize;

	// Generate border voxels for all 6 directions (North, South, East, West, Top, Bottom)

	// North border (Z = -1)
	for (int x = -borderSize; x < CHUNK_SIZE + borderSize; ++x)
	{
		for (int y = 0; y < CHUNK_HEIGHT; ++y)
		{
			int worldX = chunkX + x;
			int worldZ = chunkZ - borderSize;

			// Generate height and determine voxel type
			int height = generateHeightAt(worldX, worldZ);
			TextureType voxelType = getVoxelTypeAt(worldX, y, worldZ, height);

			if (voxelType != TextureType::AIR)
			{
				glm::ivec3 borderPos(x, y, -borderSize);
				chunkData.borderVoxels[borderPos] = voxelType;
			}
		}
	}

	// South border (Z = CHUNK_SIZE)
	for (int x = -borderSize; x < CHUNK_SIZE + borderSize; ++x)
	{
		for (int y = 0; y < CHUNK_HEIGHT; ++y)
		{
			int worldX = chunkX + x;
			int worldZ = chunkZ + CHUNK_SIZE;

			int height = generateHeightAt(worldX, worldZ);
			TextureType voxelType = getVoxelTypeAt(worldX, y, worldZ, height);

			if (voxelType != TextureType::AIR)
			{
				glm::ivec3 borderPos(x, y, CHUNK_SIZE);
				chunkData.borderVoxels[borderPos] = voxelType;
			}
		}
	}

	// West border (X = -1)
	for (int z = 0; z < CHUNK_SIZE; ++z)
	{
		for (int y = 0; y < CHUNK_HEIGHT; ++y)
		{
			int worldX = chunkX - borderSize;
			int worldZ = chunkZ + z;

			int height = generateHeightAt(worldX, worldZ);
			TextureType voxelType = getVoxelTypeAt(worldX, y, worldZ, height);

			if (voxelType != TextureType::AIR)
			{
				glm::ivec3 borderPos(-borderSize, y, z);
				chunkData.borderVoxels[borderPos] = voxelType;
			}
		}
	}

	// East border (X = CHUNK_SIZE)
	for (int z = 0; z < CHUNK_SIZE; ++z)
	{
		for (int y = 0; y < CHUNK_HEIGHT; ++y)
		{
			int worldX = chunkX + CHUNK_SIZE;
			int worldZ = chunkZ + z;

			int height = generateHeightAt(worldX, worldZ);
			TextureType voxelType = getVoxelTypeAt(worldX, y, worldZ, height);

			if (voxelType != TextureType::AIR)
			{
				glm::ivec3 borderPos(CHUNK_SIZE, y, z);
				chunkData.borderVoxels[borderPos] = voxelType;
			}
		}
	}

	// Top border (Y = CHUNK_HEIGHT) - Always air, but included for completeness
	// Bottom border (Y = -1) - Always bedrock below level 0, but included for completeness

	// Note: We don't generate top/bottom borders as they are typically not needed
	// for face culling in most voxel engines, but they can be added if required
}

TextureType TerrainGenerator::getVoxelTypeAt(int worldX, int worldY, int worldZ, int terrainHeight)
{
	// Similar logic to generateColumn but for a single voxel
	if (worldY <= TerrainGenerator::BEDROCK_LEVEL)
	{
		return TextureType::BEDROCK;
	}
	else if (worldY <= terrainHeight)
	{
		int depthFromSurface = terrainHeight - worldY;

		if (depthFromSurface == 0)
		{
			// Surface block - basic logic (can be enhanced with biome data)
			if (terrainHeight <= TerrainGenerator::SEA_LEVEL + 1)
			{
				return TextureType::SAND;
			}
			else
			{
				return TextureType::GRASS_TOP;
			}
		}
		else if (depthFromSurface <= 3)
		{
			// Subsurface blocks
			if (terrainHeight <= TerrainGenerator::SEA_LEVEL + 2)
			{
				return TextureType::SAND;
			}
			else
			{
				return TextureType::DIRT;
			}
		}
		else
		{
			// Deep blocks
			return TextureType::STONE;
		}
	}
	else if (worldY <= TerrainGenerator::SEA_LEVEL)
	{
		// Water
		return TextureType::WATER;
	}

	return TextureType::AIR;
}
