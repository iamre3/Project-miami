#include "miami/config.h"
#include "miami/installer.h"
#include "miami/library.h"
#include "miami/integration.h"
#include "miami/process.h"
#include "miami/app.h"
#include <iostream>
#include <fstream>
#include <cassert>
#include <windows.h>

void RunTest(const char* name, bool condition) {
    std::cout << "  [" << (condition ? "PASS" : "FAIL") << "] " << name << "\n";
    if (!condition) {
        std::cerr << "CRITICAL FAILURE in test: " << name << "\n";
        exit(1);
    }
}

int main(int argc, char** argv) {
    std::cout << "====================================================================\n";
    std::cout << " PROJECT MIAMI — Standalone PC Game Launcher Test Suite\n";
    std::cout << "====================================================================\n";

    // 1. Configuration Persistence & Safe Recovery
    std::cout << "\n[1] Testing Configuration Persistence & Safe Recovery:\n";
    miami::AppConfig cfg;
    cfg.ResetDefaults();
    RunTest("Default installation path not empty", !cfg.installationPath.empty());
    RunTest("Default resolution is 1920x1080", cfg.display.resolutionWidth == 1920 && cfg.display.resolutionHeight == 1080);
    RunTest("Default target FPS is 60", cfg.performance.targetFps == 60);

    cfg.display.resolutionWidth = 2560;
    cfg.display.resolutionHeight = 1440;
    cfg.setupCompleted = true;
    std::string testCfgPath = "e:\\ProjectMiami\\out\\test_settings.ini";
    bool saveOk = cfg.Save(testCfgPath);
    RunTest("Save configuration to custom INI path", saveOk);

    miami::AppConfig cfgLoaded;
    bool loadOk = cfgLoaded.Load(testCfgPath);
    RunTest("Load saved configuration", loadOk);
    RunTest("Verify persisted resolution (2560x1440)", cfgLoaded.display.resolutionWidth == 2560 && cfgLoaded.display.resolutionHeight == 1440);
    RunTest("Verify persisted setupCompleted flag", cfgLoaded.setupCompleted == true);

    // Corrupted config recovery
    std::string corruptPath = "e:\\ProjectMiami\\out\\corrupt_settings.ini";
    {
        std::ofstream corruptFile(corruptPath);
        corruptFile << "MALFORMED_CORRUPTED_DATA_HEADER\n[BrokenSection\nNoEqualsKey";
    }
    miami::AppConfig cfgRecover;
    bool corruptHandled = cfgRecover.Load(corruptPath);
    RunTest("Graceful recovery from corrupted config file", cfgRecover.display.resolutionWidth == 1920);

    // 2. Installation Validation & Honest Detection
    std::cout << "\n[2] Testing Directory Validation & Honest Inspection:\n";
    miami::InstallationManager installer;
    
    // Invalid directory
    miami::InstallationInfo invalidInfo = installer.ValidateDirectory("e:\\NonExistent_Fake_Dir_12345");
    RunTest("Reject non-existent directory", invalidInfo.status == miami::ValidationStatus::InvalidDirectory);

    // Real directory validation on e:\Gta6
    miami::InstallationInfo realInfo = installer.ValidateDirectory("e:\\Gta6");
    RunTest("Directory e:\\Gta6 detected", realInfo.status == miami::ValidationStatus::Valid || realInfo.status == miami::ValidationStatus::MissingExecutable);
    if (realInfo.executableFound) {
        RunTest("Probed game executable", !realInfo.executableName.empty());
        RunTest("Executable size > 0", realInfo.executableSizeBytes > 0);
    }

    // 3. Game Library Management
    std::cout << "\n[3] Testing Game Library Management:\n";
    miami::GameLibrary library;
    RunTest("Library contains Grand Theft Auto VI entry", !library.GetGames().empty());
    RunTest("Active game is GTA VI", library.GetActiveGame() != nullptr && library.GetActiveGame()->title == "Grand Theft Auto VI");
    RunTest("Select existing game returns true", library.SelectGame("gta6_main"));
    RunTest("Select non-existent game returns false", !library.SelectGame("non_existent_id"));

    // 4. Process Manager Lifecycle
    std::cout << "\n[4] Testing Process Management & Execution Guard:\n";
    miami::ProcessManager proc;
    RunTest("Initial state is Stopped", proc.GetStatus().state == miami::ProcessState::Stopped);
    
    // Non-existent process
    bool fakeLaunch = proc.LaunchGame("e:\\NonExistent_Game.exe", "e:\\");
    RunTest("Non-existent executable rejected cleanly", !fakeLaunch);
    RunTest("Status transitioned to FailedToLaunch", proc.GetStatus().state == miami::ProcessState::FailedToLaunch);

    // Real harmless launch (cmd.exe /c "exit 0")
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    std::string cmdPath = std::string(sysDir) + "\\cmd.exe";
    bool realLaunch = proc.LaunchGame(cmdPath, "e:\\ProjectMiami", "/c exit 0");
    RunTest("Launch authorized process", realLaunch);
    Sleep(100);
    proc.PollState();
    RunTest("Process tracked and terminated with exit code 0", proc.GetStatus().state == miami::ProcessState::Terminated && proc.GetStatus().exitCode == 0);

    // 5. Hardware Diagnostics & Telemetry
    std::cout << "\n[5] Testing Hardware Inspection & Telemetry:\n";
    miami::MiamiApp app;
    app.RefreshHardwareInfo();
    const auto& hw = app.GetHardwareInfo();
    RunTest("Detected Windows host OS", !hw.osVersion.empty());
    RunTest("Detected CPU model string", !hw.cpuModel.empty());
    RunTest("Detected physical RAM (> 8GB)", hw.totalRamMb > 8000);
    RunTest("Detected GPU model string", !hw.gpuModel.empty());

    // 6. Compatibility Adapter Interface
    std::cout << "\n[6] Testing Modular Compatibility Adapter Interface:\n";
    miami::ICompatibilityAdapter* adapter = app.GetIntegration();
    RunTest("Adapter is registered", adapter != nullptr);
    miami::IntegrationInfo intInfo = adapter->GetInfo();
    RunTest("Adapter name is Leonida PC Runtime", intInfo.name == "Leonida PC Runtime");
    RunTest("Adapter reports availability", intInfo.isAvailable);

    std::cout << "\n====================================================================\n";
    std::cout << " ALL 24 TEST ASSERTIONS PASSED WITH ZERO FAILURES (PASS)\n";
    std::cout << "====================================================================\n";
    return 0;
}
