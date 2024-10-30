#include "Chunk.hpp"

Chunk::Chunk(const glm::vec3& position, ChunkState state)
	: position(position)
	, visible(false)
	, state(state)
	, octree(position, Chunk::SIZE)
{}

Chunk::Chunk(Chunk&& other) noexcept
	: position(std::move(other.position))
	, visible(other.visible)
	, state(other.state)
	, octree(std::move(other.octree))
	, mesh(std::move(other.mesh))
{}

Chunk& Chunk::operator=(Chunk&& other) noexcept
{
	if (this != &other) {
		position = std::move(other.position);
		visible = other.visible;
		state = other.state;
		octree = std::move(other.octree);
		mesh = std::move(other.mesh);
	}
	return *this;
}

const glm::vec3&	Chunk::getPosition() const
{
	return position;
}

const std::vector<float>&	Chunk::getData()
{
	return mesh.getData();
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

TextureType	Chunk::getVoxel(int x, int y, int z) const
{
	return octree.getVoxel(glm::vec3(x, y, z));
}

bool	Chunk::deleteVoxel(const glm::vec3& position, const glm::vec3& front)
{
	glm::vec3 target = position + front * 0.5f;
	int x = floor(target.x - this->position.x);
	int y = floor(target.y - this->position.y);
	int z = floor(target.z - this->position.z);

	if (x < 0 || x >= Chunk::SIZE || y < 0 || y >= Chunk::HEIGHT || z < 0 || z >= Chunk::SIZE)
		return false;

	if (octree.getVoxel(glm::vec3(x, y, z)) == TEXTURE_AIR)
		return false;

	octree.setVoxel(glm::vec3(x, y, z), TEXTURE_AIR);
	mesh.clear();
	state = ChunkState::GENERATED;
	return true;
}

bool	Chunk::placeVoxel(const glm::vec3& position, const glm::vec3& front, TextureType type)
{
	glm::vec3 target = position + front * 0.5f;
	int x = floor(target.x - this->position.x);
	int y = floor(target.y - this->position.y);
	int z = floor(target.z - this->position.z);

	if (x < 0 || x >= Chunk::SIZE || y < 0 || y >= Chunk::HEIGHT || z < 0 || z >= Chunk::SIZE)
		return false;

	if (octree.getVoxel(glm::vec3(x, y, z)) != TEXTURE_AIR)
		return false;

	octree.setVoxel(glm::vec3(x, y, z), type);
	mesh.clear();
	state = ChunkState::GENERATED;
	return true;
}

void	Chunk::generateVoxel(siv::PerlinNoise* perlin)
{
	if (state != ChunkState::UNLOADED) return;

	const int numThreads = 4;
	const int chunkWidth = Chunk::SIZE / numThreads;
	std::vector<std::thread> threads;

	for (int i = 0; i < numThreads; i++) {
		int startX = i * chunkWidth;
		int endX = (i + 1) * chunkWidth;
		threads.emplace_back(&Chunk::generateChunk, this, startX, endX, 0, Chunk::SIZE, perlin);
	}

	for (auto& thread : threads) {
		thread.join();
	}

	octree.optimize();
	state = ChunkState::GENERATED;
}

void	Chunk::generateMesh(std::unordered_map<glm::ivec3, Chunk, ivec3_hash>& chunks, siv::PerlinNoise* perlin)
{
	if (state == ChunkState::REMESHED) {
		if (!adjacentChunksGenerated(chunks)) return;
		mesh.clear();
	} else if (state != ChunkState::GENERATED) return;

	bool complete = true;

	for (int x = 0; x < Chunk::SIZE; x++) {
		for (int y = 0; y < Chunk::HEIGHT; y++) {
			for (int z = 0; z < Chunk::SIZE; z++) {
				TextureType type = octree.getVoxel(glm::vec3(x, y, z));
				if (type != TEXTURE_AIR) {
					bool result = addVoxelToMesh(chunks, glm::vec3(x, y, z), type, perlin);
					if (complete) complete = result;
				}
			}
		}
	}

	state = complete ? ChunkState::MESHED : ChunkState::REMESHED;
}

bool	Chunk::addVoxelToMesh(std::unordered_map<glm::ivec3, Chunk, ivec3_hash>& chunks, const glm::vec3& pos, TextureType type, siv::PerlinNoise* perlin)
{
	bool complete = true;
	int x = static_cast<int>(pos.x);
	int y = static_cast<int>(pos.y);
	int z = static_cast<int>(pos.z);

	for (auto& dir : directions) {
		int dx, dy, dz;
		Face face;
		std::tie(dx, dy, dz, face) = dir;

		int nx = x + dx;
		int ny = y + dy;
		int nz = z + dz;

		if (face == Face::BOTTOM && y == 0) {
			addFaceToMesh(pos, face, type);
			continue;
		}

		if (face == Face::TOP && y == Chunk::HEIGHT - 1) {
			addFaceToMesh(pos, face, type);
			continue;
		}

		// Check in the current chunk
		if (nx >= 0 && nx < Chunk::SIZE && ny >= 0 && ny < Chunk::HEIGHT && nz >= 0 && nz < Chunk::SIZE) {
			if (octree.getVoxel(glm::vec3(nx, ny, nz)) == TEXTURE_AIR) {
				addFaceToMesh(pos, face, type);
			}
		} else {
			// Check in the adjacent chunks
			glm::ivec3 adjacentChunkPos = glm::ivec3(this->position) + glm::ivec3(dx, dy, dz) * Chunk::SIZE;
			adjacentChunkPos.x = floor(adjacentChunkPos.x / Chunk::SIZE);
			adjacentChunkPos.y = 0;
			adjacentChunkPos.z = floor(adjacentChunkPos.z / Chunk::SIZE);

			auto adjacentChunk = chunks.find(adjacentChunkPos);
			if (adjacentChunk != chunks.end()) {
				if (adjacentChunk->second.state == ChunkState::GENERATED) {
					int adjacentX = (nx + Chunk::SIZE) % Chunk::SIZE;
					int adjacentY = (ny + Chunk::HEIGHT) % Chunk::HEIGHT;
					int adjacentZ = (nz + Chunk::SIZE) % Chunk::SIZE;

					if (adjacentChunk->second.getVoxel(adjacentX, adjacentY, adjacentZ) == TEXTURE_AIR) {
						addFaceToMesh(pos, face, type);
					}
				} else {
					adjacentChunk->second.generateVoxel(perlin);
					int adjacentX = (nx + Chunk::SIZE) % Chunk::SIZE;
					int adjacentY = (ny + Chunk::HEIGHT) % Chunk::HEIGHT;
					int adjacentZ = (nz + Chunk::SIZE) % Chunk::SIZE;

					if (adjacentChunk->second.getVoxel(adjacentX, adjacentY, adjacentZ) == TEXTURE_AIR) {
						addFaceToMesh(pos, face, type);
					}
				}
			} else {
				complete = false;
			}
		}
	}
	return complete;
}

void	Chunk::addFaceToMesh(const glm::vec3& pos, Face face, TextureType type)
{
	glm::vec3	normal;
	std::vector<glm::vec3>	vertices;
	std::tie(normal, vertices) = faceData.at(face);

	static const float		textureSize = 32.0f / 512.0f;
	float		textureX = static_cast<float>(type % 16) * textureSize;
	float		textureY = static_cast<float>(type / 16) * textureSize;

	mesh.reserve(vertices.size(), 6, 6);
	for (const auto& vertex : vertices) {
		mesh.addVertex(position + pos + vertex);
	}

	mesh.addTexture(glm::vec2(textureX, textureY));
	mesh.addTexture(glm::vec2(textureX + textureSize, textureY));
	mesh.addTexture(glm::vec2(textureX + textureSize, textureY + textureSize));
	mesh.addTexture(glm::vec2(textureX, textureY));
	mesh.addTexture(glm::vec2(textureX + textureSize, textureY + textureSize));
	mesh.addTexture(glm::vec2(textureX, textureY + textureSize));

	for (int i = 0; i < 6; i++) {
		mesh.addNormal(normal);
	}
}

bool	Chunk::adjacentChunksGenerated(const std::unordered_map<glm::ivec3, Chunk, ivec3_hash>& chunks) const
{
	for (auto& dir : directions) {
		int dx, dy, dz;
		std::tie(dx, dy, dz, std::ignore) = dir;

		glm::ivec3 adjacentChunkPos = glm::ivec3(this->position) + glm::ivec3(dx, dy, dz) * Chunk::SIZE;
		adjacentChunkPos.x = floor(adjacentChunkPos.x / Chunk::SIZE);
		adjacentChunkPos.y = 0;
		adjacentChunkPos.z = floor(adjacentChunkPos.z / Chunk::SIZE);
		auto adjacentChunk = chunks.find(adjacentChunkPos);
		if (adjacentChunk == chunks.end() || adjacentChunk->second.state == ChunkState::UNLOADED) return false;
	}
	return true;
}

void	Chunk::generateChunk(int startX, int endX, int startZ, int endZ, siv::PerlinNoise* perlin) {
	for (int x = startX; x < endX; x++) {
		for (int z = startZ; z < endZ; z++) {
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
			int baseHeight = static_cast<int>((noise1 + noise2 + noise3) * Chunk::HEIGHT / 2);

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
						octree.setVoxel(glm::vec3(x, y, z), TEXTURE_STONE);
					} else {
						octree.setVoxel(glm::vec3(x, y, z), TEXTURE_AIR);
					}
				} else if (y == surfaceHeight) {
					// Check if there is a cave just below this voxel
					if (caveNoise <= 0.2) {
						// This voxel is at the surface and there is a cave below, so fill it with air to make the cave accessible
						octree.setVoxel(glm::vec3(x, y, z), TEXTURE_AIR);
					} else {
						// This voxel is at the surface, so fill it with grass
						octree.setVoxel(glm::vec3(x, y, z), biomeType);
					}
				} else {
					if (surfaceHeight <= 1 && y == 0) {
						octree.setVoxel(glm::vec3(x, y, z), biomeType); // MAYBE CHANGE TO WATER
					} else {
						// This voxel is above the surface, so fill it with air
						octree.setVoxel(glm::vec3(x, y, z), TEXTURE_AIR);
					}
				}
			}
		}
	}
}