#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "miami/integration.h"
#include "miami/config.h"
#include <windows.h>

// 1. Direct C++ Headers from E:\XENITH
#include "xenith/backend_api.h"
#include "xenith/compat.h"
#include "xenith/loader.h"
#include "xenith/memory.h"
#include "xenith/system.h"
#include "xenith/gpu.h"
#include "xenith/audio.h"
#include "xenith/input.h"
#include "xenith/storage.h"

// 2. Direct C++ Headers from E:\XENITH\references\GTA6-COMPAT-LAYER
#include "scarlett.h"
#include "pe_loader.h"

// 3. Direct C++ Headers from E:\XENITH\references\Leonida\runtime
#include "root_signature.h"
#include "d3d12_xs_rootsig.h"
#include "gfx10_tiling.h"

namespace miami {

namespace {

// Dynamic bindings into GTA6-COMPAT-LAYER's compiled ScarlettCompat.dll
struct ScarlettCompatDll {
    HMODULE hMod = NULL;
    using FnLoadXboxPe = bool(*)(const wchar_t*, sc::LoadedPe*, char*, size_t);
    using FnFreeXboxPe = void(*)(sc::LoadedPe*);
    using FnXGameRuntimeInitialize = HRESULT(*)();
    using FnXMemAlloc = void*(*)(SIZE_T, DWORD);
    using FnXMemFree = void(*)(void*);

    FnLoadXboxPe pScLoadXboxPe = nullptr;
    FnFreeXboxPe pScFreeXboxPe = nullptr;
    FnXGameRuntimeInitialize pXGameRuntimeInitialize = nullptr;
    FnXMemAlloc pXMemAlloc = nullptr;
    FnXMemFree pXMemFree = nullptr;
    size_t resolvedExports = 0;

    bool Load() {
        if (hMod) return true;
        hMod = LoadLibraryA("e:\\ProjectMiami\\out\\ScarlettCompat.dll");
        if (!hMod) {
            hMod = LoadLibraryA("ScarlettCompat.dll");
        }
        if (!hMod) return false;

        pScLoadXboxPe = reinterpret_cast<FnLoadXboxPe>(GetProcAddress(hMod, "ScLoadXboxPe"));
        pScFreeXboxPe = reinterpret_cast<FnFreeXboxPe>(GetProcAddress(hMod, "ScFreeXboxPe"));
        pXGameRuntimeInitialize = reinterpret_cast<FnXGameRuntimeInitialize>(GetProcAddress(hMod, "XGameRuntimeInitialize"));
        pXMemAlloc = reinterpret_cast<FnXMemAlloc>(GetProcAddress(hMod, "XMemAlloc"));
        pXMemFree = reinterpret_cast<FnXMemFree>(GetProcAddress(hMod, "XMemFree"));

        const char* kExportNames[] = {
            "ScLoadXboxPe", "ScRunXboxPe", "ScFreeXboxPe",
            "XGameRuntimeInitialize", "XMemAlloc", "XMemFree",
            "D3D12XboxCreateDevice", "XAudio2Create", "GameInputCreate",
            "DStorageGetFactory", "SetFrameIntervalX", "ScheduleFrameEventX", "WaitFrameEventX"
        };
        resolvedExports = 0;
        for (const char* name : kExportNames) {
            if (GetProcAddress(hMod, name) != nullptr) {
                resolvedExports++;
            }
        }
        return resolvedExports == 13 && pXGameRuntimeInitialize && pXMemAlloc && pXMemFree;
    }
};

static ScarlettCompatDll g_scarlettDll;

} // namespace

UnifiedRuntimeAdapter::UnifiedRuntimeAdapter() {
    InitializeEngines();
}

UnifiedRuntimeAdapter::~UnifiedRuntimeAdapter() {
    if (m_enginesInitialized) {
        xenith::XenithBackend::Instance().Shutdown();
        m_enginesInitialized = false;
    }
}

bool UnifiedRuntimeAdapter::InitializeEngines() {
    if (m_enginesInitialized) return true;

    LogInfo("Initializing XENITH 6-Layer Compatibility Backend from " + m_xenithRoot);
    if (xenith::XenithBackend::Instance().Initialize(m_xenithRoot)) {
        m_enginesInitialized = true;
        LogInfo("XENITH Backend & CompatFramework initialized directly in process.");
    } else {
        LogWarn("XENITH Backend reported partial initialization.");
    }

    if (g_scarlettDll.Load()) {
        LogInfo("GTA6-COMPAT-LAYER (ScarlettCompat.dll) loaded into process with " +
                std::to_string(g_scarlettDll.resolvedExports) + " verified exports.");
    }
    return m_enginesInitialized;
}

UnifiedIntegrationStatus UnifiedRuntimeAdapter::GetStatus() {
    InitializeEngines();

    UnifiedIntegrationStatus status;

    // ------------------------------------------------------------------------
    // 1. Live Execution of XENITH Core Compatibility Subsystems (xenith::*)
    // ------------------------------------------------------------------------
    status.xenithLayer.name = "XENITH Compatibility Core";
    status.xenithLayer.role = "PE32+ Loader, GDK HLE Shims, Memory & D3D12_XS Subsystems";
    status.xenithLayer.version = "GXDK 10.0.26100.9605 (6-Layer C++ Engine)";
    status.xenithLayer.path = m_xenithRoot;

    auto& xenithBackend = xenith::XenithBackend::Instance();
    auto caps = xenithBackend.GetSubsystemCapabilities();
    auto compatRep = xenith::CompatFramework::Instance().GetCapabilityReport();

    status.xenithSubsystemsInitialized = caps.size();
    status.xenithActiveExports = compatRep.implementedExports + compatRep.partialExports;
    status.xenithTotalExports = compatRep.totalExports;
    status.xenithReadinessPercent = compatRep.readinessPercentage;

    std::string compatSelfTestReport;
    bool xenithSelfTestOk = xenith::CompatFramework::Instance().RunSelfTest(&compatSelfTestReport);

    if (xenithBackend.IsBackendAvailable() && xenithSelfTestOk && status.xenithTotalExports > 0) {
        status.xenithLayer.isAvailable = true;
        status.xenithLayer.statusDetail =
            "Active in-process: " + std::to_string(status.xenithSubsystemsInitialized) + " subsystems, " +
            std::to_string(status.xenithActiveExports) + "/" + std::to_string(status.xenithTotalExports) +
            " GDK exports (" + compatSelfTestReport + ")";
    } else {
        status.xenithLayer.isAvailable = false;
        status.xenithLayer.statusDetail = "XENITH engine self-test failed.";
    }

    // ------------------------------------------------------------------------
    // 2. Live Execution of Leonida Recompilation Runtime (rt::rootsig, rt::d3d12_xs, leonida::gfx10)
    // ------------------------------------------------------------------------
    status.leonidaRecomp.name = "Leonida Recompilation & Runtime";
    status.leonidaRecomp.role = "SGPR Root-Signature Packer, D3D12_XS Flag Sanitizer & GFX10 Surface Detiler";
    status.leonidaRecomp.version = "Leonida v1.0.2.1 Native C++20 Runtime";
    status.leonidaRecomp.path = m_leonidaRoot;

    // Call Leonida's rt::rootsig::pack & encode_canonical / decode_canonical
    rt::rootsig::RootSignature testSig;
    testSig.version = rt::rootsig::Version::V1_2;
    testSig.flags = 0x03000001u; // Xbox high-byte flag + allow input assembler
    rt::rootsig::RootParameter cbvParam;
    cbvParam.type = rt::rootsig::ParamType::Descriptor;
    cbvParam.visibility = rt::rootsig::Visibility::All;
    cbvParam.descriptor_register = 0;
    cbvParam.descriptor_space = 0;
    testSig.params.push_back(cbvParam);

    rt::rootsig::RootParameter constParam;
    constParam.type = rt::rootsig::ParamType::Constants;
    constParam.visibility = rt::rootsig::Visibility::Pixel;
    constParam.num_values = 4;
    constParam.shader_register = 1;
    testSig.params.push_back(constParam);

    rt::rootsig::PackResult packRes = rt::rootsig::pack(testSig);
    auto encodedBlob = rt::rootsig::encode_canonical(testSig);
    auto decodedRes = rt::rootsig::decode_canonical(encodedBlob.data(), encodedBlob.size());

    // Call Leonida's rt::d3d12_xs::copy_versioned_root_signature_desc_clear_xbox_flags
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC xboxDesc = {};
    xboxDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    xboxDesc.Desc_1_1.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(0x03000000u | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC pcDesc = {};
    bool flagsCleared = rt::d3d12_xs::copy_versioned_root_signature_desc_clear_xbox_flags(xboxDesc, pcDesc) &&
                        ((static_cast<uint32_t>(pcDesc.Desc_1_1.Flags) & rt::d3d12_xs::kXboxRootSignatureFlagHighByte) == 0);

    status.leonidaRootSigPackerVerified = packRes.ok && decodedRes.ok && flagsCleared;
    status.leonidaPackedUserSgprs = packRes.dword_count;

    // Call Leonida's leonida::gfx10::TiledSurface2D
    leonida::gfx10::TiledSurface2D surface;
    bool tilingOk = leonida::gfx10::TiledSurface2D::make(
        leonida::gfx10::TileMode::sw_64kb_d, 4, 256, 256, surface);
    std::size_t elemOffset = 0;
    if (tilingOk) {
        tilingOk = surface.element_offset(16, 16, elemOffset) && (surface.tiled_size() >= 256 * 256 * 4);
    }
    status.leonidaGfx10TilingVerified = tilingOk;

    if (status.leonidaRootSigPackerVerified && status.leonidaGfx10TilingVerified) {
        status.leonidaRecomp.isAvailable = true;
        status.leonidaRecomp.statusDetail =
            "Active in-process: SGPR packer (" + std::to_string(status.leonidaPackedUserSgprs) +
            "/32 dwords), D3D12_XS high-byte flag sanitizer, GFX10 64KB detiler (" +
            std::to_string(surface.tiled_size() / 1024) + " KB surface) verified.";
    } else {
        status.leonidaRecomp.isAvailable = false;
        status.leonidaRecomp.statusDetail = "Leonida runtime self-check failed.";
    }

    // ------------------------------------------------------------------------
    // 3. Live Execution of GTA6-COMPAT-LAYER (ScarlettCompat.dll & sc::*)
    // ------------------------------------------------------------------------
    status.gta6CompatLayer.name = "GTA6-COMPAT-LAYER (ScarlettCompat.dll)";
    status.gta6CompatLayer.role = "Scarlett HLE Export Resolver, PE32+ Loader & Shim Import Libraries";
    status.gta6CompatLayer.version = "Scarlett Layer v1.0 (sc::*)";
    status.gta6CompatLayer.path = m_compatLayerRoot;

    bool dllOk = g_scarlettDll.Load();
    status.scarlettDllLoaded = dllOk;
    status.scarlettHleExportCount = g_scarlettDll.resolvedExports;

    // Exercise live calls into ScarlettCompat.dll
    bool hleCallOk = false;
    if (dllOk) {
        HRESULT hrSc = g_scarlettDll.pXGameRuntimeInitialize();
        void* testMem = g_scarlettDll.pXMemAlloc(256, 0);
        if (testMem) {
            g_scarlettDll.pXMemFree(testMem);
            hleCallOk = SUCCEEDED(hrSc);
        }
    }

    if (dllOk && hleCallOk && status.scarlettHleExportCount == 13) {
        status.gta6CompatLayer.isAvailable = true;
        status.gta6CompatLayer.statusDetail =
            "Active in-process: ScarlettCompat.dll loaded (" + std::to_string(status.scarlettHleExportCount) +
            "/13 exports verified, XGameRuntimeInitialize & XMemAlloc tested live).";
    } else {
        status.gta6CompatLayer.isAvailable = false;
        status.gta6CompatLayer.statusDetail = "ScarlettCompat.dll load or HLE verification failed.";
    }

    status.allLayersReady = (status.xenithLayer.isAvailable &&
                             status.leonidaRecomp.isAvailable &&
                             status.gta6CompatLayer.isAvailable);
    status.summary = status.allLayersReady
        ? "All 3 C++ engines (XENITH + Leonida + GTA6-COMPAT-LAYER) linked and executing in-process."
        : "One or more C++ runtime engines failed initialization.";

    return status;
}

bool UnifiedRuntimeAdapter::PrepareGameEnvironment(const std::string& gameDir, std::string& outExecutable, std::string& outError) {
    InitializeEngines();

    LogInfo("Preparing game execution environment with XENITH + Leonida + GTA6-COMPAT-LAYER in: " + gameDir);

    // 1. Validate game directory
    DWORD dwDir = GetFileAttributesA(gameDir.c_str());
    if (dwDir == INVALID_FILE_ATTRIBUTES || !(dwDir & FILE_ATTRIBUTE_DIRECTORY)) {
        outError = "Game directory does not exist: " + gameDir;
        return false;
    }

    // 2. Locate master executable
    std::string masterExe = gameDir + "\\game_scarlett_master.exe";
    DWORD exeAttr = GetFileAttributesA(masterExe.c_str());
    if (exeAttr == INVALID_FILE_ATTRIBUTES) {
        outError = "game_scarlett_master.exe not found in " + gameDir;
        return false;
    }

    // 3. Configure XENITH VFS & Storage mount points for the game directory
    xenith::StorageSubsystem::Instance().Mount("GAME:", gameDir, true, true);
    LogInfo("XENITH StorageSubsystem mounted GAME:\\ -> " + gameDir);

    // 4. Initialize XENITH Game Core Runtime state (XGameRuntimeInitialize via HLE)
    HRESULT hrInit = xenith::HleXGameRuntimeInitialize();
    if (FAILED(hrInit)) {
        outError = "XENITH HleXGameRuntimeInitialize failed.";
        return false;
    }

    // 5. Verify GTA6-COMPAT-LAYER ScarlettCompat.dll live initialization
    if (!g_scarlettDll.Load() || FAILED(g_scarlettDll.pXGameRuntimeInitialize())) {
        outError = "GTA6-COMPAT-LAYER ScarlettCompat.dll initialization failed.";
        return false;
    }

    // 6. Verify Leonida D3D12_XS Root Signature translation readiness
    rt::rootsig::RootSignature bootSig;
    bootSig.version = rt::rootsig::Version::V1_1;
    auto packCheck = rt::rootsig::pack(bootSig);
    if (!packCheck.ok) {
        outError = "Leonida root signature packer check failed: " + packCheck.error;
        return false;
    }

    outExecutable = masterExe;
    LogInfo("Environment prepared via XENITH, Leonida, and GTA6-COMPAT-LAYER. Target: " + outExecutable);
    return true;
}

} // namespace miami
