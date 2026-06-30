#pragma once

#include <glad/glad.h>
#include <string>
#include <string_view>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view txt) const {
        return std::hash<std::string_view>{}(txt);
    }
};

class Shader
{
public:
	Shader(const char *vertexPath, const char *fragmentPath);
	~Shader();
	void use() const;

	bool usingMeshShaders;

	GLuint getId() const;
	void setMat4(std::string_view name, const glm::mat4 &mat) const;
	void setVec3(std::string_view name, const glm::vec3 &vec) const;
	void setVec2(std::string_view name, const glm::vec2 &vec) const;
	void setInt(std::string_view name, int value) const;
	void setFloat(std::string_view name, float value) const;

private:
	GLuint id;
	GLuint vertexShader;
	GLuint fragmentShader;

	std::string getShaderSource(const char *path) const;
	GLuint compileShader(GLenum shaderType, const char *source) const;
	GLuint linkProgram(GLuint vertexShader, GLuint fragmentShader) const;

	GLint getUniformLocation(std::string_view name) const;
	mutable std::unordered_map<std::string, GLint, StringHash, std::equal_to<>> uniformLocationCache;
};
