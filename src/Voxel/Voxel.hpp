#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <unordered_set>
#include <functional>

struct VoxelHash;
struct VoxelEqual;
class Voxel;

namespace std {
	template<> struct hash<glm::vec3>
	{
		typedef glm::vec3 argument_type;
		typedef std::size_t result_type;
		result_type operator()(argument_type const& s) const noexcept
		{
			result_type const h1 ( std::hash<float>{}(s.x) );
			result_type const h2 ( std::hash<float>{}(s.y) );
			result_type const h3 ( std::hash<float>{}(s.z) );
			return (h1 ^ (h2 << 1)) ^ h3;
		}
	};
}

using VoxelSet = std::unordered_set<Voxel, VoxelHash, VoxelEqual>;

class Voxel {
	public:
		Voxel();
		Voxel(glm::vec3 position, glm::vec3 color);
		~Voxel();

		glm::mat4	getModelMatrix() const;
		glm::vec3	getPosition() const;
		glm::vec3	getColor() const;
		float		getSize() const;

		void	setPosition(glm::vec3 position);
		void	setColor(glm::vec3 color);

		bool	isSurrounded(const VoxelSet& voxels) const;

	private:
		glm::vec3	position;
		glm::vec3	color;
};

struct VoxelHash {
	std::size_t operator()(const Voxel& voxel) const {
		return std::hash<glm::vec3>()(voxel.getPosition());
	}
};

struct VoxelEqual {
	bool operator()(const Voxel& a, const Voxel& b) const {
		return a.getPosition() == b.getPosition();
	}
};
