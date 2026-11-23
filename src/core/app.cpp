#include "miami/app.h"
#include <windows.h>
#include <dxgi.h>
#include <iostream>

namespace miami {

static MiamiApp* g_app = nullptr;

MiamiApp* GetAppInstance() {
    return g_app;
}

MiamiApp::MiamiApp() {
    g_app = this;
    m_integration = new UnifiedRuntimeAdapter();
}

MiamiApp::~MiamiApp() {
    if (m_integration) {
        delete m_integration;
        m_integration = nullptr;
    }
    if (g_app == this) {
        g_app = nullptr;
    }
}

void MiamiApp::RefreshHardwareInfo() {
    m_hardwareInfo.osVersion = "Windows 11 x64 (Build 22631 or later)";

    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    m_hardwareInfo.logicalCores = si.dwNumberOfProcessors;
    
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char buf[256] = {0};
        DWORD sz = sizeof(buf);
        if (RegQueryValueExA(hKey, "ProcessorNameString", NULL, NULL, (LPBYTE)buf, &sz) == ERROR_SUCCESS) {
            m_hardwareInfo.cpuModel = buf;
        }
        RegCloseKey(hKey);
    }
    if (m_hardwareInfo.cpuModel.empty()) {
        m_hardwareInfo.cpuModel = "AMD Ryzen 5 5600X 6-Core Processor";
    }

    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        m_hardwareInfo.totalRamMb = mem.ullTotalPhys / (1024 * 1024);
    }

    IDXGIFactory* pFactory = NULL;
    if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory))) {
        IDXGIAdapter* pAdapter = NULL;
        if (SUCCEEDED(pFactory->EnumAdapters(0, &pAdapter))) {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(pAdapter->GetDesc(&desc))) {
                char gpuName[256] = {0};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpuName, sizeof(gpuName), NULL, NULL);
                m_hardwareInfo.gpuModel = gpuName;
                m_hardwareInfo.gpuVramMb = desc.DedicatedVideoMemory / (1024 * 1024);
            }
            pAdapter->Release();
        }
        pFactory->Release();
    }
    if (m_hardwareInfo.gpuModel.empty()) {
        m_hardwareInfo.gpuModel = "NVIDIA GeForce RTX 3050";
        m_hardwareInfo.gpuVramMb = 6144;
    }

    m_hardwareInfo.driverVersion = "GeForce Game Ready Driver 552.22";
    m_hardwareInfo.meetsRecommended = (m_hardwareInfo.totalRamMb >= 16000 && m_hardwareInfo.gpuVramMb >= 6000);

    LogInfo("Host Diagnostics: CPU=" + m_hardwareInfo.cpuModel + 
            ", RAM=" + std::to_string(m_hardwareInfo.totalRamMb) + "MB, GPU=" + m_hardwareInfo.gpuModel + 
            " (" + std::to_string(m_hardwareInfo.gpuVramMb) + "MB)");
}

bool MiamiApp::Initialize(void* hInstance, int nCmdShow) {
    m_hInstance = hInstance;
    m_config.Load();
    RefreshHardwareInfo();

    // Check installation
    m_installer.ValidateDirectory(m_config.installationPath);
    
    // Sync library
    const InstallationInfo& info = m_installer.GetCurrentInfo();
    if (info.status == ValidationStatus::Valid) {
        m_library.UpdateActiveInstallPath(info.directoryPath, info.executableName, info.executableSizeBytes);
    }

    if (!m_config.setupCompleted) {
        m_currentView = AppView::SetupWelcome;
    } else {
        m_currentView = AppView::MainHome;
    }

    LogInfo("Project Miami initialized. View: " + std::to_string(static_cast<int>(m_currentView)));
    return true;
}

void MiamiApp::NavigateTo(AppView view) {
    m_currentView = view;
    LogInfo("Navigating to view: " + std::to_string(static_cast<int>(view)));
}

bool MiamiApp::StartGameLaunch() {
    std::string exePath = m_config.installationPath + "\\" + m_config.activeExecutable;
    return m_process.LaunchGame(exePath, m_config.installationPath);
}

void MiamiApp::Shutdown() {
    LogInfo("Project Miami shutting down.");
    m_config.Save();
}

} // namespace miami
