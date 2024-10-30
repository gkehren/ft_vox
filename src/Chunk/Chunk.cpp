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
	static const float textureSize = 32.0f / 512.0f;
	const auto& [normal, vertices] = faceData.at(face);

	const float textureX = static_cast<float>(type % 16) * textureSize;
	const float textureY = static_cast<float>(type / 16) * textureSize;
	const float textureXEnd = textureX + textureSize;
	const float textureYEnd = textureY + textureSize;

	mesh.reserve(4, 6, 6);
	const glm::vec3 worldPos = position + pos;
	for (const auto& vertex : vertices) {
		mesh.addVertex(worldPos + vertex);
	}

	const std::array<glm::vec2, 6> texCoords = {
		glm::vec2(textureX, textureY),
		glm::vec2(textureXEnd, textureY),
		glm::vec2(textureXEnd, textureYEnd),
		glm::vec2(textureX, textureY),
		glm::vec2(textureXEnd, textureYEnd),
		glm::vec2(textureX, textureYEnd)
	};

	for (const auto& texCoord : texCoords) {
		mesh.addTexture(texCoord);
	}

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

void	Chunk::generateChunk(int startX, int endX, int startZ, int endZ, siv::PerlinNoise* perlin)
{
	std::vector<ChunkColumn> columns((endX - startX) * (endZ - startZ));

	for (int x = startX; x < endX; x++) {
		for (int z = startZ; z < endZ; z++) {
			int index = (x - startX) * (endZ - startZ) + (z - startZ);
			ChunkColumn& column = columns[index];

			float worldX = position.x + x;
			float worldZ = position.z + z;

			float baseNoise = perlin->noise2D_01(worldX / 200.0f, worldZ / 200.0f);
			float detailNoise = perlin->noise2D_01(worldX / 50.0f, worldZ / 50.0f);
			float mountainNoise = perlin->noise2D_01(worldX / 500.0f, worldZ / 500.0f);
			float biomeNoise = perlin->noise2D_01(worldX / 250.0f, worldZ / 250.0f);

			column.biomeType = getBiomeType(biomeNoise);

			int baseHeight = static_cast<int>((baseNoise * 1.5f + detailNoise * 0.5f) * Chunk::HEIGHT / 2);
			column.isMountain = mountainNoise > 0.6f;

			if (column.isMountain) {
				float factor = ((mountainNoise - 0.6f) / 0.4f);
				factor *= factor;
				int mountainHeight = static_cast<int>(mountainNoise * 128);
				column.surfaceHeight = static_cast<int>(baseHeight * (1.0f - factor) + mountainHeight * factor);
			} else {
				column.surfaceHeight = column.biomeType == TEXTURE_GRASS ? baseHeight : static_cast<int>(baseHeight * 0.8f + baseHeight * 0.2f * detailNoise);
			}
		}
	}

	const int octantSize = 8;
	for (int ox = startX; ox < endX; ox += octantSize) {
		for (int oy = 0; oy < Chunk::HEIGHT; oy += octantSize) {
			for (int oz = startZ; oz < endZ; oz += octantSize) {
				generateOctant(ox, oy, oz,
							 std::min(ox + octantSize, endX),
							 std::min(oy + octantSize, Chunk::HEIGHT),
							 std::min(oz + octantSize, endZ),
							 columns, perlin);
			}
		}
	}
}

TextureType	Chunk::getBiomeType(float biomeNoise)
{
	if (biomeNoise < 0.2f) return TEXTURE_SAND;
	if (biomeNoise < 0.5f) return TEXTURE_GRASS;
	if (biomeNoise < 0.7f) return TEXTURE_SNOW;
	if (biomeNoise < 0.8f) return TEXTURE_NETHER;
	if (biomeNoise < 0.9f) return TEXTURE_SOUL;
	return TEXTURE_DIRT;
}

void	Chunk::generateOctant(int startX, int startY, int startZ, int endX, int endY, int endZ, const std::vector<ChunkColumn>& columns, siv::PerlinNoise* perlin)
{
	bool canBeSolid = true;
	bool canBeAir = true;
	TextureType commonType = TEXTURE_AIR;

	for (int x = startX; x < endX && (canBeSolid || canBeAir); x++) {
		for (int z = startZ; z < endZ && (canBeSolid || canBeAir); z++) {
			int columnIndex = (x - startX) * (endZ - startZ) + (z - startZ);
			const ChunkColumn& column = columns[columnIndex];

			for (int y = startY; y < endY && (canBeSolid || canBeAir); y++) {
				TextureType currentType = determineVoxelType(x, y, z, column, perlin);

				if (x == startX && y == startY && z == startZ) {
					commonType = currentType;
				} else if (currentType != commonType) {
					canBeSolid = false;
					if (currentType != TEXTURE_AIR) canBeAir = false;
				}
			}
		}
	}

	if (canBeSolid || canBeAir) {
		glm::vec3 octantPos(startX, startY, startZ);
		octree.setVoxel(octantPos, commonType);
	} else {
		// Sinon, on définit chaque voxel individuellement
		for (int x = startX; x < endX; x++) {
			for (int z = startZ; z < endZ; z++) {
				int columnIndex = (x - startX) * (endZ - startZ) + (z - startZ);
				const ChunkColumn& column = columns[columnIndex];

				for (int y = startY; y < endY; y++) {
					TextureType type = determineVoxelType(x, y, z, column, perlin);
					if (type != TEXTURE_AIR) {
						octree.setVoxel(glm::vec3(x, y, z), type);
					}
				}
			}
		}
	}
}

TextureType	Chunk::determineVoxelType(int x, int y, int z, const ChunkColumn& column, siv::PerlinNoise* perlin)
{
	if (y > column.surfaceHeight) {
		return TEXTURE_AIR;
	}

	if (y == column.surfaceHeight) {
		double caveNoise = perlin->noise3D_01((x + position.x) / SIZE,
											(y + position.y) / SIZE,
											(z + position.z) / SIZE);
		return caveNoise <= 0.2 ? TEXTURE_AIR : column.biomeType;
	}

	if (y < column.surfaceHeight) {
		double caveNoise = perlin->noise3D_01((x + position.x) / SIZE,
											(y + position.y) / SIZE,
											(z + position.z) / SIZE);
		return (y == 0 || caveNoise > 0.25) ? TEXTURE_STONE : TEXTURE_AIR;
	}

	return TEXTURE_AIR;
}
