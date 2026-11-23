#pragma once

#include <cerrno>
#include <filesystem>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace client
{
enum class FileReservation { Created, Exists, Failed };

// Claim an empty client output file without truncating an existing file or
// following a symlink. Concurrent clients use the same exclusive claim.
inline FileReservation reserveFile(const std::filesystem::path &path)
{
#ifdef _WIN32
	const int fd = ::_wopen(path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY | _O_NOINHERIT,
		_S_IREAD | _S_IWRITE);
#else
	const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
#endif
	if (fd < 0)
		return errno == EEXIST ? FileReservation::Exists : FileReservation::Failed;
#ifdef _WIN32
	::_close(fd);
#else
	::close(fd);
#endif
	return FileReservation::Created;
}
}
