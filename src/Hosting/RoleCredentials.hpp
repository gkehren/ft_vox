#pragma once

#include <Network/WireProtocol.hpp>

#include <filesystem>
#include <span>

namespace localcontrol
{
using ControlKey = std::array<uint8_t, 32>;

struct Bootstrap
{
	net::wire::PlayerKey player;
	ControlKey control;
};

inline constexpr std::array<uint8_t, 8> kBootstrapHeader{'F', 'T', 'L', 'B', 1, 0, 0, 0};
using BootstrapFrame = std::array<uint8_t, kBootstrapHeader.size() + 2 * ControlKey{}.size()>;

BootstrapFrame encodeBootstrap(const Bootstrap &bootstrap);
std::optional<Bootstrap> decodeBootstrap(const BootstrapFrame &frame);
bool validBootstrap(const Bootstrap &bootstrap);

// Local role files use restrictive creation permissions on both OS families.
bool writePrivateFile(const std::filesystem::path &path, std::span<const uint8_t> bytes, bool replace);
std::optional<ControlKey> readControlKey(const std::filesystem::path &path);
bool createControlKey(const std::filesystem::path &path);
std::optional<ControlKey> loadOrCreateControlKey(const std::filesystem::path &path);
bool matches(const ControlKey &a, const ControlKey &b);
}
