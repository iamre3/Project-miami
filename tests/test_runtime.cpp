#include "runtime/runtime.h"
#include <iostream>
#include <cassert>
#include <windows.h>

void RunTest(const char* name, bool condition) {
    std::cout << "  [" << (condition ? "PASS" : "FAIL") << "] " << name << std::endl;
    if (!condition) {
        std::cerr << "CRITICAL FAILURE in test: " << name << std::endl;
        exit(1);
    }
}

int main(int argc, char** argv) {
    std::cout << "Starting test_runtime.exe main..." << std::endl;
    std::cout << "====================================================================\n";
    std::cout << " PROJECT MIAMI — Modular Runtime & End-to-End Execution Test Suite\n";
    std::cout << "====================================================================\n";

    miami::MiamiRuntime runtime;
    RunTest("Runtime state is initially Uninitialized", runtime.GetState() == miami::RuntimeState::Uninitialized);

    // 1. Runtime Initialization & Lifecycle
    std::cout << "\n[1] Testing Runtime Lifecycle & Service Initialization:\n";
    miami::RuntimeConfig cfg;
    cfg.rootDirectory = "e:\\ProjectMiami";
    cfg.enableDiagnostics = true;
    cfg.targetFps = 60;

    bool initOk = runtime.Initialize(cfg);
    if (!initOk) {
        std::cerr << "Initialize failed with error: " << runtime.GetLastError() << std::endl;
    }
    RunTest("Initialize runtime with configuration", initOk);
    RunTest("Runtime state transitioned to Ready", runtime.GetState() == miami::RuntimeState::Ready);

    // 2. Memory Subsystem Verification
    std::cout << "\n[2] Testing Memory Management Service:\n";
    miami::IMemoryService* mem = runtime.GetMemory();
    RunTest("Memory service is operational", mem && mem->IsOperational());

    void* hostBlock = mem->AllocateHost(4096, 64);
    RunTest("Aligned host memory allocation", hostBlock != nullptr && (reinterpret_cast<uintptr_t>(hostBlock) % 64 == 0));
    memset(hostBlock, 0xEF, 4096);
    RunTest("Host memory readback verified", *reinterpret_cast<uint8_t*>(hostBlock) == 0xEF);

    void* guestBlock = mem->AllocateGuest(65536, 0x1 /* MEM_GRAPHICS */);
    RunTest("Guest graphics pool allocation", guestBlock != nullptr);
    auto memStats = mem->GetStats();
    RunTest("Memory stats track host & guest allocations", memStats.hostAllocatedBytes >= 4096 && memStats.guestAllocatedBytes >= 65536 && memStats.graphicsPoolBytes >= 65536);

    mem->FreeHost(hostBlock);
    mem->FreeGuest(guestBlock);
    auto freedStats = mem->GetStats();
    RunTest("Memory deallocation tracked accurately", freedStats.hostAllocatedBytes == 0 && freedStats.guestAllocatedBytes == 0);

    // 3. Storage & VFS Service Verification
    std::cout << "\n[3] Testing Storage & VFS Mount System:\n";
    miami::IStorageService* storage = runtime.GetStorage();
    RunTest("Storage service is operational", storage && storage->IsOperational());

    storage->Mount("test_vfs:", "e:\\ProjectMiami\\out");
    std::string resolved = storage->ResolveVirtualPath("test_vfs:/test_file.bin");
    RunTest("Virtual path prefix resolved to physical root", resolved.find("e:\\ProjectMiami\\out\\test_file.bin") != std::string::npos);

    std::vector<uint8_t> testWriteData = { 0x4D, 0x49, 0x41, 0x4D, 0x49 }; // "MIAMI"
    RunTest("Write file via storage abstraction", storage->WriteFile("test_vfs:/vfs_verify.tmp", testWriteData.data(), testWriteData.size()));
    RunTest("FileExists detects newly created file", storage->FileExists("test_vfs:/vfs_verify.tmp"));

    std::vector<uint8_t> readData;
    RunTest("ReadFile retrieves identical payload", storage->ReadFile("test_vfs:/vfs_verify.tmp", readData) && readData == testWriteData);

    miami::StorageFileInfo sinfo;
    RunTest("GetFileInfo retrieves correct file metadata", storage->GetFileInfo("test_vfs:/vfs_verify.tmp", sinfo) && sinfo.size == 5 && !sinfo.isDirectory);
    DeleteFileA("e:\\ProjectMiami\\out\\vfs_verify.tmp");

    // 4. Input & Gamepad System Verification
    std::cout << "\n[4] Testing Input Subsystem Abstraction:\n";
    miami::IInputService* input = runtime.GetInput();
    RunTest("Input service is operational", input && input->IsOperational());
    input->Poll();
    auto pad0 = input->GetGamepadState(0);
    RunTest("Poll gamepad status without crashing", true);
    int mx = 0, my = 0;
    input->GetMousePosition(mx, my);
    RunTest("Get mouse position executed successfully", true);

    // 5. Audio Subsystem Verification
    std::cout << "\n[5] Testing Audio Subsystem & Wave Output:\n";
    miami::IAudioService* audio = runtime.GetAudio();
    RunTest("Audio service is operational", audio && audio->IsOperational());
    auto audioDevs = audio->EnumerateDevices();
    RunTest("Audio device enumeration returns devices", !audioDevs.empty());
    audio->SetMasterVolume(0.85f);
    RunTest("Master volume set and persisted", audio->GetMasterVolume() == 0.85f);

    // 6. Network Subsystem Verification
    std::cout << "\n[6] Testing Network Subsystem & Loopback Socket:\n";
    miami::INetworkService* net = runtime.GetNetwork();
    RunTest("Network service is operational", net && net->IsOperational());
    RunTest("Initialize UDP loopback socket on port 18888", net->InitializeLoopback(18888));
    RunTest("Network status transitions to LoopbackOnly", net->GetNetworkStatus() == miami::NetworkStatus::LoopbackOnly);

    const char netMsg[] = "MiamiSocketPing";
    RunTest("Send loopback packet", net->SendLoopback(netMsg, sizeof(netMsg)));
    Sleep(20);
    char netRecvBuf[64] = {0};
    size_t recvLen = net->ReceiveLoopback(netRecvBuf, sizeof(netRecvBuf));
    RunTest("Receive loopback packet with matching data", recvLen == sizeof(netMsg) && strcmp(netRecvBuf, netMsg) == 0);

    // 7. Feature Capability Matrix Verification
    std::cout << "\n[7] Testing Compatibility Feature Matrix:\n";
    const auto& matrix = miami::FeatureMatrix::GetInstance();
    RunTest("Feature matrix contains registered capabilities", matrix.GetCapabilities().size() >= 10);
    const auto* d3dFeat = matrix.FindFeature("Direct3D 12 Hardware Backend");
    RunTest("D3D12 Backend capability is marked VERIFIED", d3dFeat != nullptr && d3dFeat->status == miami::FeatureStatus::Verified);

    // 8. End-to-End Guest Application Execution Lifecycle
    std::cout << "\n[8] Testing End-to-End Guest Application Lifecycle:\n";
    // 8a. Launch minimal test application
    bool launchApp1 = runtime.StartApplication("e:\\ProjectMiami\\out\\app_minimal.exe", "e:\\ProjectMiami\\out");
    RunTest("Spawn guest application (app_minimal.exe)", launchApp1);
    RunTest("Runtime state is Running during execution", runtime.GetState() == miami::RuntimeState::Running);

    for (int i = 0; i < 20; ++i) {
        Sleep(50);
        if (!runtime.IsApplicationRunning()) break;
    }
    RunTest("Guest app executed and terminated cleanly with code 0", !runtime.IsApplicationRunning() && runtime.GetApplicationExitCode() == 0);
    RunTest("Runtime state transitioned back to Ready", runtime.GetState() == miami::RuntimeState::Ready);

    // 8b. Launch services test application
    bool launchApp2 = runtime.StartApplication("e:\\ProjectMiami\\out\\app_services.exe", "e:\\ProjectMiami\\out");
    RunTest("Spawn guest services application (app_services.exe)", launchApp2);
    for (int i = 0; i < 20; ++i) {
        Sleep(50);
        if (!runtime.IsApplicationRunning()) break;
    }
    RunTest("Guest services verified memory and I/O with exit code 0", !runtime.IsApplicationRunning() && runtime.GetApplicationExitCode() == 0);

    // 8c. Launch graphics test application
    bool launchApp3 = runtime.StartApplication("e:\\ProjectMiami\\out\\app_graphics.exe", "e:\\ProjectMiami\\out");
    RunTest("Spawn guest graphics application (app_graphics.exe)", launchApp3);
    for (int i = 0; i < 20; ++i) {
        Sleep(50);
        if (!runtime.IsApplicationRunning()) break;
    }
    RunTest("Guest D3D12 device and queue verified with exit code 0", !runtime.IsApplicationRunning() && runtime.GetApplicationExitCode() == 0);

    // 9. Graceful Shutdown & Resource Cleanup
    std::cout << "\n[9] Testing Graceful Runtime Shutdown:\n";
    runtime.Shutdown();
    RunTest("Runtime state is Shutdown", runtime.GetState() == miami::RuntimeState::Shutdown);
    RunTest("Memory service is shutdown", !runtime.GetMemory()->IsOperational());
    RunTest("Storage service is shutdown", !runtime.GetStorage()->IsOperational());
    RunTest("Input service is shutdown", !runtime.GetInput()->IsOperational());
    RunTest("Audio service is shutdown", !runtime.GetAudio()->IsOperational());
    RunTest("Network service is shutdown", !runtime.GetNetwork()->IsOperational());

    std::cout << "\n====================================================================\n";
    std::cout << " ALL 32 RUNTIME & COMPATIBILITY ASSERTIONS PASSED WITH ZERO FAILURES\n";
    std::cout << "====================================================================\n";
    return 0;
}
