#pragma once

#include <Core/ServerStatus.hpp>

#include <memory>

namespace server
{
// The launcher supplies a separate write-only pipe. The tick owner offers
// samples to a single overwrite slot and never writes to the pipe itself.
class MonitorWriter
{
public:
	explicit MonitorWriter(std::optional<uintptr_t> inheritedPipe);
	~MonitorWriter();
	explicit operator bool() const { return m_impl != nullptr; }
	void offer(const monitoring::Status &status);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
}
