#pragma once

#include "Engine/EngineDefs.hpp"

/// Policy for which composite sampler slots use always-valid 1×1 defaults
/// instead of half-res effect targets (when the effect pass was skipped).
struct PostCompositeSources
{
	/// true → sample 1×1 black (no bloom contribution)
	bool bloomUseDefault{false};
	/// true → sample 1×1 black (no god-ray contribution)
	bool godRaysUseDefault{false};
	/// true → sample 1×1 white (ao = 1)
	bool ssaoUseDefault{false};

	bool operator==(const PostCompositeSources &) const = default;
};

/// Pure policy used by PostStack and unit tests (shipped).
inline PostCompositeSources postCompositeSources(bool bloomEnabled, bool ssaoEnabled, bool godRaysProduced)
{
	PostCompositeSources s{};
	s.bloomUseDefault = !bloomEnabled;
	s.ssaoUseDefault = !ssaoEnabled;
	s.godRaysUseDefault = !godRaysProduced;
	return s;
}

/// Overload from settings + god-ray production flag.
inline PostCompositeSources postCompositeSources(const PostProcessSettings &settings, bool godRaysProduced)
{
	return postCompositeSources(settings.bloomEnabled, settings.ssaoEnabled, godRaysProduced);
}
