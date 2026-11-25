#pragma once

#include <Hosting/AdminProtocol.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace server
{
enum class LauncherInput { Open, Closed, Invalid };

inline bool hasLauncherPipe()
{
#ifdef _WIN32
	return GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_PIPE;
#else
	struct stat status{};
	return ::fstat(STDIN_FILENO, &status) == 0 && S_ISFIFO(status.st_mode);
#endif
}

// Bootstrap and legacy EOF-only launches accept no buffered input here.
inline LauncherInput pollLauncherPipe()
{
#ifdef _WIN32
	DWORD available = 0;
	if (PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), nullptr, 0, nullptr, &available, nullptr))
		return available == 0 ? LauncherInput::Open : LauncherInput::Invalid;
	return GetLastError() == ERROR_BROKEN_PIPE ? LauncherInput::Closed : LauncherInput::Invalid;
#else
	pollfd input{STDIN_FILENO, POLLIN, 0};
	const int result = ::poll(&input, 1, 0);
	if (result == 0 || (result < 0 && errno == EINTR))
		return LauncherInput::Open;
	if (result > 0 && (input.revents & (POLLIN | POLLERR | POLLNVAL)) == 0 &&
		(input.revents & POLLHUP) != 0)
		return LauncherInput::Closed;
	return LauncherInput::Invalid;
#endif
}

inline std::optional<localcontrol::Bootstrap> readLauncherBootstrap()
{
	using namespace std::chrono_literals;
	localcontrol::BootstrapFrame frame{};
	size_t size = 0;
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (std::chrono::steady_clock::now() < deadline)
	{
#ifdef _WIN32
		const auto pipe = GetStdHandle(STD_INPUT_HANDLE);
		DWORD available = 0;
		if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
			return std::nullopt;
		if (!available)
		{
			std::this_thread::sleep_for(10ms);
			continue;
		}
		DWORD read = 0;
		const auto count = static_cast<DWORD>(std::min<size_t>(available, frame.size() - size));
		if (available > frame.size() - size || !ReadFile(pipe, frame.data() + size, count, &read, nullptr) || !read)
			return std::nullopt;
#else
		pollfd pending{STDIN_FILENO, POLLIN, 0};
		const auto ready = ::poll(&pending, 1, 20);
		if (ready == 0 || (ready < 0 && errno == EINTR))
			continue;
		if (ready < 0 || !(pending.revents & POLLIN))
			return std::nullopt;
		const auto read = ::read(STDIN_FILENO, frame.data() + size, frame.size() - size);
		if (read < 0 && errno == EINTR)
			continue;
		if (read <= 0)
			return std::nullopt;
#endif
		size += static_cast<size_t>(read);
		if (size == frame.size())
			return pollLauncherPipe() == LauncherInput::Open ? localcontrol::decodeBootstrap(frame) : std::nullopt;
	}
	return std::nullopt;
}

class AdminReader
{
public:
	enum class Event { Idle, Closed, Request, Malformed };

	Event poll(bool ready)
	{
		const auto now = std::chrono::steady_clock::now();
		size_t available = 0;
#ifdef _WIN32
		const auto pipe = GetStdHandle(STD_INPUT_HANDLE);
		DWORD pending = 0;
		if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &pending, nullptr))
			return Event::Closed;
		available = pending;
#else
		pollfd pending{STDIN_FILENO, POLLIN, 0};
		const auto polled = ::poll(&pending, 1, 0);
		if (polled < 0 && errno == EINTR)
			return Event::Idle;
		if (polled < 0 || (pending.revents & (POLLHUP | POLLERR | POLLNVAL)))
			return Event::Closed;
		int count = 0;
		if ((pending.revents & POLLIN) && ::ioctl(STDIN_FILENO, FIONREAD, &count) != 0)
			return Event::Closed;
		available = static_cast<size_t>(count);
#endif
		const bool malformed = !m_disabled && (available > m_frame.size() - m_used ||
			(m_used && now - m_started >= std::chrono::seconds(2)));
		m_disabled |= malformed;
		m_reportMalformed |= malformed;
		if (available && (ready || m_disabled))
		{
			std::array<uint8_t, 512> junk{};
			auto *output = m_disabled ? junk.data() : m_frame.data() + m_used;
			const auto wanted = std::min(available, m_disabled ? junk.size() : m_frame.size() - m_used);
#ifdef _WIN32
			DWORD actual = 0;
			if (!ReadFile(pipe, output, static_cast<DWORD>(wanted), &actual, nullptr) || !actual)
				return Event::Closed;
#else
			const auto actual = ::read(STDIN_FILENO, output, wanted);
			if (actual < 0 && errno == EINTR)
				return Event::Idle;
			if (actual <= 0)
				return Event::Closed;
#endif
			if (!m_disabled)
			{
				if (!m_used)
					m_started = now;
				m_used += static_cast<size_t>(actual);
				if (m_used == m_frame.size())
				{
					m_used = 0;
					return Event::Request;
				}
			}
		}
		if (ready && m_reportMalformed)
		{
			m_reportMalformed = false;
			return Event::Malformed;
		}
		return Event::Idle;
	}

	const localcontrol::RequestFrame &frame() const { return m_frame; }

private:
	localcontrol::RequestFrame m_frame{};
	size_t m_used{};
	std::chrono::steady_clock::time_point m_started{};
	bool m_disabled{}, m_reportMalformed{};
};
}
