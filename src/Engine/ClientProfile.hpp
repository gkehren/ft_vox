#pragma once

#include <Network/WireProtocol.hpp>

#include <filesystem>

namespace client
{
// CLI callers retain their existing create-on-first-use behavior. The
// join form uses separate Load and Create actions to protect identities.
enum class ProfileAccess { Load, Create, LoadOrCreate };
net::wire::PlayerKey playerKey(const std::filesystem::path &profile, ProfileAccess access);
}
