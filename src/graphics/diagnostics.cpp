#include "graphics/diagnostics.h"
#include <chrono>
#include <algorithm>

namespace miami::gfx {

GraphicsDiagnostics::GraphicsDiagnostics() {
    m_timingStats.targetFps = 60.0f;
}

GraphicsDiagnostics& GraphicsDiagnostics::Instance() {
    static GraphicsDiagnostics s_inst;
    return s_inst;
}

void GraphicsDiagnostics::Log(DiagnosticLogEntry::Level level, const std::string& subsystem, const std::string& message) {
    static auto s_startTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double elapsedSec = std::chrono::duration<double>(now - s_startTime).count();

    DiagnosticLogEntry entry;
    entry.level = level;
    entry.subsystem = subsystem;
    entry.message = message;
    entry.timestampSeconds = elapsedSec;

    if (m_logs.size() > 500) {
        m_logs.erase(m_logs.begin(), m_logs.begin() + 100);
    }
    m_logs.push_back(entry);
}

void GraphicsDiagnostics::UpdateFrameTiming(double frameTimeMs, double gpuTimeMs, double presentTimeMs) {
    m_timingStats.frameIndex++;
    m_timingStats.cpuFrameMs = frameTimeMs;
    m_timingStats.gpuDurationMs = gpuTimeMs;
    m_timingStats.presentDurationMs = presentTimeMs;
    if (frameTimeMs > 0.0001) {
        m_timingStats.currentFps = static_cast<float>(1000.0 / frameTimeMs);
    }
}

std::vector<DiagnosticLogEntry> GraphicsDiagnostics::GetRecentLogs(size_t maxCount) const {
    if (m_logs.size() <= maxCount) return m_logs;
    return std::vector<DiagnosticLogEntry>(m_logs.end() - maxCount, m_logs.end());
}

} // namespace miami::gfx
