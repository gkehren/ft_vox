#pragma once

#include "../PerlinNoise/PerlinNoise.hpp"
#include <memory>

class NoiseGenerator
{
private:
	uint32_t seed;

public:
	std::unique_ptr<siv::PerlinNoise> perlin;
	NoiseGenerator(uint32_t seed) : seed(seed)
	{
		perlin = std::make_unique<siv::PerlinNoise>(seed);
	}

	// Basic noise methods
	float noise2D(float x, float z, float scale) const
	{
		return perlin->noise2D_01(x / scale, z / scale);
	}

	float noise3D(float x, float y, float z, float scale) const
	{
		return perlin->noise3D_01(x / scale, y / scale, z / scale);
	}

	// Specialized noise methods
	float biomeNoise(float x, float z) const
	{
		return noise2D(x, z, 250.0f);
	}

	float mountainNoise(float x, float z) const
	{
		// Large scale base variation
		float base = noise2D(x, z, 320.0f);

		float detail = perlin->octave2D_01(x / 120.0f, z / 120.0f, 3, 0.7) * 0.4f;

		float combined = std::clamp(base + detail, 0.0f, 1.0f);
		return std::pow(combined, 1.3f); // Exponential to enhance peaks
	}

	// Valley noise - creates erosion patterns
	float valleyNoise(float x, float z) const
	{
		return perlin->octave2D_01(x / 120.0f, z / 120.0f, 4, 0.5);
	}

	float terrainBaseNoise(float x, float z) const
	{
		float continentNoise = noise2D(x, z, 400.0f);
		float mediumNoise = perlin->octave2D_01(x / 120.0f, z / 120.0f, 4, 0.5) * 0.3f;
		float detailNoise = perlin->octave2D_01(x / 40.0f, z / 40.0f, 3, 0.6) * 0.1f;
		return (continentNoise * 0.6f + mediumNoise + detailNoise);
	}

	float caveNoise(float x, float y, float z) const
	{
		// Vertical stretching - makes caves more horizontal
		float stretchedY = y / 1.8f;

		// Main cave noise
		float base = noise3D(x, stretchedY, z, 42.0f);

		// Add details for interesting cave features
		float detail = noise3D(x, stretchedY, z, 15.0f) * 0.3f;

		return base + detail;
	}

	float temperatureNoise(int x, int z) const
	{
		// Create a base temperature that changes gradually across the world
		float base = noise2D(x, z, 600.0f);

		// Add medium-scale variations
		float medium = noise2D(x, z, 250.0f) * 0.25f;

		// Add some smaller details for more interesting patterns
		float detail = noise2D(x, z, 80.0f) * 0.12f;

		return std::clamp(base + medium + detail, 0.0f, 1.0f);
	}

	float humidityNoise(float x, float z) const
	{
		// Offset coordinates to make humidity different from temperature
		x += 1000.0f;
		z -= 1000.0f;

		// Create a base humidity that changes gradually
		float base = noise2D(x, z, 500.0f);

		// Add medium-scale variations
		float medium = noise2D(x, z, 180.0f) * 0.3f;

		// Add smaller details
		float detail = noise2D(x, z, 60.0f) * 0.15f;

		return std::clamp(base + medium + detail, 0.0f, 1.0f);
	}

	float biomeBoundaryNoise(float x, float z) const
	{
		// Large scale noise for smooth biome transitions
		float largeScale = noise2D(x, z, 120.0f) * 0.5f;
		// Medium scale noise for varied boundary shapes
		float mediumScale = noise2D(x, z, 60.0f) * 0.3f;
		return largeScale + mediumScale;
	}
};