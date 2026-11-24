// guest_opt.cpp — optimization proof guest.
// A synthetic Game Core title that exercises the layer's optimization
// entry points (docs/OPTIMIZATION.md) through the normal loader path:
//   XGameRuntimeInitialize -> D3D12XboxCreateDevice (real desktop
//   device) -> one stage per optimization item, each reporting
//   opt_<stage> in guest_report.txt with measured before/after numbers
//   in the detail string.
// Stages: opt_pso (PSO/root-sig cache), opt_detile (de-tiled cache),
// opt_mesh (mesh-shader fallback, pixel-verified), opt_async (async
// compute serial vs async, checksum-compared), opt_desc (descriptor
// heap pool), opt_dstorage (async read queue + decompression stage),
// opt_shader (shader precompilation cache).
// This file contains NO game code — it is a test program written for
// the layer, and it is never run through the Windows loader directly
// (its Xbox imports only resolve inside the layer).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include "xbox_shims.h"
#include "sc_opt_common.h"
#include "opt_pso_cache.h"
#include "opt_detile.h"
#include "opt_mesh_fallback.h"
#include "opt_async_compute.h"
#include "opt_desc_heap.h"
#include "opt_dstorage.h"
#include "opt_shader_cache.h"
#include "opt_batching.h"
#include "opt_texcache.h"

// Xbox build-info section, like the shipped DLLs carry.
#pragma section(".xbld", read)
__declspec(allocate(".xbld")) static const char kXbld[] =
    "_xbld_edition_name=GXDK,October 2026 GXDK\0"
    "_xbld_edition_full_productbuild=GXDK,10.0.26100.9605\0"
    "_xbld_edition_sdktype=GXDK,261000\0"
    "_xbld_edition_mscver=GXDK,195237025.0\0"
    "_xbld_edition_build=GXDK,0x65F42585\0";

static FILE* g_report = nullptr;
static int g_stagesTotal = 0;
static int g_stagesPassed = 0;

static void Report(const char* name, bool pass, const char* detail) {
    printf("[guest] %-16s %s %s\n", name, pass ? "PASS" : "FAIL",
           detail ? detail : "");
    if (g_report) {
        fprintf(g_report, "%s=%s\n", name, pass ? "PASS" : "FAIL");
        if (detail) fprintf(g_report, "%s_detail=%s\n", name, detail);
        fflush(g_report);
    }
    if (strncmp(name, "opt", 3) == 0) {
        g_stagesTotal++;
        if (pass) g_stagesPassed++;
    }
}

using namespace sc;

// Small shaders for the PSO stage (same shape as the proof guest's).
static const char* kVsSrc =
    "struct VSIn { float3 pos : POSITION; float4 col : COLOR; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "VSOut main(VSIn i) { VSOut o; o.pos = float4(i.pos, 1.0); "
    "o.col = i.col; return o; }\n";
static const char* kPsSrc =
    "struct PSIn { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "float4 main(PSIn i) : SV_Target { return i.col; }\n";
static const char* kPsGreenSrc =
    "struct PSIn { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "float4 main(PSIn i) : SV_Target { return float4(0,1,0,1); }\n";

static bool CompileShader(const char* src, const char* profile,
                          ID3DBlob** blob) {
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            "main", profile, 0, 0, blob, &errors);
    if (FAILED(hr) && errors) {
        printf("[guest] D3DCompile %s failed: %s\n", profile,
               (const char*)errors->GetBufferPointer());
        errors->Release();
    }
    return SUCCEEDED(hr);
}

// Read back the center + corner pixels of a 256x256 RT. The caller
// must have already waited for the draw to finish.
static bool ReadbackPixels(ID3D12Device* device, ID3D12CommandQueue* q,
                           ID3D12Resource* rt, uint8_t center[4],
                           uint8_t corner[4]) {
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEv = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceVal = 0;
    HRESULT hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       alloc, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) hr = list->Close();
    if (SUCCEEDED(hr))
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&fence));
    if (FAILED(hr) || !fenceEv) {
        if (alloc) alloc->Release();
        if (list) list->Release();
        if (fence) fence->Release();
        if (fenceEv) CloseHandle(fenceEv);
        return false;
    }
    D3D12_RESOURCE_DESC texDesc = rt->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSize = 0, totalBytes = 0;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows,
                                  &rowSize, &totalBytes);
    ID3D12Resource* readback = nullptr;
    D3D12_HEAP_PROPERTIES hpRb = {};
    hpRb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbDesc = {};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = totalBytes;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = device->CreateCommittedResource(&hpRb, D3D12_HEAP_FLAG_NONE,
                                         &rbDesc,
                                         D3D12_RESOURCE_STATE_COPY_DEST,
                                         nullptr, IID_PPV_ARGS(&readback));
    if (SUCCEEDED(hr)) hr = alloc->Reset();
    if (SUCCEEDED(hr)) hr = list->Reset(alloc, nullptr);
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = rt;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = rt;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        hr = list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        q->ExecuteCommandLists(1, lists);
        q->Signal(fence, ++fenceVal);
        fence->SetEventOnCompletion(fenceVal, fenceEv);
        if (WaitForSingleObject(fenceEv, 30000) != WAIT_OBJECT_0)
            hr = E_FAIL;
    }
    bool ok = false;
    if (SUCCEEDED(hr)) {
        void* mapped = nullptr;
        D3D12_RANGE range = {0, totalBytes};
        hr = readback->Map(0, &range, &mapped);
        if (SUCCEEDED(hr) && mapped) {
            const uint8_t* base = (const uint8_t*)mapped;
            auto pxAt = [&](int x, int y) -> const uint8_t* {
                return base + footprint.Offset +
                       (uint64_t)y * footprint.Footprint.RowPitch + x * 4;
            };
            memcpy(center, pxAt(128, 128), 4);
            memcpy(corner, pxAt(10, 10), 4);
            ok = true;
            D3D12_RANGE empty = {0, 0};
            readback->Unmap(0, &empty);
        }
    }
    if (readback) readback->Release();
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (fenceEv) CloseHandle(fenceEv);
    return ok;
}

// Read back a whole w*h RGBA8 texture into `out` (w*h*4 bytes).
// The texture must be in PIXEL_SHADER_RESOURCE state; it is left in
// PIXEL_SHADER_RESOURCE state afterwards.
static bool ReadbackTexture(ID3D12Device* device, ID3D12CommandQueue* q,
                            ID3D12Resource* tex, uint32_t w, uint32_t h,
                            uint8_t* out) {
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEv = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceVal = 0;
    HRESULT hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       alloc, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) hr = list->Close();
    if (SUCCEEDED(hr))
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&fence));
    if (FAILED(hr) || !fenceEv) {
        if (alloc) alloc->Release();
        if (list) list->Release();
        if (fence) fence->Release();
        if (fenceEv) CloseHandle(fenceEv);
        return false;
    }
    D3D12_RESOURCE_DESC texDesc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSize = 0, totalBytes = 0;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows,
                                  &rowSize, &totalBytes);
    ID3D12Resource* readback = nullptr;
    D3D12_HEAP_PROPERTIES hpRb = {};
    hpRb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbDesc = {};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = totalBytes;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = device->CreateCommittedResource(&hpRb, D3D12_HEAP_FLAG_NONE,
                                         &rbDesc,
                                         D3D12_RESOURCE_STATE_COPY_DEST,
                                         nullptr, IID_PPV_ARGS(&readback));
    if (SUCCEEDED(hr)) hr = alloc->Reset();
    if (SUCCEEDED(hr)) hr = list->Reset(alloc, nullptr);
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.StateBefore =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = tex;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER back = {};
        back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        back.Transition.pResource = tex;
        back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        back.Transition.StateAfter =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        back.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &back);
        hr = list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        q->ExecuteCommandLists(1, lists);
        q->Signal(fence, ++fenceVal);
        fence->SetEventOnCompletion(fenceVal, fenceEv);
        if (WaitForSingleObject(fenceEv, 30000) != WAIT_OBJECT_0)
            hr = E_FAIL;
    }
    bool ok = false;
    if (SUCCEEDED(hr)) {
        void* mapped = nullptr;
        D3D12_RANGE range = {0, totalBytes};
        hr = readback->Map(0, &range, &mapped);
        if (SUCCEEDED(hr) && mapped) {
            const uint8_t* base = (const uint8_t*)mapped;
            for (uint32_t y = 0; y < h; y++)
                memcpy(out + (size_t)y * w * 4,
                       base + footprint.Offset +
                           (uint64_t)y * footprint.Footprint.RowPitch,
                       (size_t)w * 4);
            ok = true;
            D3D12_RANGE empty = {0, 0};
            readback->Unmap(0, &empty);
        }
    }
    if (readback) readback->Release();
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (fenceEv) CloseHandle(fenceEv);
    return ok;
}

int main() {
    fopen_s(&g_report, "guest_report.txt", "w");
    bool allOk = true;
    char detail[1024] = {};
    HRESULT hr = S_OK;

    // 1. Runtime init.
    hr = XGameRuntimeInitialize();
    Report("opt_runtime", SUCCEEDED(hr), "XGameRuntimeInitialize");
    allOk &= SUCCEEDED(hr);

    // 2. Real device via the shim.
    void* dev = nullptr;
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params = {};
    params.Version = 0x20004;  // SDK 2.4, as the real exe passes
    hr = D3D12XboxCreateDevice(nullptr, &params,
                               __uuidof(ID3D12Device), &dev);
    bool devOk = SUCCEEDED(hr) && dev != nullptr;
    snprintf(detail, sizeof(detail), "hr=0x%08x device=%p (REAL)",
             (unsigned)hr, dev);
    Report("opt_device", devOk, detail);
    allOk &= devOk;
    ID3D12Device* device = (ID3D12Device*)dev;

    // 3. PSO + root-signature cache.
    bool psoOk = false;
    detail[0] = 0;
    if (device) {
        hr = ScOpt_PsoCacheInit(device);
        ID3DBlob* vs = nullptr;
        ID3DBlob* ps = nullptr;
        ID3DBlob* psGreen = nullptr;
        ID3DBlob* rsBlob = nullptr;
        ID3D12RootSignature* rs = nullptr;
        bool ok = SUCCEEDED(hr);
        if (ok) ok = CompileShader(kVsSrc, "vs_5_0", &vs);
        if (ok) ok = CompileShader(kPsSrc, "ps_5_0", &ps);
        if (ok) ok = CompileShader(kPsGreenSrc, "ps_5_0", &psGreen);
        if (ok) {
            D3D12_ROOT_SIGNATURE_DESC rsd = {};
            rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &rsBlob, nullptr);
            ok = SUCCEEDED(hr);
        }
        if (ok) {
            // Root signature through the cache twice: 1 create, 1 hit.
            BOOL hit = FALSE;
            hr = ScOpt_PsoCacheGetRootSignature(
                device, rsBlob->GetBufferPointer(),
                rsBlob->GetBufferSize(), &rs, &hit);
            ok = SUCCEEDED(hr) && rs != nullptr && !hit;
            if (ok) {
                ID3D12RootSignature* rs2 = nullptr;
                hr = ScOpt_PsoCacheGetRootSignature(
                    device, rsBlob->GetBufferPointer(),
                    rsBlob->GetBufferSize(), &rs2, &hit);
                ok = SUCCEEDED(hr) && rs2 != nullptr && hit && rs2 == rs;
                if (rs2) rs2->Release();
            }
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        if (ok) {
            D3D12_INPUT_ELEMENT_DESC elems[2] = {};
            elems[0].SemanticName = "POSITION";
            elems[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
            elems[0].InputSlotClass =
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            elems[1].SemanticName = "COLOR";
            elems[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            elems[1].AlignedByteOffset = 12;
            elems[1].InputSlotClass =
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            pd.InputLayout.pInputElementDescs = elems;
            pd.InputLayout.NumElements = 2;
            pd.pRootSignature = rs;
            pd.VS.pShaderBytecode = vs->GetBufferPointer();
            pd.VS.BytecodeLength = vs->GetBufferSize();
            pd.PS.pShaderBytecode = ps->GetBufferPointer();
            pd.PS.BytecodeLength = ps->GetBufferSize();
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.SampleMask = UINT_MAX;
            pd.PrimitiveTopologyType =
                D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pd.SampleDesc.Count = 1;
            // Baseline: 10 raw creates (no layer cache).
            ScOptTimer t;
            t.Start();
            for (int i = 0; i < 10; i++) {
                ID3D12PipelineState* raw = nullptr;
                hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&raw));
                if (FAILED(hr)) { ok = false; break; }
                raw->Release();
            }
            double rawMs = t.ElapsedMs();
            // Cached: 10 gets with the same desc (1 create + 9 hits).
            t.Start();
            ID3D12PipelineState* cached = nullptr;
            for (int i = 0; i < 10; i++) {
                BOOL hit = FALSE;
                ID3D12PipelineState* p = nullptr;
                hr = ScOpt_PsoCacheGetGraphics(device, &pd, &p, &hit);
                if (FAILED(hr) || !p) { ok = false; break; }
                if (i == 0 && hit) ok = false;   // first must be a miss
                if (i > 0 && !hit) ok = false;   // rest must be hits
                if (i == 0) cached = p;
                else p->Release();
            }
            double cachedMs = t.ElapsedMs();
            // A different desc (green PS) must miss and create.
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd2 = pd;
            pd2.PS.pShaderBytecode = psGreen->GetBufferPointer();
            pd2.PS.BytecodeLength = psGreen->GetBufferSize();
            ID3D12PipelineState* other = nullptr;
            BOOL hit2 = TRUE;
            hr = ScOpt_PsoCacheGetGraphics(device, &pd2, &other, &hit2);
            if (FAILED(hr) || !other || hit2) ok = false;
            if (other) other->Release();
            uint32_t created = 0, hits = 0, misses = 0;
            hr = ScOpt_PsoCacheStats(&created, &hits, &misses);
            // Expect: created 3 (1 root sig + 1 PSO + 1 different PSO),
            // hits 10 (1 root sig + 9 PSO), misses 3.
            if (SUCCEEDED(hr) && (created != 3 || hits != 10 || misses != 3))
                ok = false;
            psoOk = ok;
            snprintf(detail, sizeof(detail),
                     "raw 10x CreateGraphicsPipelineState %.3f ms; cached "
                     "10x ScOpt_PsoCacheGetGraphics %.3f ms; "
                     "created=%u hits=%u misses=%u (want 3/10/3)",
                     rawMs, cachedMs, created, hits, misses);
            if (cached) cached->Release();
        }
        if (rs) rs->Release();
        if (vs) vs->Release();
        if (ps) ps->Release();
        if (psGreen) psGreen->Release();
        if (rsBlob) rsBlob->Release();
    } else {
        snprintf(detail, sizeof(detail), "no device");
    }
    Report("opt_pso", psoOk, detail);
    allOk &= psoOk;

    // 4. De-tiled texture cache.
    bool detileOk = false;
    detail[0] = 0;
    {
        const uint32_t W = 256, H = 256, BPP = 4;
        const uint32_t TILE_W = 128, TILE_H = 128;  // 128*128*4 = 64KB
        const size_t BYTES = (size_t)W * H * BPP;
        uint8_t* linear = (uint8_t*)malloc(BYTES);
        uint8_t* tiled = (uint8_t*)malloc(BYTES);
        uint8_t* back = (uint8_t*)malloc(BYTES);
        if (linear && tiled && back) {
            for (uint32_t y = 0; y < H; y++)
                for (uint32_t x = 0; x < W; x++) {
                    uint32_t v = (x * 2654435761u) ^ (y * 40503u) ^
                                 (x * y * 97u);
                    uint8_t* p = linear + ((size_t)y * W + x) * BPP;
                    p[0] = (uint8_t)(v & 0xFF);
                    p[1] = (uint8_t)((v >> 8) & 0xFF);
                    p[2] = (uint8_t)((v >> 16) & 0xFF);
                    p[3] = 255;
                }
            hr = ScOpt_Tile2D(linear, W, H, BPP, TILE_W, TILE_H, tiled);
            bool ok = SUCCEEDED(hr);
            ScOptTimer t;
            BOOL hit = TRUE;
            t.Start();
            if (ok) hr = ScOpt_DetileCached(tiled, W, H, BPP, TILE_W,
                                            TILE_H, back, &hit);
            double missMs = t.ElapsedMs();
            if (FAILED(hr) || hit) ok = false;  // first call must miss
            if (ok && memcmp(linear, back, BYTES) != 0) ok = false;
            t.Start();
            if (ok) hr = ScOpt_DetileCached(tiled, W, H, BPP, TILE_W,
                                            TILE_H, back, &hit);
            double hitMs = t.ElapsedMs();
            if (FAILED(hr) || !hit) ok = false;  // second call must hit
            if (ok && memcmp(linear, back, BYTES) != 0) ok = false;
            uint32_t detiles = 0, hits = 0, evictions = 0;
            hr = ScOpt_DetileStats(&detiles, &hits, &evictions);
            if (SUCCEEDED(hr) &&
                (detiles != 1 || hits != 1 || evictions != 0))
                ok = false;
            detileOk = ok;
            snprintf(detail, sizeof(detail),
                     "256x256 RGBA8 round-trip %s; miss (de-tile) %.3f ms, "
                     "hit (cache copy) %.3f ms; detiles=%u hits=%u "
                     "evictions=%u (want 1/1/0)",
                     ok ? "byte-exact" : "MISMATCH", missMs, hitMs, detiles,
                     hits, evictions);
        } else {
            snprintf(detail, sizeof(detail), "malloc failed");
        }
        free(linear);
        free(tiled);
        free(back);
    }
    Report("opt_detile", detileOk, detail);
    allOk &= detileOk;

    // 5. Mesh-shader fallback (pixel-verified draw of an expanded quad).
    bool meshOk = false;
    detail[0] = 0;
    if (device) {
        hr = ScOpt_MeshFallbackInit(device);
        bool ok = SUCCEEDED(hr);
        const void* vsBlob = nullptr;
        const void* psBlob = nullptr;
        SIZE_T vsLen = 0, psLen = 0;
        if (ok) hr = ScOpt_MeshFallbackGetShaders(&vsBlob, &vsLen, &psBlob,
                                                  &psLen);
        if (FAILED(hr) || !vsBlob || !psBlob) ok = false;
        ID3D12CommandQueue* q = nullptr;
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        ID3D12Fence* fence = nullptr;
        HANDLE fenceEv = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        UINT64 fenceVal = 0;
        ID3D12Resource* rt = nullptr;
        ID3D12RootSignature* rs = nullptr;
        ID3D12PipelineState* pso = nullptr;
        if (ok) {
            D3D12_COMMAND_QUEUE_DESC qd = {};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
        }
        if (SUCCEEDED(hr))
            hr = device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           alloc, nullptr,
                                           IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr)) hr = list->Close();
        if (SUCCEEDED(hr))
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                     IID_PPV_ARGS(&fence));
        if (ok && SUCCEEDED(hr)) {
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = 256;
            rd.Height = 256;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv = {};
            cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            cv.Color[0] = 51.0f / 255.0f;
            cv.Color[1] = 102.0f / 255.0f;
            cv.Color[2] = 204.0f / 255.0f;
            cv.Color[3] = 1.0f;
            hr = device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt));
        }
        if (ok && SUCCEEDED(hr)) {
            // Empty root signature + PSO from the fallback shaders.
            D3D12_ROOT_SIGNATURE_DESC rsd = {};
            rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            ID3DBlob* rsBlob = nullptr;
            hr = D3D12SerializeRootSignature(&rsd,
                                             D3D_ROOT_SIGNATURE_VERSION_1,
                                             &rsBlob, nullptr);
            if (SUCCEEDED(hr))
                hr = device->CreateRootSignature(
                    0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                    IID_PPV_ARGS(&rs));
            if (rsBlob) rsBlob->Release();
            if (SUCCEEDED(hr)) {
                D3D12_INPUT_ELEMENT_DESC elems[2] = {};
                elems[0].SemanticName = "POSITION";
                elems[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
                elems[0].InputSlotClass =
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                elems[1].SemanticName = "COLOR";
                elems[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                elems[1].AlignedByteOffset = 12;
                elems[1].InputSlotClass =
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
                pd.InputLayout.pInputElementDescs = elems;
                pd.InputLayout.NumElements = 2;
                pd.pRootSignature = rs;
                pd.VS.pShaderBytecode = vsBlob;
                pd.VS.BytecodeLength = vsLen;
                pd.PS.pShaderBytecode = psBlob;
                pd.PS.BytecodeLength = psLen;
                pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.BlendState.RenderTarget[0].RenderTargetWriteMask =
                    D3D12_COLOR_WRITE_ENABLE_ALL;
                pd.SampleMask = UINT_MAX;
                pd.PrimitiveTopologyType =
                    D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pd.NumRenderTargets = 1;
                pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                pd.SampleDesc.Count = 1;
                hr = device->CreateGraphicsPipelineState(&pd,
                                                         IID_PPV_ARGS(&pso));
            }
        }
        if (ok && SUCCEEDED(hr)) {
            // One red quad meshlet (4 verts, 6 indices).
            static const float kPos[12] = {-0.8f, -0.8f, 0.0f,
                                           0.8f,  -0.8f, 0.0f,
                                           0.8f,  0.8f,  0.0f,
                                           -0.8f, 0.8f,  0.0f};
            static const float kCol[16] = {1, 0, 0, 1, 1, 0, 0, 1,
                                           1, 0, 0, 1, 1, 0, 0, 1};
            static const uint32_t kIdx[6] = {0, 1, 2, 0, 2, 3};
            ScOptMeshlet meshlet = {};
            meshlet.positions = kPos;
            meshlet.colors = kCol;
            meshlet.indices = kIdx;
            meshlet.vertexCount = 4;
            meshlet.indexCount = 6;
            ScOptExpandedMesh expanded = {};
            hr = ScOpt_MeshFallbackExpand(device, &meshlet, &expanded);
            if (SUCCEEDED(hr) && expanded.vertexCount == 4 &&
                expanded.indexCount == 6) {
                hr = alloc->Reset();
                if (SUCCEEDED(hr)) hr = list->Reset(alloc, pso);
                if (SUCCEEDED(hr)) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
                    // The RT has no RTV heap yet — create a scratch one.
                    ID3D12DescriptorHeap* rtvHeap = nullptr;
                    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
                    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                    hd.NumDescriptors = 1;
                    hr = device->CreateDescriptorHeap(&hd,
                                                      IID_PPV_ARGS(&rtvHeap));
                    if (SUCCEEDED(hr)) {
                        rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
                        device->CreateRenderTargetView(rt, nullptr, rtv);
                        float clearCol[4] = {51.0f / 255.0f, 102.0f / 255.0f,
                                             204.0f / 255.0f, 1.0f};
                        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                        list->ClearRenderTargetView(rtv, clearCol, 0, nullptr);
                        D3D12_VIEWPORT vp = {0, 0, 256, 256, 0, 1};
                        D3D12_RECT sc = {0, 0, 256, 256};
                        list->RSSetViewports(1, &vp);
                        list->RSSetScissorRects(1, &sc);
                        list->IASetPrimitiveTopology(
                            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        D3D12_VERTEX_BUFFER_VIEW vbv = {};
                        vbv.BufferLocation =
                            expanded.vertexBuffer->GetGPUVirtualAddress();
                        vbv.SizeInBytes = 4 * 28;
                        vbv.StrideInBytes = 28;
                        list->IASetVertexBuffers(0, 1, &vbv);
                        D3D12_INDEX_BUFFER_VIEW ibv = {};
                        ibv.BufferLocation =
                            expanded.indexBuffer->GetGPUVirtualAddress();
                        ibv.SizeInBytes = 6 * 4;
                        ibv.Format = DXGI_FORMAT_R32_UINT;
                        list->IASetIndexBuffer(&ibv);
                        list->DrawIndexedInstanced(6, 1, 0, 0, 0);
                        hr = list->Close();
                        if (SUCCEEDED(hr)) {
                            ID3D12CommandList* lists[] = {list};
                            q->ExecuteCommandLists(1, lists);
                            q->Signal(fence, ++fenceVal);
                            fence->SetEventOnCompletion(fenceVal, fenceEv);
                            if (WaitForSingleObject(fenceEv, 30000) !=
                                WAIT_OBJECT_0)
                                hr = E_FAIL;
                        }
                        if (SUCCEEDED(hr)) {
                            uint8_t center[4] = {}, corner[4] = {};
                            if (ReadbackPixels(device, q, rt, center,
                                               corner)) {
                                bool centerRed =
                                    (abs((int)center[0] - 255) <= 3) &&
                                    (abs((int)center[1] - 0) <= 3) &&
                                    (abs((int)center[2] - 0) <= 3) &&
                                    (abs((int)center[3] - 255) <= 3);
                                bool cornerClear =
                                    (abs((int)corner[0] - 51) <= 3) &&
                                    (abs((int)corner[1] - 102) <= 3) &&
                                    (abs((int)corner[2] - 204) <= 3) &&
                                    (abs((int)corner[3] - 255) <= 3);
                                ok = centerRed && cornerClear;
                                snprintf(detail, sizeof(detail),
                                         "meshlet 4 verts/6 idx expanded + "
                                         "drawn; center (%d,%d,%d,%d) want "
                                         "(255,0,0,255); corner "
                                         "(%d,%d,%d,%d) want (51,102,204,255)",
                                         center[0], center[1], center[2],
                                         center[3], corner[0], corner[1],
                                         corner[2], corner[3]);
                            } else {
                                ok = false;
                                snprintf(detail, sizeof(detail),
                                         "readback failed");
                            }
                        }
                        rtvHeap->Release();
                    }
                }
                ScOpt_MeshFallbackFreeExpanded(&expanded);
            } else {
                ok = false;
                snprintf(detail, sizeof(detail),
                         "expand hr=0x%08x verts=%u idx=%u",
                         (unsigned)hr, expanded.vertexCount,
                         expanded.indexCount);
            }
        } else if (ok) {
            ok = false;
            snprintf(detail, sizeof(detail), "mesh setup hr=0x%08x",
                     (unsigned)hr);
        }
        meshOk = ok;
        if (pso) pso->Release();
        if (rs) rs->Release();
        if (rt) rt->Release();
        if (fence) fence->Release();
        if (list) list->Release();
        if (alloc) alloc->Release();
        if (q) q->Release();
        if (fenceEv) CloseHandle(fenceEv);
    } else {
        snprintf(detail, sizeof(detail), "no device");
    }
    Report("opt_mesh", meshOk, detail);
    allOk &= meshOk;

    // 6. Async compute mapping — measured, not assumed.
    bool asyncOk = false;
    detail[0] = 0;
    if (device) {
        hr = ScOpt_AsyncInit(device);
        bool ok = SUCCEEDED(hr);
        double serialMs = 0, asyncMs = 0;
        uint64_t serialSum = 0, asyncSum = 0;
        if (ok) hr = ScOpt_AsyncRunTimed(device, SC_OPT_ASYNC_SERIAL,
                                         &serialMs);
        if (SUCCEEDED(hr)) hr = ScOpt_AsyncChecksum(SC_OPT_ASYNC_SERIAL,
                                                    &serialSum);
        if (SUCCEEDED(hr)) hr = ScOpt_AsyncRunTimed(device, SC_OPT_ASYNC_ASYNC,
                                                    &asyncMs);
        if (SUCCEEDED(hr)) hr = ScOpt_AsyncChecksum(SC_OPT_ASYNC_ASYNC,
                                                    &asyncSum);
        if (FAILED(hr)) ok = false;
        if (ok && serialSum != asyncSum) ok = false;  // same result required
        asyncOk = ok;
        double pct = 0;
        const char* verdict = "same";
        if (serialMs > 0) {
            pct = (serialMs - asyncMs) / serialMs * 100.0;
            verdict = pct > 2.0 ? "async FASTER" : (pct < -2.0 ? "async SLOWER" : "same");
        }
        snprintf(detail, sizeof(detail),
                 "serial %.3f ms, async %.3f ms (%s, %.1f%%); checksums "
                 "%s (serial 0x%016llx, async 0x%016llx)",
                 serialMs, asyncMs, verdict, pct,
                 (serialSum == asyncSum) ? "equal" : "DIFFER",
                 (unsigned long long)serialSum,
                 (unsigned long long)asyncSum);
    } else {
        snprintf(detail, sizeof(detail), "no device");
    }
    Report("opt_async", asyncOk, detail);
    allOk &= asyncOk;

    // 7. Descriptor heap management.
    bool descOk = false;
    detail[0] = 0;
    if (device) {
        ScOptDescHeapHandle heap = nullptr;
        hr = ScOpt_DescHeapInit(device,
                                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16,
                                &heap);
        bool ok = SUCCEEDED(hr) && heap != nullptr;
        uint32_t idx[32] = {};
        uint32_t allocated = 0;
        if (ok) {
            for (uint32_t i = 0; i < 16; i++) {
                D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
                D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
                hr = ScOpt_DescAlloc(heap, &cpu, &gpu, &idx[allocated]);
                if (FAILED(hr) || cpu.ptr == 0 || gpu.ptr == 0) {
                    ok = false;
                    break;
                }
                allocated++;
            }
        }
        // One more allocation must grow the chain (heap count 2).
        uint32_t cap = 0, live = 0, heaps = 0;
        if (ok) {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
            hr = ScOpt_DescAlloc(heap, &cpu, &gpu, &idx[allocated]);
            if (FAILED(hr)) ok = false;
            else allocated++;
            hr = ScOpt_DescHeapStats(heap, &cap, &live, &heaps);
            if (FAILED(hr) || cap != 32 || live != 17 || heaps != 2)
                ok = false;
        }
        // Free every other slot, re-allocate 8: no further growth, and
        // every live index must be unique.
        if (ok) {
            for (uint32_t i = 0; i < allocated; i += 2) {
                hr = ScOpt_DescFree(heap, idx[i]);
                if (FAILED(hr)) { ok = false; break; }
            }
        }
        if (ok) {
            bool seen[32] = {};
            uint32_t reallocated = 0;
            for (uint32_t i = 0; i < 8; i++) {
                D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
                D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
                uint32_t nidx = 0;
                hr = ScOpt_DescAlloc(heap, &cpu, &gpu, &nidx);
                if (FAILED(hr) || nidx >= 32 || seen[nidx]) {
                    ok = false;
                    break;
                }
                seen[nidx] = true;
                reallocated++;
            }
            hr = ScOpt_DescHeapStats(heap, &cap, &live, &heaps);
            if (FAILED(hr) || cap != 32 || live != 16 || heaps != 2)
                ok = false;
            (void)reallocated;
        }
        // Timing: 10,000 alloc/free cycles vs 10,000 heap creates.
        double poolMs = 0, naiveMs = 0;
        if (ok) {
            ScOptTimer t;
            t.Start();
            for (int i = 0; i < 10000; i++) {
                D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
                D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
                uint32_t nidx = 0;
                hr = ScOpt_DescAlloc(heap, &cpu, &gpu, &nidx);
                if (FAILED(hr)) { ok = false; break; }
                hr = ScOpt_DescFree(heap, nidx);
                if (FAILED(hr)) { ok = false; break; }
            }
            poolMs = t.ElapsedMs();
            t.Start();
            for (int i = 0; i < 10000; i++) {
                D3D12_DESCRIPTOR_HEAP_DESC hd = {};
                hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                hd.NumDescriptors = 1;
                hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                ID3D12DescriptorHeap* h = nullptr;
                hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&h));
                if (FAILED(hr)) { ok = false; break; }
                h->Release();
            }
            naiveMs = t.ElapsedMs();
        }
        if (ok) {
            for (uint32_t i = 0; i < allocated; i++) {
                // Some were freed already; freeing a dead index is an
                // error, so only free the odd ones still live.
                if (i % 2 == 1) ScOpt_DescFree(heap, idx[i]);
            }
            hr = ScOpt_DescHeapStats(heap, &cap, &live, &heaps);
            if (SUCCEEDED(hr) && live != 8) ok = false;  // 8 re-allocs live
        }
        descOk = ok;
        snprintf(detail, sizeof(detail),
                 "pool alloc/free 10,000x %.3f ms vs heap-create 10,000x "
                 "%.3f ms; growth 16->32 slots (2 heaps), reuse ok, "
                 "live=%u heaps=%u",
                 poolMs, naiveMs, live, heaps);
        if (heap) ScOpt_DescHeapDestroy(heap);
    } else {
        snprintf(detail, sizeof(detail), "no device");
    }
    Report("opt_desc", descOk, detail);
    allOk &= descOk;

    // 8. DirectStorage queue + decompression stage.
    bool dsOk = false;
    detail[0] = 0;
    {
        // --- zlib test encoders (test-only; the layer only decodes) ---
        struct GuestBitWriter {
            uint8_t* buf = nullptr;
            size_t cap = 0, pos = 0;
            uint32_t bitbuf = 0;
            int bitcnt = 0;
            bool Ensure(size_t extra) {
                if (pos + extra + 8 <= cap) return true;
                size_t ncap = cap ? cap * 2 : 1024;
                while (pos + extra + 8 > ncap) ncap *= 2;
                uint8_t* nb = (uint8_t*)realloc(buf, ncap);
                if (!nb) return false;
                buf = nb;
                cap = ncap;
                return true;
            }
            // LSB-first (block headers, extra bits).
            bool PutBits(uint32_t v, int n) {
                if (!Ensure(4)) return false;
                bitbuf |= v << bitcnt;
                bitcnt += n;
                while (bitcnt >= 8) {
                    buf[pos++] = (uint8_t)bitbuf;
                    bitbuf >>= 8;
                    bitcnt -= 8;
                }
                return true;
            }
            // MSB-first (Huffman codes).
            bool PutCode(uint32_t code, int len) {
                for (int i = len - 1; i >= 0; i--) {
                    if (!PutBits((code >> i) & 1u, 1)) return false;
                }
                return true;
            }
            bool AlignByte() {
                if (bitcnt > 0) {
                    if (!Ensure(1)) return false;
                    buf[pos++] = (uint8_t)bitbuf;
                    bitbuf = 0;
                    bitcnt = 0;
                }
                return true;
            }
        };
        auto BuildCanon = [](const uint8_t* lens, int n, uint16_t* codes) {
            uint16_t counts[16] = {};
            for (int i = 0; i < n; i++) counts[lens[i]]++;
            counts[0] = 0;
            uint16_t code = 0;
            uint16_t nextCode[16] = {};
            for (int len = 1; len < 16; len++) {
                code = (uint16_t)((code + counts[len - 1]) << 1);
                nextCode[len] = code;
            }
            for (int i = 0; i < n; i++) {
                if (lens[i]) codes[i] = nextCode[lens[i]]++;
            }
        };
        auto GuestAdler = [](const uint8_t* d, size_t n) -> uint32_t {
            uint32_t a = 1, b = 0;
            for (size_t i = 0; i < n; i++) {
                a = (a + d[i]) % 65521;
                b = (b + a) % 65521;
            }
            return (b << 16) | a;
        };
        // Wrap a raw deflate payload in the zlib container.
        auto ZlibWrap = [&](GuestBitWriter* bw, const uint8_t* raw,
                            size_t rawLen, size_t* outLen) -> uint8_t* {
            size_t total = 2 + bw->pos + 4;
            uint8_t* out = (uint8_t*)malloc(total);
            if (!out) return nullptr;
            out[0] = 0x78;
            out[1] = 0x01;
            memcpy(out + 2, bw->buf, bw->pos);
            uint32_t ad = GuestAdler(raw, rawLen);
            out[2 + bw->pos + 0] = (uint8_t)(ad >> 24);
            out[2 + bw->pos + 1] = (uint8_t)(ad >> 16);
            out[2 + bw->pos + 2] = (uint8_t)(ad >> 8);
            out[2 + bw->pos + 3] = (uint8_t)ad;
            *outLen = total;
            return out;
        };
        // Stored-block stream (BTYPE=00).
        auto EncodeStored = [&](const uint8_t* raw, size_t rawLen,
                                size_t* outLen) -> uint8_t* {
            GuestBitWriter bw;
            if (!bw.PutBits(1, 1) || !bw.PutBits(0, 2) ||
                !bw.AlignByte())
                return nullptr;
            if (!bw.Ensure(rawLen + 4)) return nullptr;
            bw.buf[bw.pos++] = (uint8_t)(rawLen & 0xFF);
            bw.buf[bw.pos++] = (uint8_t)((rawLen >> 8) & 0xFF);
            uint16_t nlen = (uint16_t)~(uint16_t)rawLen;
            bw.buf[bw.pos++] = (uint8_t)(nlen & 0xFF);
            bw.buf[bw.pos++] = (uint8_t)((nlen >> 8) & 0xFF);
            memcpy(bw.buf + bw.pos, raw, rawLen);
            bw.pos += rawLen;
            uint8_t* out = ZlibWrap(&bw, raw, rawLen, outLen);
            free(bw.buf);
            return out;
        };
        // Fixed-Huffman stream (BTYPE=01): literals + one real match
        // (len 6, dist 45 — the 45-byte sentence repeats, so the bytes
        // 45 back match the next 6 input bytes) + EOB.
        auto EncodeFixed = [&](const uint8_t* raw, size_t rawLen,
                               size_t* outLen) -> uint8_t* {
            GuestBitWriter bw;
            uint8_t litLen[288] = {};
            uint16_t litCode[288] = {};
            for (int i = 0; i < 144; i++) litLen[i] = 8;
            for (int i = 144; i < 256; i++) litLen[i] = 9;
            for (int i = 256; i < 280; i++) litLen[i] = 7;
            for (int i = 280; i < 288; i++) litLen[i] = 8;
            BuildCanon(litLen, 288, litCode);
            auto PutLit = [&](int sym) -> bool {
                return bw.PutCode(litCode[sym], litLen[sym]);
            };
            bool ok = bw.PutBits(1, 1) && bw.PutBits(1, 2);
            for (size_t i = 0; ok && i < 50; i++)
                ok = PutLit(raw[i]);
            if (ok) ok = bw.PutCode(litCode[260], 7);  // length 6
            if (ok) ok = bw.PutCode(10, 5);            // distance code 45
            if (ok) ok = bw.PutBits(12, 4);            // 45 - 33 = 12
            for (size_t i = 56; ok && i < rawLen; i++)
                ok = PutLit(raw[i]);
            if (ok) ok = bw.PutCode(litCode[256], 7);  // EOB
            if (!ok) {
                free(bw.buf);
                return nullptr;
            }
            if (!bw.AlignByte()) {
                free(bw.buf);
                return nullptr;
            }
            uint8_t* out = ZlibWrap(&bw, raw, rawLen, outLen);
            free(bw.buf);
            return out;
        };
        // Dynamic-Huffman stream (BTYPE=10) for "aaaaabbbbbccccc":
        // lit/len lengths {97,98,99,256} = 2; dist {0} = 1;
        // CL lengths {0,1,2,18} = 3 (canonical CL codes 000/001/010/011).
        auto EncodeDynamic = [&](const uint8_t* raw, size_t rawLen,
                                 size_t* outLen) -> uint8_t* {
            GuestBitWriter bw;
            static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10,
                                               5, 11, 4, 12, 3, 13, 2, 14, 1,
                                               15};
            uint8_t clLen[19] = {};
            clLen[0] = 3;
            clLen[1] = 3;
            clLen[2] = 3;
            clLen[18] = 3;
            uint16_t clCode[19] = {};
            BuildCanon(clLen, 19, clCode);
            uint8_t litLen[257] = {};
            litLen[97] = 2;
            litLen[98] = 2;
            litLen[99] = 2;
            litLen[256] = 2;
            uint16_t litCode[257] = {};
            BuildCanon(litLen, 257, litCode);
            bool ok = bw.PutBits(1, 1) && bw.PutBits(2, 2);  // BFINAL, BTYPE
            if (ok) ok = bw.PutBits(0, 5);   // HLIT - 257 = 0
            if (ok) ok = bw.PutBits(0, 5);   // HDIST - 1 = 0
            if (ok) ok = bw.PutBits(14, 4);  // HCLEN - 4 = 14 (18 entries)
            // CL lengths in kOrder: symbol 16 -> 0, 17 -> 0, 18 -> 3,
            // 0 -> 3, ..., 2 -> 3, ..., 1 -> 3 (positions 2, 3, 15, 17).
            static const uint8_t kClInOrder[18] = {0, 0, 3, 3, 0, 0, 0, 0,
                                                   0, 0, 0, 0, 0, 0, 0, 3,
                                                   0, 3};
            for (int i = 0; ok && i < 18; i++)
                ok = bw.PutBits(kClInOrder[i], 3);
            auto PutCl = [&](int sym) -> bool {
                return bw.PutCode(clCode[sym], clLen[sym]);
            };
            // Lengths array (257 lit/len + 1 dist = 258):
            // 97 zeros, 2,2,2, 156 zeros, 2 (EOB), 1 (dist).
            if (ok) ok = PutCl(18) && bw.PutBits(86, 7);    // 97 zeros
            if (ok) ok = PutCl(2) && PutCl(2) && PutCl(2);  // 97,98,99 = 2
            if (ok) ok = PutCl(18) && bw.PutBits(127, 7);   // 138 zeros
            if (ok) ok = PutCl(18) && bw.PutBits(7, 7);     // 18 zeros
            if (ok) ok = PutCl(2);                          // 256 = 2
            if (ok) ok = PutCl(1);                          // dist 0 = 1
            for (size_t i = 0; ok && i < rawLen; i++)
                ok = bw.PutCode(litCode[raw[i]], 2);
            if (ok) ok = bw.PutCode(litCode[256], 2);  // EOB
            if (!ok || !bw.AlignByte()) {
                free(bw.buf);
                return nullptr;
            }
            uint8_t* out = ZlibWrap(&bw, raw, rawLen, outLen);
            free(bw.buf);
            return out;
        };

        ScOptDsPath path = SC_OPT_DS_PATH_DIRECTSTORAGE;
        hr = ScOpt_DStorageInit(&path);
        bool ok = SUCCEEDED(hr);
        char tmpPath[MAX_PATH] = {};
        GetTempPathA(sizeof(tmpPath), tmpPath);
        char filePath[MAX_PATH] = {};
        snprintf(filePath, sizeof(filePath), "%sScarlettOptTest.bin",
                 tmpPath);
        const uint32_t CHUNKS = 16;
        const uint32_t CHUNK = 4 * 1024 * 1024;
        uint64_t expectHash[16] = {};
        double batchMs = 0, syncMs = 0;
        bool batchOk = false, syncOk = false, singleOk = false;
        bool noneOk = false, zlibOk = false, bcpackOk = false;
        if (ok) {
            // Write the 64MB synthetic file (deterministic pattern).
            HANDLE f = CreateFileA(filePath, GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
            if (f == INVALID_HANDLE_VALUE) {
                ok = false;
            } else {
                uint8_t* chunk = (uint8_t*)malloc(CHUNK);
                if (!chunk) {
                    ok = false;
                } else {
                    for (uint32_t i = 0; i < CHUNKS && ok; i++) {
                        for (uint32_t j = 0; j < CHUNK; j++)
                            chunk[j] = (uint8_t)((i * 131 + j) & 0xFF);
                        expectHash[i] = ScOptFnv1a(chunk, CHUNK);
                        DWORD written = 0;
                        if (!WriteFile(f, chunk, CHUNK, &written, nullptr) ||
                            written != CHUNK)
                            ok = false;
                    }
                    free(chunk);
                }
                CloseHandle(f);
            }
        }
        if (ok) {
            // Batch (concurrent) read through the queue.
            uint64_t offsets[16] = {};
            uint32_t sizes[16] = {};
            uint8_t* datas[16] = {};
            uint32_t readBack[16] = {};
            for (uint32_t i = 0; i < CHUNKS; i++) {
                offsets[i] = (uint64_t)i * CHUNK;
                sizes[i] = CHUNK;
            }
            ScOptTimer t;
            t.Start();
            hr = ScOpt_DStorageReadFileBatch(filePath, offsets, sizes,
                                            CHUNKS, datas, readBack);
            batchMs = t.ElapsedMs();
            if (SUCCEEDED(hr)) {
                batchOk = true;
                for (uint32_t i = 0; i < CHUNKS; i++) {
                    if (readBack[i] != CHUNK ||
                        ScOptFnv1a(datas[i], CHUNK) != expectHash[i])
                        batchOk = false;
                    free(datas[i]);
                }
            }
            // Sequential synchronous reads (plain ReadFile, no queue).
            HANDLE f = CreateFileA(filePath, GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) {
                ok = false;
            } else {
                uint8_t* chunk = (uint8_t*)malloc(CHUNK);
                if (!chunk) {
                    ok = false;
                } else {
                    ScOptTimer t2;
                    t2.Start();
                    syncOk = true;
                    for (uint32_t i = 0; i < CHUNKS; i++) {
                        DWORD got = 0;
                        if (!ReadFile(f, chunk, CHUNK, &got, nullptr) ||
                            got != CHUNK ||
                            ScOptFnv1a(chunk, CHUNK) != expectHash[i])
                            syncOk = false;
                    }
                    syncMs = t2.ElapsedMs();
                    free(chunk);
                }
                CloseHandle(f);
            }
            // Single async read (chunk 0).
            uint8_t* one = nullptr;
            uint32_t oneLen = 0;
            hr = ScOpt_DStorageReadFile(filePath, 0, CHUNK, &one, &oneLen);
            singleOk = SUCCEEDED(hr) && oneLen == CHUNK && one &&
                       ScOptFnv1a(one, CHUNK) == expectHash[0];
            free(one);
            // Decompression: NONE (memcpy round-trip on chunk 0's data).
            uint8_t* src = (uint8_t*)malloc(CHUNK);
            uint8_t* dst = (uint8_t*)malloc(CHUNK);
            if (src && dst) {
                for (uint32_t j = 0; j < CHUNK; j++)
                    src[j] = (uint8_t)((0 * 131 + j) & 0xFF);
                size_t outLen = 0;
                hr = ScOpt_DStorageDecompress(SC_OPT_DS_DECOMP_NONE, src,
                                              CHUNK, dst, CHUNK, &outLen);
                noneOk = SUCCEEDED(hr) && outLen == CHUNK &&
                         memcmp(src, dst, CHUNK) == 0;
            }
            free(src);
            free(dst);
            // Decompression: ZLIB, all three block types.
            bool zStored = false, zFixed = false, zDynamic = false;
            uint8_t storedRaw[512] = {};
            for (int i = 0; i < 512; i++) storedRaw[i] = (uint8_t)(i * 7 + 3);
            size_t encLen = 0;
            uint8_t* enc = EncodeStored(storedRaw, 512, &encLen);
            if (enc) {
                uint8_t out[512] = {};
                size_t outLen = 0;
                hr = ScOpt_DStorageDecompress(SC_OPT_DS_DECOMP_ZLIB, enc,
                                              encLen, out, 512, &outLen);
                zStored = SUCCEEDED(hr) && outLen == 512 &&
                          memcmp(out, storedRaw, 512) == 0;
                free(enc);
            }
            const char* sentence =
                "The quick brown fox jumps over the lazy dog. ";
            uint8_t fixedRaw[200] = {};
            for (int i = 0; i < 200; i++)
                fixedRaw[i] = (uint8_t)sentence[i % 45];
            enc = EncodeFixed(fixedRaw, 200, &encLen);
            if (enc) {
                uint8_t out[200] = {};
                size_t outLen = 0;
                hr = ScOpt_DStorageDecompress(SC_OPT_DS_DECOMP_ZLIB, enc,
                                              encLen, out, 200, &outLen);
                zFixed = SUCCEEDED(hr) && outLen == 200 &&
                         memcmp(out, fixedRaw, 200) == 0;
                free(enc);
            }
            const uint8_t dynRaw[15] = {'a', 'a', 'a', 'a', 'a', 'b', 'b',
                                        'b', 'b', 'b', 'c', 'c', 'c', 'c',
                                        'c'};
            enc = EncodeDynamic(dynRaw, 15, &encLen);
            if (enc) {
                uint8_t out[15] = {};
                size_t outLen = 0;
                hr = ScOpt_DStorageDecompress(SC_OPT_DS_DECOMP_ZLIB, enc,
                                              encLen, out, 15, &outLen);
                zDynamic = SUCCEEDED(hr) && outLen == 15 &&
                           memcmp(out, dynRaw, 15) == 0;
                free(enc);
            }
            zlibOk = zStored && zFixed && zDynamic;
            // BCPack must refuse (rANS bitstream not public).
            uint8_t dummy[16] = {};
            uint8_t dout[16] = {};
            size_t doutLen = 0;
            hr = ScOpt_DStorageDecompress(SC_OPT_DS_DECOMP_BCPACK_BC7,
                                          dummy, 16, dout, 16, &doutLen);
            bcpackOk = (hr == E_NOTIMPL);
            DeleteFileA(filePath);
        }
        dsOk = ok && batchOk && syncOk && singleOk && noneOk && zlibOk &&
               bcpackOk;
        snprintf(detail, sizeof(detail),
                 "path=%s; batch 16x4MB %.3f ms, sync 16x4MB %.3f ms; "
                 "single/batch hashes %s; NONE %s; ZLIB stored/fixed/"
                 "dynamic %s; BCPack BC7 %s",
                 path == SC_OPT_DS_PATH_OVERLAPPED ? "overlapped"
                                                   : "directstorage",
                 batchMs, syncMs, (batchOk && syncOk) ? "match" : "MISMATCH",
                 noneOk ? "ok" : "FAIL", zlibOk ? "all ok" : "FAIL",
                 bcpackOk ? "E_NOTIMPL (as designed)" : "FAIL");
    }
    Report("opt_dstorage", dsOk, detail);
    allOk &= dsOk;

    // 9. Shader precompilation cache.
    bool shaderOk = false;
    detail[0] = 0;
    {
        hr = ScOpt_ShaderCacheInit(nullptr);
        bool ok = SUCCEEDED(hr);
        // Clear the cache dir first: a re-run of this guest must see
        // cold-cache misses on the first compiles, not hits left over
        // from the previous run.
        if (ok) {
            char localAppData[MAX_PATH] = {};
            GetEnvironmentVariableA("LOCALAPPDATA", localAppData,
                                    sizeof(localAppData));
            char pattern[MAX_PATH] = {};
            snprintf(pattern, sizeof(pattern),
                     "%s\\ScarlettCompat\\ShaderCache\\*.cso", localAppData);
            WIN32_FIND_DATAA fd = {};
            HANDLE hFind = FindFirstFileA(pattern, &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    char fpath[MAX_PATH] = {};
                    snprintf(fpath, sizeof(fpath),
                             "%s\\ScarlettCompat\\ShaderCache\\%s",
                             localAppData, fd.cFileName);
                    DeleteFileA(fpath);
                } while (FindNextFileA(hFind, &fd));
                FindClose(hFind);
            }
        }
        uint8_t* vs1 = nullptr;
        uint8_t* ps1 = nullptr;
        uint8_t* vs2 = nullptr;
        uint8_t* ps2 = nullptr;
        size_t vs1Len = 0, ps1Len = 0, vs2Len = 0, ps2Len = 0;
        double compileMs = 0, loadMs = 0;
        if (ok) {
            ScOptTimer t;
            t.Start();
            hr = ScOpt_CompileCached(kVsSrc, strlen(kVsSrc), "vs_5_0",
                                     "main", &vs1, &vs1Len);
            compileMs = t.ElapsedMs();
            if (FAILED(hr) || !vs1) ok = false;
            if (ok) hr = ScOpt_CompileCached(kPsSrc, strlen(kPsSrc),
                                             "ps_5_0", "main", &ps1, &ps1Len);
            if (FAILED(hr) || !ps1) ok = false;
            t.Start();
            if (ok) hr = ScOpt_CompileCached(kVsSrc, strlen(kVsSrc),
                                             "vs_5_0", "main", &vs2, &vs2Len);
            loadMs = t.ElapsedMs();
            if (FAILED(hr) || !vs2) ok = false;
            if (ok) hr = ScOpt_CompileCached(kPsSrc, strlen(kPsSrc),
                                             "ps_5_0", "main", &ps2, &ps2Len);
            if (FAILED(hr) || !ps2) ok = false;
            if (ok && (vs1Len != vs2Len || ps1Len != ps2Len ||
                       memcmp(vs1, vs2, vs1Len) != 0 ||
                       memcmp(ps1, ps2, ps1Len) != 0))
                ok = false;
            uint64_t hits = 0, misses = 0;
            ScOpt_ShaderCacheStats(&hits, &misses);
            if (hits != 2 || misses != 2) ok = false;
            // Corrupt every cached blob on disk: a re-compile must
            // return the original bytes (miss + recompile, never the
            // corrupt bytes).
            if (ok) {
                char localAppData[MAX_PATH] = {};
                GetEnvironmentVariableA("LOCALAPPDATA", localAppData,
                                        sizeof(localAppData));
                char dir[MAX_PATH] = {};
                snprintf(dir, sizeof(dir),
                         "%s\\ScarlettCompat\\ShaderCache\\*.cso",
                         localAppData);
                WIN32_FIND_DATAA fd = {};
                HANDLE hFind = FindFirstFileA(dir, &fd);
                int corrupted = 0;
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        char fpath[MAX_PATH] = {};
                        snprintf(fpath, sizeof(fpath),
                                 "%s\\ScarlettCompat\\ShaderCache\\%s",
                                 localAppData, fd.cFileName);
                        HANDLE f = CreateFileA(fpath, GENERIC_READ |
                                               GENERIC_WRITE, 0, nullptr,
                                               OPEN_EXISTING,
                                               FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (f != INVALID_HANDLE_VALUE) {
                            // Header is 28 bytes; flip a blob byte.
                            SetFilePointer(f, 28, nullptr, FILE_BEGIN);
                            uint8_t b = 0;
                            DWORD got = 0;
                            if (ReadFile(f, &b, 1, &got, nullptr) &&
                                got == 1) {
                                SetFilePointer(f, 28, nullptr, FILE_BEGIN);
                                b ^= 0xFF;
                                DWORD put = 0;
                                if (WriteFile(f, &b, 1, &put, nullptr) &&
                                    put == 1)
                                    corrupted++;
                            }
                            CloseHandle(f);
                        }
                    } while (FindNextFileA(hFind, &fd));
                    FindClose(hFind);
                }
                if (corrupted < 2) ok = false;
                uint8_t* vs3 = nullptr;
                uint8_t* ps3 = nullptr;
                size_t vs3Len = 0, ps3Len = 0;
                if (ok) hr = ScOpt_CompileCached(kVsSrc, strlen(kVsSrc),
                                                 "vs_5_0", "main", &vs3,
                                                 &vs3Len);
                if (FAILED(hr) || !vs3) ok = false;
                if (ok) hr = ScOpt_CompileCached(kPsSrc, strlen(kPsSrc),
                                                 "ps_5_0", "main", &ps3,
                                                 &ps3Len);
                if (FAILED(hr) || !ps3) ok = false;
                if (ok && (vs3Len != vs1Len || ps3Len != ps1Len ||
                           memcmp(vs3, vs1, vs1Len) != 0 ||
                           memcmp(ps3, ps1, ps1Len) != 0))
                    ok = false;
                ScOpt_ShaderCacheStats(&hits, &misses);
                if (hits != 2 || misses != 4) ok = false;
                free(vs3);
                free(ps3);
                snprintf(detail, sizeof(detail),
                         "compile (miss) %.3f ms, load (hit) %.3f ms; "
                         "hit/miss blobs byte-identical; %d cache files "
                         "corrupted then re-compiled to original bytes; "
                         "hits=%llu misses=%llu (want 2/4)",
                         compileMs, loadMs, corrupted,
                         (unsigned long long)hits,
                         (unsigned long long)misses);
            }
            if (detail[0] == 0) {
                uint64_t h2 = 0, m2 = 0;
                ScOpt_ShaderCacheStats(&h2, &m2);
                snprintf(detail, sizeof(detail),
                         "shader cache check failed (hits=%llu misses=%llu, "
                         "want 2/2 before corruption)",
                         (unsigned long long)h2, (unsigned long long)m2);
            }
        }
        shaderOk = ok;
        free(vs1);
        free(ps1);
        free(vs2);
        free(ps2);
    }
    Report("opt_shader", shaderOk, detail);
    allOk &= shaderOk;

    // 10. Frame batching, lever 1 — command-list submission batching.
    bool batchSubmitOk = false;
    detail[0] = 0;
    if (device) {
        bool ok = true;
        ID3D12CommandQueue* q = nullptr;
        ID3D12Fence* fence = nullptr;
        HANDLE fenceEv = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        UINT64 fenceVal = 0;
        ID3D12Resource* rt = nullptr;
        ID3D12DescriptorHeap* rtvHeap = nullptr;
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
        if (SUCCEEDED(hr))
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                     IID_PPV_ARGS(&fence));
        if (SUCCEEDED(hr)) {
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = 64;
            rd.Height = 64;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            hr = device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                IID_PPV_ARGS(&rt));
        }
        if (SUCCEEDED(hr)) {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = 1;
            hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
        }
        // Record 16 small lists (each clears the RT). They stay closed
        // and pending, so the same 16 lists can be submitted both ways.
        const int K = 16;
        ID3D12CommandAllocator* allocs[16] = {};
        ID3D12GraphicsCommandList* lists[16] = {};
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
        if (SUCCEEDED(hr)) {
            rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            // The heap slot starts uninitialized: the RTV descriptor must
            // be written before any ClearRenderTargetView reads it.
            // (Missing this crashed the AMD driver in build 5 — the
            // clear dereferenced a garbage descriptor.)
            device->CreateRenderTargetView(rt, nullptr, rtv);
        }
        // Setup must have succeeded before the loop touches the RTV:
        // a null rtv handle (setup failed, ok still true) makes
        // ClearRenderTargetView fault inside the AMD driver (amdxc64.dll),
        // which is what killed the opt guest in build 3.
        if (FAILED(hr)) ok = false;
        printf("[guest] batch_submit: setup hr=0x%08x rtv.ptr=0x%llx "
               "ok=%d\n",
               (unsigned)hr, (unsigned long long)rtv.ptr, (int)ok);
        fflush(stdout);
        printf("[guest] batch_submit: setup done, recording %d lists\n", K);
        fflush(stdout);
        for (int i = 0; ok && i < K; i++) {
            hr = device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocs[i]));
            if (SUCCEEDED(hr))
                hr = device->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs[i], nullptr,
                    IID_PPV_ARGS(&lists[i]));
            if (SUCCEEDED(hr)) {
                float clearCol[4] = {0.2f, 0.4f, 0.8f, 1.0f};
                lists[i]->ClearRenderTargetView(rtv, clearCol, 0, nullptr);
                hr = lists[i]->Close();
            }
            if (FAILED(hr)) ok = false;
        }
        printf("[guest] batch_submit: recorded ok=%d\n", (int)ok);
        fflush(stdout);
        double separateMs = 0, batchedMs = 0;
        uint32_t listsSubmitted = 0, submitCalls = 0;
        if (ok) {
            // Baseline: 16 separate ExecuteCommandLists calls.
            ScOptTimer t;
            t.Start();
            for (int i = 0; i < K; i++) {
                ID3D12CommandList* one[] = {lists[i]};
                q->ExecuteCommandLists(1, one);
            }
            separateMs = t.ElapsedMs();
            q->Signal(fence, ++fenceVal);
            fence->SetEventOnCompletion(fenceVal, fenceEv);
            if (WaitForSingleObject(fenceEv, 30000) != WAIT_OBJECT_0)
                ok = false;
            printf("[guest] batch_submit: baseline separate ok=%d %.3f ms\n",
                   (int)ok, separateMs);
            fflush(stdout);
        }
        if (ok) {
            // Batched: one ExecuteCommandLists call with all 16 lists.
            ScOptBatchHandle batch = nullptr;
            hr = ScOpt_BatchInit(q, 16, &batch);
            printf("[guest] batch_submit: BatchInit hr=0x%08x\n",
                   (unsigned)hr);
            fflush(stdout);
            if (SUCCEEDED(hr)) {
                ScOptTimer t;
                t.Start();
                for (int i = 0; i < K; i++) {
                    hr = ScOpt_BatchAdd(batch, lists[i]);
                    if (FAILED(hr)) break;
                }
                printf("[guest] batch_submit: BatchAdd loop hr=0x%08x\n",
                       (unsigned)hr);
                fflush(stdout);
                if (SUCCEEDED(hr)) hr = ScOpt_BatchFlush(batch);
                batchedMs = t.ElapsedMs();
                printf("[guest] batch_submit: BatchFlush hr=0x%08x\n",
                       (unsigned)hr);
                fflush(stdout);
                if (SUCCEEDED(hr))
                    hr = ScOpt_BatchStats(batch, &listsSubmitted,
                                          &submitCalls);
                if (SUCCEEDED(hr) &&
                    (listsSubmitted != 16 || submitCalls != 1))
                    ok = false;
                ScOpt_BatchDestroy(batch);
                printf("[guest] batch_submit: stats lists=%u calls=%u "
                       "ok=%d\n",
                       listsSubmitted, submitCalls, (int)ok);
                fflush(stdout);
            } else {
                ok = false;
            }
            q->Signal(fence, ++fenceVal);
            fence->SetEventOnCompletion(fenceVal, fenceEv);
            if (WaitForSingleObject(fenceEv, 30000) != WAIT_OBJECT_0)
                ok = false;
            printf("[guest] batch_submit: batched fence wait ok=%d\n",
                   (int)ok);
            fflush(stdout);
        }
        batchSubmitOk = ok;
        snprintf(detail, sizeof(detail),
                 "16 lists: 16 separate submits %.3f ms vs 1 batched "
                 "submit %.3f ms; batcher lists=%u calls=%u (want 16/1)",
                 separateMs, batchedMs, listsSubmitted, submitCalls);
        for (int i = 0; i < K; i++) {
            if (lists[i]) lists[i]->Release();
            if (allocs[i]) allocs[i]->Release();
        }
        if (rtvHeap) rtvHeap->Release();
        if (rt) rt->Release();
        if (fence) fence->Release();
        if (q) q->Release();
        if (fenceEv) CloseHandle(fenceEv);
    } else {
        snprintf(detail, sizeof(detail), "no device");
    }
    Report("opt_batch_submit", batchSubmitOk, detail);
    allOk &= batchSubmitOk;

    // 11. Frame batching, lever 2 — draw/state batching.
    bool batchDrawOk = false;
    detail[0] = 0;
    if (device) {
        bool ok = true;
        ID3D12CommandQueue* q = nullptr;
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        ID3D12CommandAllocator* rawAlloc = nullptr;
        ID3D12GraphicsCommandList* rawList = nullptr;
        ID3D12Fence* fence = nullptr;
        HANDLE fenceEv = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        UINT64 fenceVal = 0;
        ID3D12Resource* rt = nullptr;
        ID3D12DescriptorHeap* rtvHeap = nullptr;
        ID3D12RootSignature* rs = nullptr;
        ID3D12PipelineState* pso = nullptr;
        ID3D12PipelineState* pso2 = nullptr;
        ID3D12Resource* vb = nullptr;
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           alloc, nullptr,
                                           IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr)) hr = list->Close();
        if (SUCCEEDED(hr))
            hr = device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&rawAlloc));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           rawAlloc, nullptr,
                                           IID_PPV_ARGS(&rawList));
        if (SUCCEEDED(hr)) hr = rawList->Close();
        if (SUCCEEDED(hr))
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                     IID_PPV_ARGS(&fence));
        if (SUCCEEDED(hr)) {
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = 256;
            rd.Height = 256;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv = {};
            cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            cv.Color[0] = 51.0f / 255.0f;
            cv.Color[1] = 102.0f / 255.0f;
            cv.Color[2] = 204.0f / 255.0f;
            cv.Color[3] = 1.0f;
            hr = device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt));
        }
        if (SUCCEEDED(hr)) {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = 1;
            hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
        }
        // PSOs from the mesh fallback shaders (red) + the green PS.
        const void* fbVs = nullptr;
        const void* fbPs = nullptr;
        SIZE_T fbVsLen = 0, fbPsLen = 0;
        ID3DBlob* psGreen = nullptr;
        if (SUCCEEDED(hr)) hr = ScOpt_MeshFallbackInit(device);
        if (SUCCEEDED(hr))
            hr = ScOpt_MeshFallbackGetShaders(&fbVs, &fbVsLen, &fbPs,
                                              &fbPsLen);
        if (SUCCEEDED(hr) && (!fbVs || !fbPs)) hr = E_FAIL;
        if (SUCCEEDED(hr)) ok = CompileShader(kPsGreenSrc, "ps_5_0",
                                               &psGreen);
        if (SUCCEEDED(hr)) {
            D3D12_ROOT_SIGNATURE_DESC rsd = {};
            rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            ID3DBlob* rsBlob = nullptr;
            hr = D3D12SerializeRootSignature(&rsd,
                                             D3D_ROOT_SIGNATURE_VERSION_1,
                                             &rsBlob, nullptr);
            if (SUCCEEDED(hr))
                hr = device->CreateRootSignature(
                    0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                    IID_PPV_ARGS(&rs));
            if (rsBlob) rsBlob->Release();
        }
        auto MakePso = [&](const void* psBlob, SIZE_T psLen,
                           ID3D12PipelineState** out) -> HRESULT {
            D3D12_INPUT_ELEMENT_DESC elems[2] = {};
            elems[0].SemanticName = "POSITION";
            elems[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
            elems[0].InputSlotClass =
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            elems[1].SemanticName = "COLOR";
            elems[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            elems[1].AlignedByteOffset = 12;
            elems[1].InputSlotClass =
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
            pd.InputLayout.pInputElementDescs = elems;
            pd.InputLayout.NumElements = 2;
            pd.pRootSignature = rs;
            pd.VS.pShaderBytecode = fbVs;
            pd.VS.BytecodeLength = fbVsLen;
            pd.PS.pShaderBytecode = psBlob;
            pd.PS.BytecodeLength = psLen;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.SampleMask = UINT_MAX;
            pd.PrimitiveTopologyType =
                D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pd.SampleDesc.Count = 1;
            return device->CreateGraphicsPipelineState(&pd,
                                                       IID_PPV_ARGS(out));
        };
        if (SUCCEEDED(hr)) hr = MakePso(fbPs, fbPsLen, &pso);
        if (SUCCEEDED(hr) && psGreen)
            hr = MakePso(psGreen->GetBufferPointer(),
                         psGreen->GetBufferSize(), &pso2);
        // Vertex buffer: 200 red triangles in [-0.8, 0.8]^2 (600 verts).
        if (SUCCEEDED(hr)) {
            const uint32_t VERTS = 600;
            float* data = (float*)malloc((size_t)VERTS * 7 * sizeof(float));
            if (!data) {
                hr = E_OUTOFMEMORY;
            } else {
                for (uint32_t t = 0; t < 200; t++) {
                    // Half-step offset: triangle 44 lands centred on
                    // the origin, so it covers the centre pixel.
                    float cx = -0.7f + 0.0775f + (float)(t % 10) * 0.155f;
                    float cy = -0.7f + 0.0775f + (float)(t / 10) * 0.155f;
                    float s = 0.07f;
                    float tri[3][7] = {
                        {cx - s, cy - s, 0, 1, 0, 0, 1},
                        {cx + s, cy - s, 0, 1, 0, 0, 1},
                        {cx, cy + s, 0, 1, 0, 0, 1},
                    };
                    memcpy(data + (size_t)t * 21, tri, sizeof(tri));
                }
                D3D12_HEAP_PROPERTIES hp = {};
                hp.Type = D3D12_HEAP_TYPE_UPLOAD;
                D3D12_RESOURCE_DESC rd = {};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = (uint64_t)VERTS * 7 * sizeof(float);
                rd.Height = 1;
                rd.DepthOrArraySize = 1;
                rd.MipLevels = 1;
                rd.SampleDesc.Count = 1;
                rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                hr = device->CreateCommittedResource(
                    &hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&vb));
                if (SUCCEEDED(hr)) {
                    void* mapped = nullptr;
                    D3D12_RANGE range = {0, (size_t)VERTS * 7 * sizeof(float)};
                    hr = vb->Map(0, &range, &mapped);
                    if (SUCCEEDED(hr) && mapped) {
                        memcpy(mapped, data,
                               (size_t)VERTS * 7 * sizeof(float));
                        D3D12_RANGE empty = {0, 0};
                        vb->Unmap(0, &empty);
                    }
                }
                free(data);
            }
        }
        uint32_t drawsReq = 0, drawsRec = 0, setsReq = 0, setsIss = 0;
        double batchMs = 0, rawMs = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        if (SUCCEEDED(hr)) {
            rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            // Write the RTV descriptor before any ClearRenderTargetView
            // reads the slot (same missing-call bug as batch_submit).
            device->CreateRenderTargetView(rt, nullptr, rtv);
            vbv.BufferLocation = vb->GetGPUVirtualAddress();
            vbv.SizeInBytes = 600 * 7 * sizeof(float);
            vbv.StrideInBytes = 7 * sizeof(float);
            // Batched recording (timed). The clear/viewport/topology go
            // direct on the list BEFORE the batcher session starts, so
            // they cannot land after a pending merged draw.
            hr = alloc->Reset();
            if (SUCCEEDED(hr)) hr = list->Reset(alloc, nullptr);
            if (SUCCEEDED(hr)) {
                float clearCol[4] = {51.0f / 255.0f, 102.0f / 255.0f,
                                     204.0f / 255.0f, 1.0f};
                list->ClearRenderTargetView(rtv, clearCol, 0, nullptr);
                D3D12_VIEWPORT vp = {0, 0, 256, 256, 0, 1};
                D3D12_RECT sc = {0, 0, 256, 256};
                list->RSSetViewports(1, &vp);
                list->RSSetScissorRects(1, &sc);
                list->IASetPrimitiveTopology(
                    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            }
            ScOptDrawBatchHandle db = nullptr;
            if (SUCCEEDED(hr)) hr = ScOpt_DrawBatchBegin(list, &db);
            ScOptTimer t;
            if (SUCCEEDED(hr)) {
                t.Start();
                hr = ScOpt_DrawBatchSetPSO(db, pso);
                if (SUCCEEDED(hr))
                    hr = ScOpt_DrawBatchSetRootSignature(db, rs);
                if (SUCCEEDED(hr))
                    hr = ScOpt_DrawBatchSetRenderTargets(db, 1, &rtv, FALSE,
                                                         nullptr);
                if (SUCCEEDED(hr))
                    hr = ScOpt_DrawBatchSetVertexBuffer(db, 0, &vbv);
                // Duplicate sets (must be skipped, not re-issued).
                if (SUCCEEDED(hr)) hr = ScOpt_DrawBatchSetPSO(db, pso);
                if (SUCCEEDED(hr))
                    hr = ScOpt_DrawBatchSetRootSignature(db, rs);
                if (SUCCEEDED(hr))
                    hr = ScOpt_DrawBatchSetRenderTargets(db, 1, &rtv, FALSE,
                                                         nullptr);
                if (SUCCEEDED(hr))
                    hr = ScOpt_DrawBatchSetVertexBuffer(db, 0, &vbv);
                // 4 contiguous draws -> merge into 1 pending draw.
                if (SUCCEEDED(hr))
                    hr = ScOpt_DrawBatchDrawInstanced(db, 3, 0, 1, 0);
                if (SUCCEEDED(hr))
                    hr = ScOpt_DrawBatchDrawInstanced(db, 3, 3, 1, 0);
                if (SUCCEEDED(hr))
                    hr = ScOpt_DrawBatchDrawInstanced(db, 3, 6, 1, 0);
                if (SUCCEEDED(hr))
                    hr = ScOpt_DrawBatchDrawInstanced(db, 3, 9, 1, 0);
                // PSO change flushes the pending draw, then 2 draws.
                if (SUCCEEDED(hr)) hr = ScOpt_DrawBatchSetPSO(db, pso2);
                if (SUCCEEDED(hr))
                    hr = ScOpt_DrawBatchDrawInstanced(db, 3, 12, 1, 0);
                if (SUCCEEDED(hr))
                    hr = ScOpt_DrawBatchDrawInstanced(db, 3, 15, 1, 0);
                // Back to the first PSO, then a gapped pair. The pair
                // draws triangles 44/45 (vertices 132/135) — triangle 44
                // is centred on the origin, so the merged draw covers
                // the centre pixel the stage pixel-checks.
                if (SUCCEEDED(hr)) hr = ScOpt_DrawBatchSetPSO(db, pso);
                if (SUCCEEDED(hr))
                    hr = ScOpt_DrawBatchDrawInstanced(db, 3, 132, 1, 0);
                if (SUCCEEDED(hr))
                    hr = ScOpt_DrawBatchDrawInstanced(db, 3, 135, 1, 0);
                // Flush the pending merged draw BEFORE Stats: Stats
                // counts issued draws, and the last merged draw is
                // still pending until it is flushed (End flushes it
                // too, but End releases the handle, so Stats must come
                // after an explicit flush).
                if (SUCCEEDED(hr)) hr = ScOpt_DrawBatchFlushDraw(db);
                // Stats BEFORE End (End releases the handle).
                if (SUCCEEDED(hr))
                    hr = ScOpt_DrawBatchStats(db, &drawsReq, &drawsRec,
                                              &setsReq, &setsIss);
                if (SUCCEEDED(hr)) hr = ScOpt_DrawBatchEnd(db);
                batchMs = t.ElapsedMs();
                if (SUCCEEDED(hr)) hr = list->Close();
            }
            if (FAILED(hr)) ok = false;
        }
        // Raw recording of the same sequence (no batcher), timed only —
        // it is never executed; the batched list is the one that runs.
        if (ok) {
            hr = rawAlloc->Reset();
            if (SUCCEEDED(hr)) hr = rawList->Reset(rawAlloc, nullptr);
            if (SUCCEEDED(hr)) {
                ScOptTimer t2;
                t2.Start();
                float clearCol[4] = {51.0f / 255.0f, 102.0f / 255.0f,
                                     204.0f / 255.0f, 1.0f};
                rawList->ClearRenderTargetView(rtv, clearCol, 0, nullptr);
                D3D12_VIEWPORT vp = {0, 0, 256, 256, 0, 1};
                D3D12_RECT sc = {0, 0, 256, 256};
                rawList->RSSetViewports(1, &vp);
                rawList->RSSetScissorRects(1, &sc);
                rawList->IASetPrimitiveTopology(
                    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                rawList->SetPipelineState(pso);
                rawList->SetGraphicsRootSignature(rs);
                rawList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                rawList->IASetVertexBuffers(0, 1, &vbv);
                rawList->SetPipelineState(pso);
                rawList->SetGraphicsRootSignature(rs);
                rawList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                rawList->IASetVertexBuffers(0, 1, &vbv);
                for (int i = 0; i < 4; i++)
                    rawList->DrawInstanced(3, 1, (UINT)(i * 3), 0);
                rawList->SetPipelineState(pso2);
                rawList->DrawInstanced(3, 1, 12, 0);
                rawList->DrawInstanced(3, 1, 15, 0);
                rawList->SetPipelineState(pso);
                rawList->DrawInstanced(3, 1, 132, 0);
                rawList->DrawInstanced(3, 1, 135, 0);
                hr = rawList->Close();
                rawMs = t2.ElapsedMs();
            }
            if (FAILED(hr)) ok = false;
        }
        // Execute the BATCHED list and pixel-check it (the merge must
        // render the same triangles in the same order).
        if (ok) {
            ID3D12CommandList* lists[] = {list};
            q->ExecuteCommandLists(1, lists);
            q->Signal(fence, ++fenceVal);
            fence->SetEventOnCompletion(fenceVal, fenceEv);
            if (WaitForSingleObject(fenceEv, 30000) != WAIT_OBJECT_0)
                ok = false;
            if (ok) {
                uint8_t center[4] = {}, corner[4] = {};
                if (ReadbackPixels(device, q, rt, center, corner)) {
                    bool centerRed =
                        (abs((int)center[0] - 255) <= 3) &&
                        (abs((int)center[1] - 0) <= 3) &&
                        (abs((int)center[2] - 0) <= 3) &&
                        (abs((int)center[3] - 255) <= 3);
                    bool cornerClear =
                        (abs((int)corner[0] - 51) <= 3) &&
                        (abs((int)corner[1] - 102) <= 3) &&
                        (abs((int)corner[2] - 204) <= 3) &&
                        (abs((int)corner[3] - 255) <= 3);
                    if (!centerRed || !cornerClear) ok = false;
                    snprintf(detail, sizeof(detail),
                             "draws %u requested -> %u recorded; sets %u "
                             "requested -> %u issued; batched record %.3f ms "
                             "vs raw %.3f ms; center (%d,%d,%d,%d) corner "
                             "(%d,%d,%d,%d)",
                             drawsReq, drawsRec, setsReq, setsIss, batchMs,
                             rawMs, center[0], center[1], center[2],
                             center[3], corner[0], corner[1], corner[2],
                             corner[3]);
                } else {
                    ok = false;
                    snprintf(detail, sizeof(detail), "readback failed");
                }
            }
        }
        // Expected counts: draws 8 requested -> 3 recorded
        // ({12,0}, {6,12}, {6,132}); sets 10 requested -> 6 issued.
        if (ok && (drawsReq != 8 || drawsRec != 3 || setsReq != 10 ||
                   setsIss != 6))
            ok = false;
        batchDrawOk = ok;
        if (psGreen) psGreen->Release();
        if (pso2) pso2->Release();
        if (pso) pso->Release();
        if (rs) rs->Release();
        if (vb) vb->Release();
        if (rtvHeap) rtvHeap->Release();
        if (rt) rt->Release();
        if (fence) fence->Release();
        if (rawList) rawList->Release();
        if (rawAlloc) rawAlloc->Release();
        if (list) list->Release();
        if (alloc) alloc->Release();
        if (q) q->Release();
        if (fenceEv) CloseHandle(fenceEv);
    } else {
        snprintf(detail, sizeof(detail), "no device");
    }
    Report("opt_batch_draw", batchDrawOk, detail);

    allOk &= batchDrawOk;

    // 12. Frame batching, lever 3 — frames in flight / queue depth.
    // Runs the same fixed workload (compute dispatch + triangle draw)
    // for 60 frames at each depth (2, 3, 4) and reports the tradeoff:
    // average frame time, average CPU wait-for-slot, implied input
    // latency (depth x frame time). Back-to-back frames (no pacing)
    // so the queue depth actually matters; pacing is verified
    // separately by the proof guest (SetFrameIntervalX).
    bool flightOk = false;
    detail[0] = 0;
    if (device) {
        bool ok = true;
        // Fixed workload shaders: a xorshift compute kernel + the
        // mesh fallback's triangle shaders.
        static const char kFlightCS[] =
            "RWStructuredBuffer<uint> outBuf : register(u0);\n"
            "cbuffer Params : register(b0) { uint count; uint iters; };\n"
            "[numthreads(256, 1, 1)]\n"
            "void main(uint3 tid : SV_DispatchThreadID) {\n"
            "    if (tid.x >= count) return;\n"
            "    uint x = tid.x * 2654435761u + 12345u;\n"
            "    for (uint i = 0; i < iters; i++) {\n"
            "        x ^= x << 13; x ^= x >> 17; x ^= x << 5;\n"
            "        x += i * 97u;\n"
            "    }\n"
            "    outBuf[tid.x] = x;\n"
            "}\n";
        ID3D12CommandQueue* q = nullptr;
        ID3D12Fence* fence = nullptr;
        HANDLE fenceEv = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        ID3D12RootSignature* csRs = nullptr;
        ID3D12PipelineState* csPso = nullptr;
        ID3D12RootSignature* gfxRs = nullptr;
        ID3D12PipelineState* gfxPso = nullptr;
        ID3D12Resource* triVb = nullptr;
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
        if (SUCCEEDED(hr))
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                     IID_PPV_ARGS(&fence));
        ID3DBlob* csBlob = nullptr;
        if (SUCCEEDED(hr)) ok = CompileShader(kFlightCS, "cs_5_0", &csBlob);
        if (ok) {
            D3D12_ROOT_PARAMETER params[2] = {};
            params[0].ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[0].Constants.Num32BitValues = 2;
            params[0].Constants.ShaderRegister = 0;
            params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            params[1].Descriptor.ShaderRegister = 0;
            params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_ROOT_SIGNATURE_DESC rsd = {};
            rsd.NumParameters = 2;
            rsd.pParameters = params;
            ID3DBlob* rsBlob = nullptr;
            hr = D3D12SerializeRootSignature(&rsd,
                                             D3D_ROOT_SIGNATURE_VERSION_1,
                                             &rsBlob, nullptr);
            if (SUCCEEDED(hr))
                hr = device->CreateRootSignature(
                    0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                    IID_PPV_ARGS(&csRs));
            if (rsBlob) rsBlob->Release();
        }
        if (SUCCEEDED(hr) && csBlob) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC cpd = {};
            cpd.pRootSignature = csRs;
            cpd.CS.pShaderBytecode = csBlob->GetBufferPointer();
            cpd.CS.BytecodeLength = csBlob->GetBufferSize();
            hr = device->CreateComputePipelineState(&cpd,
                                                    IID_PPV_ARGS(&csPso));
        }
        const void* fbVs = nullptr;
        const void* fbPs = nullptr;
        SIZE_T fbVsLen = 0, fbPsLen = 0;
        if (SUCCEEDED(hr)) hr = ScOpt_MeshFallbackInit(device);
        if (SUCCEEDED(hr))
            hr = ScOpt_MeshFallbackGetShaders(&fbVs, &fbVsLen, &fbPs,
                                              &fbPsLen);
        if (SUCCEEDED(hr) && (!fbVs || !fbPs)) hr = E_FAIL;
        if (SUCCEEDED(hr)) {
            D3D12_ROOT_SIGNATURE_DESC rsd = {};
            rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            ID3DBlob* rsBlob = nullptr;
            hr = D3D12SerializeRootSignature(&rsd,
                                             D3D_ROOT_SIGNATURE_VERSION_1,
                                             &rsBlob, nullptr);
            if (SUCCEEDED(hr))
                hr = device->CreateRootSignature(
                    0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                    IID_PPV_ARGS(&gfxRs));
            if (rsBlob) rsBlob->Release();
            if (SUCCEEDED(hr)) {
                D3D12_INPUT_ELEMENT_DESC elems[2] = {};
                elems[0].SemanticName = "POSITION";
                elems[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
                elems[0].InputSlotClass =
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                elems[1].SemanticName = "COLOR";
                elems[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                elems[1].AlignedByteOffset = 12;
                elems[1].InputSlotClass =
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                D3D12_GRAPHICS_PIPELINE_STATE_DESC gpd = {};
                gpd.InputLayout.pInputElementDescs = elems;
                gpd.InputLayout.NumElements = 2;
                gpd.pRootSignature = gfxRs;
                gpd.VS.pShaderBytecode = fbVs;
                gpd.VS.BytecodeLength = fbVsLen;
                gpd.PS.pShaderBytecode = fbPs;
                gpd.PS.BytecodeLength = fbPsLen;
                gpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                gpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                gpd.BlendState.RenderTarget[0].RenderTargetWriteMask =
                    D3D12_COLOR_WRITE_ENABLE_ALL;
                gpd.SampleMask = UINT_MAX;
                gpd.PrimitiveTopologyType =
                    D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                gpd.NumRenderTargets = 1;
                gpd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                gpd.SampleDesc.Count = 1;
                hr = device->CreateGraphicsPipelineState(
                    &gpd, IID_PPV_ARGS(&gfxPso));
            }
        }
        // Triangle vertex buffer (upload heap, 3 red verts).
        if (SUCCEEDED(hr)) {
            static const float kTri[3 * 7] = {
                -0.8f, -0.8f, 0.0f, 1, 0, 0, 1,
                0.8f, -0.8f, 0.0f, 1, 0, 0, 1,
                0.0f, 0.8f, 0.0f, 1, 0, 0, 1,
            };
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = sizeof(kTri);
            rd.Height = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            hr = device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&triVb));
            if (SUCCEEDED(hr)) {
                void* mapped = nullptr;
                D3D12_RANGE range = {0, sizeof(kTri)};
                hr = triVb->Map(0, &range, &mapped);
                if (SUCCEEDED(hr) && mapped) {
                    memcpy(mapped, kTri, sizeof(kTri));
                    D3D12_RANGE empty = {0, 0};
                    triVb->Unmap(0, &empty);
                }
            }
        }
        char flightDetail[1024] = {};
        const uint32_t kFrames = 60;
        const uint32_t kCount = 262144, kIters = 512;
        if (SUCCEEDED(hr)) {
            for (int depth = 2; depth <= 4 && ok; depth++) {
                // Per-slot resources (allocator + list + RT + UAV).
                ID3D12CommandAllocator* allocs[4] = {};
                ID3D12GraphicsCommandList* lists[4] = {};
                ID3D12Resource* rts[4] = {};
                ID3D12Resource* uavs[4] = {};
                ID3D12DescriptorHeap* rtvHeap = nullptr;
                ScOptFlightHandle flight = nullptr;
                hr = ScOpt_FlightInit((uint32_t)depth, &flight);
                if (SUCCEEDED(hr)) {
                    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
                    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                    hd.NumDescriptors = (UINT)depth;
                    hr = device->CreateDescriptorHeap(&hd,
                                                      IID_PPV_ARGS(&rtvHeap));
                }
                for (int s = 0; SUCCEEDED(hr) && s < depth; s++) {
                    hr = device->CreateCommandAllocator(
                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                        IID_PPV_ARGS(&allocs[s]));
                    if (SUCCEEDED(hr))
                        hr = device->CreateCommandList(
                            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs[s],
                            nullptr, IID_PPV_ARGS(&lists[s]));
                    if (SUCCEEDED(hr)) hr = lists[s]->Close();
                    if (SUCCEEDED(hr)) {
                        D3D12_HEAP_PROPERTIES hp = {};
                        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                        D3D12_RESOURCE_DESC rd = {};
                        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                        rd.Width = 256;
                        rd.Height = 256;
                        rd.DepthOrArraySize = 1;
                        rd.MipLevels = 1;
                        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                        rd.SampleDesc.Count = 1;
                        rd.Flags =
                            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                        hr = device->CreateCommittedResource(
                            &hp, D3D12_HEAP_FLAG_NONE, &rd,
                            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                            IID_PPV_ARGS(&rts[s]));
                        if (SUCCEEDED(hr)) {
                            D3D12_CPU_DESCRIPTOR_HANDLE h =
                                rtvHeap->GetCPUDescriptorHandleForHeapStart();
                            UINT inc = device->GetDescriptorHandleIncrementSize(
                                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                            h.ptr += (SIZE_T)s * inc;
                            device->CreateRenderTargetView(rts[s], nullptr, h);
                        }
                    }
                    if (SUCCEEDED(hr)) {
                        D3D12_HEAP_PROPERTIES hp = {};
                        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                        D3D12_RESOURCE_DESC rd = {};
                        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                        rd.Width = (uint64_t)kCount * sizeof(uint32_t);
                        rd.Height = 1;
                        rd.DepthOrArraySize = 1;
                        rd.MipLevels = 1;
                        rd.SampleDesc.Count = 1;
                        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                        rd.Flags =
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                        hr = device->CreateCommittedResource(
                            &hp, D3D12_HEAP_FLAG_NONE, &rd,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                            IID_PPV_ARGS(&uavs[s]));
                    }
                    if (FAILED(hr)) break;
                }
                double totalWaitMs = 0;
                double wallMs = 0;
                if (SUCCEEDED(hr)) {
                    ScOptTimer wall;
                    wall.Start();
                    for (uint32_t f = 0; f < kFrames && SUCCEEDED(hr); f++) {
                        uint32_t slot = f % (uint32_t)depth;
                        double waitMs = 0;
                        hr = ScOpt_FlightBeginFrame(flight, q, fence,
                                                    &waitMs);
                        if (FAILED(hr)) break;
                        totalWaitMs += waitMs;
                        hr = allocs[slot]->Reset();
                        if (SUCCEEDED(hr))
                            hr = lists[slot]->Reset(allocs[slot], nullptr);
                        if (SUCCEEDED(hr)) {
                            // Compute dispatch (UAV[slot]).
                            lists[slot]->SetComputeRootSignature(csRs);
                            lists[slot]->SetPipelineState(csPso);
                            UINT constants[2] = {kCount, kIters};
                            lists[slot]->SetComputeRoot32BitConstants(
                                0, 2, constants, 0);
                            lists[slot]->SetComputeRootUnorderedAccessView(
                                1, uavs[slot]->GetGPUVirtualAddress());
                            lists[slot]->Dispatch((kCount + 255) / 256, 1, 1);
                            // Triangle draw (RT[slot]).
                            D3D12_CPU_DESCRIPTOR_HANDLE rtv =
                                rtvHeap->GetCPUDescriptorHandleForHeapStart();
                            UINT inc = device->GetDescriptorHandleIncrementSize(
                                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                            rtv.ptr += (SIZE_T)slot * inc;
                            const float clearCol[4] = {0.2f, 0.4f, 0.8f, 1.0f};
                            lists[slot]->ClearRenderTargetView(rtv, clearCol,
                                                               0, nullptr);
                            lists[slot]->OMSetRenderTargets(1, &rtv, FALSE,
                                                            nullptr);
                            D3D12_VIEWPORT vp = {0, 0, 256, 256, 0, 1};
                            D3D12_RECT sc = {0, 0, 256, 256};
                            lists[slot]->RSSetViewports(1, &vp);
                            lists[slot]->RSSetScissorRects(1, &sc);
                            lists[slot]->SetGraphicsRootSignature(gfxRs);
                            lists[slot]->SetPipelineState(gfxPso);
                            lists[slot]->IASetPrimitiveTopology(
                                D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                            D3D12_VERTEX_BUFFER_VIEW vbv = {};
                            vbv.BufferLocation =
                                triVb->GetGPUVirtualAddress();
                            vbv.SizeInBytes = 3 * 7 * sizeof(float);
                            vbv.StrideInBytes = 7 * sizeof(float);
                            lists[slot]->IASetVertexBuffers(0, 1, &vbv);
                            lists[slot]->DrawInstanced(3, 1, 0, 0);
                            hr = lists[slot]->Close();
                        }
                        if (SUCCEEDED(hr)) {
                            ID3D12CommandList* one[] = {lists[slot]};
                            q->ExecuteCommandLists(1, one);
                            hr = ScOpt_FlightEndFrame(flight, q, fence);
                        }
                        if (FAILED(hr)) break;
                    }
                    wallMs = wall.ElapsedMs();
                    // Drain the queue: wait for the last signaled fence
                    // value (frames == number of signals, values 1..N).
                    if (SUCCEEDED(hr)) {
                        uint32_t frames = 0;
                        double tw = 0, aw = 0;
                        if (SUCCEEDED(ScOpt_FlightStats(flight, &frames,
                                                        &tw, &aw)) &&
                            frames > 0) {
                            fence->SetEventOnCompletion((UINT64)frames,
                                                        fenceEv);
                            if (WaitForSingleObject(fenceEv, 30000) !=
                                WAIT_OBJECT_0)
                                hr = E_FAIL;
                        }
                    }
                }
                uint32_t frames = 0;
                double tw = 0, aw = 0;
                if (SUCCEEDED(hr))
                    hr = ScOpt_FlightStats(flight, &frames, &tw, &aw);
                if (SUCCEEDED(hr) && frames != kFrames) hr = E_FAIL;
                if (SUCCEEDED(hr)) {
                    double frameMs = wallMs / kFrames;
                    double latencyMs = (double)depth * frameMs;
                    char one[256] = {};
                    snprintf(one, sizeof(one),
                             "depth %d: frame %.3f ms, CPU wait %.3f ms/frame "
                             "(total %.1f ms), input latency ~%.1f ms",
                             depth, frameMs, aw, tw, latencyMs);
                    strncat(flightDetail, one,
                            sizeof(flightDetail) - strlen(flightDetail) - 1);
                    if (depth < 4)
                        strncat(flightDetail, "; ",
                                sizeof(flightDetail) -
                                    strlen(flightDetail) - 1);
                }
                if (flight) ScOpt_FlightDestroy(flight);
                for (int s = 0; s < depth; s++) {
                    if (uavs[s]) uavs[s]->Release();
                    if (rts[s]) rts[s]->Release();
                    if (lists[s]) lists[s]->Release();
                    if (allocs[s]) allocs[s]->Release();
                }
                if (rtvHeap) rtvHeap->Release();
                if (FAILED(hr)) ok = false;
            }
        } else if (ok) {
            ok = false;
        }
        flightOk = ok;
        if (ok) {
            snprintf(detail, sizeof(detail), "%s", flightDetail);
        } else {
            snprintf(detail, sizeof(detail),
                     "flight test failed hr=0x%08x", (unsigned)hr);
        }
        if (triVb) triVb->Release();
        if (gfxPso) gfxPso->Release();
        if (gfxRs) gfxRs->Release();
        if (csPso) csPso->Release();
        if (csRs) csRs->Release();
        if (csBlob) csBlob->Release();
        if (fence) fence->Release();
        if (q) q->Release();
        if (fenceEv) CloseHandle(fenceEv);
    } else {
        snprintf(detail, sizeof(detail), "no device");
    }
    Report("opt_flight", flightOk, detail);
    allOk &= flightOk;

    // 13. De-tiled texture cache — memory management (item 2).
    // GPU texture cache under a VRAM budget with LRU eviction:
    // (A) on a 1MB cache (4 x 256KB textures fit) store keys 1..4,
    // store 5 (evicts 1, the LRU), look up 1 (miss) / 2, 3 (hits),
    // store 6 (evicts 4, now the LRU), look up 4 (miss) / 5 (hit);
    // the GPU compute de-tile is proven byte-identical to the CPU
    // de-tiler by full-texture readback; the game-unload path
    // (Evict) drops a texture so a later lookup misses.
    // (B) streaming workload on a 2MB cache: 60 accesses, 8 of 10 to
    // the hot set {1,2,3} and 2 of 10 to cold keys {4,5} — all five
    // textures fit, so after the 5 warmup misses the hit rate must
    // be >= 80% and bytesUsed must stay within the budget.
    bool texcacheOk = false;
    detail[0] = 0;
    if (device) {
        bool ok = true;
        const uint32_t W = 256, H = 256, BPP = 4;
        const uint32_t TILE_W = 128, TILE_H = 128;  // 128*128*4 = 64KB
        const size_t BYTES = (size_t)W * H * BPP;   // 256 KB per texture
        const uint64_t kBudget1MB = 1024ULL * 1024;
        const uint64_t kBudget2MB = 2ULL * 1024 * 1024;
        ID3D12CommandQueue* q = nullptr;
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
        // Six distinct tiled textures (keys 1..6), deterministic
        // linear patterns tiled with ScOpt_Tile2D.
        uint8_t* tiled[6] = {};
        uint8_t* linear[6] = {};
        if (SUCCEEDED(hr)) {
            for (int k = 0; k < 6 && SUCCEEDED(hr); k++) {
                linear[k] = (uint8_t*)malloc(BYTES);
                tiled[k] = (uint8_t*)malloc(BYTES);
                if (!linear[k] || !tiled[k]) { hr = E_OUTOFMEMORY; break; }
                for (uint32_t y = 0; y < H; y++)
                    for (uint32_t x = 0; x < W; x++) {
                        uint32_t v = (x * 2654435761u) ^ (y * 40503u) ^
                                     (x * y * 97u) ^
                                     ((uint32_t)(k + 1) * 0x9E3779B1u);
                        uint8_t* p = linear[k] + ((size_t)y * W + x) * BPP;
                        p[0] = (uint8_t)(v & 0xFF);
                        p[1] = (uint8_t)((v >> 8) & 0xFF);
                        p[2] = (uint8_t)((v >> 16) & 0xFF);
                        p[3] = 255;
                    }
                hr = ScOpt_Tile2D(linear[k], W, H, BPP, TILE_W, TILE_H,
                                  tiled[k]);
            }
        }
        // (A) LRU + GPU de-tile correctness, 1MB cache.
        ScOptTexCacheHandle cacheA = nullptr;
        uint32_t entriesA = 0, hitsA = 0, missesA = 0, evictA = 0;
        uint64_t usedA = 0, budgetA = 0;
        if (SUCCEEDED(hr)) hr = ScOpt_TexCacheInit(device, kBudget1MB,
                                                   &cacheA);
        auto lookupExpect = [&](uint64_t key, BOOL wantHit) -> bool {
            ID3D12Resource* res = nullptr;
            BOOL hit = FALSE;
            HRESULT h2 = ScOpt_TexCacheLookup(cacheA, key, &res, &hit);
            if (FAILED(h2) || hit != wantHit) {
                if (res) res->Release();
                return false;
            }
            if (wantHit && !res) return false;
            if (res) res->Release();
            return true;
        };
        auto storeKey = [&](uint64_t key) -> bool {
            ID3D12Resource* res = nullptr;
            HRESULT h2 = ScOpt_TexCacheStore(
                cacheA, key, tiled[key - 1], W, H, BPP, TILE_W, TILE_H, q,
                &res);
            if (FAILED(h2) || !res) return false;
            res->Release();
            return true;
        };
        if (SUCCEEDED(hr)) {
            for (uint64_t k = 1; k <= 4; k++)
                if (!storeKey(k)) { ok = false; break; }
        }
        if (ok && !storeKey(5)) ok = false;              // evicts 1
        if (ok && !lookupExpect(1, FALSE)) ok = false;   // 1 is gone
        if (ok && !lookupExpect(2, TRUE)) ok = false;
        if (ok && !lookupExpect(3, TRUE)) ok = false;
        if (ok && !storeKey(6)) ok = false;              // evicts 4
        if (ok && !lookupExpect(4, FALSE)) ok = false;   // 4 is gone
        if (ok && !lookupExpect(5, TRUE)) ok = false;
        if (ok)
            hr = ScOpt_TexCacheStats(cacheA, &entriesA, &usedA, &budgetA,
                                     &hitsA, &missesA, &evictA);
        if (ok &&
            (entriesA != 4 || usedA != kBudget1MB || evictA != 2))
            ok = false;
        // GPU de-tile byte-exactness: read back key 3 and compare
        // with the CPU de-tile of the same tiled input.
        bool gpuExact = false;
        if (ok) {
            ID3D12Resource* res3 = nullptr;
            BOOL hit3 = FALSE;
            hr = ScOpt_TexCacheLookup(cacheA, 3, &res3, &hit3);
            if (SUCCEEDED(hr) && hit3 && res3) {
                uint8_t* back = (uint8_t*)malloc(BYTES);
                uint8_t* cpu = (uint8_t*)malloc(BYTES);
                if (back && cpu) {
                    hr = ScOpt_Detile2D(tiled[2], W, H, BPP, TILE_W, TILE_H,
                                        cpu);
                    if (SUCCEEDED(hr) &&
                        ReadbackTexture(device, q, res3, W, H, back) &&
                        memcmp(cpu, back, BYTES) == 0)
                        gpuExact = true;
                }
                free(back);
                free(cpu);
                res3->Release();
            }
        }
        if (!gpuExact) ok = false;
        // Game-unload path: evict key 2, then it must miss.
        if (ok) {
            hr = ScOpt_TexCacheEvict(cacheA, 2);
            if (hr != S_OK) ok = false;
        }
        if (ok && !lookupExpect(2, FALSE)) ok = false;
        if (ok)
            hr = ScOpt_TexCacheStats(cacheA, &entriesA, &usedA, &budgetA,
                                     &hitsA, &missesA, &evictA);
        if (ok && entriesA != 3) ok = false;
        if (cacheA) ScOpt_TexCacheDestroy(cacheA);

        // (B) Streaming workload, 2MB cache.
        ScOptTexCacheHandle cacheB = nullptr;
        uint32_t entriesB = 0, hitsB = 0, missesB = 0, evictB = 0;
        uint64_t usedB = 0, budgetB = 0;
        uint32_t streamHits = 0, streamMisses = 0;
        if (SUCCEEDED(hr)) hr = ScOpt_TexCacheInit(device, kBudget2MB,
                                                   &cacheB);
        if (SUCCEEDED(hr)) {
            for (int i = 0; i < 60 && SUCCEEDED(hr); i++) {
                uint64_t key = (i % 10 < 8) ? (uint64_t)(1 + (i % 3))
                                            : (uint64_t)(4 + (i % 2));
                ID3D12Resource* res = nullptr;
                BOOL hit = FALSE;
                hr = ScOpt_TexCacheLookup(cacheB, key, &res, &hit);
                if (FAILED(hr)) break;
                if (hit) {
                    streamHits++;
                    if (res) res->Release();
                } else {
                    streamMisses++;
                    ID3D12Resource* stored = nullptr;
                    hr = ScOpt_TexCacheStore(cacheB, key, tiled[key - 1],
                                             W, H, BPP, TILE_W, TILE_H, q,
                                             &stored);
                    if (SUCCEEDED(hr) && stored) stored->Release();
                }
            }
        }
        double hitRate = 0;
        if (SUCCEEDED(hr)) {
            hr = ScOpt_TexCacheStats(cacheB, &entriesB, &usedB, &budgetB,
                                     &hitsB, &missesB, &evictB);
            if (SUCCEEDED(hr) && streamHits + streamMisses > 0)
                hitRate = 100.0 * (double)streamHits /
                          (double)(streamHits + streamMisses);
        }
        if (SUCCEEDED(hr) &&
            (hitRate < 80.0 || entriesB != 5 || usedB > budgetB ||
             evictB != 0))
            ok = false;
        if (cacheB) ScOpt_TexCacheDestroy(cacheB);

        texcacheOk = ok;
        snprintf(detail, sizeof(detail),
                 "GPU de-tile %s; LRU 1MB cache: entries=%u used=%llu/%llu "
                 "evictions=%u (want 3 entries / 768KB after the unload "
                 "evict, 2 evictions), unload evict ok; streaming "
                 "2MB cache: %u hits / %u misses (%.1f%%, want >=80%%), "
                 "entries=%u used=%llu/%llu evictions=%u",
                 gpuExact ? "byte-exact" : "MISMATCH", entriesA,
                 (unsigned long long)usedA, (unsigned long long)budgetA,
                 evictA, streamHits, streamMisses, hitRate, entriesB,
                 (unsigned long long)usedB, (unsigned long long)budgetB,
                 evictB);
        for (int k = 0; k < 6; k++) {
            free(linear[k]);
            free(tiled[k]);
        }
        if (q) q->Release();
    } else {
        snprintf(detail, sizeof(detail), "no device");
    }
    Report("opt_texcache", texcacheOk, detail);
    allOk &= texcacheOk;

    if (device) device->Release();
    snprintf(detail, sizeof(detail),
             "%d/%d optimization stages pass — PSO cache, de-tiled cache, "
             "mesh fallback, async compute, descriptor heap, storage "
             "queue, shader cache, batch submit, batch draw, frames in "
             "flight, texture cache (VRAM budget + LRU)",
             g_stagesPassed, g_stagesTotal);
    Report("opt", allOk, detail);
    printf("[guest] optimization %s\n", allOk ? "ALL PASS" : "FAIL");
    if (g_report) fclose(g_report);
    return allOk ? 0 : 1;
}
