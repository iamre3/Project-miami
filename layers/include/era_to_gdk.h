// era_to_gdk.h — ERA (Xbox One) -> GDK translation boundary (phase 5).
// ERA entry points -> GDK equivalents, wired into the HLE registry.
// See docs/ERA-GDK-MAPPING.md for the inventory and the honesty notes.
// Signatures marked GROUNDED follow the public Xbox One shim-DLL
// XDK-derived d3d11.x.cpp. Anything still marked ASSUMED has an
// XDK-gated real signature that no public source confirms yet.
#pragma once
#include "scarlett.h"
#include <d3d11.h>   // ID3D11Device / D3D11CreateDevice shapes
#include <hstring.h> // HSTRING (RoGetActivationFactory)

namespace sc {

// True when the module is an ERA (Xbox One) DLL the layer translates:
// d3d11_x, d3d12_x, kernelx, xg_x, AcpHal. (XAudio2_9 is shared by ERA
// and GDK, so it is not ERA-only and is not in this list.)
bool EraIsEraModule(const char* module);

// ---- ERA kernel memory (1:1 maps + defaults) ----
// kernelx XMemAlloc/Free are the same API as xmem.dll's (see
// hle_registry.cpp). XMemAllocDefault/FreeDefault are the default
// implementations the CRT hooks point at; XMemSetAllocationHooks lets a
// title install its own alloc hooks (accepted, logged, not routed —
// the layer's allocations do not call back into title hooks).
// TitleMemoryStatus fills a memory-status struct; its real signature
// and struct layout are XDK-gated, so the declared form (one pointer,
// zero-fill 64 bytes, return 0) is ASSUMED. All four are in the ERA
// surface (imported by name; the ordinals in the registry are the
// Xbox One shim-DLL ordinals).
extern "C" void* EraXMemAllocDefault(SIZE_T size, DWORD attrs);
extern "C" void EraXMemFreeDefault(void* p);
extern "C" HRESULT EraXMemSetAllocationHooks(void* allocHook,
                                             void* freeHook);
extern "C" DWORD EraTitleMemoryStatus(void* status);

// ---- ERA graphics (adaptation) ----
// D3D11XCreateDevice / D3D11XCreateDeviceAndSwapChain —
// D3D11CreateDevice-shaped (the same 10-arg form the public Xbox One
// shim-DLL d3d11.x.cpp uses for EraD3D11CreateDevice). The translation
// lands on a real desktop D3D11 device: GDK on Xbox does not support
// D3D11.x at all, so there is no d3d12_xs equivalent for a D3D11.x
// title.
// D3D11XCreateDeviceX is the device entry (Xbox One shim-DLL ordinal
// 4). Its real form (the public Xbox One shim-DLL d3d11.x.cpp,
// XDK-derived) is THREE args — a D3D11X_CREATE_DEVICE_PARAMETERS
// pointer, a device out-pointer, and a context out-pointer — NOT the
// 11-arg D3D11CreateDevice-shaped form phase 5 first assumed. The
// struct is reproduced below from that source. The other two d3d11_x
// imports in the ERA surface are now IMPLEMENTED too:
// D3DMapEsramMemory (ESRAM mapping, see below) and DXGIXPresentArray
// (present every swapchain in the array).
// D3DAllocateGraphicsMemory / D3DFreeGraphicsMemory (d3d11_x ordinals
// 6 / 8 in the Xbox One shim-DLL build) are the graphics-memory
// allocator the ESRAM work sits alongside: VirtualAlloc-backed on the
// host. They are supporting infrastructure, tested by the era guest.
struct D3D11X_CREATE_DEVICE_PARAMETERS {
    UINT Version;
    UINT Flags;
    void* pOffchipTessellationBuffer;
    void* pTessellationFactorsBuffer;
    UINT DeferredDeletionThreadAffinityMask;
    UINT ImmediateContextDeRingSizeBytes;
    UINT ImmediateContextCeRingSizeBytes;
    UINT ImmediateContextDeSegmentSizeBytes;
    UINT ImmediateContextCeSegmentSizeBytes;
};
extern "C" HRESULT EraD3D11XCreateDevice(
    IUnknown* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext);
extern "C" HRESULT EraD3D11XCreateDeviceAndSwapChain(
    IUnknown* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext);
extern "C" HRESULT EraD3D11XCreateDeviceX(
    const D3D11X_CREATE_DEVICE_PARAMETERS* pParameters,
    ID3D11Device** ppDevice, ID3D11DeviceContext** ppImmediateContext);
// ESRAM (Xbox One, 32 MB on-die SRAM — NOT the Xbox 360's 10 MB
// EDRAM). D3DMapEsramMemory maps ESRAM pages into the title's virtual
// address space: (Flags, pVirtualAddress, NumPages, pPageList) —
// the public Xbox One shim-DLL d3d11.x.cpp. Flags bit 0 = large pages,
// bit 1 = 4 MB pages. The host has no ESRAM, so the layer maps real
// committed memory at the requested VA and enforces the 32 MB arena
// budget (see src/era_to_gdk.cpp). DXGIXPresentArray presents every
// swapchain in the array (SyncInterval, PresentImmediateThreshold,
// Flags, NumSwapChains, ppSwapChains, pPresentParameters) — the public
// Xbox One shim-DLL d3d11.x.cpp; per-swapchain present parameters
// (source rect, scale) are accepted and ignored (full back buffer is
// presented).
extern "C" HRESULT EraD3DMapEsramMemory(UINT Flags, void* pVirtualAddress,
                                        UINT NumPages,
                                        const UINT* pPageList);
extern "C" HRESULT EraDXGIXPresentArray(
    UINT SyncInterval, UINT PresentImmediateThreshold, UINT Flags,
    UINT NumSwapChains, void** ppSwapChains, const void* pPresentParameters);
extern "C" HRESULT EraD3DAllocateGraphicsMemory(SIZE_T SizeInBytes,
                                                UINT64 AlignmentBytes,
                                                UINT64 DesiredGpuVirtualAddress,
                                                UINT Flags,
                                                void** ppAddress);
extern "C" HRESULT EraD3DFreeGraphicsMemory(void* pAddress);

// ---- ERA texture tiling (xg_x.dll — no GDK equivalent) ----
// Xbox One shim-DLL ordinals 2,3,5,6,8,9,10. Xbox textures are
// tiled/swizzled for the console GPU; desktop D3D11 has no equivalent
// layout computation, so these are explicit E_NOTIMPL stubs (logged
// with their first four raw args — the real signatures are XDK-gated).
// A real translation would need to compute a linear layout and
// re-tile on upload; that is genuinely missing, not stubbed as
// working.
extern "C" HRESULT EraXGComputeOptimalDepthStencilTileModes(
    uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);
extern "C" HRESULT EraXGComputeOptimalTileMode(uintptr_t a0, uintptr_t a1,
                                               uintptr_t a2, uintptr_t a3);
extern "C" HRESULT EraXGComputeTexture2DLayout(uintptr_t a0, uintptr_t a1,
                                               uintptr_t a2, uintptr_t a3);
extern "C" HRESULT EraXGComputeTexture3DLayout(uintptr_t a0, uintptr_t a1,
                                               uintptr_t a2, uintptr_t a3);
extern "C" HRESULT EraXGCreateTexture1DComputer(uintptr_t a0, uintptr_t a1,
                                                uintptr_t a2, uintptr_t a3);
extern "C" HRESULT EraXGCreateTexture2DComputer(uintptr_t a0, uintptr_t a1,
                                                uintptr_t a2, uintptr_t a3);
extern "C" HRESULT EraXGCreateTexture3DComputer(uintptr_t a0, uintptr_t a1,
                                                uintptr_t a2, uintptr_t a3);

// ---- ERA audio DSP heap (AcpHal.dll — no GDK equivalent) ----
// Xbox One shim-DLL ordinals 1,2,3,4,5,6,8,9,10. The ACP is the
// console's audio DSP; desktop has no equivalent heap, so these are
// explicit E_NOTIMPL stubs (logged with their first four raw args —
// the real signatures are XDK-gated).
extern "C" HRESULT EraAcpHalAllocateShapeContexts(uintptr_t a0, uintptr_t a1,
                                                  uintptr_t a2, uintptr_t a3);
extern "C" HRESULT EraAcpHalCreate(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                   uintptr_t a3);
extern "C" HRESULT EraAcpHalReleaseShapeContexts(uintptr_t a0, uintptr_t a1,
                                                 uintptr_t a2, uintptr_t a3);
extern "C" HRESULT EraApuAlloc(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                               uintptr_t a3);
extern "C" HRESULT EraApuCreateHeap(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                    uintptr_t a3);
extern "C" HRESULT EraApuFree(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                              uintptr_t a3);
extern "C" HRESULT EraApuIsVirtualAddressValid(uintptr_t a0, uintptr_t a1,
                                               uintptr_t a2, uintptr_t a3);
extern "C" HRESULT EraApuMapApuAddress(uintptr_t a0, uintptr_t a1,
                                       uintptr_t a2, uintptr_t a3);
extern "C" HRESULT EraApuMapVirtualAddress(uintptr_t a0, uintptr_t a1,
                                           uintptr_t a2, uintptr_t a3);

// ---- WinRT activation routing ----
// RoGetActivationFactory hook target (same signature as combase.dll's).
// Xbox-namespace class names (Windows.Xbox.*, Windows.ApplicationModel.
// Package) are refused with REGDB_E_CLASSNOTREG and logged — the layer
// has no COM objects for them yet (XDK-gated interface definitions).
// Every other class name forwards to the real RoGetActivationFactory.
// The loader stashes the real function with EraSetRealRoGetActivationFactory
// when it installs the hook (see pe_loader.cpp).
typedef HRESULT(WINAPI* RoGetActivationFactoryFn)(HSTRING activatableClassId,
                                                  REFIID iid,
                                                  void** factory);
void EraSetRealRoGetActivationFactory(RoGetActivationFactoryFn realFn);
extern "C" HRESULT EraRoGetActivationFactory(HSTRING activatableClassId,
                                             REFIID iid, void** factory);

}  // namespace sc
