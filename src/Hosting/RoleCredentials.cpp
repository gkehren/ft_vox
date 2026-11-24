#include <Hosting/RoleCredentials.hpp>

#include <Network/UdpSocket.hpp>
#include <World/WorldCheckpoint.hpp>

#include <algorithm>
#include <cerrno>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace localcontrol
{
namespace
{
namespace fs = std::filesystem;
constexpr std::array<uint8_t, 8> kKeyHeader{'F', 'T', 'V', 'K', 1, 0, 0, 0};

#ifdef _WIN32
std::vector<uint8_t> processUser()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return {};
	DWORD count = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &count);
	std::vector<uint8_t> user(count);
	const bool gotUser = GetTokenInformation(token, TokenUser, user.data(), count, &count) != 0;
	CloseHandle(token);
	if (!gotUser)
		user.clear();
	return user;
}

HANDLE createPrivateStage(const fs::path &path)
{
	const auto user = processUser();
	LPWSTR sid = nullptr;
	if (user.empty() || !ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER *>(user.data())->User.Sid, &sid))
		return INVALID_HANDLE_VALUE;
	const std::wstring userSid(sid);
	LocalFree(sid);
	const std::wstring acl = L"O:" + userSid + L"D:P(A;;GA;;;SY)(A;;GA;;;" + userSid + L")";
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
		return INVALID_HANDLE_VALUE;
	SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
	const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE | READ_CONTROL, 0, &attributes,
		CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	LocalFree(descriptor);
	return file;
}

bool aclAllowsOnlyTrustedUsers(PSID owner, PACL acl, PSECURITY_DESCRIPTOR descriptor)
{
	SECURITY_DESCRIPTOR_CONTROL control{};
	DWORD revision = 0;
	if (!acl || !GetSecurityDescriptorControl(descriptor, &control, &revision) || !(control & SE_DACL_PROTECTED))
		return false;
	const auto user = processUser();
	if (user.empty())
		return false;
	const PSID userSid = reinterpret_cast<const TOKEN_USER *>(user.data())->User.Sid;
	const auto trusted = [userSid](PSID sid) {
		return sid && (EqualSid(sid, userSid) || IsWellKnownSid(sid, WinLocalSystemSid));
	};
	if (!trusted(owner))
		return false;
	for (DWORD index = 0; index < acl->AceCount; ++index)
	{
		void *entry = nullptr;
		if (!GetAce(acl, index, &entry))
			return false;
		const auto *header = static_cast<const ACE_HEADER *>(entry);
		if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
			return false;
		auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(entry);
		if (!trusted(&ace->SidStart))
			return false;
	}
	return true;
}

bool hasPrivateAcl(HANDLE file)
{
	PACL acl = nullptr;
	PSID owner = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	if (GetSecurityInfo(file, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION, &owner, nullptr,
		&acl, nullptr, &descriptor) != ERROR_SUCCESS)
		return false;
	const bool privateAcl = aclAllowsOnlyTrustedUsers(owner, acl, descriptor);
	LocalFree(descriptor);
	return privateAcl;
}
#endif
}

bool matches(const ControlKey &a, const ControlKey &b)
{
	unsigned difference = 0;
	for (size_t i = 0; i < a.size(); ++i)
		difference |= a[i] ^ b[i];
	return difference == 0;
}

BootstrapFrame encodeBootstrap(const Bootstrap &bootstrap)
{
	BootstrapFrame frame{};
	auto output = std::ranges::copy(kBootstrapHeader, frame.begin()).out;
	output = std::ranges::copy(bootstrap.player, output).out;
	std::ranges::copy(bootstrap.control, output);
	return frame;
}

std::optional<Bootstrap> decodeBootstrap(const BootstrapFrame &frame)
{
	if (!std::ranges::equal(kBootstrapHeader, std::span{frame}.first(kBootstrapHeader.size())))
		return std::nullopt;
	Bootstrap bootstrap;
	const auto bytes = std::span{frame}.subspan(kBootstrapHeader.size());
	std::ranges::copy(bytes.first(bootstrap.player.size()), bootstrap.player.begin());
	std::ranges::copy(bytes.last(bootstrap.control.size()), bootstrap.control.begin());
	if (bootstrap.player == net::wire::PlayerKey{} ||
		(bootstrap.control != ControlKey{} && matches(bootstrap.player, bootstrap.control)))
		return std::nullopt;
	return bootstrap;
}

bool validBootstrap(const Bootstrap &bootstrap)
{
	return bootstrap.player != net::wire::PlayerKey{} && bootstrap.control != ControlKey{} &&
		!matches(bootstrap.player, bootstrap.control);
}

bool isFreshWorldDirectory(const fs::path &path)
{
	return std::ranges::none_of(fs::directory_iterator(path), [](const auto &entry) {
		return entry.path().filename() != "server.lock";
	});
}

bool writePrivateFile(const fs::path &path, std::span<const uint8_t> bytes, bool replace)
{
	auto stage = path;
	stage += ".tmp-" + std::to_string(net::udp::randomId());
#ifdef _WIN32
	const HANDLE file = createPrivateStage(stage);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	DWORD written = 0;
	const bool saved = hasPrivateAcl(file) && bytes.size() <= MAXDWORD &&
		WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
		written == bytes.size() && FlushFileBuffers(file);
	CloseHandle(file);
	const bool published = saved && MoveFileExW(stage.c_str(), path.c_str(),
		MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0));
#else
	const int fd = ::open(stage.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return false;
	struct stat status{};
	const bool privateFile = ::fchmod(fd, 0600) == 0 && ::fstat(fd, &status) == 0 && (status.st_mode & 0777) == 0600;
	while (privateFile && !bytes.empty())
	{
		const auto written = ::write(fd, bytes.data(), bytes.size());
		if (written > 0)
			bytes = bytes.subspan(static_cast<size_t>(written));
		else if (written == 0 || errno != EINTR)
			break;
	}
	const bool saved = privateFile && bytes.empty() && ::fsync(fd) == 0;
	::close(fd);
	const bool published = saved && (replace ? ::rename(stage.c_str(), path.c_str()) :
		::link(stage.c_str(), path.c_str())) == 0;
#endif
	std::error_code ignored;
	fs::remove(stage, ignored);
	if (published)
		worldsave::syncDirForCheckpoint(path.has_parent_path() ? path.parent_path() : fs::path("."));
	return published;
}

bool readPrivateFile(const fs::path &path, std::vector<uint8_t> &bytes, size_t maxBytes)
{
#ifdef _WIN32
	const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	BY_HANDLE_FILE_INFORMATION info{};
	LARGE_INTEGER size{};
	const bool canRead = GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandle(file, &info) &&
		!(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
		GetFileSizeEx(file, &size) && size.QuadPart >= 0 && static_cast<uint64_t>(size.QuadPart) <= maxBytes &&
		size.QuadPart <= MAXDWORD && hasPrivateAcl(file);
	DWORD count = 0;
	if (canRead)
		bytes.resize(static_cast<size_t>(size.QuadPart));
	const bool read = canRead && ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) &&
		count == bytes.size();
	CloseHandle(file);
	return read;
#else
	std::error_code error;
	const auto status = fs::symlink_status(path, error);
	return !error && status.type() == fs::file_type::regular &&
		(status.permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none &&
		worldsave::readFileInto(path, bytes, maxBytes) == worldsave::SaveStatus::Ok;
#endif
}

std::optional<ControlKey> readControlKey(const fs::path &path)
{
	std::vector<uint8_t> bytes;
	ControlKey key{};
	const auto size = kKeyHeader.size() + key.size();
	if (!readPrivateFile(path, bytes, size) ||
		bytes.size() != size || !std::ranges::equal(kKeyHeader, std::span{bytes}.first(kKeyHeader.size())))
		return std::nullopt;
	std::ranges::copy(std::span{bytes}.last(key.size()), key.begin());
	return key == ControlKey{} ? std::nullopt : std::optional{key};
}

bool createControlKey(const fs::path &path)
{
	const auto key = net::udp::randomPlayerKey();
	std::array<uint8_t, kKeyHeader.size() + ControlKey{}.size()> bytes{};
	std::ranges::copy(key, std::ranges::copy(kKeyHeader, bytes.begin()).out);
	return writePrivateFile(path, bytes, false);
}

std::optional<ControlKey> loadOrCreateControlKey(const fs::path &path)
{
	std::error_code error;
	if (fs::symlink_status(path, error).type() == fs::file_type::not_found)
		createControlKey(path); // An independent launcher may publish it first.
	return readControlKey(path);
}
}
