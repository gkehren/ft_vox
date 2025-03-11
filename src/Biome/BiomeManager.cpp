#include "BiomeManager.hpp"
#include "NoiseGenerator.hpp"

BiomeManager::BiomeManager()
{
	// Register default biomes
	BiomeProperties plains;
	plains.name = "Plains";
	plains.surfaceBlock = TEXTURE_GRASS;
	plains.subsurfaceBlock = TEXTURE_DIRT;
	plains.underwaterSurfaceBlock = TEXTURE_DIRT;
	plains.baseHeight = 64.0f;
	plains.heightVariation = 4.0f;
	plains.temperature = 0.5f;
	plains.humidity = 0.7f;
	registerBiome(std::make_unique<Biome>(plains));

	BiomeProperties desert;
	desert.name = "Desert";
	desert.surfaceBlock = TEXTURE_SAND;
	desert.subsurfaceBlock = TEXTURE_SAND;
	desert.underwaterSurfaceBlock = TEXTURE_SAND;
	desert.baseHeight = 62.0f;
	desert.heightVariation = 6.0f;
	desert.temperature = 0.9f;
	desert.humidity = 0.1f;
	registerBiome(std::make_unique<Biome>(desert));

	BiomeProperties mountains;
	mountains.name = "Mountains";
	mountains.surfaceBlock = TEXTURE_STONE;
	mountains.subsurfaceBlock = TEXTURE_STONE;
	mountains.underwaterSurfaceBlock = TEXTURE_STONE;
	mountains.baseHeight = 75.0f;
	mountains.heightVariation = 40.0f;
	mountains.temperature = 0.3f;
	mountains.humidity = 0.4f;
	registerBiome(std::make_unique<Biome>(mountains));

	BiomeProperties snowyCaps;
	snowyCaps.name = "SnowyCaps";
	snowyCaps.surfaceBlock = TEXTURE_SNOW;
	snowyCaps.subsurfaceBlock = TEXTURE_DIRT;
	snowyCaps.underwaterSurfaceBlock = TEXTURE_DIRT;
	snowyCaps.baseHeight = 70.0f;
	snowyCaps.heightVariation = 12.0f;
	snowyCaps.temperature = 0.1f;
	snowyCaps.humidity = 0.6f;
	registerBiome(std::make_unique<Biome>(snowyCaps));

	BiomeProperties swamp;
	swamp.name = "Swamp";
	swamp.surfaceBlock = TEXTURE_DIRT;
	swamp.subsurfaceBlock = TEXTURE_DIRT;
	swamp.underwaterSurfaceBlock = TEXTURE_DIRT;
	swamp.baseHeight = 59.0f;
	swamp.heightVariation = 1.0f;
	swamp.temperature = 0.6f;
	swamp.humidity = 0.9f;
	registerBiome(std::make_unique<Biome>(swamp));

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

	// Find the closest biome in climate space
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

	// Add some variation to temperature/humidity to create more natural boundaries
	// Higher frequency noise creates sharper boundaries
	float boundaryNoise = noise.noise2D(x, z, 80.0f) * 0.06f;
	temperature += boundaryNoise;
	humidity += boundaryNoise * 0.7f;

	// Clamp values to valid range
	temperature = std::clamp(temperature, 0.0f, 1.0f);
	humidity = std::clamp(humidity, 0.0f, 1.0f);

	// Number of biomes to blend - fewer biomes for clearer boundaries
	const int MAX_BIOMES_TO_BLEND = 3;

	// Store biomes and their influence weights
	std::vector<BiomeInfluence> influences;
	std::vector<std::pair<float, const Biome *>> distanceToBiome;

	// Calculate distance to each biome in climate space
	for (const auto &biome : biomes)
	{
		const BiomeProperties &props = biome->getProperties();
		float dx = temperature - props.temperature;
		float dy = humidity - props.humidity;
		float distance = dx * dx + dy * dy; // Squared distance in climate space

		distanceToBiome.push_back({distance, biome.get()});
	}

	// Sort by distance (closest biomes first)
	std::sort(distanceToBiome.begin(), distanceToBiome.end());

	// Take only the closest few biomes
	int biomesToConsider = std::min(MAX_BIOMES_TO_BLEND, static_cast<int>(distanceToBiome.size()));
	float totalWeight = 0.0f;

	// Use exponential weighting for stronger dominance of the primary biome
	// Higher power creates sharper visual transitions while still allowing height blending
	const float EXPONENT = 4.0f; // Increase for sharper transitions

	for (int i = 0; i < biomesToConsider; i++)
	{
		float distance = distanceToBiome[i].first;
		const Biome *biome = distanceToBiome[i].second;

		// Enhanced weighting formula with higher exponent for sharper transitions
		float weight = 1.0f / std::pow(distance + 0.001f, EXPONENT);

		// Stronger falloff for distant biomes
		float falloffFactor = std::max(0.0f, 1.0f - (i / (float)(MAX_BIOMES_TO_BLEND - 1)));
		falloffFactor = falloffFactor * falloffFactor; // Square for stronger falloff
		weight *= falloffFactor;

		influences.push_back({biome, weight});
		totalWeight += weight;
	}

	// Normalize weights so they sum to 1.0
	for (auto &influence : influences)
	{
		influence.weight /= totalWeight;
	}

	return influences;
}