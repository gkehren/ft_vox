#pragma once

#include <Network/UdpSocket.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace client
{
inline constexpr size_t kMaxProfileName = 32;
inline constexpr size_t kMaxSavedEndpoints = 8;

std::optional<net::udp::Address> parseEndpoint(std::string_view text);
std::string formatEndpoint(net::udp::Address address);
std::optional<std::string> profileName(std::string_view text);
std::vector<std::string> listProfiles(const std::filesystem::path &directory);

struct JoinTarget
{
	net::udp::Address address;
	std::optional<uint32_t> expectedSeed;
};

struct JoinRequest
{
	std::string endpoint, profile, expectedSeed;
};

std::optional<JoinTarget> validateJoin(std::string_view endpoint, std::string_view seed, std::string &error);

struct JoinPreferences
{
	std::string selectedProfile;
	std::vector<net::udp::Address> recent, favorites;

	void recordRecent(net::udp::Address address);
	void toggleFavorite(net::udp::Address address);
	bool load(const std::filesystem::path &path);
	bool save(const std::filesystem::path &path) const;
};
}
