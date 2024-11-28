#include "Chunk.hpp"

Chunk::Chunk(const glm::vec3& position, ChunkState state)
	: position(position)
	, visible(false)
	, state(state)
	, voxels(SIZE * SIZE * HEIGHT)
	, neighbours(SIZE * HEIGHT * 4)
	, meshNeedsUpdate(true)
	, cachedMesh()
	, VBO(0)
	, VBONeedsUpdate(true)
{}

Chunk::Chunk(Chunk&& other) noexcept
	: position(std::move(other.position))
	, visible(other.visible)
	, state(other.state)
	, voxels(std::move(other.voxels))
	, neighbours(std::move(other.neighbours))
	, meshNeedsUpdate(other.meshNeedsUpdate)
	, cachedMesh(std::move(other.cachedMesh))
	, VBO(other.VBO)
	, VBONeedsUpdate(other.VBONeedsUpdate)
{}

Chunk& Chunk::operator=(Chunk&& other) noexcept
{
	if (this != &other) {
		position = std::move(other.position);
		visible = other.visible;
		state = other.state;
		voxels = std::move(other.voxels);
		neighbours = std::move(other.neighbours);
		meshNeedsUpdate = other.meshNeedsUpdate;
		cachedMesh = std::move(other.cachedMesh);
		VBO = other.VBO;
		VBONeedsUpdate = other.VBONeedsUpdate;
	}
	return *this;
}

const glm::vec3&	Chunk::getPosition() const
{
	return position;
}

bool	Chunk::isVisible() const
{
	return visible;
}

void	Chunk::setVisible(bool visible)
{
	this->visible = visible;
}

void	Chunk::setState(ChunkState state)
{
	if (state == ChunkState::GENERATED || state == ChunkState::UNLOADED) {
		meshNeedsUpdate = true;
	}
	this->state = state;
}

ChunkState	Chunk::getState() const
{
	return state;
}

bool	Chunk::getMeshNeedsUpdate() const
{
	return meshNeedsUpdate;
}

Voxel&	Chunk::getVoxel(uint32_t x, uint32_t y, uint32_t z)
{
	return voxels[getIndex(x, y, z)];
}

const Voxel&	Chunk::getVoxel(uint32_t x, uint32_t y, uint32_t z) const
{
	return voxels[getIndex(x, y, z)];
}

Voxel& Chunk::getNeighbourVoxel(int x, int y, int z)
{
	// std::vector<Voxel> neighbours; 3D linear storage of the voxels in direction of the neighbors
	// neighbours.reserve(SIZE * HEIGHT * 4); // 4 neighbors in 2D plane (left, right, front, back) for each height level (y) in the chunk
	const int directionSize = SIZE * HEIGHT; // 16 * 256 = 4096
	if (x < 0) {
		// Left neighbor
		int idx = y * SIZE + z; // Index within the left neighbor
		return neighbours[idx];
	} else if (x >= SIZE) {
		// Right neighbor
		int idx = directionSize + y * SIZE + z; // Offset by directionSize (4096)
		return neighbours[idx];
	} else if (z < 0) {
		// Front neighbor
		int idx = 2 * directionSize + y * SIZE + x; // Offset by 2 * directionSize
		return neighbours[idx];
	} else if (z >= SIZE) {
		// Back neighbor
		int idx = 3 * directionSize + y * SIZE + x; // Offset by 3 * directionSize
		return neighbours[idx];
	} else {
		std::cout << "Warning: getNeighbourVoxel called with invalid coordinates!" << std::endl;
		return getVoxel(x, y, z);
	}
}

void	Chunk::setVoxel(int x, int y, int z, TextureType type)
{
	if (x < 0 || x >= SIZE || y < 0 || y >= HEIGHT || z < 0 || z >= SIZE) {
		Voxel& neighbour = getNeighbourVoxel(x, y, z);
		neighbour.type = static_cast<uint8_t>(type);
		neighbour.active = type != TEXTURE_AIR;
	} else {
		Voxel& voxel = getVoxel(x, y, z);
		voxel.type = static_cast<uint8_t>(type);
		voxel.active = type != TEXTURE_AIR;
	}
}

bool	Chunk::deleteVoxel(const glm::vec3& position, const glm::vec3& front)
{
	glm::vec3 target = position + front * 0.5f;
	uint32_t x = static_cast<uint32_t>(target.x - this->position.x);
	uint32_t y = static_cast<uint32_t>(target.y - this->position.y);
	uint32_t z = static_cast<uint32_t>(target.z - this->position.z);

	if (getVoxel(x, y, z).active) {
		setVoxel(x, y, z, TEXTURE_AIR);
		meshNeedsUpdate = true;
		state = ChunkState::GENERATED;
		return true;
	}
	return false;
}

bool	Chunk::placeVoxel(const glm::vec3& position, const glm::vec3& front, TextureType type)
{
	glm::vec3 target = position + front * 0.5f;
	uint32_t x = static_cast<uint32_t>(target.x - this->position.x);
	uint32_t y = static_cast<uint32_t>(target.y - this->position.y);
	uint32_t z = static_cast<uint32_t>(target.z - this->position.z);

	if (!getVoxel(x, y, z).active) {
		setVoxel(x, y, z, type);
		meshNeedsUpdate = true;
		state = ChunkState::GENERATED;
		return true;
	}
	return false;
}

const std::vector<float>&	Chunk::getMeshData()
{
	if (meshNeedsUpdate) {
		//cachedMesh.clear();
		state = ChunkState::GENERATED;
		//generateMesh();
		//if (cachedMesh.empty()) {
		//	std::cout << "Warning: Mesh data is empty!" << std::endl;
		//}

		meshNeedsUpdate = false;
		VBONeedsUpdate = true;
		return cachedMesh;
	}
	state = ChunkState::MESHED;
	return cachedMesh;
}

void	Chunk::generateVoxels(siv::PerlinNoise* perlin)
{
	if (state != ChunkState::UNLOADED) return;

	// DEBUG
	//for (uint32_t x = 0; x < Chunk::SIZE; ++x) {
	//	for (uint32_t z = 0; z < Chunk::SIZE; ++z) {
	//		setVoxel(x, 0, z, TEXTURE_GRASS);
	//	}
	//}

	generateChunk(perlin);
	state = ChunkState::GENERATED;
}

void	Chunk::generateChunk(siv::PerlinNoise* perlin)
{
	for (int x = -1; x <= Chunk::SIZE; x++) {
		for (int z = -1; z <= Chunk::SIZE; z++) {
			if ((x == -1 || x == Chunk::SIZE) && (z == -1 || z == Chunk::SIZE)) {
				continue;
			}
			// Generate Perlin noise values at different scales
			float noise1 = perlin->noise2D_01((position.x + x) / 200.0f, (position.z + z) / 200.0f) * 1.5f;
			float noise2 = perlin->noise2D_01((position.x + x) / 50.0f, (position.z + z) / 50.0f) * 0.5f;
			float noise3 = perlin->noise2D_01((position.x + x) / 25.0f, (position.z + z) / 25.0f) * 0.2f;
			float mountainNoise = perlin->noise2D_01((position.x + x) / 500.0f, (position.z + z) / 500.0f); // New noise layer for mountains
			float biomeNoise = perlin->noise2D_01((position.x + x) / 250.0f, (position.z + z) / 250.0f); // New noise layer for biomes

			TextureType biomeType;
			if (biomeNoise < 0.2f) {
				biomeType = TEXTURE_SAND;
			} else if (biomeNoise < 0.5f) {
				biomeType = TEXTURE_GRASS;
			} else if (biomeNoise < 0.7f) {
				biomeType = TEXTURE_SNOW;
			} else if (biomeNoise < 0.8f) {
				biomeType = TEXTURE_NETHER;
			} else if (biomeNoise < 0.9f) {
				biomeType = TEXTURE_SOUL;
			} else {
				biomeType = TEXTURE_DIRT;
			}

			// Combine the Perlin noise values to determine the surface height at this point
			int baseHeight = static_cast<int>((noise1 + noise2 + noise3) * Chunk::HEIGHT / 4);

			// Adjust height for mountains
			int mountainHeight = static_cast<int>(mountainNoise * 128); // Increased impact for higher mountains

			// Determine if this point is in a mountain region
			bool isMountain = mountainNoise > 0.6f; // Threshold for mountain regions

			// Interpolate between baseHeight and mountainHeight for smoother transitions at the borders
			float interpolationFactor = 0.0f;
			if (isMountain) {
				interpolationFactor = (mountainNoise - 0.6f) / 0.4f; // Normalize to range [0, 1]
				interpolationFactor = interpolationFactor * interpolationFactor; // Squaring to smooth the transition
			}
			int surfaceHeight = static_cast<int>(baseHeight * (1.0f - interpolationFactor) + mountainHeight * interpolationFactor);

			// Ensure plains are flatter by reducing noise influence
			if (biomeType == TEXTURE_GRASS && !isMountain) {
				surfaceHeight = baseHeight;
			}

			for (int y = 0; y < Chunk::HEIGHT; y++) {
				double caveNoise = perlin->noise3D_01((x + position.x) / Chunk::SIZE, (y + position.y) / Chunk::SIZE, (z + position.z) / Chunk::SIZE);
				if (y < surfaceHeight) {
					if (y == 0 || caveNoise > 0.25) {
						setVoxel(x, y, z, TEXTURE_STONE);
					} else {
						setVoxel(x, y, z, TEXTURE_AIR);
					}
				} else if (y == surfaceHeight) {
					// Check if there is a cave just below this voxel
					if (caveNoise <= 0.2) {
						// This voxel is at the surface and there is a cave below, so fill it with air to make the cave accessible
						setVoxel(x, y, z, TEXTURE_AIR);
					} else {
						// This voxel is at the surface, so fill it with grass
						setVoxel(x, y, z, biomeType);
					}
				} else {
					if (surfaceHeight <= 1 && y == 0) {
						setVoxel(x, y, z, biomeType);
					} else {
						// This voxel is above the surface, so fill it with air
						setVoxel(x, y, z, TEXTURE_AIR);
					}
				}
			}
		}
	}
}

void Chunk::generateMesh()
{
	cachedMesh.clear();
	cachedMesh.reserve(SIZE * SIZE * HEIGHT);

	// Directional offsets for neighbor voxels
	static const glm::ivec3 directions[6] = {
		{  0,  1,  0 }, // Up
		{  0, -1,  0 }, // Down
		{  0,  0,  1 }, // Front
		{  0,  0, -1 }, // Back
		{ -1,  0,  0 }, // Left
		{  1,  0,  0 }  // Right
	};

	// Predefined face vertices for a cube
	static const float faceVertices[6][12] = {
		{ 0,1,0, 1,1,0, 1,1,1, 0,1,1 }, // Up
		{ 0,0,0, 0,0,1, 1,0,1, 1,0,0 }, // Down
		{ 0,0,1, 1,0,1, 1,1,1, 0,1,1 }, // Front
		{ 0,0,0, 0,1,0, 1,1,0, 1,0,0 }, // Back
		{ 0,0,0, 0,0,1, 0,1,1, 0,1,0 }, // Left
		{ 1,0,0, 1,1,0, 1,1,1, 1,0,1 }  // Right
	};

	// Normals for each face
	static const float normals[6][3] = {
		{  0,  1,  0 }, // Up
		{  0, -1,  0 }, // Down
		{  0,  0,  1 }, // Front
		{  0,  0, -1 }, // Back
		{ -1,  0,  0 }, // Left
		{  1,  0,  0 }  // Right
	};

	// Texture coordinates for a face
	static const float texCoords[4][2] = {
		{ 0.0f, 0.0f },
		{ 1.0f, 0.0f },
		{ 1.0f, 1.0f },
		{ 0.0f, 1.0f }
	};

	const float TEXTURE_ATLAS_SIZE = 512.0f;
	const float TEXTURE_SIZE = 32.0f / TEXTURE_ATLAS_SIZE;

	static const int indices[6][6] = {
		{ 0, 3, 2, 0, 2, 1 }, // Up
		{ 0, 3, 2, 0, 2, 1 }, // Down
		{ 0, 1, 2, 0, 2, 3 }, // Front
		{ 0, 1, 2, 0, 2, 3 }, // Back
		{ 0, 1, 2, 0, 2, 3 }, // Left
		{ 0, 1, 2, 0, 2, 3 }  // Right
	};

	for (int x = 0; x < SIZE; ++x) {
		for (int y = 0; y < HEIGHT; ++y) {
			for (int z = 0; z < SIZE; ++z) {
				Voxel& voxel = getVoxel(x, y, z);
				if (!voxel.active) continue;

				for (int face = 0; face < 6; ++face) {
					glm::ivec3 neighborPos = glm::ivec3{ x, y, z } + directions[face];
					bool isFaceVisible = false;

					// Check within current chunk
					if (neighborPos.x >= 0 && neighborPos.x < SIZE &&
						neighborPos.y >= 0 && neighborPos.y < HEIGHT &&
						neighborPos.z >= 0 && neighborPos.z < SIZE) {
						Voxel& neighborVoxel = getVoxel(neighborPos.x, neighborPos.y, neighborPos.z);
						isFaceVisible = !neighborVoxel.active;
					} else {
						if (neighborPos.y >= 0 && neighborPos.y < HEIGHT) {
							Voxel& neighborVoxel = getNeighbourVoxel(neighborPos.x, neighborPos.y, neighborPos.z);
							isFaceVisible = !neighborVoxel.active;
						} else {
							isFaceVisible = true;
						}
					}

					if (isFaceVisible) {
						// Add face to mesh
						for (int i = 0; i < 6; ++i) {
							int vert = indices[face][i];

							float vx = x + faceVertices[face][vert * 3 + 0];
							float vy = y + faceVertices[face][vert * 3 + 1];
							float vz = z + faceVertices[face][vert * 3 + 2];

							cachedMesh.push_back(vx + position.x);
							cachedMesh.push_back(vy + position.y);
							cachedMesh.push_back(vz + position.z);

							cachedMesh.push_back(normals[face][0]);
							cachedMesh.push_back(normals[face][1]);
							cachedMesh.push_back(normals[face][2]);

							int texIndex = voxel.type;
							float u = (texIndex % (int)(TEXTURE_ATLAS_SIZE / 32)) * TEXTURE_SIZE;
							float v = (texIndex / (int)(TEXTURE_ATLAS_SIZE / 32)) * TEXTURE_SIZE;

							cachedMesh.push_back(u + texCoords[vert][0] * TEXTURE_SIZE);
							cachedMesh.push_back(v + texCoords[vert][1] * TEXTURE_SIZE);
						}
					}
				}
			}
		}
	}

	meshNeedsUpdate = false;
	VBONeedsUpdate = true;
	state = ChunkState::MESHED;
}