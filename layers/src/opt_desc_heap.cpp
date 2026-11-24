// opt_desc_heap.cpp — descriptor heap management (docs/OPTIMIZATION.md
// item 5). Pooled shader-visible heaps with a free-list; growth chains
// a second heap of the same capacity. Slot indices are global across
// the chain (heapIndex * capacity + slot).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include "opt_desc_heap.h"
#include "scarlett.h"

using namespace sc;

struct DescHeap {
    ID3D12Device* device = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uint32_t capacity = 0;
    uint32_t increment = 0;
    bool shaderVisible = false;
    std::vector<ID3D12DescriptorHeap*> heaps;
    std::vector<uint32_t> freeList;  // stack of free global indices
    std::vector<uint8_t> live;       // per global index: 1 = allocated
    uint32_t liveCount = 0;
    CRITICAL_SECTION lock;
};

static HRESULT GrowHeap(DescHeap* h) {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = h->type;
    hd.NumDescriptors = h->capacity;
    hd.Flags = h->shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ID3D12DescriptorHeap* heap = nullptr;
    HRESULT hr = h->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    if (FAILED(hr)) return hr;
    uint32_t base = (uint32_t)h->heaps.size() * h->capacity;
    h->heaps.push_back(heap);
    h->live.resize(h->heaps.size() * h->capacity, 0);
    for (uint32_t i = 0; i < h->capacity; i++)
        h->freeList.push_back(base + h->capacity - 1 - i);
    Log("opt_desc", "descriptor heap grew: chain now %llu heaps",
        (unsigned long long)h->heaps.size());
    return S_OK;
}

extern "C" HRESULT ScOpt_DescHeapInit(ID3D12Device* device,
                                      D3D12_DESCRIPTOR_HEAP_TYPE heapType,
                                      uint32_t capacity,
                                      ScOptDescHeapHandle* heap) {
    if (!device || capacity == 0 || !heap) return E_INVALIDARG;
    if (heapType != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        heapType != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
        heapType != D3D12_DESCRIPTOR_HEAP_TYPE_RTV &&
        heapType != D3D12_DESCRIPTOR_HEAP_TYPE_DSV)
        return E_INVALIDARG;
    DescHeap* h = new (std::nothrow) DescHeap();
    if (!h) return E_OUTOFMEMORY;
    h->device = device;
    h->type = heapType;
    h->capacity = capacity;
    h->shaderVisible =
        heapType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        heapType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    h->increment = device->GetDescriptorHandleIncrementSize(heapType);
    InitializeCriticalSection(&h->lock);
    HRESULT hr = GrowHeap(h);
    if (FAILED(hr)) {
        DeleteCriticalSection(&h->lock);
        delete h;
        return hr;
    }
    *heap = (ScOptDescHeapHandle)h;
    Log("opt_desc", "descriptor heap init: type=%d capacity=%u increment=%u",
        (int)heapType, capacity, h->increment);
    return S_OK;
}

extern "C" HRESULT ScOpt_DescAlloc(ScOptDescHeapHandle heap,
                                   D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
                                   D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle,
                                   uint32_t* index) {
    if (!heap || !cpuHandle || !gpuHandle || !index) return E_INVALIDARG;
    DescHeap* h = (DescHeap*)heap;
    EnterCriticalSection(&h->lock);
    if (h->freeList.empty()) {
        HRESULT hr = GrowHeap(h);
        if (FAILED(hr)) {
            LeaveCriticalSection(&h->lock);
            return hr;
        }
    }
    uint32_t idx = h->freeList.back();
    h->freeList.pop_back();
    h->live[idx] = 1;
    h->liveCount++;
    uint32_t heapIdx = idx / h->capacity;
    uint32_t slot = idx % h->capacity;
    ID3D12DescriptorHeap* dh = h->heaps[heapIdx];
    cpuHandle->ptr = dh->GetCPUDescriptorHandleForHeapStart().ptr +
                     (SIZE_T)slot * h->increment;
    gpuHandle->ptr = h->shaderVisible
        ? dh->GetGPUDescriptorHandleForHeapStart().ptr +
              (SIZE_T)slot * h->increment
        : 0;
    *index = idx;
    LeaveCriticalSection(&h->lock);
    return S_OK;
}

extern "C" HRESULT ScOpt_DescFree(ScOptDescHeapHandle heap, uint32_t index) {
    if (!heap) return E_INVALIDARG;
    DescHeap* h = (DescHeap*)heap;
    EnterCriticalSection(&h->lock);
    if (index >= h->live.size() || !h->live[index]) {
        LeaveCriticalSection(&h->lock);
        return E_INVALIDARG;
    }
    h->live[index] = 0;
    h->liveCount--;
    h->freeList.push_back(index);
    LeaveCriticalSection(&h->lock);
    return S_OK;
}

extern "C" HRESULT ScOpt_DescHeapStats(ScOptDescHeapHandle heap,
                                       uint32_t* capacity, uint32_t* live,
                                       uint32_t* heaps) {
    if (!heap || !capacity || !live || !heaps) return E_INVALIDARG;
    DescHeap* h = (DescHeap*)heap;
    EnterCriticalSection(&h->lock);
    *capacity = (uint32_t)h->heaps.size() * h->capacity;
    *live = h->liveCount;
    *heaps = (uint32_t)h->heaps.size();
    LeaveCriticalSection(&h->lock);
    return S_OK;
}

extern "C" void ScOpt_DescHeapDestroy(ScOptDescHeapHandle heap) {
    if (!heap) return;
    DescHeap* h = (DescHeap*)heap;
    EnterCriticalSection(&h->lock);
    for (auto* dh : h->heaps) dh->Release();
    LeaveCriticalSection(&h->lock);
    DeleteCriticalSection(&h->lock);
    delete h;
}
