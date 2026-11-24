// scarlett.cpp — ScarlettCompat layer DLL.
// Exports the loader API (ScLoadXboxPe / ScRunXboxPe) and the HLE shim
// functions, so guest titles (and the verification harness) link against
// this one DLL. Shim import libs for the Xbox DLL names are generated from
// defs/*.def; at runtime the loader resolves Xbox imports to these
// functions directly — it never loads a real xgameruntime.dll / d3d12_xs.dll
// (and never lets Windows' own System32 xgameruntime.dll win the search).
#include "scarlett.h"
#include "pe_loader.h"
#include "hle_registry.h"

#define SC_API extern "C" __declspec(dllexport)

namespace sc {

// Loader API (exported for the harness).
SC_API bool ScLoadXboxPe(const wchar_t* path, LoadedPe* out, char* err,
                         size_t errSize) {
    return LoadXboxPe(path, out, err, errSize);
}

SC_API bool ScRunXboxPe(LoadedPe* pe, DWORD* exitCode) {
    return RunXboxPe(pe, exitCode);
}

SC_API void ScFreeXboxPe(LoadedPe* pe) { FreeXboxPe(pe); }

// HLE shim functions, exported under their real Xbox names so the shim
// import libs (defs/*.def) can link guest titles against them.
SC_API HRESULT XGameRuntimeInitialize() { return HleXGameRuntimeInitialize(); }
SC_API void* XMemAlloc(SIZE_T size, DWORD attrs) {
    return HleXMemAlloc(size, attrs);
}
SC_API void XMemFree(void* p) { HleXMemFree(p); }
SC_API HRESULT D3D12XboxCreateDevice(IUnknown* adapter,
                                     D3D_FEATURE_LEVEL minFL, REFIID riid,
                                     void** ppDevice) {
    return HleD3D12XboxCreateDevice(adapter, minFL, riid, ppDevice);
}
SC_API HRESULT XAudio2Create(void** ppXAudio2, UINT32 flags, DWORD proc) {
    return HleXAudio2Create(ppXAudio2, flags, proc);
}
SC_API HRESULT GameInputCreate(void** ppGameInput) {
    return HleGameInputCreate(ppGameInput);
}
SC_API HRESULT DStorageGetFactory(void** ppFactory) {
    return HleDStorageGetFactory(ppFactory);
}
SC_API HRESULT SetFrameIntervalX(UINT32 interval, UINT32 flags) {
    return HleSetFrameIntervalX(interval, flags);
}
SC_API HRESULT ScheduleFrameEventX(UINT64 frameId) {
    return HleScheduleFrameEventX(frameId);
}
SC_API HRESULT WaitFrameEventX(UINT64 frameId, DWORD timeoutMs) {
    return HleWaitFrameEventX(frameId, timeoutMs);
}

}  // namespace sc

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)hModule;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        sc::Log("layer", "ScarlettCompat loaded (scaffolding build)");
    }
    return TRUE;
}
