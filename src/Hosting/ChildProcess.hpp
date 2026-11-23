#pragma once

#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hosting
{
// Owns only the process it spawned. Closing its private writer requests a
// checkpoint after the optional startup record.
class ChildProcess
{
public:
	ChildProcess(const std::filesystem::path &executable, const std::vector<std::filesystem::path> &arguments);
	~ChildProcess();
	ChildProcess(const ChildProcess &) = delete;
	ChildProcess &operator=(const ChildProcess &) = delete;

	size_t readOutput(std::span<char> buffer);
	bool writeControl(std::span<const uint8_t> buffer);
	std::optional<int> pollExit();
	void closeControl();
	bool forceStop();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
}
