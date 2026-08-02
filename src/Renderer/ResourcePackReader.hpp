#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/// Abstraction for reading Minecraft block textures from a .zip archive or directory.
class ResourcePackReader
{
public:
	ResourcePackReader();
	explicit ResourcePackReader(const std::string &path);
	~ResourcePackReader();

	ResourcePackReader(const ResourcePackReader &) = delete;
	ResourcePackReader &operator=(const ResourcePackReader &) = delete;
	ResourcePackReader(ResourcePackReader &&) noexcept;
	ResourcePackReader &operator=(ResourcePackReader &&) noexcept;

	/// Open a zip file or directory path.
	bool open(const std::string &path);
	void close();

	bool isValid() const { return m_isOpen; }
	bool isZip() const { return m_isZip; }
	const std::string &getPath() const { return m_path; }

	/// Read raw PNG byte stream for `assets/minecraft/textures/block/<basename>`.
	/// Returns true if found and read into `outBuffer`.
	bool readBlockTexture(const std::string &basename, std::vector<uint8_t> &outBuffer);

private:
	std::string m_path;
	bool m_isZip{false};
	bool m_isOpen{false};
	std::string m_zipBlockPrefix;

	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
