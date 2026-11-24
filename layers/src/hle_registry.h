// hle_registry.h — the HLE export registry (Xenia-style).
// Central table mapping (Xbox DLL, function name) -> host implementation +
// status tag. The loader resolves guest imports through this table.
#pragma once
#include "scarlett.h"
#include <d3d12.h>  // IUnknown + D3D_FEATURE_LEVEL for the device-creation shim

namespace sc {

// Host implementations of the shimmed Xbox functions.
extern "C" HRESULT HleXGameRuntimeInitialize();
extern "C" void* HleXMemAlloc(SIZE_T size, DWORD attrs);
extern "C" void HleXMemFree(void* p);
extern "C" HRESULT HleD3D12XboxCreateDevice(IUnknown* adapter,
                                            D3D_FEATURE_LEVEL minFL,
                                            REFIID riid, void** ppDevice);
extern "C" HRESULT HleXAudio2Create(void** ppXAudio2, UINT32 flags, DWORD proc);
extern "C" HRESULT HleGameInputCreate(void** ppGameInput);
extern "C" HRESULT HleDStorageGetFactory(void** ppFactory);
extern "C" HRESULT HleSetFrameIntervalX(UINT32 interval, UINT32 flags);
extern "C" HRESULT HleScheduleFrameEventX(UINT64 frameId);
extern "C" HRESULT HleWaitFrameEventX(UINT64 frameId, DWORD timeoutMs);

}  // namespace sc
