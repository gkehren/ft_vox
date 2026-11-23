#include <Engine/MultiplayerJoin.hpp>
#include <World/WorldSave.hpp>

#include <charconv>
#include <fstream>

namespace client
{
namespace
{
template<class Integer>
bool parseUnsigned(std::string_view text, Integer &value)
{
	if (text.empty())
		return false;
	const char *last = text.data() + text.size();
	const auto [end, error] = std::from_chars(text.data(), last, value);
	return end == last && error == std::errc{};
}

constexpr std::string_view kSettingsVersion = "FTVOX-JOIN-1";
constexpr size_t kMaxSettingsBytes = 1024;
}

std::optional<net::udp::Address> parseEndpoint(std::string_view text)
{
	const auto split = text.find(':');
	uint16_t port = 0;
	if (split == std::string_view::npos || !parseUnsigned(text.substr(split + 1), port) || port == 0)
		return std::nullopt;
	try
	{
		const auto address = net::udp::parseAddress(text.substr(0, split), port);
		if (address.ipv4 != 0)
			return address;
	}
	catch (const std::invalid_argument &) {}
	return std::nullopt;
}

std::string formatEndpoint(net::udp::Address address)
{
	return std::to_string(address.ipv4 >> 24) + "." +
		std::to_string((address.ipv4 >> 16) & 255) + "." +
		std::to_string((address.ipv4 >> 8) & 255) + "." +
		std::to_string(address.ipv4 & 255) + ":" + std::to_string(address.port);
}

std::optional<std::string> profileName(std::string_view text)
{
	if (text.empty() || text.size() > kMaxProfileName)
		return std::nullopt;
	std::string name;
	for (char c : text)
	{
		if (c >= 'A' && c <= 'Z')
			c = char(c - 'A' + 'a');
		const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (!alnum && (name.empty() || (c != '-' && c != '_')))
			return std::nullopt;
		name += c;
	}
	if (name == "con" || name == "prn" || name == "aux" || name == "nul" ||
	    (name.size() == 4 && (name.starts_with("com") || name.starts_with("lpt")) &&
	     name.back() >= '0' && name.back() <= '9'))
		return std::nullopt;
	return name;
}

std::vector<std::string> listProfiles(const std::filesystem::path &directory)
{
	std::vector<std::string> names;
	std::error_code ec;
	std::filesystem::directory_iterator it(directory, ec), end;
	for (; !ec && it != end && names.size() < 32; it.increment(ec))
	{
		const std::string name = it->path().filename().generic_string();
		std::error_code typeError;
		if (profileName(name) == name && it->symlink_status(typeError).type() == std::filesystem::file_type::directory)
			names.push_back(name);
	}
	std::ranges::sort(names);
	return names;
}

std::optional<JoinTarget> validateJoin(std::string_view endpoint, std::string_view seed, std::string &error)
{
	error.clear();
	const auto address = endpoint.size() <= 21 ? parseEndpoint(endpoint) : std::nullopt;
	if (!address)
	{
		error = "Enter a numeric IPv4 address and a port from 1 to 65535, such as 127.0.0.1:7777.";
		return std::nullopt;
	}
	JoinTarget result{*address, std::nullopt};
	if (!seed.empty())
	{
		uint32_t value = 0;
		if (!parseUnsigned(seed, value))
		{
			error = "Enter an expected seed from 0 to 4294967295, or leave it blank.";
			return std::nullopt;
		}
		result.expectedSeed = value;
	}
	return result;
}

void JoinPreferences::recordRecent(net::udp::Address address)
{
	std::erase(recent, address);
	recent.insert(recent.begin(), address);
	if (recent.size() > kMaxSavedEndpoints)
		recent.pop_back();
}

void JoinPreferences::toggleFavorite(net::udp::Address address)
{
	if (!std::erase(favorites, address))
	{
		favorites.insert(favorites.begin(), address);
		if (favorites.size() > kMaxSavedEndpoints)
			favorites.pop_back();
	}
}

bool JoinPreferences::load(const std::filesystem::path &path)
{
	std::vector<uint8_t> bytes;
	const auto status = worldsave::readFileInto(path, bytes, kMaxSettingsBytes);
	if (status == worldsave::SaveStatus::NotFound)
		return true;
	if (status != worldsave::SaveStatus::Ok || bytes.empty())
		return false;

	JoinPreferences next;
	const std::string file(bytes.begin(), bytes.end());
	std::string_view lines(file);
	const auto takeLine = [&]() {
		const auto end = lines.find('\n');
		const auto line = lines.substr(0, end);
		lines.remove_prefix(end == std::string_view::npos ? lines.size() : end + 1);
		return line;
	};
	if (takeLine() != kSettingsVersion)
		return false;
	while (!lines.empty())
	{
		const auto line = takeLine();
		if (line.starts_with("profile ") && next.selectedProfile.empty())
		{
			const auto name = profileName(line.substr(8));
			if (!name)
				return false;
			next.selectedProfile = *name;
			continue;
		}
		auto *list = line.starts_with("recent ") ? &next.recent :
			line.starts_with("favorite ") ? &next.favorites : nullptr;
		if (!list || list->size() == kMaxSavedEndpoints)
			return false;
		const auto address = parseEndpoint(line.substr(line.find(' ') + 1));
		if (!address || std::ranges::find(*list, *address) != list->end())
			return false;
		list->push_back(*address);
	}
	*this = std::move(next);
	return true;
}

bool JoinPreferences::save(const std::filesystem::path &path) const
{
	std::string data = std::string(kSettingsVersion) + '\n';
	if (!selectedProfile.empty())
		data += "profile " + selectedProfile + '\n';
	for (const auto address : recent)
		data += "recent " + formatEndpoint(address) + '\n';
	for (const auto address : favorites)
		data += "favorite " + formatEndpoint(address) + '\n';

	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	if (ec)
		return false;
	// Two clients can share preferences. Unique staging files keep each
	// save atomic, with the last completed save replacing the previous one.
	auto temporary = path;
	temporary += ".tmp." + std::to_string(net::udp::randomId());
	{
		std::ofstream out(temporary, std::ios::binary);
		out.write(data.data(), static_cast<std::streamsize>(data.size()));
		out.close();
		if (out)
		{
			std::filesystem::rename(temporary, path, ec);
			if (!ec)
				return true;
		}
	}
	std::filesystem::remove(temporary, ec);
	return false;
}
}
