#include <Server/MonitorWriter.hpp>

#include <climits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace server
{
struct MonitorWriter::Impl
{
	explicit Impl(uintptr_t inheritedPipe)
	{
#ifdef _WIN32
		pipe = reinterpret_cast<HANDLE>(inheritedPipe);
		if (!pipe || GetFileType(pipe) != FILE_TYPE_PIPE ||
			pipe == GetStdHandle(STD_INPUT_HANDLE) || pipe == GetStdHandle(STD_OUTPUT_HANDLE) ||
			pipe == GetStdHandle(STD_ERROR_HANDLE) || !SetHandleInformation(pipe, HANDLE_FLAG_INHERIT, 0))
			throw std::invalid_argument("invalid inherited monitoring pipe");
#else
		if (inheritedPipe <= STDERR_FILENO || inheritedPipe > static_cast<uintptr_t>(INT_MAX))
			throw std::invalid_argument("invalid inherited monitoring descriptor");
		pipe = static_cast<int>(inheritedPipe);
		struct stat info{};
		const int flags = ::fcntl(pipe, F_GETFL);
		if (flags == -1 || (flags & O_ACCMODE) != O_WRONLY || ::fstat(pipe, &info) == -1 ||
			!S_ISFIFO(info.st_mode) || ::fcntl(pipe, F_SETFL, flags | O_NONBLOCK) == -1 ||
			::fcntl(pipe, F_SETFD, FD_CLOEXEC) == -1)
			throw std::invalid_argument("invalid inherited monitoring pipe");
#endif
	}

	~Impl()
	{
		if (worker.joinable())
		{
			worker.request_stop();
#ifdef _WIN32
			// Catch cancellation races if the worker passed its stop check
			// immediately before entering a synchronous WriteFile.
			while (WaitForSingleObject(worker.native_handle(), 5) == WAIT_TIMEOUT)
				CancelSynchronousIo(worker.native_handle());
#endif
			worker.join();
		}
#ifdef _WIN32
		CloseHandle(pipe);
#else
		::close(pipe);
#endif
	}

	bool send(const monitoring::Frame &frame, std::stop_token stop)
	{
#ifdef _WIN32
		DWORD written = 0;
		return !stop.stop_requested() &&
			WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) && written == frame.size();
#else
		std::span<const uint8_t> remaining(frame);
		while (!remaining.empty() && !stop.stop_requested())
		{
			const auto written = ::write(pipe, remaining.data(), remaining.size());
			if (written > 0)
				remaining = remaining.subspan(static_cast<size_t>(written));
			else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			{
				pollfd available{pipe, POLLOUT, 0};
				::poll(&available, 1, 20);
			}
			else if (written >= 0 || errno != EINTR)
				break;
		}
		return remaining.empty();
#endif
	}

	void run(std::stop_token stop)
	{
		try
		{
			while (!stop.stop_requested())
			{
				std::optional<monitoring::Status> sample;
				{
					std::lock_guard lock(mutex);
					sample = std::exchange(latest, std::nullopt);
				}
				if (!sample)
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				else if (!send(monitoring::encode(*sample), stop))
					return;
			}
		}
		catch (const std::exception &)
		{
			// Monitoring is optional. The launcher will expire the sample.
		}
	}

#ifdef _WIN32
	HANDLE pipe{};
#else
	int pipe{-1};
#endif
	std::mutex mutex;
	std::optional<monitoring::Status> latest;
	std::jthread worker;
};

MonitorWriter::MonitorWriter(std::optional<uintptr_t> inheritedPipe)
{
	if (inheritedPipe)
	{
		m_impl = std::make_unique<Impl>(*inheritedPipe);
		m_impl->worker = std::jthread([impl = m_impl.get()](std::stop_token stop) { impl->run(stop); });
	}
}

MonitorWriter::~MonitorWriter() = default;

void MonitorWriter::offer(const monitoring::Status &status)
{
	std::unique_lock lock(m_impl->mutex, std::try_to_lock);
	if (!lock)
		return;
	m_impl->latest = status;
}
}
