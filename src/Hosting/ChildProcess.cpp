#include <Hosting/ChildProcess.hpp>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace hosting
{
namespace
{
#ifdef _WIN32
class Handle
{
public:
	~Handle() { reset(); }
	Handle() = default;
	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;
	void reset() { if (value) CloseHandle(std::exchange(value, nullptr)); }
	HANDLE value{};
};

[[noreturn]] void fail(const char *operation)
{
	throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

void makePipe(Handle &reader, Handle &writer, HANDLE *parent)
{
	SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
	if (!CreatePipe(&reader.value, &writer.value, &security, 0) ||
		!SetHandleInformation(*parent, HANDLE_FLAG_INHERIT, 0))
		fail("create private server pipe");
}

std::wstring quoteArgument(const std::wstring &text)
{
	// Reverse the Microsoft CRT's backslash/quote splitting rules. Quote
	// every argument, including empty strings and paths ending in a slash.
	std::wstring quoted = L"\"";
	size_t slashes = 0;
	for (const wchar_t c : text)
	{
		if (c == L'\\')
		{
			++slashes;
			continue;
		}
		quoted.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
		slashes = 0;
		quoted += c;
	}
	quoted.append(slashes * 2, L'\\');
	quoted += L'"';
	return quoted;
}

struct InheritedHandles
{
	explicit InheritedHandles(std::span<HANDLE> handles)
	{
		SIZE_T bytes = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
		storage.resize(bytes);
		list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
		if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes))
			fail("initialize server handle allowlist");
		if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
			handles.data(), handles.size_bytes(), nullptr, nullptr))
		{
			const auto error = GetLastError();
			DeleteProcThreadAttributeList(list);
			SetLastError(error);
			fail("allowlist server pipe handles");
		}
	}
	~InheritedHandles() { DeleteProcThreadAttributeList(list); }
	std::vector<unsigned char> storage;
	LPPROC_THREAD_ATTRIBUTE_LIST list{};
};
#else
class Descriptor
{
public:
	~Descriptor() { reset(); }
	Descriptor() = default;
	Descriptor(const Descriptor &) = delete;
	Descriptor &operator=(const Descriptor &) = delete;
	void reset() { if (value >= 0) ::close(std::exchange(value, -1)); }
	int value{-1};
};

void check(int error, const char *operation)
{
	if (error)
		throw std::system_error(error, std::generic_category(), operation);
}

void makePipe(Descriptor &reader, Descriptor &writer)
{
	int descriptors[2];
#if defined(__linux__)
	if (::pipe2(descriptors, O_CLOEXEC) == -1)
#else
	if (::pipe(descriptors) == -1)
#endif
		check(errno, "create private server pipe");
	reader.value = descriptors[0];
	writer.value = descriptors[1];
	for (auto *descriptor : {&reader, &writer})
	{
		// Keep file actions correct even when the launcher has a closed
		// standard descriptor.
		if (descriptor->value <= STDERR_FILENO)
		{
			const int copy = ::fcntl(descriptor->value, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
			if (copy < 0)
				check(errno, "duplicate server pipe");
			descriptor->reset();
			descriptor->value = copy;
		}
#if !defined(__linux__)
		else if (::fcntl(descriptor->value, F_SETFD, FD_CLOEXEC) == -1)
			check(errno, "protect server pipe from inheritance");
#endif
	}
}

struct SpawnActions
{
	SpawnActions() { check(posix_spawn_file_actions_init(&actions), "initialize server spawn actions"); }
	~SpawnActions() { posix_spawn_file_actions_destroy(&actions); }
	posix_spawn_file_actions_t actions;
};
#endif

size_t readPipe(
#ifdef _WIN32
	HANDLE pipe,
#else
	int pipe,
#endif
	std::span<char> buffer)
{
#ifdef _WIN32
	DWORD available = 0, count = 0;
	if (!pipe || !PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0)
		return 0;
	if (!ReadFile(pipe, buffer.data(),
		available < buffer.size() ? available : static_cast<DWORD>(buffer.size()), &count, nullptr))
		return 0;
	return count;
#else
	const auto count = pipe < 0 ? 0 : ::read(pipe, buffer.data(), buffer.size());
	return count > 0 ? static_cast<size_t>(count) : 0;
#endif
}
}

struct ChildProcess::Impl
{
#ifdef _WIN32
	Handle control, output, monitor, process;
#else
	Descriptor control, output, monitor;
	pid_t pid{-1};
#endif
	std::optional<int> exit;
};

ChildProcess::ChildProcess(const std::filesystem::path &executable,
	const std::vector<std::filesystem::path> &arguments, bool monitor) : m_impl(std::make_unique<Impl>())
{
#ifdef _WIN32
	Handle childInput, childOutput, childMonitor;
	makePipe(childInput, m_impl->control, &m_impl->control.value);
	makePipe(m_impl->output, childOutput, &m_impl->output.value);
	if (monitor)
		makePipe(m_impl->monitor, childMonitor, &m_impl->monitor.value);
	HANDLE handles[] = {childInput.value, childOutput.value, childMonitor.value};
	InheritedHandles inherited(std::span{handles}.first(monitor ? 3 : 2));
	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = childInput.value;
	startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = childOutput.value;
	startup.lpAttributeList = inherited.list;

	std::wstring command = quoteArgument(executable.native());
	for (const auto &argument : arguments)
		command += L" " + quoteArgument(argument.native());
	if (monitor)
		command += L" --launcher-monitor " + std::to_wstring(reinterpret_cast<uintptr_t>(childMonitor.value));
	PROCESS_INFORMATION process{};
	if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
		EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &process))
		fail("launch dedicated server");
	m_impl->process.value = process.hProcess;
	CloseHandle(process.hThread);
#else
	Descriptor childInput, childOutput, childMonitor;
	makePipe(childInput, m_impl->control);
	makePipe(m_impl->output, childOutput);
	if (::fcntl(m_impl->control.value, F_SETFL, O_NONBLOCK) == -1)
		check(errno, "set server control pipe nonblocking");
	if (::fcntl(m_impl->output.value, F_SETFL, O_NONBLOCK) == -1)
		check(errno, "set server log pipe nonblocking");
	if (monitor)
	{
		makePipe(m_impl->monitor, childMonitor);
		if (::fcntl(m_impl->monitor.value, F_SETFL, O_NONBLOCK) == -1)
			check(errno, "set server monitor reader nonblocking");
	}
	SpawnActions actions;
	check(posix_spawn_file_actions_adddup2(&actions.actions, childInput.value, STDIN_FILENO), "set server stdin");
	check(posix_spawn_file_actions_adddup2(&actions.actions, childOutput.value, STDOUT_FILENO), "set server stdout");
	check(posix_spawn_file_actions_adddup2(&actions.actions, childOutput.value, STDERR_FILENO), "set server stderr");
	constexpr int monitorFd = STDERR_FILENO + 1;
	if (monitor)
		check(posix_spawn_file_actions_adddup2(&actions.actions, childMonitor.value, monitorFd), "set server monitoring pipe");
	// Pipe ends are close-on-exec. Only the dup2 targets survive.
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
#if __GLIBC_PREREQ(2, 34)
	check(posix_spawn_file_actions_addclosefrom_np(&actions.actions, monitorFd + (monitor ? 1 : 0)), "close other server descriptors");
#endif
#endif
	std::vector<std::string> strings{executable.native()};
	for (const auto &argument : arguments)
		strings.push_back(argument.native());
	if (monitor)
	{
		strings.emplace_back("--launcher-monitor");
		strings.push_back(std::to_string(monitorFd));
	}
	std::vector<char *> argv;
	for (auto &argument : strings)
		argv.push_back(argument.data());
	argv.push_back(nullptr);
	check(posix_spawn(&m_impl->pid, executable.c_str(), &actions.actions, nullptr, argv.data(), environ),
		"launch dedicated server");
#endif
}

ChildProcess::~ChildProcess() { closeControl(); }

size_t ChildProcess::readOutput(std::span<char> buffer)
{
	return readPipe(m_impl->output.value, buffer);
}

size_t ChildProcess::readMonitoring(std::span<char> buffer)
{
	return readPipe(m_impl->monitor.value, buffer);
}

bool ChildProcess::writeControl(std::span<const uint8_t> buffer)
{
	// Records fit within the minimum POSIX PIPE_BUF. The launcher permits
	// one request at a time and waits for its reply before writing again.
	if (buffer.empty() || buffer.size() > 512)
		return false;
#ifdef _WIN32
	DWORD written = 0;
	return WriteFile(m_impl->control.value, buffer.data(), static_cast<DWORD>(buffer.size()), &written, nullptr) &&
		written == buffer.size();
#else
	sigset_t pipeSignal, previous, pending;
	sigemptyset(&pipeSignal);
	sigaddset(&pipeSignal, SIGPIPE);
	if (pthread_sigmask(SIG_BLOCK, &pipeSignal, &previous) != 0)
		return false;
	const bool wasPending = sigpending(&pending) != 0 || sigismember(&pending, SIGPIPE) == 1;
	ssize_t written;
	do
		written = ::write(m_impl->control.value, buffer.data(), buffer.size());
	while (written < 0 && errno == EINTR);
	if (written < 0 && errno == EPIPE && !wasPending && sigpending(&pending) == 0 &&
		sigismember(&pending, SIGPIPE) == 1)
	{
		int consumed = 0;
		sigwait(&pipeSignal, &consumed);
	}
	pthread_sigmask(SIG_SETMASK, &previous, nullptr);
	return written == static_cast<ssize_t>(buffer.size());
#endif
}

std::optional<int> ChildProcess::pollExit()
{
	if (m_impl->exit)
		return m_impl->exit;
#ifdef _WIN32
	const DWORD wait = WaitForSingleObject(m_impl->process.value, 0);
	if (wait == WAIT_TIMEOUT)
		return std::nullopt;
	DWORD code = 0;
	if (wait != WAIT_OBJECT_0 || !GetExitCodeProcess(m_impl->process.value, &code))
		fail("observe dedicated server exit");
	m_impl->exit = static_cast<int>(code);
	m_impl->process.reset();
#else
	int status = 0;
	const auto result = ::waitpid(m_impl->pid, &status, WNOHANG);
	if (result == 0 || (result == -1 && errno == EINTR))
		return std::nullopt;
	if (result == -1)
		check(errno, "observe dedicated server exit");
	m_impl->exit = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
	m_impl->pid = -1;
#endif
	return m_impl->exit;
}

void ChildProcess::closeControl() { m_impl->control.reset(); }

bool ChildProcess::forceStop()
{
#ifdef _WIN32
	return m_impl->process.value && TerminateProcess(m_impl->process.value, 1);
#else
	// An unreaped child retains its PID. Never signal a PID after waitpid.
	return m_impl->pid > 0 && ::kill(m_impl->pid, SIGKILL) == 0;
#endif
}
}
