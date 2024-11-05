#include "Chunk.hpp"

Chunk::Chunk(const glm::vec3& position, ChunkState state)
	: position(position)
	, visible(false)
	, state(state)
	, svo(std::make_unique<SVO>())
	, meshNeedsUpdate(true)
{}

Chunk::Chunk(Chunk&& other) noexcept
	: position(std::move(other.position))
	, visible(other.visible)
	, state(other.state)
	, svo(std::move(other.svo))
	, meshNeedsUpdate(true)
{}

Chunk& Chunk::operator=(Chunk&& other) noexcept
{
	if (this != &other) {
		position = std::move(other.position);
		visible = other.visible;
		state = other.state;
		svo = std::move(other.svo);
		meshNeedsUpdate = other.meshNeedsUpdate;
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
	this->state = state;
}

ChunkState	Chunk::getState() const
{
	return state;
}

bool	Chunk::deleteVoxel(const glm::vec3& position, const glm::vec3& front)
{
	glm::vec3 target = position + front * 0.5f;
	int x = static_cast<int>(target.x - this->position.x);
	int y = static_cast<int>(target.y - this->position.y);
	int z = static_cast<int>(target.z - this->position.z);

	if (svo->getVoxel(x, y, z, Chunk::HEIGHT) != TEXTURE_AIR) {
		svo->addVoxel(x, y, z, Chunk::HEIGHT, TEXTURE_AIR);
		meshNeedsUpdate = true;
		return true;
	}
	return false;
}

bool	Chunk::placeVoxel(const glm::vec3& position, const glm::vec3& front, TextureType type)
{
	glm::vec3 target = position + front * 0.5f;
	int x = static_cast<int>(target.x - this->position.x);
	int y = static_cast<int>(target.y - this->position.y);
	int z = static_cast<int>(target.z - this->position.z);

	if (svo->getVoxel(x, y, z, Chunk::HEIGHT) == TEXTURE_AIR) {
		svo->addVoxel(x, y, z, Chunk::HEIGHT, type);
		meshNeedsUpdate = true;
		return true;
	}
	return false;
}

const std::vector<float>&	Chunk::getMeshData()
{
	if (meshNeedsUpdate) {
		cachedMesh.clear();
		generateMesh();
		if (cachedMesh.empty()) {
			std::cout << "Warning: Mesh data is empty!" << std::endl;
		}

		meshNeedsUpdate = false;
	}
	state = ChunkState::MESHED;
	return cachedMesh;
}

void	Chunk::generateMesh()
{
	const float textureSize = 32.0f / 512.0f;

	auto addFace = [&](int x, int y, int z, int face, TextureType type) {
		float textureX = static_cast<float>(type % 16) * textureSize;
		float textureY = static_cast<float>(type / 16) * textureSize;

		const std::array<std::array<float, 2>, 6> texCoords = {{
			{textureX, textureY + textureSize},             // Bottom-left
			{textureX + textureSize, textureY + textureSize}, // Bottom-right
			{textureX + textureSize, textureY},             // Top-right
			{textureX, textureY + textureSize},             // Bottom-left
			{textureX + textureSize, textureY},             // Top-right
			{textureX, textureY}                            // Top-left
		}};

		for (int i = 0; i < 6; ++i) {
			const auto& vertex = faceVertices[face][i];
			// Position
			cachedMesh.push_back(x + vertex[0] + position.x);
			cachedMesh.push_back(y + vertex[1] + position.y);
			cachedMesh.push_back(z + vertex[2] + position.z);

			// Normal
			cachedMesh.insert(cachedMesh.end(), faceNormals[face].begin(), faceNormals[face].end());

			// Texture coordinates
			cachedMesh.push_back(texCoords[i][0]);
			cachedMesh.push_back(texCoords[i][1]);
		}
	};

	auto traverseNode = [&](const Node& node, int x, int y, int z, int size, auto& recurse) -> void {
		if (node.isLeaf && node.voxelType != TEXTURE_AIR) {
			for (int face = 0; face < 6; ++face) {
				int nx = x + faceOffsets[face][0] * size;
				int ny = y + faceOffsets[face][1] * size;
				int nz = z + faceOffsets[face][2] * size;

				if (svo->getVoxel(nx, ny, nz, Chunk::HEIGHT) == TEXTURE_AIR) {
					addFace(x, y, z, face, node.voxelType);
				}
			}
			return;
		}

		if (size <= 1) return;

		int half = size / 2;
		for (int i = 0; i < 8; ++i) {
			const Node* child = node.getChild(i);
			if (!child) continue;

			int dx = (i & 4) ? half : 0;
			int dy = (i & 2) ? half : 0;
			int dz = (i & 1) ? half : 0;
			recurse(*child, x + dx, y + dy, z + dz, half, recurse);
		}
	};

	traverseNode(*svo->getRoot(), 0, 0, 0, Chunk::HEIGHT, traverseNode);
}

void	Chunk::generateVoxels(siv::PerlinNoise* perlin)
{
	if (state != ChunkState::UNLOADED) return;

	// DEBUG
	for (int x = 0; x < Chunk::SIZE; ++x) {
		for (int z = 0; z < Chunk::SIZE; ++z) {
			for (int y = 0; y < Chunk::HEIGHT / 3; ++y) {
				svo->addVoxel(x, y, z, Chunk::HEIGHT, TEXTURE_STONE);
			}
		}
	}

	//generateChunk(perlin);

	state = ChunkState::GENERATED;
}

void	Chunk::generateChunk(siv::PerlinNoise* perlin)
{
	for (int x = 0; x < Chunk::SIZE; x++) {
		for (int z = 0; z < Chunk::SIZE; z++) {
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
						svo->addVoxel(x, y, z, Chunk::HEIGHT, TEXTURE_STONE);
					} else {
						svo->addVoxel(x, y, z, Chunk::HEIGHT, TEXTURE_AIR);
					}
				} else if (y == surfaceHeight) {
					// Check if there is a cave just below this voxel
					if (caveNoise <= 0.2) {
						// This voxel is at the surface and there is a cave below, so fill it with air to make the cave accessible
						svo->addVoxel(x, y, z, Chunk::HEIGHT, TEXTURE_AIR);
					} else {
						// This voxel is at the surface, so fill it with grass
						svo->addVoxel(x, y, z, Chunk::HEIGHT, biomeType);
					}
				} else {
					if (surfaceHeight <= 1 && y == 0) {
						svo->addVoxel(x, y, z, Chunk::HEIGHT, biomeType);
					} else {
						// This voxel is above the surface, so fill it with air
						svo->addVoxel(x, y, z, Chunk::HEIGHT, TEXTURE_AIR);
					}
				}
			}
		}
	}
}