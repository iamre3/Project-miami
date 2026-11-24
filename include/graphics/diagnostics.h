#pragma once
#include "graphics/types.h"
#include <string>
#include <vector>

namespace miami::gfx {

struct DiagnosticLogEntry {
    enum class Level { Info, Warning, Error };
    Level level;
    std::string subsystem;
    std::string message;
    double timestampSeconds;
};

class GraphicsDiagnostics {
public:
    static GraphicsDiagnostics& Instance();

    void Log(DiagnosticLogEntry::Level level, const std::string& subsystem, const std::string& message);
    void UpdateFrameTiming(double frameTimeMs, double gpuTimeMs, double presentTimeMs);
    void SetTargetFps(float fps) { m_targetFps = fps; }

    const FrameTimingStats& GetTimingStats() const { return m_timingStats; }
    std::vector<DiagnosticLogEntry> GetRecentLogs(size_t maxCount = 50) const;

private:
    GraphicsDiagnostics();
    FrameTimingStats m_timingStats;
    float m_targetFps = 60.0f;
    std::vector<DiagnosticLogEntry> m_logs;
};

} // namespace miami::gfx
