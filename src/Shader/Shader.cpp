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
	glDeleteShader(this->vertexShader);
	glDeleteShader(this->fragmentShader);
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
		throw std::runtime_error(infoLog);
	}

	glDeleteShader(vertexShader);
	glDeleteShader(fragmentShader);

	return program;
}

void Shader::setMat4(const std::string &name, const glm::mat4 &mat) const
{
	glUniformMatrix4fv(glGetUniformLocation(this->id, name.c_str()), 1, GL_FALSE, glm::value_ptr(mat));
}

void Shader::setVec3(const std::string &name, const glm::vec3 &vec) const
{
	glUniform3fv(glGetUniformLocation(this->id, name.c_str()), 1, glm::value_ptr(vec));
}

void Shader::setInt(const std::string &name, int value) const
{
	glUniform1i(glGetUniformLocation(this->id, name.c_str()), value);
}

void Shader::setFloat(const std::string &name, float value) const
{
	glUniform1f(glGetUniformLocation(this->id, name.c_str()), value);
}
