#pragma once
#include <string>
#include <vector>
#include "miami/config.h"
#include "miami/installer.h"
#include "miami/library.h"
#include "miami/process.h"
#include "miami/integration.h"

namespace miami {

enum class AppView {
    SetupWelcome,
    SetupDirectory,
    SetupValidation,
    SetupConfiguration,
    SetupComplete,
    MainHome,
    MainLibrary,
    MainSettings,
    MainDiagnostics,
    MainAbout
};

struct SystemHardwareInfo {
    std::string osVersion;
    std::string cpuModel;
    uint32_t logicalCores = 0;
    uint64_t totalRamMb = 0;
    std::string gpuModel;
    uint64_t gpuVramMb = 0;
    std::string driverVersion;
    bool meetsRecommended = true;
};

class MiamiApp {
public:
    MiamiApp();
    ~MiamiApp();

    bool Initialize(void* hInstance, int nCmdShow);
    void Shutdown();

    // Navigation & View State
    AppView GetCurrentView() const { return m_currentView; }
    void NavigateTo(AppView view);
    
    // Subsystem accessors
    AppConfig& GetConfig() { return m_config; }
    InstallationManager& GetInstaller() { return m_installer; }
    GameLibrary& GetLibrary() { return m_library; }
    ProcessManager& GetProcess() { return m_process; }
    ICompatibilityAdapter* GetIntegration() { return m_integration; }
    const SystemHardwareInfo& GetHardwareInfo() const { return m_hardwareInfo; }

    // Actions
    bool StartGameLaunch();
    void RefreshHardwareInfo();

private:
    void* m_hInstance = nullptr;
    AppView m_currentView = AppView::SetupWelcome;
    
    AppConfig m_config;
    InstallationManager m_installer;
    GameLibrary m_library;
    ProcessManager m_process;
    ICompatibilityAdapter* m_integration = nullptr;
    SystemHardwareInfo m_hardwareInfo;
};

MiamiApp* GetAppInstance();

} // namespace miami
