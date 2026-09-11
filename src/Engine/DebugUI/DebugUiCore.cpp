// Pure aggregation/formatting logic for the developer console (issue #179).
// No ImGui and no engine types here — everything is unit-testable in
// isolation. The engine-facing sampler (updateDebugUiState) lives in
// DebugUiCore.cpp too but is declared in DebugUiEngine.hpp.

#include <Engine/DebugUI/DebugUiCore.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace debugui
{

// --- Formatting ---

std::string formatBytes(uint64_t bytes)
{
	char buf[32];
	if (bytes >= (100ull << 30))
	{
		std::snprintf(buf, sizeof(buf), "%.1f GiB", double(bytes) / double(1ull << 30));
		return buf;
	}
	if (bytes >= (1ull << 30))
	{
		std::snprintf(buf, sizeof(buf), "%.2f GiB", double(bytes) / double(1ull << 30));
		return buf;
	}
	if (bytes >= (1ull << 20))
	{
		std::snprintf(buf, sizeof(buf), "%.1f MiB", double(bytes) / double(1ull << 20));
		return buf;
	}
	if (bytes >= (1ull << 10))
	{
		std::snprintf(buf, sizeof(buf), "%.1f KiB", double(bytes) / double(1ull << 10));
		return buf;
	}
	std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
	return buf;
}

std::string formatCount(uint64_t n)
{
	char buf[32];
	if (n >= 1000000)
		std::snprintf(buf, sizeof(buf), "%.1fM", double(n) / 1e6);
	else if (n >= 10000)
		std::snprintf(buf, sizeof(buf), "%.1fk", double(n) / 1e3);
	else
		std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(n));
	return buf;
}

std::string formatMs(float ms)
{
	char buf[32];
	if (ms >= 100.f || ms == 0.f)
		std::snprintf(buf, sizeof(buf), "%.0f", ms);
	else if (ms >= 1.f)
		std::snprintf(buf, sizeof(buf), "%.2f", ms);
	else
		std::snprintf(buf, sizeof(buf), "%.3f", ms);
	return buf;
}

// --- MetricHistory ---

void MetricHistory::push(float v)
{
	m_samples[m_write] = v;
	m_write = (m_write + 1) % kMetricHistorySize;
	if (m_count < kMetricHistorySize)
		++m_count;
}

float MetricHistory::back(size_t backIndex) const
{
	if (m_count == 0)
		return 0.f;
	backIndex = std::min(backIndex, m_count - 1);
	return m_samples[(m_write + kMetricHistorySize - 1 - backIndex) % kMetricHistorySize];
}

void MetricHistory::copyOrdered(float *dst) const
{
	for (size_t i = 0; i < m_count; ++i)
		dst[i] = back(m_count - 1 - i);
}

// --- HealthMonitor ---

HealthMonitor::HealthMonitor(float enterSeconds, float exitSeconds)
	: m_enterSeconds(enterSeconds), m_exitSeconds(exitSeconds)
{
}

bool HealthMonitor::update(bool condition, float dtSeconds)
{
	dtSeconds = std::max(0.f, dtSeconds);
	if (condition)
	{
		m_conditionSeconds += dtSeconds;
		m_clearSeconds = 0.f;
	}
	else
	{
		m_clearSeconds += dtSeconds;
		m_conditionSeconds = 0.f;
	}

	if (!m_active && m_conditionSeconds >= m_enterSeconds)
		m_active = true;
	else if (m_active && m_clearSeconds >= m_exitSeconds)
		m_active = false;
	return m_active;
}

void HealthMonitor::reset()
{
	m_conditionSeconds = 0.f;
	m_active = false;
}

} // namespace debugui
