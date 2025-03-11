#pragma once

#include "../PerlinNoise/PerlinNoise.hpp"

class NoiseGenerator
{
private:
	std::unique_ptr<siv::PerlinNoise> perlin;
	uint32_t seed;

public:
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
		return noise2D(x, z, 500.0f);
	}

	float terrainBaseNoise(float x, float z) const
	{
		return noise2D(x, z, 200.0f) * 1.5f +
			   noise2D(x, z, 50.0f) * 0.5f +
			   noise2D(x, z, 25.0f) * 0.2f;
	}

	float caveNoise(float x, float y, float z) const
	{
		return noise3D(x, y, z, 24.0f); // Adjust scale for better caves
	}

	float temperatureNoise(int x, int z) const
	{
		// Create a base temperature that changes gradually across the world
		float base = noise2D(x, z, 1200.0f);

		// Add medium-scale variations
		float medium = noise2D(x, z, 400.0f) * 0.15f;

		// Add some smaller details for more interesting patterns
		float detail = noise2D(x, z, 150.0f) * 0.07f;

		return std::clamp(base + medium + detail, 0.0f, 1.0f);
	}

	float humidityNoise(float x, float z) const
	{
		// Offset coordinates to make humidity different from temperature
		x += 1000.0f;
		z -= 1000.0f;

		// Create a base humidity that changes gradually
		float base = noise2D(x, z, 1000.0f);

		// Add medium-scale variations
		float medium = noise2D(x, z, 350.0f) * 0.2f;

		// Add smaller details
		float detail = noise2D(x, z, 120.0f) * 0.08f;

		return std::clamp(base + medium + detail, 0.0f, 1.0f);
	}
};