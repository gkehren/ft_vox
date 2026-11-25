#include <Engine/PlayerUiPreferences.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace gameui
{
namespace
{
bool parseStep(std::string_view text, float &value)
{
	char buffer[16]{};
	if (text.empty() || text.size() >= sizeof(buffer))
		return false;
	for (size_t i = 0; i < text.size(); ++i)
		buffer[i] = text[i];
	char *end = nullptr;
	const double parsed = std::strtod(buffer, &end);
	if (end != buffer + text.size() || !(parsed > 0.0) || parsed > double(kMaxScale))
		return false;
	value = static_cast<float>(parsed);
	return true;
}

std::string_view trimTrailingCarriageReturn(std::string_view line)
{
	if (!line.empty() && line.back() == '\r')
		line.remove_suffix(1);
	return line;
}
} // namespace

std::string formatScalePolicy(ScalePolicy policy)
{
	if (policy.mode == ScaleMode::Auto)
		return "scale auto";
	char value[32];
	std::snprintf(value, sizeof(value), "scale fixed %.2f", double(nearestUserScale(policy.value)));
	return value;
}

std::string PlayerUiSettings::serialize() const
{
	return std::string(kPlayerUiSettingsHeader) + '\n' + formatScalePolicy(scale) + '\n';
}

bool PlayerUiSettings::parse(std::string_view record)
{
	if (record.empty() || record.size() > kMaxPlayerUiSettingsBytes)
		return false;
	const auto end = record.find('\n');
	if (end == std::string_view::npos)
		return false;
	if (trimTrailingCarriageReturn(record.substr(0, end)) != kPlayerUiSettingsHeader)
		return false;
	std::string_view rest = record.substr(end + 1);
	PlayerUiSettings next;
	bool sawScale = false;
	while (!rest.empty())
	{
		const auto lineEnd = rest.find('\n');
		const std::string_view line = trimTrailingCarriageReturn(rest.substr(0, lineEnd));
		rest.remove_prefix(lineEnd == std::string_view::npos ? rest.size() : lineEnd + 1);
		if (line.empty())
			return false;
		if (line == "scale auto" && !sawScale)
		{
			next.scale = {ScaleMode::Auto, 1.f};
			sawScale = true;
			continue;
		}
		if (line.starts_with("scale fixed ") && !sawScale)
		{
			float value = 0.f;
			if (!parseStep(line.substr(12), value) || !isUserScale(value))
				return false;
			next.scale = {ScaleMode::Fixed, nearestUserScale(value)};
			sawScale = true;
			continue;
		}
		return false; // unknown key, duplicate key or malformed value
	}
	if (!sawScale)
		return false;
	*this = next;
	return true;
}

LoadStatus PlayerUiSettings::load(const std::filesystem::path &path)
{
	std::error_code ec;
	const auto size = std::filesystem::file_size(path, ec);
	if (ec)
		return ec == std::errc::no_such_file_or_directory ? LoadStatus::Missing : LoadStatus::Corrupt;
	if (size > kMaxPlayerUiSettingsBytes)
		return LoadStatus::Corrupt;
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return LoadStatus::Corrupt;
	std::string record(static_cast<size_t>(size), '\0');
	if (!record.empty() && !in.read(record.data(), static_cast<std::streamsize>(record.size())))
		return LoadStatus::Corrupt;
	return parse(record) ? LoadStatus::Ok : LoadStatus::Corrupt;
}

bool PlayerUiSettings::save(const std::filesystem::path &path) const
{
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	if (ec)
		return false;
	auto temporary = path;
	temporary += ".tmp";
	{
		std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
		const std::string record = serialize();
		out.write(record.data(), static_cast<std::streamsize>(record.size()));
		out.close();
		if (!out)
		{
			std::filesystem::remove(temporary, ec);
			return false;
		}
	}
	std::filesystem::rename(temporary, path, ec);
	if (ec)
	{
		// Windows keeps a hard failure when the destination exists; the record
		// is small and re-derivable, so replace a stale FILE explicitly and
		// retry once. Never remove anything else (a directory stays untouched).
		std::error_code ignored;
		if (std::filesystem::is_regular_file(path, ignored))
			std::filesystem::remove(path, ignored);
		ec.clear();
		std::filesystem::rename(temporary, path, ec);
	}
	if (ec)
	{
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		return false;
	}
	return true;
}
} // namespace gameui
