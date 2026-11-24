#pragma once

#include <Hosting/RoleCredentials.hpp>

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

// Once started, the only accepted control signal is EOF.
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
}
