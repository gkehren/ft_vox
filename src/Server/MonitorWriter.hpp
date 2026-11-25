#pragma once

#include <Core/ServerStatus.hpp>
#include <Hosting/AdminProtocol.hpp>

#include <memory>

namespace server
{
// The tick owner publishes samples and replies through distinct single
// slots. The writer thread owns the pipe and any OS backpressure.
class MonitorWriter
{
public:
	explicit MonitorWriter(std::optional<uintptr_t> inheritedPipe);
	~MonitorWriter();
	explicit operator bool() const { return m_impl != nullptr; }
	void offer(const monitoring::Status &status);
	bool adminReady() const;
	void offerAdmin(localcontrol::Reply reply);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
}
