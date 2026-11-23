#pragma once
#include <string>
#include <cstdint>
#include <windows.h>

namespace miami {

enum class ProcessState {
    Stopped,
    Launching,
    Running,
    Terminated,
    FailedToLaunch
};

struct ProcessStatus {
    ProcessState state = ProcessState::Stopped;
    uint32_t processId = 0;
    int exitCode = 0;
    std::string executablePath;
    std::string errorMessage;
    uint64_t uptimeMs = 0;
};

class ProcessManager {
public:
    ProcessManager();
    ~ProcessManager();

    bool LaunchGame(const std::string& exePath, const std::string& workingDir, const std::string& commandLine = "");
    void PollState();
    bool Terminate();
    
    const ProcessStatus& GetStatus() const { return m_status; }
    bool IsRunning() const { return m_status.state == ProcessState::Running; }

private:
    ProcessStatus m_status;
    HANDLE m_hProcess = NULL;
    HANDLE m_hThread = NULL;
    uint64_t m_launchTimestamp = 0;
};

} // namespace miami
