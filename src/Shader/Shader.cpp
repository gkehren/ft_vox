#include "Shader.hpp"

Shader::Shader(const char *vertexPath, const char *fragmentPath)
{
	std::string vertexSource = this->getShaderSource(vertexPath);
	std::string fragmentSource = this->getShaderSource(fragmentPath);

	this->vertexShader = this->compileShader(GL_VERTEX_SHADER, vertexSource.c_str());
	this->fragmentShader = this->compileShader(GL_FRAGMENT_SHADER, fragmentSource.c_str());
	this->id = this->linkProgram(this->vertexShader, this->fragmentShader);
}

Shader::~Shader()
{
	glDeleteProgram(this->id);
}

void Shader::use() const
{
	glUseProgram(this->id);
}

GLuint Shader::getId() const
{
	return this->id;
}

std::string Shader::getShaderSource(const char *path) const
{
	std::ifstream shaderFile(path);
	std::string shaderSource;
	std::string line;

	if (!shaderFile.is_open())
	{
		throw std::runtime_error("Failed to open shader file");
	}

	while (std::getline(shaderFile, line))
	{
		shaderSource += line + "\n";
	}

	shaderFile.close();
	return shaderSource;
}

GLuint Shader::compileShader(GLenum shaderType, const char *source) const
{
	GLuint shader = glCreateShader(shaderType);

	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);

	GLint success;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
	if (!success)
	{
		GLchar infoLog[512];
		glGetShaderInfoLog(shader, 512, nullptr, infoLog);
		glDeleteShader(shader);
		throw std::runtime_error(infoLog);
	}

	return shader;
}

GLuint Shader::linkProgram(GLuint vertexShader, GLuint fragmentShader) const
{
	GLuint program = glCreateProgram();

	glAttachShader(program, vertexShader);
	glAttachShader(program, fragmentShader);
	glLinkProgram(program);

	GLint success;
	glGetProgramiv(program, GL_LINK_STATUS, &success);
	if (!success)
	{
		GLchar infoLog[512];
		glGetProgramInfoLog(program, 512, nullptr, infoLog);
		glDeleteShader(vertexShader);
		glDeleteShader(fragmentShader);
		glDeleteProgram(program);
		throw std::runtime_error(infoLog);
	}

	glDeleteShader(vertexShader);
	glDeleteShader(fragmentShader);

	return program;
}

GLint Shader::getUniformLocation(std::string_view name) const
{
	auto it = uniformLocationCache.find(name);
	if (it != uniformLocationCache.end())
	{
		return it->second;
	}

	std::string nameStr(name);
	GLint location = glGetUniformLocation(this->id, nameStr.c_str());
	uniformLocationCache[std::move(nameStr)] = location;
	return location;
}

void Shader::setMat4(std::string_view name, const glm::mat4 &mat) const
{
	glUniformMatrix4fv(this->getUniformLocation(name), 1, GL_FALSE, glm::value_ptr(mat));
}

void Shader::setVec3(std::string_view name, const glm::vec3 &vec) const
{
	glUniform3fv(this->getUniformLocation(name), 1, glm::value_ptr(vec));
}

void Shader::setVec2(std::string_view name, const glm::vec2 &vec) const
{
	glUniform2fv(this->getUniformLocation(name), 1, glm::value_ptr(vec));
}

void Shader::setInt(std::string_view name, int value) const
{
	glUniform1i(this->getUniformLocation(name), value);
}

void Shader::setFloat(std::string_view name, float value) const
{
	glUniform1f(this->getUniformLocation(name), value);
}
