#pragma once

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

// Version one only accepts EOF. A future bootstrap exchange can run over
// the inherited control stream before opening the UDP socket.
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
}
