#include "BiomeManager.hpp"
#include "NoiseGenerator.hpp"

BiomeManager::BiomeManager()
{
	biomes.clear();
	biomeIndices.clear();

	// Register default biomes

	// Plains - the most common, flat biome
	BiomeProperties plains;
	plains.name = "Plains";
	plains.surfaceBlock = TEXTURE_GRASS;
	plains.subsurfaceBlock = TEXTURE_DIRT;
	plains.underwaterSurfaceBlock = TEXTURE_DIRT;
	plains.baseHeight = 68.0f;
	plains.heightVariation = 5.0f;
	plains.subsurfaceDepth = 3;
	plains.temperature = 0.5f;
	plains.humidity = 0.5f;
	registerBiome(std::make_unique<Biome>(plains));

	// Desert - sand dunes
	BiomeProperties desert;
	desert.name = "Desert";
	desert.surfaceBlock = TEXTURE_SAND;
	desert.subsurfaceBlock = TEXTURE_SAND;
	desert.underwaterSurfaceBlock = TEXTURE_SAND;
	desert.baseHeight = 70.0f;
	desert.heightVariation = 12.0f;
	desert.subsurfaceDepth = 4;
	desert.temperature = 0.85f;
	desert.humidity = 0.1f;
	registerBiome(std::make_unique<Biome>(desert));

	// Mountains - dramatic peaks
	BiomeProperties mountains;
	mountains.name = "Mountains";
	mountains.surfaceBlock = TEXTURE_STONE;
	mountains.subsurfaceBlock = TEXTURE_STONE;
	mountains.underwaterSurfaceBlock = TEXTURE_STONE;
	mountains.baseHeight = 100.0f;
	mountains.heightVariation = 100.0f;
	mountains.subsurfaceDepth = 1;
	mountains.temperature = 0.4f;
	mountains.humidity = 0.5f;
	registerBiome(std::make_unique<Biome>(mountains));

	// Snowy Mountains - higher and colder
	BiomeProperties snowyCaps;
	snowyCaps.name = "SnowyCaps";
	snowyCaps.surfaceBlock = TEXTURE_SNOW;
	snowyCaps.subsurfaceBlock = TEXTURE_DIRT;
	snowyCaps.underwaterSurfaceBlock = TEXTURE_DIRT;
	snowyCaps.baseHeight = 120.0f;
	snowyCaps.heightVariation = 80.0f;
	snowyCaps.subsurfaceDepth = 3;
	snowyCaps.temperature = 0.25f;
	snowyCaps.humidity = 0.55f;
	registerBiome(std::make_unique<Biome>(snowyCaps));

	// Swamp - flat and wet
	// BiomeProperties swamp;
	// swamp.name = "Swamp";
	// swamp.surfaceBlock = TEXTURE_DIRT;
	// swamp.subsurfaceBlock = TEXTURE_DIRT;
	// swamp.underwaterSurfaceBlock = TEXTURE_DIRT;
	// swamp.baseHeight = 62.0f;
	// swamp.heightVariation = 3.0f;
	// swamp.subsurfaceDepth = 5;
	// swamp.temperature = 0.7f;
	// swamp.humidity = 0.9f;
	// registerBiome(std::make_unique<Biome>(swamp));

	// Hills - gentler version of mountains
	BiomeProperties hills;
	hills.name = "Hills";
	hills.surfaceBlock = TEXTURE_GRASS;
	hills.subsurfaceBlock = TEXTURE_DIRT;
	hills.underwaterSurfaceBlock = TEXTURE_DIRT;
	hills.baseHeight = 78.0f;
	hills.heightVariation = 30.0f;
	hills.subsurfaceDepth = 3;
	hills.temperature = 0.45f;
	hills.humidity = 0.65f;
	registerBiome(std::make_unique<Biome>(hills));

	// Valley
	BiomeProperties valley;
	valley.name = "Valley";
	valley.surfaceBlock = TEXTURE_GRASS;
	valley.subsurfaceBlock = TEXTURE_DIRT;
	valley.underwaterSurfaceBlock = TEXTURE_DIRT;
	valley.baseHeight = 55.0f;
	valley.heightVariation = 35.0f;
	valley.subsurfaceDepth = 3;
	valley.temperature = 0.55f;
	valley.humidity = 0.45f;
	registerBiome(std::make_unique<Biome>(valley));

	// Add more biomes as needed
}

void BiomeManager::registerBiome(std::unique_ptr<Biome> biome)
{
	biomeIndices[biome->getName()] = biomes.size();
	biomes.push_back(std::move(biome));
}

const Biome *BiomeManager::getBiomeAt(int x, int z, NoiseGenerator &noise) const
{
	// Generate climate values
	float temperature = noise.temperatureNoise(x, z);
	float humidity = noise.humidityNoise(x, z);

	// Extra noise for biome distribution adjustment
	float specialBiomeNoise = noise.noise2D(x, z, 400.0f);

	// Occasionally force a mountain biome (10% chance)
	if (specialBiomeNoise > 0.9f)
	{
		// Find a mountain biome
		for (const auto &biome : biomes)
		{
			if (biome->getName() == "Mountains")
			{
				return biome.get();
			}
		}
	}

	// Occasionally force a snowy mountain (8% chance)
	if (specialBiomeNoise < 0.08f)
	{
		// Find a snowy biome
		for (const auto &biome : biomes)
		{
			if (biome->getName() == "SnowyCaps")
			{
				return biome.get();
			}
		}
	}

	// Standard biome selection based on climate
	const Biome *selectedBiome = nullptr;
	float minDistance = std::numeric_limits<float>::max();

	for (const auto &biome : biomes)
	{
		const BiomeProperties &props = biome->getProperties();
		float dx = temperature - props.temperature;
		float dy = humidity - props.humidity;
		float distance = dx * dx + dy * dy;

		if (distance < minDistance)
		{
			minDistance = distance;
			selectedBiome = biome.get();
		}
	}

	return selectedBiome;
}

std::vector<BiomeInfluence> BiomeManager::getBiomeInfluences(int x, int z, NoiseGenerator &noise) const
{
	// Generate climate values
	float temperature = noise.temperatureNoise(x, z);
	float humidity = noise.humidityNoise(x, z);

	// Add some variation to create more natural boundaries
	float boundaryNoise = noise.biomeBoundaryNoise(x, z);
	temperature += boundaryNoise * 0.1f;
	humidity += boundaryNoise * 0.1f;

	// Clamp values to valid range
	temperature = std::clamp(temperature, 0.0f, 1.0f);
	humidity = std::clamp(humidity, 0.0f, 1.0f);

	const int MAX_BIOMES_TO_BLEND = 4;

	std::vector<BiomeInfluence> influences;
	std::vector<std::pair<float, const Biome *>> distanceToBiome;

	// Calculate distance to each biome in climate space
	for (const auto &biome : biomes)
	{
		const BiomeProperties &props = biome->getProperties();
		float dx = temperature - props.temperature;
		float dy = humidity - props.humidity;
		float distance = dx * dx + dy * dy;

		distanceToBiome.push_back({distance, biome.get()});
	}

	// Sort by distance
	std::sort(distanceToBiome.begin(), distanceToBiome.end());

	int biomesToConsider = std::min(MAX_BIOMES_TO_BLEND, static_cast<int>(distanceToBiome.size()));
	float totalWeight = 0.0f;

	const float EXPONENT = 1.5f; // Decreased for smoother blending

	for (int i = 0; i < biomesToConsider; i++)
	{
		float distance = distanceToBiome[i].first;
		const Biome *biome = distanceToBiome[i].second;

		float weight = 1.0f / std::pow(distance + 0.001f, EXPONENT);

		// Stronger falloff
		float falloffFactor = std::max(0.0f, 1.0f - (i / (float)(MAX_BIOMES_TO_BLEND - 1)));
		falloffFactor = falloffFactor * falloffFactor * falloffFactor; // Cubic for even stronger falloff
		weight *= falloffFactor;

		influences.push_back({biome, weight});
		totalWeight += weight;
	}

	// Normalize weights
	for (auto &influence : influences)
	{
		influence.weight /= totalWeight;
	}

	return influences;
}