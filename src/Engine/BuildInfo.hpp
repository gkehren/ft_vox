#pragma once

#include <string>

/// Build-time git / version metadata (regenerated each compile via CMake).
namespace BuildInfo
{
/// Short commit hash, or "unknown".
const char *gitHash();
/// Current branch name, or "unknown".
const char *gitBranch();
/// True if the working tree had uncommitted changes at build time.
bool gitDirty();
/// `git describe --always --tags --dirty`, or hash fallback.
const char *gitDescribe();
/// UTC timestamp when BuildInfo was generated (ISO-8601).
const char *buildUtc();

/// Compact label for reports, e.g. "a1b2c3d4e5f6*" (dirty) or "a1b2c3d4e5f6".
std::string revisionLabel();
/// Multi-line block for benchmark / logs.
std::string summaryLine();
} // namespace BuildInfo
