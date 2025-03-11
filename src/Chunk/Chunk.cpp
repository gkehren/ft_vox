#include "Chunk.hpp"

Chunk::Chunk(const glm::vec3 &position, ChunkState state)
	: position(position), visible(false), state(state), VAO(0), VBO(0), EBO(0), meshNeedsUpdate(true)
{
}

Chunk::Chunk(Chunk &&other) noexcept
	: position(std::move(other.position)), visible(other.visible), state(other.state), voxels(std::move(other.voxels)), VAO(other.VAO), VBO(other.VBO), EBO(other.EBO), meshNeedsUpdate(other.meshNeedsUpdate)
{
}

Chunk &Chunk::operator=(Chunk &&other) noexcept
{
	if (this != &other)
	{
		position = std::move(other.position);
		visible = other.visible;
		state = other.state;
		voxels = std::move(other.voxels);
		VAO = other.VAO;
		VBO = other.VBO;
		EBO = other.EBO;
		meshNeedsUpdate = other.meshNeedsUpdate;
	}
	return *this;
}

Chunk::~Chunk()
{
	if (VAO != 0)
	{
		glDeleteVertexArrays(1, &VAO);
	}
	if (VBO != 0)
	{
		glDeleteBuffers(1, &VBO);
	}
	if (EBO != 0)
	{
		glDeleteBuffers(1, &EBO);
	}
}

const glm::vec3 &Chunk::getPosition() const
{
	return position;
}

size_t Chunk::getIndex(uint32_t x, uint32_t y, uint32_t z) const
{
	return x + SIZE * (z + SIZE * y);
}

bool Chunk::isVisible() const
{
	return visible;
}

void Chunk::setVisible(bool visible)
{
	this->visible = visible;
}

void Chunk::setState(ChunkState state)
{
	if (state == ChunkState::GENERATED || state == ChunkState::UNLOADED)
	{
		meshNeedsUpdate = true;
	}
	this->state = state;
}

ChunkState Chunk::getState() const
{
	return state;
}

Voxel &Chunk::getVoxel(uint32_t x, uint32_t y, uint32_t z)
{
	return voxels[getIndex(x, y, z)];
}

const Voxel &Chunk::getVoxel(uint32_t x, uint32_t y, uint32_t z) const
{
	return voxels[getIndex(x, y, z)];
}

size_t Chunk::getNeighbourIndex(int x, int y, int z) const
{
	if (x < 0)
	{
		return y * SIZE + z;
	}
	else if (x >= SIZE)
	{
		return SIZE * HEIGHT + y * SIZE + z;
	}
	else if (z < 0)
	{
		return 2 * SIZE * HEIGHT + y * SIZE + x;
	}
	else if (z >= SIZE)
	{
		return 3 * SIZE * HEIGHT + y * SIZE + x;
	}
	else
	{
		std::cout << "Warning: getNeighbourIndex called with invalid coordinates!" << std::endl;
		return 0;
	}
}

void Chunk::setVoxel(int x, int y, int z, TextureType type)
{
	if (x < 0 || x >= SIZE || y < 0 || y >= HEIGHT || z < 0 || z >= SIZE)
	{
		size_t index = getNeighbourIndex(x, y, z);
		if (type != TEXTURE_AIR)
		{
			neighboursActiveMap.set(index);
		}
		else
		{
			neighboursActiveMap.reset(index);
		}
	}
	else
	{
		size_t index = getIndex(x, y, z);
		voxels[index].type = static_cast<uint8_t>(type);
		if (type != TEXTURE_AIR)
		{
			activeVoxels.set(index);
		}
		else
		{
			activeVoxels.reset(index);
		}
	}
}

bool Chunk::deleteVoxel(const glm::vec3 &position)
{
	int x = static_cast<int>(position.x - this->position.x);
	int y = static_cast<int>(position.y - this->position.y);
	int z = static_cast<int>(position.z - this->position.z);
	if (x < 0)
		x += SIZE;
	if (z < 0)
		z += SIZE;

	if (isVoxelActive(x, y, z))
	{
		setVoxel(x, y, z, TEXTURE_AIR);
		meshNeedsUpdate = true;
		state = ChunkState::GENERATED;
		return true;
	}
	return false;
}

bool Chunk::placeVoxel(const glm::vec3 &position, TextureType type)
{
	int x = static_cast<int>(position.x - this->position.x);
	int y = static_cast<int>(position.y - this->position.y);
	int z = static_cast<int>(position.z - this->position.z);
	if (x < 0)
		x += SIZE;
	if (z < 0)
		z += SIZE;

	if (!isVoxelActive(x, y, z))
	{
		setVoxel(x, y, z, type);
		meshNeedsUpdate = true;
		state = ChunkState::GENERATED;
		return true;
	}
	return false;
}

bool Chunk::isVoxelActive(int x, int y, int z) const
{
	if (x < 0 || x >= SIZE || y < 0 || y >= HEIGHT || z < 0 || z >= SIZE)
	{
		size_t index = getNeighbourIndex(x, y, z);
		return neighboursActiveMap.test(index);
	}
	else
	{
		size_t index = getIndex(x, y, z);
		return activeVoxels.test(index);
	}
}

bool Chunk::isVoxelActiveGlobalPos(int x, int y, int z) const
{
	int localX = x - static_cast<int>(position.x);
	int localY = y - static_cast<int>(position.y);
	int localZ = z - static_cast<int>(position.z);

	return isVoxelActive(localX, localY, localZ);
}

void Chunk::generateVoxels(NoiseGenerator *noise)
{
	if (state != ChunkState::UNLOADED)
		return;

	// DEBUG
	// for (uint32_t x = 0; x < Chunk::SIZE; ++x) {
	//	for (uint32_t z = 0; z < Chunk::SIZE; ++z) {
	//		setVoxel(x, 0, z, TEXTURE_GRASS);
	//	}
	//}

	generateChunk(noise);
	state = ChunkState::GENERATED;
}

void Chunk::generateChunk(NoiseGenerator *noise)
{
	static BiomeManager biomeManager;

	for (int x = -1; x <= Chunk::SIZE; x++)
	{
		for (int z = -1; z <= Chunk::SIZE; z++)
		{
			if ((x == -1 || x == Chunk::SIZE) && (z == -1 || z == Chunk::SIZE))
			{
				continue;
			}

			// Get absolute world coordinates
			int worldX = static_cast<int>(position.x) + x;
			int worldZ = static_cast<int>(position.z) + z;

			// Get biome influences for this position
			std::vector<BiomeInfluence> biomeInfluences = biomeManager.getBiomeInfluences(worldX, worldZ, *noise);
			const Biome *biome = biomeManager.getBiomeAt(worldX, worldZ, *noise);

			// Get individual heights from each biome
			std::vector<int> biomeHeights;
			float totalWeight = 0.0f;

			for (const auto &influence : biomeInfluences)
			{
				biomeHeights.push_back(influence.biome->generateHeight(worldX, worldZ, *noise));
			}

			// Calculate blended height - with enhanced smoothing between drastically different heights
			float blendedHeight = 0.0f;

			// Check if mountains are involved - they need special handling
			bool hasMountainBiome = false;
			bool hasSwampBiome = false;
			float mountainWeight = 0.0f;
			float swampWeight = 0.0f;

			for (size_t i = 0; i < biomeInfluences.size(); i++)
			{
				const auto &influence = biomeInfluences[i];
				if (influence.biome->getName() == "Mountains")
				{
					hasMountainBiome = true;
					mountainWeight = influence.weight;
				}
				if (influence.biome->getName() == "Swamp")
				{
					hasSwampBiome = true;
					swampWeight = influence.weight;
				}
			}

			// Special handling for extreme differences between biomes
			if (hasMountainBiome)
			{
				// Apply a sigmoid-like function to make mountains rise more naturally
				// from other biomes - steeper at the center of mountain biome, gentler at edges
				float adjustedMountainWeight = mountainWeight * mountainWeight * (3.0f - 2.0f * mountainWeight);

				for (size_t i = 0; i < biomeInfluences.size(); i++)
				{
					float weight = biomeInfluences[i].weight;

					if (biomeInfluences[i].biome->getName() == "Mountains")
					{
						// Use adjusted weight for mountains
						blendedHeight += biomeHeights[i] * adjustedMountainWeight;
					}
					else
					{
						// Other biomes are weighted down in proportion to mountain influence
						float adjustedWeight = weight * (1.0f - adjustedMountainWeight);
						blendedHeight += biomeHeights[i] * adjustedWeight;
					}
				}
			}
			else if (hasSwampBiome)
			{
				// Swamps should maintain their flatness even in transitions
				// Use a stronger influence of the swamp's height where its weight is significant
				float adjustedSwampWeight = swampWeight * swampWeight * (3.0f - 2.0f * swampWeight);
				if (adjustedSwampWeight > 0.3f)
				{
					// Strengthen swamp flatness where it's the dominant biome
					for (size_t i = 0; i < biomeInfluences.size(); i++)
					{
						float weight = biomeInfluences[i].weight;

						if (biomeInfluences[i].biome->getName() == "Swamp")
						{
							// Use adjusted weight for swamp
							blendedHeight += biomeHeights[i] * adjustedSwampWeight;
						}
						else
						{
							// Other biomes are weighted down in proportion to swamp influence
							float adjustedWeight = weight * (1.0f - adjustedSwampWeight);
							blendedHeight += biomeHeights[i] * adjustedWeight;
						}
					}
				}
				else
				{
					// Normal blending for minimal swamp influence
					for (size_t i = 0; i < biomeInfluences.size(); i++)
					{
						blendedHeight += biomeHeights[i] * biomeInfluences[i].weight;
					}
				}
			}
			else
			{
				// Standard weighted average for normal biome transitions
				for (size_t i = 0; i < biomeInfluences.size(); i++)
				{
					blendedHeight += biomeHeights[i] * biomeInfluences[i].weight;
				}
			}

			// Get final integer height
			int surfaceHeight = static_cast<int>(blendedHeight);

			for (int y = 0; y < Chunk::HEIGHT; y++)
			{
				double caveNoise = noise->caveNoise(worldX, y + position.y, worldZ);
				bool shouldBeCave = false;

				for (const auto &influence : biomeInfluences)
				{
					if (influence.biome->shouldBeCave(worldX, y, worldZ, caveNoise))
					{
						shouldBeCave = true;
						break;
					}
				}

				if (y < surfaceHeight)
				{
					if (shouldBeCave)
						setVoxel(x, y, z, TEXTURE_AIR);
					else
					{
						TextureType blockType = determineBlendedBlockType(worldX, y, worldZ, surfaceHeight, biomeInfluences, *noise);
						setVoxel(x, y, z, blockType);
					}
				}
				else if (y == surfaceHeight)
				{
					if (caveNoise <= 0.2)
						setVoxel(x, y, z, TEXTURE_AIR); // Cave opening
					else
						setVoxel(x, y, z, biomeInfluences[0].biome->getProperties().surfaceBlock);
				}
				else
				{
					setVoxel(x, y, z, TEXTURE_AIR);

					if (surfaceHeight <= 1 && y == 0)
						setVoxel(x, y, z, biome->getProperties().surfaceBlock);
				}
			}
		}
	}
}

TextureType Chunk::determineBlendedBlockType(
	int x, int y, int z, int surfaceHeight,
	const std::vector<BiomeInfluence> &biomeInfluences,
	NoiseGenerator &noise) const
{
	// For deep underground, use the dominant biome's block
	if (y < surfaceHeight - 8)
	{ // Deeper underground
		return biomeInfluences[0].biome->getBlockAt(x, y, z, surfaceHeight, noise);
	}

	// For transition layers near the surface (8 blocks from surface)
	// Calculate a blend factor based on depth from surface
	float depthFactor = (surfaceHeight - y) / 8.0f;
	depthFactor = std::clamp(depthFactor, 0.0f, 1.0f);

	// The closer to the surface, the more we care about the transition
	float transitionStrength = 1.0f - depthFactor;

	// Use a noise function to create a natural pattern for block type selection
	float blockSelectNoise = noise.noise3D(x, y, z, 8.0f);

	// Create a weighted randomness factor that considers multiple biomes
	// but favors the primary one closer to the surface
	float cumulativeWeight = 0.0f;

	// Adjust weights based on depth
	std::vector<float> adjustedWeights;
	float totalAdjusted = 0.0f;

	for (const auto &influence : biomeInfluences)
	{
		// Strengthen the dominant biome influence as we go deeper
		float adjustedWeight = influence.weight;

		// Apply depth-based adjustment
		if (influence.biome == biomeInfluences[0].biome)
		{ // Dominant biome
			adjustedWeight = influence.weight * (1.0f + depthFactor);
		}
		else
		{
			adjustedWeight = influence.weight * (1.0f - depthFactor * 0.5f);
		}

		adjustedWeights.push_back(adjustedWeight);
		totalAdjusted += adjustedWeight;
	}

	// Normalize the adjusted weights
	for (auto &weight : adjustedWeights)
	{
		weight /= totalAdjusted;
	}

	// Select biome based on adjusted weights
	for (size_t i = 0; i < biomeInfluences.size(); i++)
	{
		cumulativeWeight += adjustedWeights[i];
		if (blockSelectNoise <= cumulativeWeight)
		{
			return biomeInfluences[i].biome->getBlockAt(x, y, z, surfaceHeight, noise);
		}
	}

	// Default fallback
	return biomeInfluences[0].biome->getBlockAt(x, y, z, surfaceHeight, noise);
}

void Chunk::generateMesh()
{
	vertices.clear();
	indices.clear();

	std::unordered_map<Vertex, uint32_t, VertexHasher> vertexMap;
	uint32_t indexCounter = 0;

	for (int x = 0; x < SIZE; ++x)
	{
		for (int y = 0; y < HEIGHT; ++y)
		{
			for (int z = 0; z < SIZE; ++z)
			{
				if (!isVoxelActive(x, y, z))
					continue;

				for (int face = 0; face < 6; ++face)
				{
					glm::ivec3 neighborPos = glm::ivec3{x, y, z} + directions[face];
					bool isFaceVisible = false;

					// Check within current chunk
					if (neighborPos.x >= 0 && neighborPos.x < SIZE &&
						neighborPos.y >= 0 && neighborPos.y < HEIGHT &&
						neighborPos.z >= 0 && neighborPos.z < SIZE)
					{
						isFaceVisible = !isVoxelActive(neighborPos.x, neighborPos.y, neighborPos.z);
					}
					else
					{
						if (neighborPos.y >= 0 && neighborPos.y < HEIGHT)
						{
							isFaceVisible = !isVoxelActive(neighborPos.x, neighborPos.y, neighborPos.z);
						}
						else
						{
							isFaceVisible = true;
						}
					}

					if (isFaceVisible)
					{
						uint32_t faceVertexIndices[4]; // Stocker les indices des sommets de la face
						for (int i = 0; i < 4; ++i)
						{
							Vertex vertex;
							vertex.position = position + glm::vec3(x, y, z) + faceVertexOffsets[face][i];
							vertex.normal = faceNormals[face];

							// Calcul des coordonnées de texture
							int texIndex = getVoxel(x, y, z).type;
							float u = (texIndex % (int)(TEXTURE_ATLAS_SIZE / 32)) * TEXTURE_SIZE;
							float v = (texIndex / (int)(TEXTURE_ATLAS_SIZE / 32)) * TEXTURE_SIZE;
							vertex.texCoord = glm::vec2(
								u + texCoords[i].x * TEXTURE_SIZE,
								v + texCoords[i].y * TEXTURE_SIZE);

							// Vérifiez si le sommet existe déjà
							auto it = vertexMap.find(vertex);
							uint32_t vertexIndex;
							if (it != vertexMap.end())
							{
								// Utilisez l'indice existant
								vertexIndex = it->second;
							}
							else
							{
								// Ajoutez le nouveau sommet
								vertices.push_back(vertex);
								vertexIndex = indexCounter;
								vertexMap[vertex] = indexCounter;
								indexCounter++;
							}
							faceVertexIndices[i] = vertexIndex; // Stockez l'indice du sommet
						}

						indices.push_back(faceVertexIndices[0]);
						indices.push_back(faceVertexIndices[1]);
						indices.push_back(faceVertexIndices[2]);

						indices.push_back(faceVertexIndices[0]);
						indices.push_back(faceVertexIndices[2]);
						indices.push_back(faceVertexIndices[3]);
					}
				}
			}
		}
	}

	meshNeedsUpdate = true;
	state = ChunkState::MESHED;
}

void Chunk::uploadMeshToGPU()
{
	// Générer les buffers si nécessaire
	if (VAO == 0)
	{
		glGenVertexArrays(1, &VAO);
	}
	if (VBO == 0)
	{
		glGenBuffers(1, &VBO);
	}
	if (EBO == 0)
	{
		glGenBuffers(1, &EBO);
	}

	glBindVertexArray(VAO);

	// Charger les données de sommets
	glBindBuffer(GL_ARRAY_BUFFER, VBO);
	glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);

	// Charger les indices
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_STATIC_DRAW);

	// Configurer les attributs de sommets

	// Position (location = 0)
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, position));

	// Normale (location = 1)
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, normal));

	// Coordonnées de texture (location = 2)
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, texCoord));

	glBindVertexArray(0);

	meshNeedsUpdate = false;
}

uint32_t Chunk::draw(const Shader &shader, const Camera &camera, GLuint textureAtlas, const ShaderParameters &params)
{
	if (meshNeedsUpdate)
	{
		uploadMeshToGPU();
	}

	shader.use();

	shader.setMat4("view", camera.getViewMatrix());
	shader.setMat4("projection", camera.getProjectionMatrix(1920, 1080, 320));
	shader.setInt("textureSampler", 0);

	glm::vec3 sunDirection = glm::normalize(glm::vec3(0.8, 1.0, 0.6));
	glm::vec3 sunPosition = camera.getPosition() + params.sunDirection * 2000.0f;

	shader.setVec3("lightPos", sunPosition);
	shader.setVec3("viewPos", camera.getPosition());

	// Paramètres du fog
	shader.setFloat("fogStart", params.fogStart);
	shader.setFloat("fogEnd", params.fogEnd);
	shader.setVec3("fogColor", params.fogColor);
	shader.setFloat("fogDensity", params.fogDensity);

	// Paramètres visuels optionnels
	shader.setFloat("ambientStrength", params.ambientStrength);
	shader.setFloat("diffuseIntensity", params.diffuseIntensity);
	shader.setFloat("lightLevels", params.lightLevels);
	shader.setFloat("saturationLevel", params.saturationLevel);
	shader.setFloat("colorBoost", params.colorBoost);
	shader.setFloat("gamma", params.gamma);

	glBindTexture(GL_TEXTURE_2D, textureAtlas);

	glBindVertexArray(VAO);
	glDrawElements(GL_TRIANGLES, indices.size(), GL_UNSIGNED_SHORT, 0);
	glBindVertexArray(0);

	return indices.size();
}