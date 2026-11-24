// opt_async_compute.cpp — async compute mapping (docs/OPTIMIZATION.md
// item 4). Fixed synthetic workload (compute xorshift dispatch + triangle
// draw), run serial on DIRECT vs overlapped on COMPUTE+DIRECT, timed with
// QPC around fence completion. The measurement is the deliverable: on
// RDNA1 async may be slower, and the doc reports whatever the numbers
// say. Readback + checksum happen AFTER the timed region, so they do not
// pollute the timing; both modes must produce identical checksums.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "opt_async_compute.h"
#include "sc_opt_common.h"
#include "scarlett.h"

using namespace sc;

static const uint32_t kComputeCount = 262144;  // threads
static const uint32_t kComputeIters = 512;     // xorshift rounds per thread
static const uint32_t kRtW = 256, kRtH = 256;
static const int kTimedIters = 10;

static const char kAsyncCS[] =
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

static const char kAsyncVS[] =
    "struct VSIn { float3 pos : POSITION; float4 col : COLOR; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "VSOut main(VSIn i) { VSOut o; o.pos = float4(i.pos, 1.0); "
    "o.col = i.col; return o; }\n";

static const char kAsyncPS[] =
    "struct PSIn { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "float4 main(PSIn i) : SV_Target { return i.col; }\n";

static ID3D12CommandQueue* g_directQ = nullptr;
static ID3D12CommandQueue* g_computeQ = nullptr;
static ID3D12CommandAllocator* g_directAlloc = nullptr;
static ID3D12CommandAllocator* g_computeAlloc = nullptr;
static ID3D12GraphicsCommandList* g_directList = nullptr;
static ID3D12GraphicsCommandList* g_computeList = nullptr;
static ID3D12Fence* g_directFence = nullptr;
static ID3D12Fence* g_computeFence = nullptr;
static HANDLE g_fenceEvent = nullptr;
static UINT64 g_fenceVal = 0;
static ID3D12RootSignature* g_csRootSig = nullptr;
static ID3D12PipelineState* g_csPso = nullptr;
static ID3D12RootSignature* g_gfxRootSig = nullptr;
static ID3D12PipelineState* g_gfxPso = nullptr;
static ID3D12Resource* g_computeOut = nullptr;   // UAV, kComputeCount uints
static ID3D12Resource* g_readback = nullptr;     // readback for checksums
static ID3D12Resource* g_rt = nullptr;           // 256x256 draw target
static ID3D12DescriptorHeap* g_rtvHeap = nullptr;
static uint64_t g_checksum[2] = {0, 0};
static bool g_asyncInit = false;

static HRESULT CompileOne(const char* src, const char* target,
                          ID3DBlob** out) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &blob, &err);
    if (FAILED(hr)) {
        if (err) {
            Log("opt_async", "D3DCompile %s failed: %s", target,
                (const char*)err->GetBufferPointer());
            err->Release();
        }
        return hr;
    }
    *out = blob;
    return S_OK;
}

static HRESULT CreateQueueAndList(ID3D12Device* device,
                                  D3D12_COMMAND_LIST_TYPE type,
                                  ID3D12CommandQueue** q,
                                  ID3D12CommandAllocator** alloc,
                                  ID3D12GraphicsCommandList** list) {
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = type;
    HRESULT hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(q));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(type, IID_PPV_ARGS(alloc));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, type, *alloc, nullptr,
                                       IID_PPV_ARGS(list));
    // A fresh command list starts open; close it once so the allocator
    // can be Reset below (phase 4 gotcha).
    if (SUCCEEDED(hr)) hr = (*list)->Close();
    return hr;
}

extern "C" HRESULT ScOpt_AsyncInit(ID3D12Device* device) {
    if (g_asyncInit) return S_OK;
    if (!device) return E_INVALIDARG;
    HRESULT hr = S_OK;
    ID3DBlob* csBlob = nullptr;
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* rsBlob = nullptr;
    ID3DBlob* rsErr = nullptr;

    hr = CreateQueueAndList(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                            &g_directQ, &g_directAlloc, &g_directList);
    if (SUCCEEDED(hr))
        hr = CreateQueueAndList(device, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                &g_computeQ, &g_computeAlloc, &g_computeList);
    if (SUCCEEDED(hr))
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&g_directFence));
    if (SUCCEEDED(hr))
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&g_computeFence));
    if (SUCCEEDED(hr)) {
        g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!g_fenceEvent) hr = E_FAIL;
    }
    // Compute root signature: root constants (count, iters) + root UAV.
    if (SUCCEEDED(hr)) {
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 2;
        params[0].Constants.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor.ShaderRegister = 0;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 2;
        rsd.pParameters = params;
        hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                         &rsBlob, &rsErr);
        if (SUCCEEDED(hr))
            hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                             rsBlob->GetBufferSize(),
                                             IID_PPV_ARGS(&g_csRootSig));
        if (rsBlob) rsBlob->Release();
        if (rsErr) rsErr->Release();
        rsBlob = nullptr;
        rsErr = nullptr;
    }
    if (SUCCEEDED(hr)) hr = CompileOne(kAsyncCS, "cs_5_0", &csBlob);
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd = {};
        cpd.pRootSignature = g_csRootSig;
        cpd.CS.pShaderBytecode = csBlob->GetBufferPointer();
        cpd.CS.BytecodeLength = csBlob->GetBufferSize();
        hr = device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&g_csPso));
    }
    // Graphics root signature: empty. Graphics PSO: passthrough triangle.
    if (SUCCEEDED(hr)) {
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        // The graphics PSO below has an input layout (POSITION + COLOR),
        // so the root signature MUST allow input-assembler input layout
        // or CreateGraphicsPipelineState fails with E_INVALIDARG.
        rsd.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                         &rsBlob, &rsErr);
        if (SUCCEEDED(hr))
            hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                             rsBlob->GetBufferSize(),
                                             IID_PPV_ARGS(&g_gfxRootSig));
        if (rsBlob) rsBlob->Release();
        if (rsErr) rsErr->Release();
        rsBlob = nullptr;
        rsErr = nullptr;
    }
    if (SUCCEEDED(hr)) hr = CompileOne(kAsyncVS, "vs_5_0", &vsBlob);
    if (SUCCEEDED(hr)) hr = CompileOne(kAsyncPS, "ps_5_0", &psBlob);
    if (SUCCEEDED(hr)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gpd = {};
        // Input layout MUST match the VS inputs (POSITION float3 +
        // COLOR float4, 28 B/vertex): a graphics PSO with a VS that
        // declares inputs and no input layout fails CreateGraphics-
        // PipelineState with E_INVALIDARG.
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
        gpd.InputLayout.pInputElementDescs = elems;
        gpd.InputLayout.NumElements = 2;
        gpd.pRootSignature = g_gfxRootSig;
        gpd.VS.pShaderBytecode = vsBlob->GetBufferPointer();
        gpd.VS.BytecodeLength = vsBlob->GetBufferSize();
        gpd.PS.pShaderBytecode = psBlob->GetBufferPointer();
        gpd.PS.BytecodeLength = psBlob->GetBufferSize();
        gpd.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        gpd.SampleMask = UINT_MAX;
        gpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        gpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gpd.NumRenderTargets = 1;
        gpd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        gpd.SampleDesc.Count = 1;
        hr = device->CreateGraphicsPipelineState(&gpd, IID_PPV_ARGS(&g_gfxPso));
    }
    // Compute output buffer (UAV) + readback buffer + draw target + RTV heap.
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (uint64_t)kComputeCount * sizeof(uint32_t);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                             nullptr, IID_PPV_ARGS(&g_computeOut));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (uint64_t)kComputeCount * sizeof(uint32_t);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&g_readback));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = kRtW;
        rd.Height = kRtH;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET,
                                             nullptr, IID_PPV_ARGS(&g_rt));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap));
        if (SUCCEEDED(hr))
            device->CreateRenderTargetView(
                g_rt, nullptr, g_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    }
    if (csBlob) csBlob->Release();
    if (vsBlob) vsBlob->Release();
    if (psBlob) psBlob->Release();
    if (FAILED(hr)) {
        Log("opt_async", "AsyncInit FAILED hr=0x%08x", (unsigned)hr);
        return hr;
    }
    g_asyncInit = true;
    Log("opt_async", "async compute initialized (count=%u iters=%u)",
        kComputeCount, kComputeIters);
    return S_OK;
}

// Record the compute dispatch into `list` (must be reset/open).
static void RecordCompute(ID3D12GraphicsCommandList* list) {
    list->SetComputeRootSignature(g_csRootSig);
    list->SetPipelineState(g_csPso);
    UINT constants[2] = {kComputeCount, kComputeIters};
    list->SetComputeRoot32BitConstants(0, 2, constants, 0);
    list->SetComputeRootUnorderedAccessView(
        1, g_computeOut->GetGPUVirtualAddress());
    list->Dispatch((kComputeCount + 255) / 256, 1, 1);
}

// Record the triangle draw into `list` (must be reset/open). The vertex
// data is a static upload buffer created once by the caller of this
// function — kept simple: a 3-vertex triangle in an upload heap.
static ID3D12Resource* g_triVb = nullptr;
static HRESULT EnsureTriangleVb(ID3D12Device* device) {
    if (g_triVb) return S_OK;
    static const float kTri[3 * 7] = {
        -0.8f, -0.8f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
        0.8f, -0.8f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.8f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
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
    HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE,
                                                 &rd,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&g_triVb));
    if (SUCCEEDED(hr)) {
        void* mapped = nullptr;
        D3D12_RANGE range = {0, sizeof(kTri)};
        hr = g_triVb->Map(0, &range, &mapped);
        if (SUCCEEDED(hr) && mapped) {
            memcpy(mapped, kTri, sizeof(kTri));
            D3D12_RANGE empty = {0, 0};
            g_triVb->Unmap(0, &empty);
        }
    }
    return hr;
}

static void RecordDraw(ID3D12GraphicsCommandList* list) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const float clear[4] = {0.2f, 0.4f, 0.8f, 1.0f};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = {0.0f, 0.0f, (float)kRtW, (float)kRtH, 0.0f, 1.0f};
    D3D12_RECT sc = {0, 0, (LONG)kRtW, (LONG)kRtH};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
    list->SetGraphicsRootSignature(g_gfxRootSig);
    list->SetPipelineState(g_gfxPso);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    vbv.BufferLocation = g_triVb->GetGPUVirtualAddress();
    vbv.SizeInBytes = 3 * 7 * sizeof(float);
    vbv.StrideInBytes = 7 * sizeof(float);
    list->IASetVertexBuffers(0, 1, &vbv);
    list->DrawInstanced(3, 1, 0, 0);
}

static HRESULT WaitFence(ID3D12Fence* fence, UINT64 value) {
    fence->SetEventOnCompletion(value, g_fenceEvent);
    return WaitForSingleObject(g_fenceEvent, 30000) == WAIT_OBJECT_0 ? S_OK
                                                                    : E_FAIL;
}

extern "C" HRESULT ScOpt_AsyncRunTimed(ID3D12Device* device,
                                       ScOptAsyncMode mode, double* avgMs) {
    if (!g_asyncInit || !device || !avgMs) return E_INVALIDARG;
    if (mode != SC_OPT_ASYNC_SERIAL && mode != SC_OPT_ASYNC_ASYNC)
        return E_INVALIDARG;
    HRESULT hr = EnsureTriangleVb(device);
    if (FAILED(hr)) return hr;
    double total = 0.0;
    for (int iter = 0; iter < kTimedIters; iter++) {
        ScOptTimer t;
        t.Start();
        if (mode == SC_OPT_ASYNC_SERIAL) {
            hr = g_directAlloc->Reset();
            if (SUCCEEDED(hr)) hr = g_directList->Reset(g_directAlloc, nullptr);
            if (SUCCEEDED(hr)) {
                RecordCompute(g_directList);
                RecordDraw(g_directList);
                hr = g_directList->Close();
            }
            if (SUCCEEDED(hr)) {
                ID3D12CommandList* lists[] = {g_directList};
                g_directQ->ExecuteCommandLists(1, lists);
                UINT64 v = ++g_fenceVal;
                g_directQ->Signal(g_directFence, v);
                hr = WaitFence(g_directFence, v);
            }
        } else {
            // Async: compute on COMPUTE, draw on DIRECT, concurrently.
            // Each fence is waited on with the value it was signaled
            // with (the two fences share one counter, so the values
            // must be captured at signal time).
            UINT64 cv = 0, dv = 0;
            hr = g_computeAlloc->Reset();
            if (SUCCEEDED(hr))
                hr = g_computeList->Reset(g_computeAlloc, nullptr);
            if (SUCCEEDED(hr)) {
                RecordCompute(g_computeList);
                hr = g_computeList->Close();
            }
            if (SUCCEEDED(hr)) {
                ID3D12CommandList* lists[] = {g_computeList};
                g_computeQ->ExecuteCommandLists(1, lists);
                cv = ++g_fenceVal;
                g_computeQ->Signal(g_computeFence, cv);
            }
            if (SUCCEEDED(hr)) hr = g_directAlloc->Reset();
            if (SUCCEEDED(hr)) hr = g_directList->Reset(g_directAlloc, nullptr);
            if (SUCCEEDED(hr)) {
                RecordDraw(g_directList);
                hr = g_directList->Close();
            }
            if (SUCCEEDED(hr)) {
                ID3D12CommandList* lists[] = {g_directList};
                g_directQ->ExecuteCommandLists(1, lists);
                dv = ++g_fenceVal;
                g_directQ->Signal(g_directFence, dv);
                hr = WaitFence(g_directFence, dv);
            }
            if (SUCCEEDED(hr)) hr = WaitFence(g_computeFence, cv);
        }
        if (FAILED(hr)) {
            Log("opt_async", "RunTimed iter %d FAILED hr=0x%08x", iter,
                (unsigned)hr);
            return hr;
        }
        total += t.ElapsedMs();
    }
    *avgMs = total / kTimedIters;
    // Readback + checksum AFTER the timed region (correctness, not timing).
    {
        HRESULT rhr = g_directAlloc->Reset();
        if (SUCCEEDED(rhr)) rhr = g_directList->Reset(g_directAlloc, nullptr);
        if (SUCCEEDED(rhr)) {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = g_computeOut;
            b.Transition.StateBefore =
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g_directList->ResourceBarrier(1, &b);
            g_directList->CopyResource(g_readback, g_computeOut);
            D3D12_RESOURCE_BARRIER b2 = {};
            b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b2.Transition.pResource = g_computeOut;
            b2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b2.Transition.StateAfter =
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b2.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g_directList->ResourceBarrier(1, &b2);
            rhr = g_directList->Close();
        }
        if (SUCCEEDED(rhr)) {
            ID3D12CommandList* lists[] = {g_directList};
            g_directQ->ExecuteCommandLists(1, lists);
            UINT64 v = ++g_fenceVal;
            g_directQ->Signal(g_directFence, v);
            rhr = WaitFence(g_directFence, v);
        }
        if (SUCCEEDED(rhr)) {
            void* mapped = nullptr;
            D3D12_RANGE range = {0, (size_t)kComputeCount * sizeof(uint32_t)};
            rhr = g_readback->Map(0, &range, &mapped);
            if (SUCCEEDED(rhr) && mapped) {
                g_checksum[mode] =
                    ScOptFnv1a(mapped, (size_t)kComputeCount * sizeof(uint32_t));
                D3D12_RANGE empty = {0, 0};
                g_readback->Unmap(0, &empty);
            }
        }
        if (FAILED(rhr))
            Log("opt_async", "checksum readback FAILED hr=0x%08x",
                (unsigned)rhr);
    }
    Log("opt_async", "RunTimed mode=%s avg=%.3f ms checksum=0x%016llx",
        mode == SC_OPT_ASYNC_SERIAL ? "serial" : "async", *avgMs,
        (unsigned long long)g_checksum[mode]);
    return S_OK;
}

extern "C" HRESULT ScOpt_AsyncChecksum(ScOptAsyncMode mode,
                                       uint64_t* checksum) {
    if (!checksum) return E_INVALIDARG;
    if (mode != SC_OPT_ASYNC_SERIAL && mode != SC_OPT_ASYNC_ASYNC)
        return E_INVALIDARG;
    *checksum = g_checksum[mode];
    return S_OK;
}

extern "C" void ScOpt_AsyncDestroy() {
    if (!g_asyncInit) return;
    if (g_fenceEvent) CloseHandle(g_fenceEvent);
    if (g_directFence) g_directFence->Release();
    if (g_computeFence) g_computeFence->Release();
    if (g_directList) g_directList->Release();
    if (g_computeList) g_computeList->Release();
    if (g_directAlloc) g_directAlloc->Release();
    if (g_computeAlloc) g_computeAlloc->Release();
    if (g_directQ) g_directQ->Release();
    if (g_computeQ) g_computeQ->Release();
    if (g_csRootSig) g_csRootSig->Release();
    if (g_csPso) g_csPso->Release();
    if (g_gfxRootSig) g_gfxRootSig->Release();
    if (g_gfxPso) g_gfxPso->Release();
    if (g_computeOut) g_computeOut->Release();
    if (g_readback) g_readback->Release();
    if (g_rt) g_rt->Release();
    if (g_rtvHeap) g_rtvHeap->Release();
    if (g_triVb) g_triVb->Release();
    g_directQ = g_computeQ = nullptr;
    g_directAlloc = g_computeAlloc = nullptr;
    g_directList = g_computeList = nullptr;
    g_directFence = g_computeFence = nullptr;
    g_fenceEvent = nullptr;
    g_csRootSig = g_gfxRootSig = nullptr;
    g_csPso = g_gfxPso = nullptr;
    g_computeOut = g_readback = g_rt = nullptr;
    g_rtvHeap = nullptr;
    g_triVb = nullptr;
    g_asyncInit = false;
}
