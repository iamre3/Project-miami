// opt_pso_cache.h — PSO + root-signature caching (docs/OPTIMIZATION.md
// item 1). A layer-side cache: the guest calls these flat entry points
// instead of the raw ID3D12Device methods, and repeated creation of an
// identical PSO/root signature returns the stored object instead of
// paying the driver compile again. These are ScarlettCompat-defined
// entry points, NOT Xbox APIs. A real title's raw COM calls reach the
// raw device until the COM wrapper exists (see docs/OPTIMIZATION.md,
// "The remaining integration").
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Create the per-process cache (safe to call more than once; later
// calls are no-ops returning S_OK). The device is not retained — the
// caller passes it on every Get call, so the cache works for any
// device the guest uses.
HRESULT ScOpt_PsoCacheInit(ID3D12Device* device);

// Graphics PSO: hash the description, return the cached PSO on a hit
// (AddRef'd) or create + store it on a miss. *hit is TRUE on a hit.
// The description is hashed field by field (shader bytecode *contents*,
// never pointers); CachedPSO is deliberately excluded — it is a driver
// hint, not part of the compiled output's identity.
HRESULT ScOpt_PsoCacheGetGraphics(ID3D12Device* device,
                                  const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                  ID3D12PipelineState** pso, BOOL* hit);

// Compute PSO: same shape as the graphics path.
HRESULT ScOpt_PsoCacheGetCompute(ID3D12Device* device,
                                 const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                 ID3D12PipelineState** pso, BOOL* hit);

// Root signature: hash the serialized blob bytes; hit returns the
// stored signature (AddRef'd), miss creates + stores it.
HRESULT ScOpt_PsoCacheGetRootSignature(ID3D12Device* device,
                                       const void* blob, SIZE_T blobLen,
                                       ID3D12RootSignature** rs, BOOL* hit);

// Counts: created = PSOs/root signatures actually compiled and stored;
// hits/misses are Get calls. All three are zero until Init runs.
HRESULT ScOpt_PsoCacheStats(uint32_t* created, uint32_t* hits,
                            uint32_t* misses);

// Release every cached object and free the tables. Safe to call when
// the cache was never initialized.
void ScOpt_PsoCacheDestroy();

#ifdef __cplusplus
}
#endif
