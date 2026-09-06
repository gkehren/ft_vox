#include "ResourcePackReader.hpp"
#include <miniz.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

struct ResourcePackReader::Impl
{
	mz_zip_archive zip{};
	bool zipInitialized{false};
};

ResourcePackReader::ResourcePackReader() = default;

ResourcePackReader::ResourcePackReader(const std::string &path)
{
	open(path);
}

ResourcePackReader::~ResourcePackReader()
{
	close();
}

ResourcePackReader::ResourcePackReader(ResourcePackReader &&other) noexcept
	: m_path(std::move(other.m_path)), m_isZip(other.m_isZip), m_isOpen(other.m_isOpen),
	  m_zipBlockPrefix(std::move(other.m_zipBlockPrefix)), m_minecraftRoot(std::move(other.m_minecraftRoot)),
	  m_impl(std::move(other.m_impl))
{
	other.m_isOpen = false;
	other.m_isZip = false;
}

ResourcePackReader &ResourcePackReader::operator=(ResourcePackReader &&other) noexcept
{
	if (this != &other)
	{
		close();
		m_path = std::move(other.m_path);
		m_isZip = other.m_isZip;
		m_isOpen = other.m_isOpen;
		m_zipBlockPrefix = std::move(other.m_zipBlockPrefix);
		m_minecraftRoot = std::move(other.m_minecraftRoot);
		m_impl = std::move(other.m_impl);
		other.m_isOpen = false;
		other.m_isZip = false;
	}
	return *this;
}

void ResourcePackReader::close()
{
	if (m_impl && m_impl->zipInitialized)
	{
		mz_zip_reader_end(&m_impl->zip);
		m_impl->zipInitialized = false;
	}
	m_impl.reset();
	m_isOpen = false;
	m_isZip = false;
	m_zipBlockPrefix.clear();
	m_minecraftRoot.clear();
	m_path.clear();
}

bool ResourcePackReader::open(const std::string &path)
{
	close();
	if (path.empty())
		return false;

	m_path = path;
	m_impl = std::make_unique<Impl>();

	// Check if path exists
	std::error_code ec;
	const fs::path p(path);
	if (!fs::exists(p, ec))
		return false;

	if (fs::is_directory(p, ec))
	{
		m_isZip = false;
		m_isOpen = true;
		// Resolve assets/minecraft once: pack root, or one wrapped subdirectory
		// (same shape as the wrapped roots ZIP packs commonly ship).
		if (fs::exists(p / "assets/minecraft", ec))
			m_minecraftRoot = (p / "assets/minecraft").string();
		else
		{
			std::error_code itEc;
			for (const fs::directory_entry &entry : fs::directory_iterator(p, itEc))
			{
				if (!entry.is_directory(itEc) || !fs::exists(entry.path() / "assets/minecraft", ec))
					continue;
				m_minecraftRoot = (entry.path() / "assets/minecraft").string();
				break;
			}
		}
		return true;
	}

	// Attempt opening as ZIP archive
	mz_zip_zero_struct(&m_impl->zip);
	if (mz_zip_reader_init_file(&m_impl->zip, path.c_str(), 0))
	{
		m_impl->zipInitialized = true;
		m_isZip = true;
		m_isOpen = true;

		// Scan ZIP entries to locate assets/minecraft/textures/block/
		const mz_uint numFiles = mz_zip_reader_get_num_files(&m_impl->zip);
		const std::string target = "assets/minecraft/textures/block/";
		for (mz_uint i = 0; i < numFiles; ++i)
		{
			char filename[512]{};
			if (mz_zip_reader_get_filename(&m_impl->zip, i, filename, sizeof(filename)))
			{
				std::string fn(filename);
				// Standardize separators to '/'
				std::replace(fn.begin(), fn.end(), '\\', '/');
				auto pos = fn.find(target);
				if (pos != std::string::npos)
				{
					m_zipBlockPrefix = fn.substr(0, pos + target.size());
					break;
				}
			}
		}

		if (m_zipBlockPrefix.empty())
		{
			// Default prefix if not found during scan
			m_zipBlockPrefix = target;
		}

		return true;
	}

	return false;
}

bool ResourcePackReader::readBlockTexture(const std::string &basename, std::vector<uint8_t> &outBuffer)
{
	outBuffer.clear();
	if (!m_isOpen)
		return false;

	if (m_isZip && m_impl && m_impl->zipInitialized)
	{
		const std::string entryName = m_zipBlockPrefix + basename;
		const int fileIdx = mz_zip_reader_locate_file(&m_impl->zip, entryName.c_str(), nullptr, 0);
		if (fileIdx < 0)
		{
			// Secondary fallback: search for basename anywhere under textures/block/
			const mz_uint numFiles = mz_zip_reader_get_num_files(&m_impl->zip);
			const std::string suffix = "textures/block/" + basename;
			for (mz_uint i = 0; i < numFiles; ++i)
			{
				char filename[512]{};
				if (mz_zip_reader_get_filename(&m_impl->zip, i, filename, sizeof(filename)))
				{
					std::string fn(filename);
					std::replace(fn.begin(), fn.end(), '\\', '/');
					if (fn.size() >= suffix.size() &&
						fn.compare(fn.size() - suffix.size(), suffix.size(), suffix) == 0)
					{
						mz_zip_archive_file_stat stat{};
						if (mz_zip_reader_file_stat(&m_impl->zip, i, &stat) && stat.m_uncomp_size > 0)
						{
							outBuffer.resize(static_cast<size_t>(stat.m_uncomp_size));
							if (mz_zip_reader_extract_to_mem(&m_impl->zip, i, outBuffer.data(), outBuffer.size(), 0))
								return true;
							outBuffer.clear();
						}
					}
				}
			}
			return false;
		}

		mz_zip_archive_file_stat stat{};
		if (!mz_zip_reader_file_stat(&m_impl->zip, static_cast<mz_uint>(fileIdx), &stat) || stat.m_uncomp_size == 0)
			return false;

		outBuffer.resize(static_cast<size_t>(stat.m_uncomp_size));
		if (!mz_zip_reader_extract_to_mem(&m_impl->zip, static_cast<mz_uint>(fileIdx), outBuffer.data(),
										  outBuffer.size(), 0))
		{
			outBuffer.clear();
			return false;
		}
		return true;
	}
	else
	{
		// Directory mode
		std::string filePath = m_path + "/assets/minecraft/textures/block/" + basename;
		std::ifstream file(filePath, std::ios::binary | std::ios::ate);
		if (!file)
		{
			// Fallback: check direct file path
			filePath = m_path + "/" + basename;
			file.open(filePath, std::ios::binary | std::ios::ate);
			if (!file)
				return false;
		}

		const std::streamsize size = file.tellg();
		if (size <= 0)
			return false;

		file.seekg(0, std::ios::beg);
		outBuffer.resize(static_cast<size_t>(size));
		if (file.read(reinterpret_cast<char *>(outBuffer.data()), size))
			return true;

		outBuffer.clear();
		return false;
	}
}

// Exact namespace-relative paths; ZIPs may wrap assets in one or more folders.
bool ResourcePackReader::readEntityTexture(const std::string &relativePath, std::vector<uint8_t> &out)
{
    out.clear();
    if (!m_isOpen || relativePath.empty()) return false;
    const fs::path relative(relativePath);
    if (relative.is_absolute() || relative.has_root_name()) return false;
    for (auto &part : relative) if (part == "..") return false;
    const std::string suffix = "assets/minecraft/textures/entity/" + relative.generic_string();
    if (m_isZip) {
        const mz_uint count = mz_zip_reader_get_num_files(&m_impl->zip);
        for (mz_uint i=0; i<count; ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&m_impl->zip,i,&stat)) continue;
            std::string name(stat.m_filename);
            std::replace(name.begin(),name.end(),'\\','/');
            if (name.size()<suffix.size() || name.compare(name.size()-suffix.size(),suffix.size(),suffix)!=0) continue;
            const auto start=name.size()-suffix.size();
            if (start && name[start-1]!='/') continue;
            if (!stat.m_uncomp_size || stat.m_uncomp_size>64*1024*1024) return false;
            out.resize(size_t(stat.m_uncomp_size));
            if (mz_zip_reader_extract_to_mem(&m_impl->zip,i,out.data(),out.size(),0)) return true;
            out.clear(); return false;
        }
    } else {
        const fs::path entityPath = m_minecraftRoot.empty()
            ? fs::path(m_path) / suffix
            : fs::path(m_minecraftRoot) / "textures/entity" / relative.generic_string();
        std::ifstream file(entityPath,std::ios::binary|std::ios::ate);
        if (!file) return false;
        auto size=file.tellg();
        if (size<=0 || size>64*1024*1024) return false;
        out.resize(size_t(size)); file.seekg(0);
        if (file.read(reinterpret_cast<char*>(out.data()),size)) return true;
        out.clear();
    }
    return false;
}
