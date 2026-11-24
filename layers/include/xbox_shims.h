// xbox_shims.h — guest-side prototypes for the Xbox (Scarlett) functions the
// layer shims. These are the signatures the SYNTHETIC guest title calls.
// Signatures marked ASSUMED are D3D12CreateDevice-shaped guesses for
// Xbox-only entry points whose real signatures are GDKX/NDA-gated; they MUST
// be confirmed against the real exe's import table + GDKX headers once the
// exe lands. Do not treat them as ground truth.
#pragma once
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// XGameRuntimeInitialize — public GDK API (XGameRuntime.h). First call a
// GDK title makes. Links XGameRuntime.lib; runtime is xgameruntime.dll.
HRESULT XGameRuntimeInitialize(void);

// XMemAlloc / XMemFree — Xbox extended memory API (xmem.dll).
// XMemAlloc(size, attributes). Attributes enum is GDKX-gated; the stub
// ignores it and allocates from the process heap.
void* XMemAlloc(SIZE_T size, DWORD allocationAttributes);
void XMemFree(void* p);

// D3D12XboxCreateDevice — ASSUMED D3D12CreateDevice-shaped signature.
// Real form takes D3D12XBOX_CREATE_DEVICE_PARAMETERS (research brief §3).
// The stub logs the call and returns S_OK with a fake device pointer.
HRESULT D3D12XboxCreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL minFeatureLevel,
                              REFIID riid, void** ppDevice);

// XAudio2Create — public XAudio2 2.9 signature (xaudio2.h).
struct IXAudio2;
HRESULT XAudio2Create(IXAudio2** ppXAudio2, UINT32 flags, DWORD processor);

// GameInputCreate — public GameInput API.
struct IGameInput;
HRESULT GameInputCreate(IGameInput** ppGameInput);

// DirectStorage factory — ASSUMED. Xbox variant is dstorage_xs.
HRESULT DStorageGetFactory(void** ppFactory);

// Frame pacing — ASSUMED signatures. Real Xbox presentation uses
// SetFrameIntervalX / ScheduleFrameEventX / WaitFrameEventX; exact
// parameter shapes are TBD from the exe.
HRESULT SetFrameIntervalX(UINT32 interval, UINT32 flags);
HRESULT ScheduleFrameEventX(UINT64 frameId);
HRESULT WaitFrameEventX(UINT64 frameId, DWORD timeoutMs);

#ifdef __cplusplus
}
#endif
