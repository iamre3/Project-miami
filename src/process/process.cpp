#include "miami/process.h"
#include "miami/config.h"
#include <iostream>

namespace miami {

ProcessManager::ProcessManager() {
}

ProcessManager::~ProcessManager() {
    Terminate();
}

bool ProcessManager::LaunchGame(const std::string& exePath, const std::string& workingDir, const std::string& commandLine) {
    if (m_status.state == ProcessState::Running) {
        LogWarn("LaunchGame requested while a process is already running (PID " + std::to_string(m_status.processId) + ")");
        return false;
    }

    LogInfo("Launching target application: " + exePath + " in dir: " + workingDir);
    m_status.state = ProcessState::Launching;
    m_status.executablePath = exePath;
    m_status.errorMessage.clear();

    // Verify file exists
    DWORD dwAttrib = GetFileAttributesA(exePath.c_str());
    if (dwAttrib == INVALID_FILE_ATTRIBUTES || (dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
        m_status.state = ProcessState::FailedToLaunch;
        m_status.errorMessage = "Target executable not found: " + exePath;
        LogError(m_status.errorMessage);
        return false;
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    std::string fullCmd = "\"" + exePath + "\"";
    if (!commandLine.empty()) {
        fullCmd += " " + commandLine;
    }

    std::vector<char> cmdBuf(fullCmd.begin(), fullCmd.end());
    cmdBuf.push_back('\0');

    const char* workDirPtr = workingDir.empty() ? NULL : workingDir.c_str();

    BOOL success = CreateProcessA(
        NULL,
        cmdBuf.data(),
        NULL,
        NULL,
        FALSE,
        0,
        NULL,
        workDirPtr,
        &si,
        &pi
    );

    if (!success) {
        DWORD err = GetLastError();
        m_status.state = ProcessState::FailedToLaunch;
        m_status.errorMessage = "CreateProcess failed with Win32 error code: " + std::to_string(err);
        LogError(m_status.errorMessage);
        return false;
    }

    m_hProcess = pi.hProcess;
    m_hThread = pi.hThread;
    m_status.processId = pi.dwProcessId;
    m_status.state = ProcessState::Running;
    m_launchTimestamp = GetTickCount64();

    LogInfo("Process started successfully. PID: " + std::to_string(m_status.processId));
    return true;
}

void ProcessManager::PollState() {
    if (m_status.state != ProcessState::Running || m_hProcess == NULL) {
        return;
    }

    DWORD exitCode = 0;
    if (GetExitCodeProcess(m_hProcess, &exitCode)) {
        if (exitCode != STILL_ACTIVE) {
            m_status.state = ProcessState::Terminated;
            m_status.exitCode = static_cast<int>(exitCode);
            m_status.uptimeMs = GetTickCount64() - m_launchTimestamp;
            LogInfo("Target process terminated. Exit code: " + std::to_string(exitCode) + 
                    ", Uptime: " + std::to_string(m_status.uptimeMs) + " ms");
            
            CloseHandle(m_hProcess);
            CloseHandle(m_hThread);
            m_hProcess = NULL;
            m_hThread = NULL;
        } else {
            m_status.uptimeMs = GetTickCount64() - m_launchTimestamp;
        }
    }
}

bool ProcessManager::Terminate() {
    if (m_hProcess != NULL) {
        LogInfo("Terminating active process (PID " + std::to_string(m_status.processId) + ")");
        TerminateProcess(m_hProcess, 0);
        CloseHandle(m_hProcess);
        CloseHandle(m_hThread);
        m_hProcess = NULL;
        m_hThread = NULL;
        m_status.state = ProcessState::Terminated;
        return true;
    }
    return false;
}

} // namespace miami
