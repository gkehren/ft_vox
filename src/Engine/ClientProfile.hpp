#pragma once

#include <Network/WireProtocol.hpp>

#include <filesystem>
#include <stdexcept>

namespace client
{
// CLI callers retain their existing create-on-first-use behavior. The
// join form uses separate Load and Create actions to protect identities.
enum class ProfileAccess { Load, Create, LoadOrCreate };
struct ProfileCreationError : std::runtime_error
{
	using std::runtime_error::runtime_error;
};
using ProfileKeyWriter = bool (*)(const std::filesystem::path &, const net::wire::PlayerKey &);
net::wire::PlayerKey playerKey(const std::filesystem::path &profile, ProfileAccess access,
	ProfileKeyWriter writer = nullptr);
}
