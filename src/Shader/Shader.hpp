#pragma once

#include <glad/glad.h>
#include <string>
#include <fstream>

class Shader {
	public:
		Shader(const char* vertexPath, const char* fragmentPath);
		~Shader();
		void use() const;

		GLuint	getId() const;

	private:
		GLuint	id;
		GLuint	vertexShader;
		GLuint	fragmentShader;

		std::string	getShaderSource(const char* path) const;
		GLuint		compileShader(GLenum shaderType, const char* source) const;
		GLuint		linkProgram(GLuint vertexShader, GLuint fragmentShader) const;
};
