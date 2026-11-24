// opt_mesh_fallback.cpp — mesh-shader fallback for RDNA1
// (docs/OPTIMIZATION.md item 3). CPU expansion of a meshlet into
// interleaved vertex + index buffers, uploaded to GPU default-heap
// resources through an internal DIRECT queue (synchronous fence wait).
// The fallback VS/PS are compiled once with D3DCompile and cached.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "opt_mesh_fallback.h"
#include "scarlett.h"

using namespace sc;

static const char kFallbackVS[] =
    "struct VSIn { float3 pos : POSITION; float4 col : COLOR; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "VSOut main(VSIn i) { VSOut o; o.pos = float4(i.pos, 1.0); "
    "o.col = i.col; return o; }\n";

static const char kFallbackPS[] =
    "struct PSIn { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "float4 main(PSIn i) : SV_Target { return i.col; }\n";

static ID3DBlob* g_vsBlob = nullptr;
static ID3DBlob* g_psBlob = nullptr;
static ID3D12CommandQueue* g_upQueue = nullptr;
static ID3D12CommandAllocator* g_upAlloc = nullptr;
static ID3D12GraphicsCommandList* g_upList = nullptr;
static ID3D12Fence* g_upFence = nullptr;
static HANDLE g_upEvent = nullptr;
static UINT64 g_upFenceVal = 0;
static bool g_meshInit = false;

static HRESULT CompileOne(const char* src, const char* target,
                          ID3DBlob** out) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &blob, &err);
    if (FAILED(hr)) {
        if (err) {
            Log("opt_mesh", "D3DCompile %s failed: %s", target,
                (const char*)err->GetBufferPointer());
            err->Release();
        }
        return hr;
    }
    *out = blob;
    return S_OK;
}

extern "C" HRESULT ScOpt_MeshFallbackInit(ID3D12Device* device) {
    if (g_meshInit) return S_OK;
    if (!device) return E_INVALIDARG;
    HRESULT hr = S_OK;
    if (!g_vsBlob) hr = CompileOne(kFallbackVS, "vs_5_0", &g_vsBlob);
    if (SUCCEEDED(hr) && !g_psBlob)
        hr = CompileOne(kFallbackPS, "ps_5_0", &g_psBlob);
    if (SUCCEEDED(hr) && !g_upQueue) {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_upQueue));
    }
    if (SUCCEEDED(hr) && !g_upAlloc)
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&g_upAlloc));
    if (SUCCEEDED(hr) && !g_upList)
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       g_upAlloc, nullptr,
                                       IID_PPV_ARGS(&g_upList));
    // A fresh command list starts open; close it once so the allocator
    // can be Reset below (phase 4 gotcha).
    if (SUCCEEDED(hr)) hr = g_upList->Close();
    if (SUCCEEDED(hr) && !g_upFence)
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&g_upFence));
    if (SUCCEEDED(hr) && !g_upEvent)
        g_upEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr) || !g_upEvent) {
        Log("opt_mesh", "MeshFallbackInit FAILED hr=0x%08x", (unsigned)hr);
        return FAILED(hr) ? hr : E_FAIL;
    }
    g_meshInit = true;
    Log("opt_mesh", "mesh fallback initialized (VS %llu B, PS %llu B)",
        (unsigned long long)g_vsBlob->GetBufferSize(),
        (unsigned long long)g_psBlob->GetBufferSize());
    return S_OK;
}

extern "C" HRESULT ScOpt_MeshFallbackGetShaders(const void** vsBlob,
                                                SIZE_T* vsLen,
                                                const void** psBlob,
                                                SIZE_T* psLen) {
    if (!g_meshInit || !vsBlob || !vsLen || !psBlob || !psLen)
        return E_INVALIDARG;
    *vsBlob = g_vsBlob->GetBufferPointer();
    *vsLen = g_vsBlob->GetBufferSize();
    *psBlob = g_psBlob->GetBufferPointer();
    *psLen = g_psBlob->GetBufferSize();
    return S_OK;
}

// Upload `bytes` from CPU memory into a new DEFAULT-heap buffer, ending
// in `finalState`. Synchronous: returns after the copy fence completes.
static HRESULT UploadBuffer(ID3D12Device* device, const void* src,
                            size_t bytes, D3D12_RESOURCE_STATES finalState,
                            ID3D12Resource** out) {
    *out = nullptr;
    ID3D12Resource* dst = nullptr;
    ID3D12Resource* up = nullptr;
    HRESULT hr = S_OK;
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&dst));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr, IID_PPV_ARGS(&up));
    }
    if (SUCCEEDED(hr)) {
        void* mapped = nullptr;
        D3D12_RANGE range = {0, bytes};
        hr = up->Map(0, &range, &mapped);
        if (SUCCEEDED(hr) && mapped) {
            memcpy(mapped, src, bytes);
            D3D12_RANGE empty = {0, 0};
            up->Unmap(0, &empty);
        }
    }
    if (SUCCEEDED(hr)) hr = g_upAlloc->Reset();
    if (SUCCEEDED(hr)) hr = g_upList->Reset(g_upAlloc, nullptr);
    if (SUCCEEDED(hr))
        g_upList->CopyBufferRegion(dst, 0, up, 0, bytes);
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = dst;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = finalState;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_upList->ResourceBarrier(1, &b);
        hr = g_upList->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {g_upList};
        g_upQueue->ExecuteCommandLists(1, lists);
        g_upQueue->Signal(g_upFence, ++g_upFenceVal);
        g_upFence->SetEventOnCompletion(g_upFenceVal, g_upEvent);
        if (WaitForSingleObject(g_upEvent, 30000) != WAIT_OBJECT_0)
            hr = E_FAIL;
    }
    if (up) up->Release();
    if (FAILED(hr)) {
        if (dst) dst->Release();
        return hr;
    }
    *out = dst;
    return S_OK;
}

extern "C" HRESULT ScOpt_MeshFallbackExpand(ID3D12Device* device,
                                            const ScOptMeshlet* meshlet,
                                            ScOptExpandedMesh* expanded) {
    if (!g_meshInit || !device || !meshlet || !expanded) return E_INVALIDARG;
    if (!meshlet->positions || !meshlet->colors || !meshlet->indices ||
        meshlet->vertexCount == 0 || meshlet->indexCount == 0)
        return E_INVALIDARG;
    memset(expanded, 0, sizeof(*expanded));
    // Interleave {float3 pos, float4 col} on the CPU.
    const size_t kStride = 7 * sizeof(float);
    size_t vbBytes = (size_t)meshlet->vertexCount * kStride;
    float* vb = (float*)malloc(vbBytes);
    if (!vb) return E_OUTOFMEMORY;
    for (uint32_t i = 0; i < meshlet->vertexCount; i++) {
        vb[i * 7 + 0] = meshlet->positions[i * 3 + 0];
        vb[i * 7 + 1] = meshlet->positions[i * 3 + 1];
        vb[i * 7 + 2] = meshlet->positions[i * 3 + 2];
        vb[i * 7 + 3] = meshlet->colors[i * 4 + 0];
        vb[i * 7 + 4] = meshlet->colors[i * 4 + 1];
        vb[i * 7 + 5] = meshlet->colors[i * 4 + 2];
        vb[i * 7 + 6] = meshlet->colors[i * 4 + 3];
    }
    size_t ibBytes = (size_t)meshlet->indexCount * sizeof(uint32_t);
    HRESULT hr = UploadBuffer(device, vb, vbBytes,
                              D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                              &expanded->vertexBuffer);
    free(vb);
    if (SUCCEEDED(hr))
        hr = UploadBuffer(device, meshlet->indices, ibBytes,
                          D3D12_RESOURCE_STATE_INDEX_BUFFER,
                          &expanded->indexBuffer);
    if (FAILED(hr)) {
        ScOpt_MeshFallbackFreeExpanded(expanded);
        return hr;
    }
    expanded->vertexCount = meshlet->vertexCount;
    expanded->indexCount = meshlet->indexCount;
    Log("opt_mesh", "meshlet expanded: %u verts, %u indices",
        meshlet->vertexCount, meshlet->indexCount);
    return S_OK;
}

extern "C" void ScOpt_MeshFallbackFreeExpanded(ScOptExpandedMesh* expanded) {
    if (!expanded) return;
    if (expanded->vertexBuffer) expanded->vertexBuffer->Release();
    if (expanded->indexBuffer) expanded->indexBuffer->Release();
    memset(expanded, 0, sizeof(*expanded));
}

extern "C" void ScOpt_MeshFallbackDestroy() {
    if (!g_meshInit) return;
    if (g_upEvent) CloseHandle(g_upEvent);
    if (g_upFence) g_upFence->Release();
    if (g_upList) g_upList->Release();
    if (g_upAlloc) g_upAlloc->Release();
    if (g_upQueue) g_upQueue->Release();
    if (g_vsBlob) g_vsBlob->Release();
    if (g_psBlob) g_psBlob->Release();
    g_vsBlob = g_psBlob = nullptr;
    g_upQueue = nullptr;
    g_upAlloc = nullptr;
    g_upList = nullptr;
    g_upFence = nullptr;
    g_upEvent = nullptr;
    g_meshInit = false;
}
