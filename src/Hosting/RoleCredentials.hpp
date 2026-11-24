#pragma once

#include <Network/WireProtocol.hpp>

#include <filesystem>
#include <span>
#include <vector>

namespace localcontrol
{
using ControlKey = std::array<uint8_t, 32>;

struct Bootstrap
{
	net::wire::PlayerKey player;
	ControlKey control; // All zero when reopening without a readable control key.
};

inline constexpr std::array<uint8_t, 8> kBootstrapHeader{'F', 'T', 'L', 'B', 1, 0, 0, 0};
using BootstrapFrame = std::array<uint8_t, kBootstrapHeader.size() + 2 * ControlKey{}.size()>;

BootstrapFrame encodeBootstrap(const Bootstrap &bootstrap);
std::optional<Bootstrap> decodeBootstrap(const BootstrapFrame &frame);
bool validBootstrap(const Bootstrap &bootstrap);
bool isFreshWorldDirectory(const std::filesystem::path &path);

// Local role files require restrictive permissions at publication and read.
bool writePrivateFile(const std::filesystem::path &path, std::span<const uint8_t> bytes, bool replace);
bool readPrivateFile(const std::filesystem::path &path, std::vector<uint8_t> &bytes, size_t maxBytes);
std::optional<ControlKey> readControlKey(const std::filesystem::path &path);
bool createControlKey(const std::filesystem::path &path);
std::optional<ControlKey> loadOrCreateControlKey(const std::filesystem::path &path);
bool matches(const ControlKey &a, const ControlKey &b);
}
