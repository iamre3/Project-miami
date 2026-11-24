// opt_texcache.cpp — GPU de-tiled texture cache with memory management
// (docs/OPTIMIZATION.md item 2). See opt_texcache.h for the contract.
//
// The cache holds DEFAULT-heap textures the game binds, under a VRAM
// budget with strict LRU eviction. A store de-tiles with a GPU
// compute shader (bpp 4/8/16 — whole-word copies) or the CPU de-tiler
// (bpp 1/2) and uploads the result; the returned texture is in
// PIXEL_SHADER_RESOURCE state and ready to bind. The GPU work is
// done OUTSIDE the cache lock (the lock only guards the table/LRU),
// so a store never blocks lookups while the GPU is busy.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <new>  // std::nothrow (not pulled in by the other headers)
#include "opt_texcache.h"
#include "opt_detile.h"
#include "sc_opt_common.h"
#include "scarlett.h"

using namespace sc;

// ---- GPU de-tile compute shader ----
// One thread per texel. For the linear texel (x, y) it computes the
// tiled byte offset with the SAME Morton mapping as opt_detile.cpp
// (tile index in row-major order, Morton (Z-order) within the tile),
// reads the texel from the tiled buffer and writes it to the linear
// output. bpp must be a whole number of 4-byte words (4/8/16).
//
// The output MUST be a buffer, not the texture: a RWByteAddressBuffer
// UAV bound to a TEXTURE2D is invalid — the 5700 XT removes the
// device (DXGI_ERROR_DEVICE_REMOVED, 0x887A0005) on the dispatch.
// GpuDetile therefore dispatches into a linear staging buffer and
// copies it into the texture with CopyTextureRegion. The staging
// rows are 256-byte aligned (rowPitch), as a placed-footprint copy
// requires.
static const char kDetileCS[] =
    "ByteAddressBuffer tiled : register(t0);\n"
    "RWByteAddressBuffer linearOut : register(u0);\n"
    "cbuffer Params : register(b0) {\n"
    "    uint width; uint height; uint bpp;\n"
    "    uint tileW; uint tileH; uint tilesPerRow;\n"
    "    uint rowPitch;\n"
    "};\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 tid : SV_DispatchThreadID) {\n"
    "    uint x = tid.x; uint y = tid.y;\n"
    "    if (x >= width || y >= height) return;\n"
    "    uint tileX = x / tileW; uint tileY = y / tileH;\n"
    "    uint tx = x % tileW; uint ty = y % tileH;\n"
    "    uint morton = 0;\n"
    "    [unroll] for (uint i = 0; i < 16; i++) {\n"
    "        morton |= ((tx >> i) & 1u) << (2u * i);\n"
    "        morton |= ((ty >> i) & 1u) << (2u * i + 1u);\n"
    "    }\n"
    "    uint tileIndex = tileY * tilesPerRow + tileX;\n"
    "    uint tiledOff = (tileIndex * tileW * tileH + morton) * bpp;\n"
    "    uint linearOff = y * rowPitch + x * bpp;\n"
    "    uint words = bpp / 4;\n"
    "    for (uint w = 0; w < words; w++) {\n"
    "        uint v = tiled.Load(tiledOff + w * 4);\n"
    "        linearOut.Store(linearOff + w * 4, v);\n"
    "    }\n"
    "}\n";

static const size_t kTexCacheSlots = 256;
static const uint32_t kAlignPitch = 256;  // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT

static bool IsPow2(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

static HRESULT Validate(uint32_t w, uint32_t h, uint32_t bpp, uint32_t tileW,
                        uint32_t tileH) {
    if (w == 0 || h == 0) return E_INVALIDARG;
    if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 && bpp != 16)
        return E_INVALIDARG;
    if (!IsPow2(tileW) || !IsPow2(tileH)) return E_INVALIDARG;
    if ((uint64_t)tileW * tileH * bpp != 65536ULL) return E_INVALIDARG;
    if (tileW > 65536 || tileH > 65536) return E_INVALIDARG;
    return S_OK;
}

static DXGI_FORMAT FormatForBpp(uint32_t bpp) {
    switch (bpp) {
        case 1: return DXGI_FORMAT_R8_UNORM;
        case 2: return DXGI_FORMAT_R16_UNORM;
        case 4: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case 8: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case 16: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

struct TexEntry {
    bool used = false;
    uint64_t key = 0;
    ID3D12Resource* resource = nullptr;  // DEFAULT texture, PIXEL_SHADER_RESOURCE
    uint32_t w = 0, h = 0, bpp = 0;
    uint64_t bytes = 0;  // w*h*bpp
    TexEntry* prev = nullptr;  // LRU list: head = MRU, tail = LRU
    TexEntry* next = nullptr;
};

struct TexCache {
    ID3D12Device* device = nullptr;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    uint64_t fenceValue = 0;
    uint64_t budget = SCOPT_TEXCACHE_DEFAULT_BUDGET;
    uint64_t bytesUsed = 0;
    TexEntry table[kTexCacheSlots];
    TexEntry* lruHead = nullptr;  // most recently used
    TexEntry* lruTail = nullptr;  // least recently used
    uint32_t entries = 0;
    uint32_t hits = 0, misses = 0, evictions = 0;
    CRITICAL_SECTION lock;
    bool init = false;
};

// ---- LRU helpers (call with the lock held) ----
static void LruUnlink(TexCache* c, TexEntry* e) {
    if (e->prev) e->prev->next = e->next; else c->lruHead = e->next;
    if (e->next) e->next->prev = e->prev; else c->lruTail = e->prev;
    e->prev = e->next = nullptr;
}

static void LruPushHead(TexCache* c, TexEntry* e) {
    e->prev = nullptr;
    e->next = c->lruHead;
    if (c->lruHead) c->lruHead->prev = e; else c->lruTail = e;
    c->lruHead = e;
}

static void LruMoveToHead(TexCache* c, TexEntry* e) {
    if (c->lruHead == e) return;
    LruUnlink(c, e);
    LruPushHead(c, e);
}

// Evict the least recently used entry. Returns false when empty.
static bool EvictTail(TexCache* c) {
    TexEntry* e = c->lruTail;
    if (!e) return false;
    LruUnlink(c, e);
    if (e->resource) e->resource->Release();
    c->bytesUsed -= e->bytes;
    c->entries--;
    c->evictions++;
    Log("opt_texcache", "evicted key=%llu (%ux%u bpp=%u, %llu bytes; "
                        "%llu/%llu bytes used)",
        (unsigned long long)e->key, e->w, e->h, e->bpp,
        (unsigned long long)e->bytes, (unsigned long long)c->bytesUsed,
        (unsigned long long)c->budget);
    *e = TexEntry{};
    return true;
}

static TexEntry* FindEntry(TexCache* c, uint64_t key) {
    size_t slot = (size_t)(key & (kTexCacheSlots - 1));
    for (size_t probe = 0; probe < kTexCacheSlots; probe++) {
        TexEntry* e = &c->table[(slot + probe) & (kTexCacheSlots - 1)];
        if (!e->used) return nullptr;
        if (e->key == key) return e;
    }
    return nullptr;
}

static TexEntry* InsertEntry(TexCache* c, uint64_t key,
                             ID3D12Resource* resource, uint32_t w,
                             uint32_t h, uint32_t bpp, uint64_t bytes) {
    // Dedupe: another store of the same key may have landed first.
    TexEntry* existing = FindEntry(c, key);
    if (existing) return existing;
    while (c->bytesUsed + bytes > c->budget && c->entries > 0)
        EvictTail(c);
    if (c->bytesUsed + bytes > c->budget && bytes > c->budget)
        Log("opt_texcache",
            "WARNING: single texture %llu bytes exceeds budget %llu — "
            "stored alone",
            (unsigned long long)bytes, (unsigned long long)c->budget);
    size_t slot = (size_t)(key & (kTexCacheSlots - 1));
    for (size_t probe = 0; probe < kTexCacheSlots; probe++) {
        TexEntry* e = &c->table[(slot + probe) & (kTexCacheSlots - 1)];
        if (!e->used) {
            e->used = true;
            e->key = key;
            e->resource = resource;  // takes ownership of one reference
            e->w = w;
            e->h = h;
            e->bpp = bpp;
            e->bytes = bytes;
            LruPushHead(c, e);
            c->bytesUsed += bytes;
            c->entries++;
            return e;
        }
    }
    // Table full (256 live entries, all within budget): evict the LRU
    // entry to make a slot and retry once.
    if (EvictTail(c)) {
        for (size_t probe = 0; probe < kTexCacheSlots; probe++) {
            TexEntry* e =
                &c->table[(slot + probe) & (kTexCacheSlots - 1)];
            if (!e->used) {
                e->used = true;
                e->key = key;
                e->resource = resource;
                e->w = w;
                e->h = h;
                e->bpp = bpp;
                e->bytes = bytes;
                LruPushHead(c, e);
                c->bytesUsed += bytes;
                c->entries++;
                return e;
            }
        }
    }
    return nullptr;
}

// ---- GPU work helpers ----
static HRESULT CreateBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heapType,
                            uint64_t size, D3D12_RESOURCE_STATES state,
                            D3D12_RESOURCE_FLAGS flags,
                            ID3D12Resource** out) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heapType;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    return device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                           state, nullptr,
                                           IID_PPV_ARGS(out));
}

static HRESULT WaitForQueue(TexCache* c, ID3D12CommandQueue* queue) {
    uint64_t v = ++c->fenceValue;
    HRESULT hr = queue->Signal(c->fence, v);
    if (FAILED(hr)) return hr;
    hr = c->fence->SetEventOnCompletion(v, c->fenceEvent);
    if (FAILED(hr)) return hr;
    if (WaitForSingleObject(c->fenceEvent, 30000) != WAIT_OBJECT_0)
        return E_FAIL;
    return S_OK;
}

static HRESULT RecordAndRun(TexCache* c, ID3D12CommandQueue* queue,
                            ID3D12GraphicsCommandList* list) {
    HRESULT hr = list->Close();
    if (FAILED(hr)) return hr;
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    return WaitForQueue(c, queue);
}

// GPU compute de-tile into `texture` (DEFAULT, UNORDERED_ACCESS).
// `tiledBuf` is the DEFAULT buffer holding the tiled bytes in
// NON_PIXEL_SHADER_RESOURCE state. Leaves the texture in
// PIXEL_SHADER_RESOURCE state. The dispatch writes into a linear
// staging BUFFER (a ByteAddressBuffer UAV must be a buffer — see the
// shader comment), which is then copied into the texture.
static HRESULT GpuDetile(TexCache* c, ID3D12CommandQueue* queue,
                         ID3D12Resource* tiledBuf, ID3D12Resource* texture,
                         uint32_t w, uint32_t h, uint32_t bpp, uint32_t tileW,
                         uint32_t tileH) {
    // 256-byte aligned staging rows (placed-footprint copy requirement).
    uint32_t rowPitch =
        (uint32_t)(((uint64_t)w * bpp + (kAlignPitch - 1)) &
                   ~(uint64_t)(kAlignPitch - 1));
    uint64_t stageBytes = (uint64_t)rowPitch * h;
    ID3D12Resource* linearBuf = nullptr;
    HRESULT hr = CreateBuffer(c->device, D3D12_HEAP_TYPE_DEFAULT, stageBytes,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                              &linearBuf);
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    if (SUCCEEDED(hr))
        hr = c->device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(hr))
        hr = c->device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
            IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) {
        list->SetComputeRootSignature(c->rs);
        list->SetPipelineState(c->pso);
        uint32_t tilesPerRow = (w + tileW - 1) / tileW;
        UINT constants[7] = {w, h, bpp, tileW, tileH, tilesPerRow, rowPitch};
        list->SetComputeRoot32BitConstants(0, 7, constants, 0);
        list->SetComputeRootShaderResourceView(
            1, tiledBuf->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            2, linearBuf->GetGPUVirtualAddress());
        list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        // Staging buffer UAV -> COPY_SOURCE; texture UAV -> COPY_DEST.
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = linearBuf;
        barriers[0].Transition.StateBefore =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[0].Transition.StateAfter =
            D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = texture;
        barriers[1].Transition.StateBefore =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[1].Transition.StateAfter =
            D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(2, barriers);
        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource = texture;
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = linearBuf;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint.Offset = 0;
        srcLoc.PlacedFootprint.Footprint.Format = FormatForBpp(bpp);
        srcLoc.PlacedFootprint.Footprint.Width = w;
        srcLoc.PlacedFootprint.Footprint.Height = h;
        srcLoc.PlacedFootprint.Footprint.Depth = 1;
        srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;
        list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
        hr = RecordAndRun(c, queue, list);
    }
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (linearBuf) linearBuf->Release();
    return hr;
}

// CPU de-tile + upload into `texture` (DEFAULT, COPY_DEST). Leaves the
// texture in PIXEL_SHADER_RESOURCE state.
static HRESULT CpuDetileUpload(TexCache* c, ID3D12CommandQueue* queue,
                               const uint8_t* tiled, ID3D12Resource* texture,
                               uint32_t w, uint32_t h, uint32_t bpp,
                               uint32_t tileW, uint32_t tileH) {
    uint64_t bytes = (uint64_t)w * h * bpp;
    uint8_t* linear = (uint8_t*)malloc((size_t)bytes);
    if (!linear) return E_OUTOFMEMORY;
    HRESULT hr = ScOpt_Detile2D(tiled, w, h, bpp, tileW, tileH, linear);
    ID3D12Resource* upload = nullptr;
    uint32_t rowPitch = (uint32_t)((w * bpp + kAlignPitch - 1) &
                                   ~(kAlignPitch - 1));
    uint64_t uploadSize = (uint64_t)rowPitch * h;
    if (SUCCEEDED(hr))
        hr = CreateBuffer(c->device, D3D12_HEAP_TYPE_UPLOAD, uploadSize,
                          D3D12_RESOURCE_STATE_GENERIC_READ,
                          D3D12_RESOURCE_FLAG_NONE, &upload);
    if (SUCCEEDED(hr)) {
        void* mapped = nullptr;
        D3D12_RANGE range = {0, (SIZE_T)uploadSize};
        hr = upload->Map(0, &range, &mapped);
        if (SUCCEEDED(hr) && mapped) {
            uint8_t* dst = (uint8_t*)mapped;
            for (uint32_t y = 0; y < h; y++)
                memcpy(dst + (size_t)y * rowPitch,
                       linear + (size_t)y * w * bpp, (size_t)w * bpp);
            D3D12_RANGE empty = {0, 0};
            upload->Unmap(0, &empty);
        }
    }
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    if (SUCCEEDED(hr))
        hr = c->device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(hr))
        hr = c->device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
            IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) {
        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource = texture;
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = upload;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint.Offset = 0;
        srcLoc.PlacedFootprint.Footprint.Format = FormatForBpp(bpp);
        srcLoc.PlacedFootprint.Footprint.Width = w;
        srcLoc.PlacedFootprint.Footprint.Height = h;
        srcLoc.PlacedFootprint.Footprint.Depth = 1;
        srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;
        list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
        hr = RecordAndRun(c, queue, list);
    }
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (upload) upload->Release();
    free(linear);
    return hr;
}

// ---- public API ----
extern "C" HRESULT ScOpt_TexCacheInit(ID3D12Device* device,
                                      uint64_t budgetBytes,
                                      ScOptTexCacheHandle* cache) {
    if (!device || !cache) return E_INVALIDARG;
    TexCache* c = new (std::nothrow) TexCache();
    if (!c) return E_OUTOFMEMORY;
    memset(c->table, 0, sizeof(c->table));
    c->device = device;
    device->AddRef();
    c->budget = budgetBytes ? budgetBytes : SCOPT_TEXCACHE_DEFAULT_BUDGET;
    InitializeCriticalSection(&c->lock);

    ID3DBlob* csBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    HRESULT hr = D3DCompile(kDetileCS, strlen(kDetileCS), "ScOptDetileCS",
                            nullptr, nullptr, "main", "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &csBlob,
                            &errBlob);
    if (FAILED(hr)) {
        if (errBlob)
            Log("opt_texcache", "D3DCompile detile CS failed: %s",
                (const char*)errBlob->GetBufferPointer());
        else
            Log("opt_texcache", "D3DCompile detile CS failed hr=0x%08x",
                (unsigned)hr);
    }
    if (SUCCEEDED(hr)) {
        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 7;
        params[0].Constants.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 0;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[2].Descriptor.ShaderRegister = 0;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 3;
        rsd.pParameters = params;
        ID3DBlob* rsBlob = nullptr;
        hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                         &rsBlob, nullptr);
        if (SUCCEEDED(hr))
            hr = device->CreateRootSignature(
                0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                IID_PPV_ARGS(&c->rs));
        if (rsBlob) rsBlob->Release();
    }
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd = {};
        cpd.pRootSignature = c->rs;
        cpd.CS.pShaderBytecode = csBlob->GetBufferPointer();
        cpd.CS.BytecodeLength = csBlob->GetBufferSize();
        hr = device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&c->pso));
    }
    if (SUCCEEDED(hr))
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&c->fence));
    if (SUCCEEDED(hr)) {
        c->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!c->fenceEvent) hr = E_FAIL;
    }
    if (csBlob) csBlob->Release();
    if (errBlob) errBlob->Release();
    if (FAILED(hr)) {
        if (c->fence) c->fence->Release();
        if (c->fenceEvent) CloseHandle(c->fenceEvent);
        if (c->pso) c->pso->Release();
        if (c->rs) c->rs->Release();
        DeleteCriticalSection(&c->lock);
        c->device->Release();
        delete c;
        return hr;
    }
    c->init = true;
    *cache = (ScOptTexCacheHandle)c;
    Log("opt_texcache", "init (budget=%llu MB)",
        (unsigned long long)(c->budget / (1024 * 1024)));
    return S_OK;
}

extern "C" HRESULT ScOpt_TexCacheLookup(ScOptTexCacheHandle cache,
                                        uint64_t key,
                                        ID3D12Resource** resource,
                                        BOOL* hit) {
    if (!cache || !resource || !hit) return E_INVALIDARG;
    TexCache* c = (TexCache*)cache;
    *resource = nullptr;
    *hit = FALSE;
    EnterCriticalSection(&c->lock);
    TexEntry* e = FindEntry(c, key);
    if (e) {
        LruMoveToHead(c, e);
        *resource = e->resource;
        (*resource)->AddRef();
        *hit = TRUE;
        c->hits++;
    } else {
        c->misses++;
    }
    LeaveCriticalSection(&c->lock);
    return S_OK;
}

extern "C" HRESULT ScOpt_TexCacheStore(ScOptTexCacheHandle cache,
                                       uint64_t key, const uint8_t* tiled,
                                       uint32_t w, uint32_t h, uint32_t bpp,
                                       uint32_t tileW, uint32_t tileH,
                                       ID3D12CommandQueue* queue,
                                       ID3D12Resource** resource) {
    if (!cache || !tiled || !queue || !resource) return E_INVALIDARG;
    HRESULT hr = Validate(w, h, bpp, tileW, tileH);
    if (FAILED(hr)) return hr;
    TexCache* c = (TexCache*)cache;
    *resource = nullptr;
    uint64_t bytes = (uint64_t)w * h * bpp;

    // Dedupe: already cached?
    EnterCriticalSection(&c->lock);
    TexEntry* existing = FindEntry(c, key);
    if (existing) {
        LruMoveToHead(c, existing);
        *resource = existing->resource;
        (*resource)->AddRef();
        c->hits++;
        LeaveCriticalSection(&c->lock);
        return S_OK;
    }
    LeaveCriticalSection(&c->lock);

    // Build the texture OUTSIDE the lock (GPU work must not block
    // lookups). GPU compute path for bpp 4/8/16; CPU fallback for 1/2.
    bool gpuPath = (bpp == 4 || bpp == 8 || bpp == 16);
    ID3D12Resource* texture = nullptr;
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = FormatForBpp(bpp);
        rd.SampleDesc.Count = 1;
        rd.Flags = gpuPath ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                           : D3D12_RESOURCE_FLAG_NONE;
        hr = c->device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            gpuPath ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                    : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&texture));
    }
    ID3D12Resource* upload = nullptr;
    ID3D12Resource* tiledBuf = nullptr;
    if (SUCCEEDED(hr) && gpuPath) {
        // Upload heap staging for the tiled bytes.
        hr = CreateBuffer(c->device, D3D12_HEAP_TYPE_UPLOAD, bytes,
                          D3D12_RESOURCE_STATE_GENERIC_READ,
                          D3D12_RESOURCE_FLAG_NONE, &upload);
        if (SUCCEEDED(hr)) {
            void* mapped = nullptr;
            D3D12_RANGE range = {0, (SIZE_T)bytes};
            hr = upload->Map(0, &range, &mapped);
            if (SUCCEEDED(hr) && mapped) {
                memcpy(mapped, tiled, (size_t)bytes);
                D3D12_RANGE empty = {0, 0};
                upload->Unmap(0, &empty);
            }
        }
        if (SUCCEEDED(hr))
            hr = CreateBuffer(c->device, D3D12_HEAP_TYPE_DEFAULT, bytes,
                              D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_FLAG_NONE, &tiledBuf);
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        if (SUCCEEDED(hr))
            hr = c->device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        if (SUCCEEDED(hr))
            hr = c->device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr)) {
            list->CopyBufferRegion(tiledBuf, 0, upload, 0, bytes);
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = tiledBuf;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &barrier);
            hr = RecordAndRun(c, queue, list);
        }
        if (list) list->Release();
        if (alloc) alloc->Release();
        if (SUCCEEDED(hr))
            hr = GpuDetile(c, queue, tiledBuf, texture, w, h, bpp, tileW,
                           tileH);
    } else if (SUCCEEDED(hr)) {
        hr = CpuDetileUpload(c, queue, tiled, texture, w, h, bpp, tileW,
                             tileH);
    }
    if (upload) upload->Release();
    if (tiledBuf) tiledBuf->Release();
    if (FAILED(hr)) {
        if (texture) texture->Release();
        return hr;
    }

    // Insert under the lock (evicting LRU entries to fit the budget).
    // Another thread may have stored the same key meanwhile — then
    // InsertEntry returns the existing entry and our texture is
    // released.
    EnterCriticalSection(&c->lock);
    TexEntry* e = InsertEntry(c, key, texture, w, h, bpp, bytes);
    if (!e) {
        LeaveCriticalSection(&c->lock);
        texture->Release();
        return E_FAIL;
    }
    if (e->resource != texture) {
        // Raced: an identical entry already existed.
        LruMoveToHead(c, e);
        texture->Release();
        *resource = e->resource;
        (*resource)->AddRef();
        c->hits++;
    } else {
        // Our texture is now owned by the cache; hand the caller its
        // own reference.
        *resource = texture;
        texture->AddRef();
    }
    LeaveCriticalSection(&c->lock);
    Log("opt_texcache", "stored key=%llu (%ux%u bpp=%u, %s path, %llu/%llu "
                        "bytes used)",
        (unsigned long long)key, w, h, bpp, gpuPath ? "GPU compute" : "CPU",
        (unsigned long long)c->bytesUsed, (unsigned long long)c->budget);
    return S_OK;
}

extern "C" HRESULT ScOpt_TexCacheEvict(ScOptTexCacheHandle cache,
                                       uint64_t key) {
    if (!cache) return E_INVALIDARG;
    TexCache* c = (TexCache*)cache;
    EnterCriticalSection(&c->lock);
    TexEntry* e = FindEntry(c, key);
    if (!e) {
        LeaveCriticalSection(&c->lock);
        return S_FALSE;
    }
    LruUnlink(c, e);
    if (e->resource) e->resource->Release();
    c->bytesUsed -= e->bytes;
    c->entries--;
    Log("opt_texcache", "evicted (game unload) key=%llu (%llu/%llu bytes "
                        "used)",
        (unsigned long long)key, (unsigned long long)c->bytesUsed,
        (unsigned long long)c->budget);
    *e = TexEntry{};
    LeaveCriticalSection(&c->lock);
    return S_OK;
}

extern "C" HRESULT ScOpt_TexCacheStats(ScOptTexCacheHandle cache,
                                       uint32_t* entries,
                                       uint64_t* bytesUsed,
                                       uint64_t* bytesBudget, uint32_t* hits,
                                       uint32_t* misses, uint32_t* evictions) {
    if (!cache || !entries || !bytesUsed || !bytesBudget || !hits ||
        !misses || !evictions)
        return E_INVALIDARG;
    TexCache* c = (TexCache*)cache;
    EnterCriticalSection(&c->lock);
    *entries = c->entries;
    *bytesUsed = c->bytesUsed;
    *bytesBudget = c->budget;
    *hits = c->hits;
    *misses = c->misses;
    *evictions = c->evictions;
    LeaveCriticalSection(&c->lock);
    return S_OK;
}

extern "C" void ScOpt_TexCacheDestroy(ScOptTexCacheHandle cache) {
    if (!cache) return;
    TexCache* c = (TexCache*)cache;
    EnterCriticalSection(&c->lock);
    for (size_t i = 0; i < kTexCacheSlots; i++) {
        if (c->table[i].used) {
            if (c->table[i].resource) c->table[i].resource->Release();
            c->table[i] = TexEntry{};
        }
    }
    c->lruHead = c->lruTail = nullptr;
    c->bytesUsed = 0;
    c->entries = 0;
    LeaveCriticalSection(&c->lock);
    if (c->fence) c->fence->Release();
    if (c->fenceEvent) CloseHandle(c->fenceEvent);
    if (c->pso) c->pso->Release();
    if (c->rs) c->rs->Release();
    DeleteCriticalSection(&c->lock);
    c->device->Release();
    delete c;
}
