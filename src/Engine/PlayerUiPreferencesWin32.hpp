#pragma once

#include <Engine/PlayerUiPreferences.hpp>

#include <windows.h>

namespace gameui::settings_io
{
enum class FilePhase { Open, Read, Rename };

struct FileAttempt
{
	DWORD error;
	FilePhase phase;
};

inline constexpr size_t kAttempts = 40;

inline bool isSharingConflict(DWORD error)
{
	return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_DELETE_PENDING;
}

inline LoadStatus loadFailure(FileAttempt result)
{
	if (result.phase == FilePhase::Open && (result.error == ERROR_FILE_NOT_FOUND || result.error == ERROR_PATH_NOT_FOUND))
		return LoadStatus::Missing;
	return isSharingConflict(result.error) ? LoadStatus::Busy : LoadStatus::Unavailable;
}

// The caller owns the operation and the wait. The fixture can drive the same
// policy without sleeping, or release a real handle at a retry boundary.
inline FileAttempt retryWindowsFile(auto operation, auto wait)
{
	for (size_t attempt = 1;; ++attempt)
	{
		const auto result = operation();
		if (attempt == kAttempts || (!isSharingConflict(result.error) && result.error != ERROR_ACCESS_DENIED))
			return result;
		wait();
	}
}
} // namespace gameui::settings_io
