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

void Chunk::generateVoxels(siv::PerlinNoise *perlin)
{
	if (state != ChunkState::UNLOADED)
		return;

	// DEBUG
	// for (uint32_t x = 0; x < Chunk::SIZE; ++x) {
	//	for (uint32_t z = 0; z < Chunk::SIZE; ++z) {
	//		setVoxel(x, 0, z, TEXTURE_GRASS);
	//	}
	//}

	generateChunk(perlin);
	state = ChunkState::GENERATED;
}

void Chunk::generateChunk(siv::PerlinNoise *perlin)
{
	for (int x = -1; x <= Chunk::SIZE; x++)
	{
		for (int z = -1; z <= Chunk::SIZE; z++)
		{
			if ((x == -1 || x == Chunk::SIZE) && (z == -1 || z == Chunk::SIZE))
			{
				continue;
			}
			// Generate Perlin noise values at different scales
			int absX = x + position.x;
			int absZ = z + position.z;
			float noise1 = perlin->noise2D_01((absX) / 200.0f, (absZ) / 200.0f) * 1.5f;
			float noise2 = perlin->noise2D_01((absX) / 50.0f, (absZ) / 50.0f) * 0.5f;
			float noise3 = perlin->noise2D_01((absX) / 25.0f, (absZ) / 25.0f) * 0.2f;
			float mountainNoise = perlin->noise2D_01((absX) / 500.0f, (absZ) / 500.0f); // New noise layer for mountains
			float biomeNoise = perlin->noise2D_01((absX) / 250.0f, (absZ) / 250.0f);	// New noise layer for biomes

			TextureType biomeType;
			if (biomeNoise < 0.2f)
			{
				biomeType = TEXTURE_SAND;
			}
			else if (biomeNoise < 0.5f)
			{
				biomeType = TEXTURE_GRASS;
			}
			else if (biomeNoise < 0.7f)
			{
				biomeType = TEXTURE_SNOW;
			}
			else if (biomeNoise < 0.8f)
			{
				biomeType = TEXTURE_NETHER;
			}
			else if (biomeNoise < 0.9f)
			{
				biomeType = TEXTURE_SOUL;
			}
			else
			{
				biomeType = TEXTURE_DIRT;
			}

			// Combine the Perlin noise values to determine the surface height at this point
			// int baseHeight = static_cast<int>((noise1 + noise2 + noise3) * Chunk::HEIGHT / 4);
			int baseHeight = static_cast<int>((noise1 + noise2 + noise3) * 64);

			// Adjust height for mountains
			int mountainHeight = static_cast<int>(mountainNoise * 128); // Increased impact for higher mountains

			// Determine if this point is in a mountain region
			bool isMountain = mountainNoise > 0.6f; // Threshold for mountain regions

			// Interpolate between baseHeight and mountainHeight for smoother transitions at the borders
			float interpolationFactor = 0.0f;
			if (isMountain)
			{
				interpolationFactor = (mountainNoise - 0.6f) / 0.4f;			 // Normalize to range [0, 1]
				interpolationFactor = interpolationFactor * interpolationFactor; // Squaring to smooth the transition
			}
			int surfaceHeight = static_cast<int>(baseHeight * (1.0f - interpolationFactor) + mountainHeight * interpolationFactor);

			// Ensure plains are flatter by reducing noise influence
			if (biomeType == TEXTURE_GRASS && !isMountain)
			{
				surfaceHeight = baseHeight;
			}

			for (int y = 0; y < Chunk::HEIGHT; y++)
			{
				double caveNoise = perlin->noise3D_01((x + position.x) / Chunk::SIZE, (y + position.y) / Chunk::SIZE, (z + position.z) / Chunk::SIZE);
				if (y < surfaceHeight)
				{
					if (y == 0 || caveNoise > 0.25)
					{
						setVoxel(x, y, z, TEXTURE_STONE);
					}
					else
					{
						setVoxel(x, y, z, TEXTURE_AIR);
					}
				}
				else if (y == surfaceHeight)
				{
					// Check if there is a cave just below this voxel
					if (caveNoise <= 0.2)
					{
						// This voxel is at the surface and there is a cave below, so fill it with air to make the cave accessible
						setVoxel(x, y, z, TEXTURE_AIR);
					}
					else
					{
						// This voxel is at the surface, so fill it with grass
						setVoxel(x, y, z, biomeType);
					}
				}
				else
				{
					if (surfaceHeight <= 1 && y == 0)
					{
						setVoxel(x, y, z, biomeType);
					}
				}
			}
		}
	}
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

uint32_t Chunk::draw(const Shader &shader, const Camera &camera, GLuint textureAtlas)
{
	if (meshNeedsUpdate)
	{
		uploadMeshToGPU();
	}

	shader.use();

	shader.setMat4("view", camera.getViewMatrix());
	shader.setMat4("projection", camera.getProjectionMatrix(1920, 1080, 320));
	shader.setInt("textureSampler", 0);
	shader.setVec3("lightPos", camera.getPosition());

	glBindTexture(GL_TEXTURE_2D, textureAtlas);

	glBindVertexArray(VAO);
	glDrawElements(GL_TRIANGLES, indices.size(), GL_UNSIGNED_SHORT, 0);
	glBindVertexArray(0);

	return indices.size();
}