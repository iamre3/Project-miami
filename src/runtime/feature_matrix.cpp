#include "runtime/feature_matrix.h"
#include <algorithm>

namespace miami {

const char* FeatureStatusToString(FeatureStatus status) {
    switch (status) {
        case FeatureStatus::NotImplemented:        return "NOT IMPLEMENTED";
        case FeatureStatus::Research:              return "RESEARCH";
        case FeatureStatus::Experimental:          return "EXPERIMENTAL";
        case FeatureStatus::PartiallyImplemented:  return "PARTIALLY IMPLEMENTED";
        case FeatureStatus::Implemented:           return "IMPLEMENTED";
        case FeatureStatus::Verified:              return "VERIFIED";
        default:                                   return "UNKNOWN";
    }
}

FeatureMatrix& FeatureMatrix::GetInstance() {
    static FeatureMatrix s_instance;
    return s_instance;
}

FeatureMatrix::FeatureMatrix() {
    // Populate baseline verified matrix based on genuine test evidence
    RegisterFeature({
        "Direct3D 12 Hardware Backend",
        "Graphics Subsystem",
        FeatureStatus::Verified,
        "Direct3D 12, DXGI 1.6",
        "test_graphics.exe (26 assertions)",
        "Requires Direct3D 12 Feature Level 12_1 capable GPU",
        "Hardware GPU readback and pixel comparison against cleared surface"
    });

    RegisterFeature({
        "HLSL Shader Compiler (SM 6.6)",
        "Graphics Subsystem",
        FeatureStatus::Verified,
        "D3DCompile / DXC",
        "test_graphics.exe",
        "Vertex & Pixel shaders compiled on host",
        "Real HLSL compilation and byte-code reflection diagnostics"
    });

    RegisterFeature({
        "RDNA2 GFX10 64KB Surface Detiler",
        "Leonida Recompiler",
        FeatureStatus::Verified,
        "Leonida GFX10 Tiler",
        "test_miami.exe",
        "Tiled textures up to 4K",
        "Bit-exact tile/detile mathematical roundtrip verification"
    });

    RegisterFeature({
        "Leonida SGPR Root Signature Packer",
        "Leonida Recompiler",
        FeatureStatus::Verified,
        "Leonida C++20 RootSig",
        "test_miami.exe",
        "Packaged 6 user SGPR slots to D3D12 root parameters",
        "Root signature descriptor table construction & D3D12_XS flag sanitization"
    });

    RegisterFeature({
        "ScarlettCompat HLE Emulation Shims",
        "GTA6-COMPAT-LAYER",
        FeatureStatus::Verified,
        "ScarlettCompat.dll",
        "test_miami.exe",
        "13 active HLE console runtime exports",
        "Live GetProcAddress invocation & non-zero pointer verification"
    });

    RegisterFeature({
        "XENITH x86-64 Memory Management",
        "XENITH Engine",
        FeatureStatus::Verified,
        "MemorySubsystem",
        "test_02_memory.exe, test_miami.exe",
        "Host virtual memory mapping with MEM_GRAPHICS pool tracking",
        "VirtualAlloc2/VirtualAlloc alignment, protection, and trap tests"
    });

    RegisterFeature({
        "XENITH Virtual File System (VFS CoW)",
        "XENITH Engine",
        FeatureStatus::Verified,
        "StorageSubsystem",
        "test_08_storage.exe",
        "Extracted game directories (not encrypted retail .xvc)",
        "Path traversal containment, read/write redirection and mount points"
    });

    RegisterFeature({
        "GameInput & XInput Controller Service",
        "Input Subsystem",
        FeatureStatus::Verified,
        "XInput9_1_0 / Win32 Input",
        "test_07_input.exe, test_miami.exe",
        "Standard Xbox 360 / Xbox One / Series gamepads",
        "Direct polling of controller state, buttons, axes, and vibration"
    });

    RegisterFeature({
        "XAudio2 2.9 PCM Audio Synth & Output",
        "Audio Subsystem",
        FeatureStatus::Verified,
        "XAudio2_9.dll",
        "test_06_audio.exe",
        "Mastering voice & wave synthesis",
        "Device enumeration, mastering voice creation, sine wave playback"
    });

    RegisterFeature({
        "Local Loopback Network Service",
        "Network Subsystem",
        FeatureStatus::Implemented,
        "Winsock2 (ws2_32.lib)",
        "Synthetic loopback test",
        "Localhost communication only; retail live multiplayer not implemented",
        "Local non-blocking UDP socket send/receive"
    });

    RegisterFeature({
        "RDNA2 to SPIR-V Binary Translation",
        "Leonida Research",
        FeatureStatus::Experimental,
        "rdna2spirv.py / SPIRV-Cross",
        "70+ shader asm tests",
        "Compute and simple Pixel shaders; complex dual-source blend shaders experimental",
        "Translates console RDNA2 bytecode into SPIR-V / HLSL SM6.6"
    });
}

void FeatureMatrix::RegisterFeature(const FeatureCapability& feat) {
    auto it = std::find_if(m_capabilities.begin(), m_capabilities.end(),
        [&](const FeatureCapability& c) { return c.name == feat.name; });
    if (it != m_capabilities.end()) {
        *it = feat;
    } else {
        m_capabilities.push_back(feat);
    }
}

const FeatureCapability* FeatureMatrix::FindFeature(const std::string& name) const {
    auto it = std::find_if(m_capabilities.begin(), m_capabilities.end(),
        [&](const FeatureCapability& c) { return c.name == name; });
    return (it != m_capabilities.end()) ? &(*it) : nullptr;
}

} // namespace miami
