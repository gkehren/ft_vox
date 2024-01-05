#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <map>

#include <Chunk/Chunk.hpp>
#include <Shader/Shader.hpp>
#include <Camera/Camera.hpp>
#include <utils.hpp>

class Renderer {
	public:
		Renderer();
		~Renderer();

		int		draw(Chunk& chunk, const Shader& shader, const Camera& camera);
		void	draw(const Voxel& voxel, const Shader& shader, const Camera& camera) const;
		void	drawBoundingBox(const Chunk& chunk, const Camera& camera) const;
		void	drawBoundingBox(const Voxel& voxel, const Camera& camera) const;

	private:
		GLuint	VAO;
		GLuint	VBO;
		GLuint	EBO;
		GLuint	instanceVBO;

		Shader*	boundingBoxShader;
		GLuint	boundingBoxVAO;
		GLuint	boundingBoxVBO;
		GLuint	boundingBoxEBO;

		GLuint	texture[TEXTURE_COUNT];

		std::map<const Voxel*, GLuint> queries;
		std::map<const Voxel*, GLuint> samples;

		void	occlusionCulling(Chunk& chunk, const Camera& camera);
		bool	isBlockOccluded(Chunk& chunk, int x, int y, int z, int blockSize, const Camera& camera);
		bool	isVoxelOccluded(const Voxel& voxel, const Camera& camera);
};
