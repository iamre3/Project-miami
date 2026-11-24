#pragma once
#include <string>
#include <memory>
#include "runtime/service.h"
#include "runtime/memory_service.h"
#include "runtime/storage_service.h"
#include "runtime/input_service.h"
#include "runtime/audio_service.h"
#include "runtime/network_service.h"
#include "runtime/feature_matrix.h"
#include "graphics/runtime_bridge.h"

namespace miami {

struct RuntimeConfig {
    std::string rootDirectory;
    bool enableValidationLayer = false;
    bool enableDiagnostics = true;
    uint32_t targetFps = 60;
};

class MiamiRuntime {
public:
    MiamiRuntime();
    ~MiamiRuntime();

    bool Initialize(const RuntimeConfig& config);
    void Shutdown();

    RuntimeState GetState() const { return m_state; }
    const std::string& GetLastError() const { return m_lastError; }

    // Service accessors
    IMemoryService* GetMemory() { return m_memory.get(); }
    IStorageService* GetStorage() { return m_storage.get(); }
    IInputService* GetInput() { return m_input.get(); }
    IAudioService* GetAudio() { return m_audio.get(); }
    INetworkService* GetNetwork() { return m_network.get(); }
    miami::gfx::IGraphicsRuntimeBridge* GetGraphics() { return m_graphics.get(); }

    // Application execution lifecycle within this runtime environment
    bool StartApplication(const std::string& appPath, const std::string& workingDir, const std::string& args = "");
    bool PauseApplication();
    bool ResumeApplication();
    bool StopApplication();
    bool IsApplicationRunning() const;
    uint32_t GetApplicationExitCode() const;

private:
    RuntimeState m_state = RuntimeState::Uninitialized;
    std::string m_lastError;
    RuntimeConfig m_config;

    std::unique_ptr<IMemoryService> m_memory;
    std::unique_ptr<IStorageService> m_storage;
    std::unique_ptr<IInputService> m_input;
    std::unique_ptr<IAudioService> m_audio;
    std::unique_ptr<INetworkService> m_network;
    std::unique_ptr<miami::gfx::IGraphicsRuntimeBridge> m_graphics;

    // Process tracking
    void* m_hProcess = nullptr;
    void* m_hThread = nullptr;
    uint32_t m_processId = 0;
    uint32_t m_exitCode = 0;
    bool m_appRunning = false;

    void SetState(RuntimeState newState);
};

} // namespace miami
