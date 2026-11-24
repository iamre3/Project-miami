#include "runtime/runtime.h"
#include <windows.h>
#include <string>
#include <iostream>
#include <vector>

namespace miami {

// Factories implemented in service translation units
std::unique_ptr<IMemoryService> CreateMemoryService();
std::unique_ptr<IStorageService> CreateStorageService();
std::unique_ptr<IInputService> CreateInputService();
std::unique_ptr<IAudioService> CreateAudioService();
std::unique_ptr<INetworkService> CreateNetworkService();

const char* RuntimeStateToString(RuntimeState state) {
    switch (state) {
        case RuntimeState::Uninitialized:        return "Uninitialized";
        case RuntimeState::Initializing:         return "Initializing";
        case RuntimeState::Ready:                return "Ready";
        case RuntimeState::StartingApplication:  return "StartingApplication";
        case RuntimeState::Running:              return "Running";
        case RuntimeState::Paused:               return "Paused";
        case RuntimeState::Stopping:             return "Stopping";
        case RuntimeState::Shutdown:             return "Shutdown";
        case RuntimeState::Failed:               return "Failed";
        default:                                 return "Unknown";
    }
}

MiamiRuntime::MiamiRuntime() {
    m_memory = CreateMemoryService();
    m_storage = CreateStorageService();
    m_input = CreateInputService();
    m_audio = CreateAudioService();
    m_network = CreateNetworkService();
    m_graphics = miami::gfx::CreateGraphicsRuntimeBridge();
}

MiamiRuntime::~MiamiRuntime() {
    Shutdown();
}

void MiamiRuntime::SetState(RuntimeState newState) {
    m_state = newState;
}

bool MiamiRuntime::Initialize(const RuntimeConfig& config) {
    std::cout << "  [Runtime::Init] Step 1: State initializing..." << std::endl;
    SetState(RuntimeState::Initializing);
    m_config = config;
    m_lastError.clear();

    // 1. Initialize Memory Service
    std::cout << "  [Runtime::Init] Step 2: Init Memory..." << std::endl;
    if (!m_memory || !m_memory->Initialize()) {
        m_lastError = "Failed to initialize Memory Service";
        SetState(RuntimeState::Failed);
        return false;
    }

    // 2. Initialize Storage Service
    std::cout << "  [Runtime::Init] Step 3: Init Storage..." << std::endl;
    if (!m_storage || !m_storage->Initialize()) {
        m_lastError = "Failed to initialize Storage Service";
        m_memory->Shutdown();
        SetState(RuntimeState::Failed);
        return false;
    }

    if (!m_config.rootDirectory.empty()) {
        m_storage->Mount("game:", m_config.rootDirectory);
    }

    // 3. Initialize Input Service
    std::cout << "  [Runtime::Init] Step 4: Init Input..." << std::endl;
    if (!m_input || !m_input->Initialize()) {
        m_lastError = "Failed to initialize Input Service";
        m_storage->Shutdown();
        m_memory->Shutdown();
        SetState(RuntimeState::Failed);
        return false;
    }

    // 4. Initialize Audio Service
    std::cout << "  [Runtime::Init] Step 5: Init Audio..." << std::endl;
    if (!m_audio || !m_audio->Initialize()) {
        m_lastError = "Failed to initialize Audio Service";
        m_input->Shutdown();
        m_storage->Shutdown();
        m_memory->Shutdown();
        SetState(RuntimeState::Failed);
        return false;
    }

    // 5. Initialize Network Service
    std::cout << "  [Runtime::Init] Step 6: Init Network..." << std::endl;
    if (!m_network || !m_network->Initialize()) {
        m_lastError = "Failed to initialize Network Service";
        m_audio->Shutdown();
        m_input->Shutdown();
        m_storage->Shutdown();
        m_memory->Shutdown();
        SetState(RuntimeState::Failed);
        return false;
    }

    // 6. Initialize Native Direct3D 12 Graphics Runtime Bridge
    std::cout << "  [Runtime::Init] Step 7: Init Graphics..." << std::endl;
    if (m_graphics) {
        miami::gfx::RuntimeGraphicsInitDesc gdesc;
        gdesc.enableValidation = m_config.enableValidationLayer;
        gdesc.enableVsync = true;
        gdesc.initialWidth = 1280;
        gdesc.initialHeight = 720;
        if (!m_graphics->Initialize(gdesc)) {
            m_lastError = "Failed to initialize D3D12 Graphics Runtime Bridge";
            m_network->Shutdown();
            m_audio->Shutdown();
            m_input->Shutdown();
            m_storage->Shutdown();
            m_memory->Shutdown();
            SetState(RuntimeState::Failed);
            return false;
        }
    }

    std::cout << "  [Runtime::Init] Step 8: Done!" << std::endl;
    SetState(RuntimeState::Ready);
    return true;
}

void MiamiRuntime::Shutdown() {
    if (m_state == RuntimeState::Shutdown || m_state == RuntimeState::Uninitialized) {
        return;
    }

    SetState(RuntimeState::Stopping);
    StopApplication();

    if (m_graphics) m_graphics->Shutdown();
    if (m_network) m_network->Shutdown();
    if (m_audio) m_audio->Shutdown();
    if (m_input) m_input->Shutdown();
    if (m_storage) m_storage->Shutdown();
    if (m_memory) m_memory->Shutdown();

    SetState(RuntimeState::Shutdown);
}

bool MiamiRuntime::StartApplication(const std::string& appPath, const std::string& workingDir, const std::string& args) {
    if (m_state != RuntimeState::Ready && m_state != RuntimeState::Running) {
        m_lastError = "Cannot start application: Runtime is not in Ready state (current: " + std::string(RuntimeStateToString(m_state)) + ")";
        return false;
    }

    SetState(RuntimeState::StartingApplication);

    DWORD dwAttrib = GetFileAttributesA(appPath.c_str());
    if (dwAttrib == INVALID_FILE_ATTRIBUTES || (dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
        m_lastError = "Executable not found: " + appPath;
        SetState(RuntimeState::Ready);
        return false;
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    std::string fullCmd = "\"" + appPath + "\"";
    if (!args.empty()) fullCmd += " " + args;

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
        m_lastError = "CreateProcess failed with Windows error code";
        SetState(RuntimeState::Ready);
        return false;
    }

    m_hProcess = pi.hProcess;
    m_hThread = pi.hThread;
    m_processId = pi.dwProcessId;
    m_appRunning = true;
    m_exitCode = 0;

    SetState(RuntimeState::Running);
    return true;
}

bool MiamiRuntime::PauseApplication() {
    if (!m_appRunning || !m_hThread) return false;
    SuspendThread(m_hThread);
    SetState(RuntimeState::Paused);
    return true;
}

bool MiamiRuntime::ResumeApplication() {
    if (!m_appRunning || !m_hThread) return false;
    ResumeThread(m_hThread);
    SetState(RuntimeState::Running);
    return true;
}

bool MiamiRuntime::StopApplication() {
    if (!m_appRunning || !m_hProcess) return false;

    TerminateProcess(m_hProcess, 0);
    WaitForSingleObject(m_hProcess, 1000);

    DWORD code = 0;
    GetExitCodeProcess(m_hProcess, &code);
    m_exitCode = code;

    CloseHandle(m_hThread);
    CloseHandle(m_hProcess);
    m_hThread = nullptr;
    m_hProcess = nullptr;
    m_appRunning = false;

    SetState(RuntimeState::Ready);
    return true;
}

bool MiamiRuntime::IsApplicationRunning() const {
    if (!m_appRunning || !m_hProcess) return false;

    DWORD code = 0;
    if (GetExitCodeProcess(m_hProcess, &code)) {
        if (code == STILL_ACTIVE) {
            return true;
        } else {
            const_cast<MiamiRuntime*>(this)->m_exitCode = code;
            const_cast<MiamiRuntime*>(this)->m_appRunning = false;
            const_cast<MiamiRuntime*>(this)->SetState(RuntimeState::Ready);
            return false;
        }
    }
    return false;
}

uint32_t MiamiRuntime::GetApplicationExitCode() const {
    return m_exitCode;
}

} // namespace miami
