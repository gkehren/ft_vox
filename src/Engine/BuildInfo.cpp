#include "Engine/BuildInfo.hpp"

// Generated every build into the binary dir (see cmake/GenerateBuildInfo.cmake).
#include "BuildInfo.inc"

namespace BuildInfo
{
const char *gitHash() { return FT_VOX_GIT_HASH; }
const char *gitBranch() { return FT_VOX_GIT_BRANCH; }
bool gitDirty() { return FT_VOX_GIT_DIRTY != 0; }
const char *gitDescribe() { return FT_VOX_GIT_DESCRIBE; }
const char *buildUtc() { return FT_VOX_BUILD_UTC; }

std::string revisionLabel()
{
	std::string s = gitHash();
	if (gitDirty())
		s += '*';
	return s;
}

std::string summaryLine()
{
	std::string s = "git ";
	s += gitHash();
	if (gitDirty())
		s += " (dirty)";
	s += "  branch ";
	s += gitBranch();
	s += "  describe ";
	s += gitDescribe();
	s += "  built ";
	s += buildUtc();
	return s;
}
} // namespace BuildInfo
