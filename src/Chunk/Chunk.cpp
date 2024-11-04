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

int getChildIndex(int x, int y, int z)
{
	return (x << 2) | (y << 1) | z;
}

void	Chunk::generateVoxels(siv::PerlinNoise* perlin)
{
	if (state != ChunkState::UNLOADED) return;

	// DEBUG
	for (int x = 0; x < Chunk::SIZE; ++x) {
		for (int z = 0; z < Chunk::SIZE; ++z) {
			svo->addVoxel(x, 0, z, Chunk::HEIGHT, TEXTURE_STONE);
		}
	}

	//generateChunk(0, SIZE, 0, SIZE, perlin);

	state = ChunkState::GENERATED;
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

void	Chunk::generateChunk(int startX, int endX, int startZ, int endZ, siv::PerlinNoise* perlin)
{
	std::vector<ChunkColumn> columns(Chunk::SIZE * Chunk::SIZE);

	for (int x = startX; x < endX; x++) {
		for (int z = startZ; z < endZ; z++) {
			int index = x * Chunk::SIZE + z;
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

void	Chunk::generateOctant(int startX, int startY, int startZ, int endX, int endY, int endZ, const std::vector<ChunkColumn>& columns, siv::PerlinNoise* perlin)
{
	for (int x = startX; x < endX; ++x) {
		for (int y = startY; y < endY; ++y) {
			for (int z = startZ; z < endZ; ++z) {
				int columnIndex = x * SIZE + z;
				const ChunkColumn& column = columns[columnIndex];
				TextureType voxelType = determineVoxelType(x, y, z, column, perlin);
				svo->addVoxel(x, y, z, Chunk::HEIGHT, voxelType);
			}
		}
	}
}

TextureType	Chunk::determineVoxelType(int x, int y, int z, const ChunkColumn& column, siv::PerlinNoise* perlin)
{
	return (y <= column.surfaceHeight) ? column.biomeType : TEXTURE_AIR;
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

	constexpr std::array<std::array<int, 3>, 6> faceOffsets = { {
		{1, 0, 0}, {-1, 0, 0},  // Right, Left
		{0, 1, 0}, {0, -1, 0},  // Top, Bottom
		{0, 0, 1}, {0, 0, -1}   // Front, Back
	} };

	constexpr std::array<std::array<std::array<float, 3>, 6>, 6> faceVertices = { {
		// Left face (-X)
		{{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f},
		{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}}},
		// Right face (+X)
		{{{1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f},
		{1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}}},
		// Top face (+Y)
		{{{0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 0.0f},
		{0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}},
		// Bottom face (-Y)
		{{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 1.0f},
		{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}}},
		// Front face (+Z)
		{{{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f},
		{0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 1.0f}}},
		// Back face (-Z)
		{{{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
		{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}}}
	} };

	constexpr std::array<std::array<float, 3>, 6> faceNormals = { {
		{-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},   // Left, Right
		{0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f},   // Top, Bottom
		{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}    // Front, Back
	} };

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