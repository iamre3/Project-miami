// software_rt_test.cpp — HOST-SIDE test for the software ray tracing
// backend (src/software_rt.cpp). NOT an Xbox guest: a normal Win32 exe
// that links the backend directly and proves it on the real GPU (RX 5700
// XT, no RT cores) without the loader. The layer guest version is
// tests/guest_software_rt.cpp; this test proves the backend itself first.
//
// Scene: one red triangle (same geometry as the proof guest's triangle),
// camera looking straight at it. Expected: center pixel red
// (255,0,0,255), corner pixel miss clear color (51,102,204,255).
// Exit 0 on PASS, 1 on FAIL.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <d3d12.h>
#include "software_rt.h"

static const uint32_t kW = 256, kH = 256;

int main() {
    printf("[srt-test] software ray tracing host test\n");
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
                                   IID_PPV_ARGS(&dev));
    if (FAILED(hr) || !dev) {
        printf("[srt-test] D3D12CreateDevice hr=0x%08x FAIL\n", (unsigned)hr);
        return 1;
    }
    printf("[srt-test] device created\n");

    ScRtPath path = SC_RT_PATH_SOFTWARE;
    uint32_t tier = 0;
    hr = ScSoftwareRT_QueryPath(dev, &path, &tier);
    printf("[srt-test] QueryPath hr=0x%08x rtTier=%u path=%s\n", (unsigned)hr,
           tier, path == SC_RT_PATH_SOFTWARE ? "software" : "hardware");
    if (FAILED(hr) || path != SC_RT_PATH_SOFTWARE || tier != 0) {
        printf("[srt-test] expected software path with tier 0 on this card "
               "FAIL\n");
        return 1;
    }

    hr = ScSoftwareRT_Init(dev);
    printf("[srt-test] Init hr=0x%08x\n", (unsigned)hr);
    if (FAILED(hr)) return 1;

    static const ScRtTriangle kTri[1] = {{
        {-0.8f, -0.8f, 0.0f},
        {0.8f, -0.8f, 0.0f},
        {0.0f, 0.8f, 0.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
    }};
    ScSoftwareRTSceneHandle scene = nullptr;
    hr = ScSoftwareRT_BuildScene(dev, kTri, 1, &scene);
    printf("[srt-test] BuildScene hr=0x%08x scene=%p\n", (unsigned)hr, scene);
    if (FAILED(hr) || !scene) return 1;

    // Output texture (UAV) + compute queue/list/fence.
    ID3D12Resource* outTex = nullptr;
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = kW;
        rd.Height = kH;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&outTex));
    }
    if (FAILED(hr) || !outTex) {
        printf("[srt-test] output texture hr=0x%08x FAIL\n", (unsigned)hr);
        return 1;
    }
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEv = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceVal = 0;
    {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        if (SUCCEEDED(hr)) hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
        if (SUCCEEDED(hr))
            hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                             IID_PPV_ARGS(&alloc));
        if (SUCCEEDED(hr))
            hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                        alloc, nullptr, IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr)) hr = list->Close();
        if (SUCCEEDED(hr))
            hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    }
    if (FAILED(hr) || !fenceEv) {
        printf("[srt-test] compute setup hr=0x%08x FAIL\n", (unsigned)hr);
        return 1;
    }

    ScRtCamera cam = {};
    cam.origin[0] = 0.0f; cam.origin[1] = 0.0f; cam.origin[2] = 2.0f;
    cam.lowerLeft[0] = -1.0f; cam.lowerLeft[1] = 1.0f; cam.lowerLeft[2] = 0.0f;
    cam.horizontal[0] = 2.0f; cam.horizontal[1] = 0.0f; cam.horizontal[2] = 0.0f;
    cam.vertical[0] = 0.0f; cam.vertical[1] = -2.0f; cam.vertical[2] = 0.0f;

    // Readback buffer for the output texture.
    D3D12_RESOURCE_DESC texDesc = outTex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSize = 0, totalBytes = 0;
    dev->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows,
                               &rowSize, &totalBytes);
    ID3D12Resource* readback = nullptr;
    {
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
        hr = dev->CreateCommittedResource(&hpRb, D3D12_HEAP_FLAG_NONE, &rbDesc,
                                          D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&readback));
    }
    if (FAILED(hr) || !readback) {
        printf("[srt-test] readback alloc hr=0x%08x FAIL\n", (unsigned)hr);
        return 1;
    }

    // Record: dispatch rays into the UAV, then copy to the readback buffer.
    hr = alloc->Reset();
    if (SUCCEEDED(hr)) hr = list->Reset(alloc, nullptr);
    if (SUCCEEDED(hr)) {
        hr = ScSoftwareRT_DispatchRays(dev, list, scene, &cam, outTex, kW, kH);
    }
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = outTex;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = outTex;
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
        if (WaitForSingleObject(fenceEv, 30000) != WAIT_OBJECT_0) hr = E_FAIL;
    }
    if (FAILED(hr)) {
        printf("[srt-test] dispatch+copy hr=0x%08x FAIL\n", (unsigned)hr);
        return 1;
    }

    bool ok = false;
    void* mapped = nullptr;
    D3D12_RANGE range = {0, totalBytes};
    hr = readback->Map(0, &range, &mapped);
    if (SUCCEEDED(hr) && mapped) {
        const uint8_t* base = (const uint8_t*)mapped;
        auto pxAt = [&](int x, int y) -> const uint8_t* {
            return base + footprint.Offset + (uint64_t)y * footprint.Footprint.RowPitch + x * 4;
        };
        const uint8_t* c = pxAt(128, 128);
        const uint8_t* k = pxAt(10, 10);
        bool centerRed = (abs((int)c[0] - 255) <= 3) && (abs((int)c[1] - 0) <= 3) &&
                         (abs((int)c[2] - 0) <= 3) && (abs((int)c[3] - 255) <= 3);
        bool cornerClear = (abs((int)k[0] - 51) <= 3) && (abs((int)k[1] - 102) <= 3) &&
                           (abs((int)k[2] - 204) <= 3) && (abs((int)k[3] - 255) <= 3);
        ok = centerRed && cornerClear;
        printf("[srt-test] center (%d,%d,%d,%d) want (255,0,0,255); corner "
               "(%d,%d,%d,%d) want (51,102,204,255)\n",
               c[0], c[1], c[2], c[3], k[0], k[1], k[2], k[3]);
        D3D12_RANGE empty = {0, 0};
        readback->Unmap(0, &empty);
    } else {
        printf("[srt-test] readback Map hr=0x%08x FAIL\n", (unsigned)hr);
    }

    ScSoftwareRT_FreeScene(scene);
    if (readback) readback->Release();
    if (outTex) outTex->Release();
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (q) q->Release();
    if (fenceEv) CloseHandle(fenceEv);
    if (dev) dev->Release();
    printf("[srt-test] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
