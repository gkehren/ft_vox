#pragma once

// =============================================================================
// Shared test harness (review §4.12): the CHECK macro used to be copy-pasted
// into ~25 test files with divergent counters and formats. One discipline
// here, one homogeneous counter, and a VACUITY GUARD: a test that executed
// zero assertions must not report success (the same class of bug as the
// Release-NDEBUG assert() test that silently passed by doing nothing).
//
// Usage:
//   #include "TestMain.hpp"
//   CHECK(cond, "message");      // two-argument form
//   CHECK_EXPR(expr);            // one-argument form (stringizes the expr)
//   ...
//   return testmain::exitCode(); // prints nothing; 0 iff checks>0 && fails==0
//
// The counters are inline globals (C++17): every TU of a test target shares
// one ledger, and the exit code always reflects what was printed.
// =============================================================================

#include <iostream>

namespace testmain
{
inline long checks = 0;
inline long fails = 0;

inline void record(bool ok, const char *msg, int line)
{
	++checks;
	if (!ok)
	{
		++fails;
		std::cerr << "FAIL: " << msg << " (" << line << ")\n";
	}
}

/// Exit code with the vacuity guard: zero executed assertions is a FAILURE,
/// never a silent pass.
inline int exitCode()
{
	if (checks == 0)
	{
		std::cerr << "FAIL: test executed no assertions (vacuous pass)\n";
		return 1;
	}
	return fails ? 1 : 0;
}
} // namespace testmain

// CHECK streams its message (review §4.12): call sites pass literals,
// std::string values AND stream expressions ("x=" << value) — the macro
// keeps every historical form working while sharing ONE counter/format.
// An immediately-invoked lambda is a single statement with no dangling-else
// hazard and no do/while(0) (MSVC /W4 C4127 on the constant condition).
#define CHECK(cond, msg)                                                       \
	[&]()                                                                      \
	{                                                                          \
		++::testmain::checks;                                                  \
		if (!(cond))                                                           \
		{                                                                      \
			++::testmain::fails;                                               \
			std::cerr << "FAIL: " << msg << " (" << __LINE__ << ")\n";         \
		}                                                                      \
	}()
#define CHECK_EXPR(x) ::testmain::record(static_cast<bool>(x), #x, __LINE__)
