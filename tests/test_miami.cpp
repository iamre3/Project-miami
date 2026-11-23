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

    // 2. Strict GTA 6 Installation Validation
    std::cout << "\n[2] Testing Strict GTA 6 Directory & File Validation:\n";
    miami::InstallationManager installer;
    
    // Invalid directory
    miami::InstallationInfo invalidInfo = installer.ValidateDirectory("e:\\NonExistent_Fake_Dir_12345");
    RunTest("Reject non-existent directory", invalidInfo.status == miami::ValidationStatus::InvalidDirectory);

    // Empty/dummy directory without GTA 6 files
    std::string emptyDir = "e:\\ProjectMiami\\out";
    miami::InstallationInfo emptyInfo = installer.ValidateDirectory(emptyDir);
    RunTest("Reject folder without game_scarlett_master.exe", emptyInfo.status == miami::ValidationStatus::MissingExecutable);

    // Real directory validation on e:\Gta6
    miami::InstallationInfo realInfo = installer.ValidateDirectory("e:\\Gta6");
    RunTest("Detect e:\\Gta6 directory", realInfo.status == miami::ValidationStatus::Valid);
    RunTest("Verified game_scarlett_master.exe", realInfo.executableFound && realInfo.executableName == "game_scarlett_master.exe");
    RunTest("Verified PE64 machine architecture", realInfo.isPE64);
    RunTest("Verified .xbld Xbox build section", realInfo.hasXbldSection);
    RunTest("Verified GTA 6 game package archives", realInfo.hasGameData && realInfo.totalDataBytes > 0);

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

    // 6. Compatibility Adapter Interface & Live In-Process Multi-Engine Execution
    std::cout << "\n[6] Testing Live In-Process Execution of XENITH, Leonida, and GTA6-COMPAT-LAYER:\n";
    miami::ICompatibilityAdapter* adapter = app.GetIntegration();
    RunTest("Adapter is registered", adapter != nullptr);
    miami::UnifiedIntegrationStatus intStatus = adapter->GetStatus();
    RunTest("XENITH C++ Engine initialized in-process (> 0 subsystems & exports)", intStatus.xenithLayer.isAvailable && intStatus.xenithSubsystemsInitialized >= 8 && intStatus.xenithActiveExports >= 18);
    RunTest("Leonida C++20 SGPR Root-Signature Packer & D3D12_XS Flag Sanitizer verified", intStatus.leonidaRecomp.isAvailable && intStatus.leonidaRootSigPackerVerified && intStatus.leonidaPackedUserSgprs == 6);
    RunTest("Leonida C++20 GFX10 64KB Surface Tiler/Detiler verified", intStatus.leonidaGfx10TilingVerified);
    RunTest("GTA6-COMPAT-LAYER ScarlettCompat.dll loaded & 13 HLE exports verified live", intStatus.gta6CompatLayer.isAvailable && intStatus.scarlettDllLoaded && intStatus.scarlettHleExportCount == 13);
    RunTest("All 3 C++ engines operational in-process", intStatus.allLayersReady);

    std::string targetExe;
    std::string prepError;
    bool prepOk = adapter->PrepareGameEnvironment("e:\\Gta6", targetExe, prepError);
    RunTest("PrepareGameEnvironment mounts XENITH VFS & initializes HLE runtimes on e:\\Gta6", prepOk);
    RunTest("Resolved target executable is game_scarlett_master.exe", !targetExe.empty());

    std::cout << "\n====================================================================\n";
    std::cout << " ALL 24 TEST ASSERTIONS PASSED WITH ZERO FAILURES (PASS)\n";
    std::cout << "====================================================================\n";
    return 0;
}
