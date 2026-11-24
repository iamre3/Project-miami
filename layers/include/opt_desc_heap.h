// opt_desc_heap.h — descriptor heap management (docs/OPTIMIZATION.md
// item 5). Xbox and D3D12 model descriptors differently; creating a
// fresh heap per allocation is churn. This module pools descriptors in
// a shader-visible heap with a free-list, growing by chaining a second
// heap when full. These are ScarlettCompat-defined entry points, NOT
// Xbox APIs.
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* ScOptDescHeapHandle;

// Create a pooled heap: one descriptor heap of `capacity` slots
// (shader-visible for CBV_SRV_UAV / SAMPLER types; RTV/DSV heaps are
// not shader-visible and return a zero GPU handle on Alloc). The
// handle owns the heaps; free it with ScOpt_DescHeapDestroy.
HRESULT ScOpt_DescHeapInit(ID3D12Device* device,
                           D3D12_DESCRIPTOR_HEAP_TYPE heapType,
                           uint32_t capacity, ScOptDescHeapHandle* heap);

// Allocate one descriptor slot. cpuHandle/gpuHandle are the real
// handles for that slot (gpuHandle is zero for non-shader-visible
// heap types). index is the global slot index across the heap chain —
// pass it to ScOpt_DescFree. Returns E_OUTOFMEMORY only if a growth
// heap cannot be created.
HRESULT ScOpt_DescAlloc(ScOptDescHeapHandle heap,
                        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
                        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle,
                        uint32_t* index);

// Return a slot to the free-list. The slot must be live (allocated
// and not already freed); freeing a dead index is E_INVALIDARG.
HRESULT ScOpt_DescFree(ScOptDescHeapHandle heap, uint32_t index);

// Counts: capacity = total slots across the heap chain; live =
// currently allocated slots; heaps = heap count in the chain
// (growth indicator: 1 means the initial heap sufficed).
HRESULT ScOpt_DescHeapStats(ScOptDescHeapHandle heap, uint32_t* capacity,
                            uint32_t* live, uint32_t* heaps);

// Release all heaps in the chain. Safe to call with a null handle.
void ScOpt_DescHeapDestroy(ScOptDescHeapHandle heap);

#ifdef __cplusplus
}
#endif
