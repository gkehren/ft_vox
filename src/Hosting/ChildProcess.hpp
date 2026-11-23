#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hosting
{
// Owns only the process it spawned. The parent writes no control bytes in
// this protocol version. Closing its private writer requests a checkpoint.
class ChildProcess
{
public:
	ChildProcess(const std::filesystem::path &executable, const std::vector<std::filesystem::path> &arguments);
	~ChildProcess();
	ChildProcess(const ChildProcess &) = delete;
	ChildProcess &operator=(const ChildProcess &) = delete;

	size_t readOutput(std::span<char> buffer);
	std::optional<int> pollExit();
	void closeControl();
	bool forceStop();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
}
