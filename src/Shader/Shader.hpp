#pragma once

#include <glad/glad.h>
#include <string>
#include <fstream>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

class Shader {
	public:
		Shader(const char* vertexPath, const char* fragmentPath);
		~Shader();
		void use() const;

		GLuint	getId() const;
		void	setMat4(const std::string &name, const glm::mat4 &mat) const;
		void	setInt(const std::string &name, int value) const;

	private:
		GLuint	id;
		GLuint	vertexShader;
		GLuint	fragmentShader;

		std::string	getShaderSource(const char* path) const;
		GLuint		compileShader(GLenum shaderType, const char* source) const;
		GLuint		linkProgram(GLuint vertexShader, GLuint fragmentShader) const;
};
