#pragma once

#include <glad/glad.h>
#include <string>
#include <fstream>
#include <sstream>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

class Shader
{
public:
	Shader(const char *vertexPath, const char *fragmentPath);
	~Shader();
	void use() const;

	bool usingMeshShaders;

	GLuint getId() const;
	void setMat4(const std::string &name, const glm::mat4 &mat) const;
	void setVec3(const std::string &name, const glm::vec3 &vec) const;
	void setInt(const std::string &name, int value) const;
	void setFloat(const std::string &name, float value) const;

private:
	GLuint id;
	GLuint vertexShader;
	GLuint fragmentShader;

	std::string getShaderSource(const char *path) const;
	GLuint compileShader(GLenum shaderType, const char *source) const;
	GLuint linkProgram(GLuint vertexShader, GLuint fragmentShader) const;
};
