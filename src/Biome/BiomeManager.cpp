#include "BiomeManager.hpp"
#include <algorithm>
#include <cmath>

BiomeManager::BiomeManager()
{
	// Initialize desert parameters
	biomeParams[BIOME_DESERT] = {
		/* baseHeight */ 64.0f,
		/* heightVariation */ 15.0f,
		/* surfaceBlock */ SAND,
		/* subSurfaceBlock */ SAND,
		/* subSurfaceDepth */ 3,
		/* noiseScale */ 160.0f,
		/* octaves */ 3,
		/* persistence */ 0.4f,
		/* waterColor */ {0.5f, 0.5f, 0.2f},
		// Mountain params not used for desert
	};

	// Initialize forest parameters
	biomeParams[BIOME_FOREST] = {
		/* baseHeight */ 64.0f,
		/* heightVariation */ 40.0f,
		/* surfaceBlock */ GRASS_TOP,
		/* subSurfaceBlock */ DIRT,
		/* subSurfaceDepth */ 3,
		/* noiseScale */ 128.0f,
		/* octaves */ 3,
		/* persistence */ 0.5f,
		/* waterColor */ {0.2f, 0.4f, 0.8f},
		// Mountain params not used for forest
	};

	// Initialize plain parameters
	biomeParams[BIOME_PLAIN] = {
		/* baseHeight */ 64.0f,
		/* heightVariation */ 80.0f,
		/* surfaceBlock */ GRASS_TOP,
		/* subSurfaceBlock */ DIRT,
		/* subSurfaceDepth */ 3,
		/* noiseScale */ 128.0f,
		/* octaves */ 3,
		/* persistence */ 0.5f,
		/* waterColor */ {0.1f, 0.3f, 0.7f},
		// Mountain params not used for plains
	};

	// Initialize mountain parameters
	biomeParams[BIOME_MOUNTAIN] = {
		/* baseHeight */ 64.0f,
		/* heightVariation */ 80.0f,
		/* surfaceBlock */ STONE,
		/* subSurfaceBlock */ STONE,
		/* subSurfaceDepth */ 3,
		/* noiseScale */ 128.0f,
		/* octaves */ 4,
		/* persistence */ 0.5f,
		/* waterColor */ {0.05f, 0.1f, 0.5f},
		/* mountainNoiseScale */ 128.0f,
		/* mountainOctaves */ 4,
		/* mountainPersistence */ 0.5f,
		/* mountainDetailScale */ 40.0f, // Increased from 24.0f for smoother detail
		/* mountainDetailInfluence */ 0.2f,
		/* peakNoiseScale */ 250.0f, // Increased from 180.0f for fewer peaks
		/* peakThreshold */ 0.78f,	 // Increased to make peaks more rare
		/* peakMultiplier */ 200.0f	 // Higher multiplier for more dramatic peaks
	};
}

BiomeType BiomeManager::getBiomeTypeAt(int worldX, int worldY, siv::PerlinNoise *noise) const
{
	// Utilisation de plusieurs couches de bruit pour une meilleure distribution des biomes
	float temperature = noise->octave2D_01(
		static_cast<float>(worldX) / 1000.0f,  // Échelle plus grande pour les zones climatiques
		static_cast<float>(worldY) / 1000.0f,
		4,
		0.5f
	);

	float humidity = noise->octave2D_01(
		static_cast<float>(worldX) / 800.0f,   // Échelle différente pour éviter la corrélation
		static_cast<float>(worldY) / 800.0f,
		4,
		0.5f
	);

	// Détermination du biome basée sur la température et l'humidité
	if (temperature < 0.3f) {
		// Zones froides
		if (humidity < 0.3f) return BIOME_DESERT;  // Désert froid
		else return BIOME_MOUNTAIN;               // Montagnes enneigées
	} else if (temperature < 0.6f) {
		// Zones tempérées
		if (humidity < 0.3f) return BIOME_PLAIN;  // Plaines
		else return BIOME_FOREST;                 // Forêt
	} else {
		// Zones chaudes
		if (humidity < 0.3f) return BIOME_DESERT; // Désert chaud
		else return BIOME_FOREST;                 // Forêt tropicale
	}
}

const BiomeParameters &BiomeManager::getBiomeParameters(BiomeType type) const
{
	return biomeParams.at(type);
}

float BiomeManager::getTerrainHeightAt(int worldX, int worldZ, BiomeType biomeType, siv::PerlinNoise *noise)
{
	const BiomeParameters &params = biomeParams.at(biomeType);

	// Bruit de base pour la structure générale du terrain
	float baseNoise = noise->octave2D_01(
		static_cast<float>(worldX) / 200.0f,
		static_cast<float>(worldZ) / 200.0f,
		4,
		0.5f
	);

	// Bruit de détail pour les petites variations
	float detailNoise = noise->octave2D_01(
		static_cast<float>(worldX) / 50.0f,
		static_cast<float>(worldZ) / 50.0f,
		2,
		0.5f
	);

	// Bruit de rugosité pour les variations locales
	float roughnessNoise = noise->octave2D_01(
		static_cast<float>(worldX) / 25.0f,
		static_cast<float>(worldZ) / 25.0f,
		2,
		0.5f
	);

	// Combinaison des bruits avec des poids différents selon le biome
	float combinedNoise;
	switch (biomeType) {
		case BIOME_DESERT:
			// Terrain plus plat avec quelques dunes
			combinedNoise = baseNoise * 0.7f + detailNoise * 0.2f + roughnessNoise * 0.1f;
			break;
		case BIOME_FOREST:
			// Terrain varié avec des collines
			combinedNoise = baseNoise * 0.6f + detailNoise * 0.3f + roughnessNoise * 0.1f;
			break;
		case BIOME_PLAIN:
			// Terrain relativement plat avec de légères variations
			combinedNoise = baseNoise * 0.8f + detailNoise * 0.15f + roughnessNoise * 0.05f;
			break;
		case BIOME_MOUNTAIN:
		{
			// Terrain très accidenté avec des pics
			combinedNoise = baseNoise * 0.5f + detailNoise * 0.3f + roughnessNoise * 0.2f;
			// Ajout de pics montagneux
			float mountainNoise = noise->octave2D_01(
				static_cast<float>(worldX) / 300.0f,
				static_cast<float>(worldZ) / 300.0f,
				3,
				0.6f
			);
			if (mountainNoise > 0.7f) {
				combinedNoise += (mountainNoise - 0.7f) * 2.0f;
			}
			break;
		}
		default:
			combinedNoise = baseNoise;
	}

	// Application de la hauteur de base et de la variation
	float height = params.baseHeight + (combinedNoise - 0.5f) * params.heightVariation;

	// Ajout de plateaux pour certains biomes
	if (biomeType == BIOME_MOUNTAIN || biomeType == BIOME_FOREST) {
		float plateauNoise = noise->octave2D_01(
			static_cast<float>(worldX) / 400.0f,
			static_cast<float>(worldZ) / 400.0f,
			2,
			0.5f
		);
		if (plateauNoise > 0.6f) {
			height = std::max(height, params.baseHeight + 20.0f);
		}
	}

	return height;
}

float BiomeManager::generateDesertHeight(int worldX, int worldZ, siv::PerlinNoise *noise)
{
	const BiomeParameters &params = biomeParams.at(BIOME_DESERT);

	// Bruit de base pour les dunes
	float duneNoise = noise->octave2D_01(
		static_cast<float>(worldX) / 100.0f,
		static_cast<float>(worldZ) / 100.0f,
		3,
		0.5f
	);

	// Bruit de détail pour les petites variations
	float detailNoise = noise->octave2D_01(
		static_cast<float>(worldX) / 30.0f,
		static_cast<float>(worldZ) / 30.0f,
		2,
		0.5f
	);

	// Combinaison des bruits pour créer des dunes
	float combinedNoise = duneNoise * 0.7f + detailNoise * 0.3f;

	return params.baseHeight + (combinedNoise - 0.5f) * params.heightVariation;
}

float BiomeManager::generateForestHeight(int worldX, int worldZ, siv::PerlinNoise *noise)
{
	const BiomeParameters &params = biomeParams.at(BIOME_FOREST);

	// Bruit de base pour les collines
	float hillNoise = noise->octave2D_01(
		static_cast<float>(worldX) / 150.0f,
		static_cast<float>(worldZ) / 150.0f,
		4,
		0.5f
	);

	// Bruit de détail pour les variations locales
	float detailNoise = noise->octave2D_01(
		static_cast<float>(worldX) / 40.0f,
		static_cast<float>(worldZ) / 40.0f,
		2,
		0.5f
	);

	// Combinaison des bruits
	float combinedNoise = hillNoise * 0.6f + detailNoise * 0.4f;

	return params.baseHeight + (combinedNoise - 0.5f) * params.heightVariation;
}

float BiomeManager::generatePlainHeight(int worldX, int worldZ, siv::PerlinNoise *noise)
{
	const BiomeParameters &params = biomeParams.at(BIOME_PLAIN);

	// Bruit de base pour le terrain général
	float baseNoise = noise->octave2D_01(
		static_cast<float>(worldX) / 200.0f,
		static_cast<float>(worldZ) / 200.0f,
		3,
		0.5f
	);

	// Bruit de détail pour les légères variations
	float detailNoise = noise->octave2D_01(
		static_cast<float>(worldX) / 50.0f,
		static_cast<float>(worldZ) / 50.0f,
		2,
		0.5f
	);

	// Combinaison des bruits pour un terrain relativement plat
	float combinedNoise = baseNoise * 0.8f + detailNoise * 0.2f;

	return params.baseHeight + (combinedNoise - 0.5f) * params.heightVariation;
}

float BiomeManager::generateMountainHeight(int worldX, int worldZ, siv::PerlinNoise *noise)
{
	const BiomeParameters &params = biomeParams.at(BIOME_MOUNTAIN);

	// Bruit de base pour la structure des montagnes
	float mountainNoise = noise->octave2D_01(
		static_cast<float>(worldX) / 300.0f,
		static_cast<float>(worldZ) / 300.0f,
		4,
		0.6f
	);

	// Bruit de détail pour les variations locales
	float detailNoise = noise->octave2D_01(
		static_cast<float>(worldX) / 100.0f,
		static_cast<float>(worldZ) / 100.0f,
		3,
		0.5f
	);

	// Bruit de rugosité pour les pics
	float roughnessNoise = noise->octave2D_01(
		static_cast<float>(worldX) / 50.0f,
		static_cast<float>(worldZ) / 50.0f,
		2,
		0.5f
	);

	// Combinaison des bruits
	float combinedNoise = mountainNoise * 0.5f + detailNoise * 0.3f + roughnessNoise * 0.2f;

	// Ajout de pics montagneux
	float peakNoise = noise->octave2D_01(
		static_cast<float>(worldX) / 400.0f,
		static_cast<float>(worldZ) / 400.0f,
		2,
		0.7f
	);

	float peakInfluence = 0.0f;
	if (peakNoise > 0.7f) {
		peakInfluence = (peakNoise - 0.7f) * 3.0f;
	}

	return params.baseHeight + (combinedNoise - 0.5f) * params.heightVariation + peakInfluence * params.peakMultiplier;
}

float BiomeManager::blendBiomes(float biomeNoise, float value1, float value2, float threshold, float blendRange)
{
	if (biomeNoise < threshold - blendRange)
	{
		return value1;
	}
	else if (biomeNoise > threshold + blendRange)
	{
		return value2;
	}
	else
	{
		float t = smoothstep(threshold - blendRange, threshold + blendRange, biomeNoise);
		return (1.0f - t) * value1 + t * value2;
	}
}

float BiomeManager::smoothstep(float edge0, float edge1, float x)
{
	float t = std::max(0.0f, std::min(1.0f, (x - edge0) / (edge1 - edge0)));
	return t * t * (3.0f - 2.0f * t);
}