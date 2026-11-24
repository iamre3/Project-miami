// guest_proof.cpp — realistic proof guest for the ScarlettCompat harness.
// A full Game Core title boot in miniature: the Xbox-style PE (image base
// 0x180000000, .xbld section) imports the FULL set of Xbox DLLs a real
// title imports (xgameruntime, xmem, d3d12_xs, XAudio2_9, dstorage_xs,
// gameinput, plus the XUser / XGameSave / XPackage / XAsync surface) and
// calls the boot sequence in real order:
//   XGameRuntimeInitialize -> profile sign-in -> profile save load ->
//   package/DLC enumeration -> XMemAlloc -> D3D12XboxCreateDevice ->
//   device surface (queues/lists/resources/heaps/root signature) ->
//   audio + input init -> REAL rendered scene (triangle, PSO + HLSL
//   shaders compiled at runtime, pixel-verified via readback) ->
//   60-frame loop (render + fence + Present + frame pacing, timing
//   stats) -> XGameSaveFiles -> package mount.
// The boot SHAPE mirrors an Xbox One title's boot (an Xbox One title
// as the reference): a title signs the profile in first, because the
// profile drives the save; it loads the profile save early, before the
// renderer is up; it enumerates related packages for DLC car packs;
// audio and input init before the main loop, which then runs at 60 Hz.
// An Xbox One title is an ERA title, NOT a Game Core title — nothing
// from a real title is loaded or run here, and no ERA support is
// added. Every call below is translated into its GDK / Game Core
// equivalent, so the guest stays a Game Core title; the Xbox One title
// only sets the order and what a real title touches at boot.
// Every stage reports as proof_<stage> in guest_report.txt; the harness
// prints a dedicated proof report. This file contains NO game code — it
// is a test program written for the layer, and it is never run through
// the Windows loader directly (its Xbox imports only resolve inside the
// layer).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <d3d12.h>
#include <dxgi1_4.h>  // IDXGIFactory2 + IDXGISwapChain3 (GetCurrentBackBufferIndex)
#include <d3dcompiler.h>  // D3DCompile (runtime HLSL compile, system DLL)
#include <comdef.h>
#include "xbox_shims.h"

// Xbox build-info section, like the shipped DLLs carry.
#pragma section(".xbld", read)
__declspec(allocate(".xbld")) static const char kXbld[] =
    "_xbld_edition_name=GXDK,October 2026 GXDK\0"
    "_xbld_edition_full_productbuild=GXDK,10.0.26100.9605\0"
    "_xbld_edition_sdktype=GXDK,261000\0"
    "_xbld_edition_mscver=GXDK,195237025.0\0"
    "_xbld_edition_build=GXDK,0x65F42585\0";

// TLS test: a __declspec(thread) variable and a .CRT$XLB callback. The
// loader must allocate a TLS slot + block, copy the template (init 42),
// and run the callback before the entry point.
static __declspec(thread) int g_tlsVar = 42;
static bool g_tlsCallbackRan = false;
static void NTAPI TlsCallback(PVOID base, DWORD reason, PVOID reserved) {
    (void)base;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) g_tlsCallbackRan = true;
}
#pragma section(".CRT$XLB", read)
__declspec(allocate(".CRT$XLB")) static PIMAGE_TLS_CALLBACK g_tlsCb =
    TlsCallback;

// TLS on worker threads: a second __declspec(thread) variable, untouched
// by the main-thread TLS test, so the worker thread's fresh template
// copy (77) can be told apart from the main thread's value.
static __declspec(thread) int g_tlsThreadVar = 77;
static volatile LONG g_workerSaw = -1;
static volatile LONG g_workerSet = 0;
static DWORD WINAPI WorkerThread(LPVOID param) {
    (void)param;
    g_workerSaw = g_tlsThreadVar;  // expect 77 (fresh template copy)
    g_tlsThreadVar = 88;
    g_workerSet = g_tlsThreadVar;  // 88
    return 0;
}

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
    if (strncmp(name, "proof", 5) == 0) {
        g_stagesTotal++;
        if (pass) g_stagesPassed++;
    }
}

static bool g_userCallbackRan = false;
static void CALLBACK UserAddCallback(XAsyncBlock* async) {
    (void)async;
    g_userCallbackRan = true;
}

static bool g_genericCallbackRan = false;
static void CALLBACK GenericCallback(XAsyncBlock* async) {
    (void)async;
    g_genericCallbackRan = true;
}

static bool CALLBACK EnumPackagesCallback(void* context,
                                          const XPackageDetails* details) {
    int* count = (int*)context;
    (*count)++;
    if (strcmp(details->packageIdentifier,
               "ScarlettCompat-Guest-00000001") != 0)
        return false;
    if (details->kind != XPackageKind_Game) return false;
    if (strcmp(details->titleID, "00000001") != 0) return false;
    return true;  // continue enumerating
}

// ---- stage: package check (real titles check packaged state early) ----
static bool TestPackageEarly(char* detail, size_t n) {
    BOOL packaged = XPackageIsPackagedProcess();
    if (!packaged) {
        snprintf(detail, n, "XPackageIsPackagedProcess returned FALSE");
        return false;
    }
    uint32_t mountSize = 0;
    HRESULT hr = XPackageGetMountPathSize(&mountSize);
    if (FAILED(hr) || mountSize == 0) {
        snprintf(detail, n, "XPackageGetMountPathSize hr=0x%08x",
                 (unsigned)hr);
        return false;
    }
    wchar_t mountPath[MAX_PATH] = {};
    uint32_t mountCap = MAX_PATH;
    hr = XPackageGetMountPath(mountPath, &mountCap);
    if (FAILED(hr) || mountCap == 0 || mountPath[0] == 0) {
        snprintf(detail, n, "XPackageGetMountPath hr=0x%08x", (unsigned)hr);
        return false;
    }
    char pkgId[XPACKAGE_IDENTIFIER_MAX_LENGTH] = {};
    hr = XPackageGetCurrentProcessPackageIdentifier(sizeof(pkgId), pkgId);
    if (FAILED(hr) || pkgId[0] == 0) {
        snprintf(detail, n,
                 "XPackageGetCurrentProcessPackageIdentifier hr=0x%08x",
                 (unsigned)hr);
        return false;
    }
    int enumCount = 0;
    hr = XPackageEnumeratePackages(XPackageKind_Game,
                                   XPackageEnumerationScope_ThisOnly,
                                   &enumCount, EnumPackagesCallback);
    if (FAILED(hr) || enumCount != 1) {
        snprintf(detail, n,
                 "XPackageEnumeratePackages hr=0x%08x count=%d",
                 (unsigned)hr, enumCount);
        return false;
    }
    // Xbox One title shape: enumerate related packages too (DLC car
    // packs live there). The layer has one synthetic package for every
    // scope.
    int enumRelated = 0;
    hr = XPackageEnumeratePackages(XPackageKind_Game,
                                   XPackageEnumerationScope_ThisAndRelated,
                                   &enumRelated, EnumPackagesCallback);
    if (FAILED(hr) || enumRelated != 1) {
        snprintf(detail, n,
                 "XPackageEnumeratePackages(ThisAndRelated) hr=0x%08x "
                 "count=%d",
                 (unsigned)hr, enumRelated);
        return false;
    }
    // Content kind has no packages in this layer.
    int enumContent = 0;
    hr = XPackageEnumeratePackages(XPackageKind_Content,
                                   XPackageEnumerationScope_ThisOnly,
                                   &enumContent, EnumPackagesCallback);
    if (FAILED(hr) || enumContent != 0) {
        snprintf(detail, n,
                 "XPackageEnumeratePackages(Content) hr=0x%08x count=%d",
                 (unsigned)hr, enumContent);
        return false;
    }
    snprintf(detail, n,
             "packaged=1 mount=%lu chars pkg=\"%s\" enum=1 related=1 "
             "content=0",
             mountCap, pkgId);
    return true;
}

// ---- stage: user sign-in (silent default user) ----
static XUserHandle g_user = nullptr;
static bool TestUser(char* detail, size_t n) {
    XAsyncBlock async = {};
    async.callback = UserAddCallback;
    g_userCallbackRan = false;
    HRESULT hr = XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &async);
    if (FAILED(hr) || !g_userCallbackRan) {
        snprintf(detail, n, "XUserAddAsync hr=0x%08x cb=%d",
                 (unsigned)hr, (int)g_userCallbackRan);
        return false;
    }
    hr = XAsyncGetStatus(&async, FALSE);
    if (FAILED(hr)) {
        snprintf(detail, n, "XAsyncGetStatus hr=0x%08x", (unsigned)hr);
        return false;
    }
    hr = XUserAddResult(&async, &g_user);
    if (FAILED(hr) || !g_user) {
        snprintf(detail, n, "XUserAddResult hr=0x%08x", (unsigned)hr);
        return false;
    }
    uint64_t xuid = 0;
    hr = XUserGetId(g_user, &xuid);
    if (FAILED(hr) || xuid == 0) {
        snprintf(detail, n, "XUserGetId hr=0x%08x", (unsigned)hr);
        return false;
    }
    char tag[32] = {};
    size_t tagSize = sizeof(tag);
    hr = XUserGetGamertag(g_user, XUserGamertagOptions_UniqueModern, tag,
                          &tagSize);
    if (FAILED(hr) || strcmp(tag, "Player") != 0) {
        snprintf(detail, n, "XUserGetGamertag hr=0x%08x tag=%s",
                 (unsigned)hr, tag);
        return false;
    }
    XUserLocalId localId = {};
    hr = XUserGetLocalId(g_user, &localId);
    if (FAILED(hr)) {
        snprintf(detail, n, "XUserGetLocalId hr=0x%08x", (unsigned)hr);
        return false;
    }
    snprintf(detail, n, "xuid=0x%llx tag=%s local=%llu",
             (unsigned long long)xuid, tag,
             (unsigned long long)localId.value);
    return true;
}

// ---- stage: memory (several sizes, incl. a 1 MB block) ----
static bool TestMemory(char* detail, size_t n) {
    void* a = XMemAlloc(1024, 0);
    void* b = XMemAlloc(65536, 0);
    void* c = XMemAlloc(1024 * 1024, 0);
    bool ok = a && b && c;
    if (ok) {
        // Prove the blocks are really writable.
        memset(a, 0xAB, 1024);
        memset(b, 0xCD, 65536);
        memset(c, 0xEF, 1024 * 1024);
        ok = ((uint8_t*)a)[1023] == 0xAB && ((uint8_t*)b)[65535] == 0xCD &&
             ((uint8_t*)c)[1024 * 1024 - 1] == 0xEF;
    }
    if (a) XMemFree(a);
    if (b) XMemFree(b);
    if (c) XMemFree(c);
    snprintf(detail, n, ok ? "1KB + 64KB + 1MB alloc/write/free ok"
                           : "alloc returned NULL or writeback failed");
    return ok;
}

// ---- stage: d3d12_xs device-method surface (queues, lists, resources,
// heaps, root signature, feature query, GPU copy round-trip) ----
static bool TestXsSurface(ID3D12Device* dev, char* detail, size_t n) {
    HRESULT hr = S_OK;
    ID3D12CommandQueue* qDirect = nullptr;
    ID3D12CommandQueue* qCompute = nullptr;
    ID3D12CommandQueue* qCopy = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEv = nullptr;
    ID3D12Resource* bufDefault = nullptr;
    ID3D12Resource* bufUpload = nullptr;
    ID3D12Resource* bufReadback = nullptr;
    ID3D12DescriptorHeap* cbvHeap = nullptr;
    ID3D12RootSignature* rootSig = nullptr;
    ID3D12Heap* heap = nullptr;
    ID3D12Resource* placed = nullptr;
    UINT64 fenceVal = 0;
    int queues = 0, buffers = 0;
    bool copyOk = false, rootSigOk = false, heapOk = false;
    uint32_t bindingTier = 0;

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (SUCCEEDED(hr)) hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&qDirect));
    if (SUCCEEDED(hr)) { queues++; qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE; hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&qCompute)); }
    if (SUCCEEDED(hr)) { queues++; qd.Type = D3D12_COMMAND_LIST_TYPE_COPY; hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&qCopy)); }
    if (SUCCEEDED(hr)) queues++;
    if (SUCCEEDED(hr)) hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(hr)) hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (SUCCEEDED(hr)) {
        fenceEv = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEv) hr = E_FAIL;
    }
    auto makeBuffer = [&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                          ID3D12Resource** out) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = type;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 256;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            state, nullptr,
                                            IID_PPV_ARGS(out));
    };
    if (SUCCEEDED(hr)) hr = makeBuffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, &bufDefault);
    if (SUCCEEDED(hr)) { buffers++; hr = makeBuffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, &bufUpload); }
    if (SUCCEEDED(hr)) { buffers++; hr = makeBuffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, &bufReadback); }
    if (SUCCEEDED(hr)) buffers++;
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 1;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&cbvHeap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {};
        cbv.BufferLocation = bufUpload->GetGPUVirtualAddress();
        cbv.SizeInBytes = 256;
        dev->CreateConstantBufferView(&cbv, cbvHeap->GetCPUDescriptorHandleForHeapStart());
    }
    if (SUCCEEDED(hr)) {
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob* blob = nullptr;
        ID3DBlob* errBlob = nullptr;
        hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errBlob);
        if (SUCCEEDED(hr)) {
            hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig));
            blob->Release();
        }
        if (errBlob) errBlob->Release();
        rootSigOk = SUCCEEDED(hr) && rootSig != nullptr;
    }
    if (SUCCEEDED(hr)) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS opts = {};
        hr = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts));
        if (SUCCEEDED(hr)) bindingTier = opts.ResourceBindingTier;
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_DESC hd = {};
        hd.SizeInBytes = 64 * 1024;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        hr = dev->CreateHeap(&hd, IID_PPV_ARGS(&heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 256;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = dev->CreatePlacedResource(heap, 0, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&placed));
        heapOk = SUCCEEDED(hr) && placed != nullptr;
    }
    if (SUCCEEDED(hr)) {
        void* mapped = nullptr;
        D3D12_RANGE wr = {0, 0};
        hr = bufUpload->Map(0, &wr, &mapped);
        if (SUCCEEDED(hr) && mapped) {
            uint8_t* p = (uint8_t*)mapped;
            for (int i = 0; i < 256; ++i) p[i] = (uint8_t)(i ^ 0x5A);
            D3D12_RANGE empty = {0, 0};
            bufUpload->Unmap(0, &empty);
        }
    }
    if (SUCCEEDED(hr)) {
        list->CopyBufferRegion(bufDefault, 0, bufUpload, 0, 256);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = bufDefault;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(bufReadback, 0, bufDefault, 0, 256);
        hr = list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        qDirect->ExecuteCommandLists(1, lists);
        qDirect->Signal(fence, ++fenceVal);
        fence->SetEventOnCompletion(fenceVal, fenceEv);
        if (WaitForSingleObject(fenceEv, 10000) != WAIT_OBJECT_0) hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) {
        void* mapped = nullptr;
        D3D12_RANGE rd = {0, 256};
        hr = bufReadback->Map(0, &rd, &mapped);
        if (SUCCEEDED(hr) && mapped) {
            const uint8_t* p = (const uint8_t*)mapped;
            copyOk = true;
            for (int i = 0; i < 256; ++i)
                if (p[i] != (uint8_t)(i ^ 0x5A)) { copyOk = false; break; }
            D3D12_RANGE empty = {0, 0};
            bufReadback->Unmap(0, &empty);
        }
    }
    bool ok = SUCCEEDED(hr) && copyOk && rootSigOk && heapOk &&
              queues == 3 && buffers == 3;
    snprintf(detail, n,
             "queues=%d buffers=%d copy=%d rootSig=%d heap+placed=%d "
             "bindingTier=%u hr=0x%08x",
             queues, buffers, (int)copyOk, (int)rootSigOk, (int)heapOk,
             bindingTier, (unsigned)hr);
    if (fenceEv) CloseHandle(fenceEv);
    if (fence) fence->Release();
    if (placed) placed->Release();
    if (heap) heap->Release();
    if (rootSig) rootSig->Release();
    if (cbvHeap) cbvHeap->Release();
    if (bufReadback) bufReadback->Release();
    if (bufUpload) bufUpload->Release();
    if (bufDefault) bufDefault->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (qCopy) qCopy->Release();
    if (qCompute) qCompute->Release();
    if (qDirect) qDirect->Release();
    return ok;
}

// ---- frame context: everything the scene + frame loop share ----
struct FrameCtx {
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEv = nullptr;
    UINT64 fenceVal = 0;
    ID3D12RootSignature* rootSig = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12Resource* vb = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    UINT rtvSize = 0;
    ID3D12Resource* offRT = nullptr;      // offscreen render target
    IDXGISwapChain3* sc = nullptr;      // null when no compositor
    bool hasSwapchain = false;
    FLOAT clearColor[4] = {0.2f, 0.4f, 0.8f, 1.0f};
};

static const char* kTriangleHLSL =
    "struct VSIn { float3 pos : POSITION; float4 col : COLOR; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
    "VSOut VS(VSIn i) { VSOut o; o.pos = float4(i.pos, 1.0); o.col = i.col; return o; }\n"
    "float4 PS(VSOut i) : SV_Target { return i.col; }\n";

struct TriVertex { float x, y, z, r, g, b, a; };

static bool CompileShader(const char* entry, const char* target,
                          ID3DBlob** out, char* errDetail, size_t n) {
    ID3DBlob* errBlob = nullptr;
    HRESULT hr = D3DCompile(kTriangleHLSL, strlen(kTriangleHLSL), nullptr,
                            nullptr, nullptr, entry, target, 0, 0, out,
                            &errBlob);
    if (FAILED(hr)) {
        const char* msg = errBlob && errBlob->GetBufferPointer()
                              ? (const char*)errBlob->GetBufferPointer()
                              : "(no error text)";
        snprintf(errDetail, n, "D3DCompile %s hr=0x%08x: %.120s", entry,
                 (unsigned)hr, msg);
        if (errBlob) errBlob->Release();
        return false;
    }
    if (errBlob) errBlob->Release();
    return true;
}

// Build the frame context: queue, allocator, list, fence, RTV heap,
// offscreen 640x360 RT, vertex buffer, root signature, PSO, and a
// best-effort swapchain (fails without a compositor — environmental).
HWND g_proofHwnd = nullptr;  // kept alive while the swapchain lives
static bool BuildFrameCtx(FrameCtx* ctx, char* detail, size_t n) {
    HRESULT hr = S_OK;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (SUCCEEDED(hr)) hr = ctx->dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&ctx->q));
    if (SUCCEEDED(hr)) hr = ctx->dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ctx->alloc));
    if (SUCCEEDED(hr)) hr = ctx->dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ctx->alloc, nullptr, IID_PPV_ARGS(&ctx->list));
    // A fresh command list starts in the recording (open) state, and an
    // allocator cannot be Reset while any of its lists is open — so close
    // it once here; every DrawTriangle then does alloc->Reset() legally.
    if (SUCCEEDED(hr)) hr = ctx->list->Close();
    if (SUCCEEDED(hr)) hr = ctx->dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ctx->fence));
    if (SUCCEEDED(hr)) {
        ctx->fenceEv = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!ctx->fenceEv) hr = E_FAIL;
    }
    if (FAILED(hr)) {
        snprintf(detail, n, "frame ctx setup hr=0x%08x", (unsigned)hr);
        return false;
    }
    // RTV heap: 1 descriptor for the offscreen RT + 2 for swapchain buffers.
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 3;
        hr = ctx->dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&ctx->rtvHeap));
        if (FAILED(hr)) {
            snprintf(detail, n, "RTV heap hr=0x%08x", (unsigned)hr);
            return false;
        }
        ctx->rtvSize = ctx->dev->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    // Offscreen RT + RTV (descriptor 0).
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = 640;
        rd.Height = 360;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv = {};
        cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = ctx->dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&ctx->offRT));
        if (FAILED(hr)) {
            snprintf(detail, n, "offscreen RT hr=0x%08x", (unsigned)hr);
            return false;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h =
            ctx->rtvHeap->GetCPUDescriptorHandleForHeapStart();
        ctx->dev->CreateRenderTargetView(ctx->offRT, nullptr, h);
    }
    // Vertex buffer (upload heap): one red triangle, apex top-center so
    // the frame center (320,180) is inside it and the corners are not.
    static const TriVertex kTri[3] = {
        {-0.8f, -0.8f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
        { 0.8f, -0.8f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
        { 0.0f,  0.8f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    };
    {
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
        hr = ctx->dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ctx->vb));
        if (FAILED(hr)) {
            snprintf(detail, n, "vertex buffer hr=0x%08x", (unsigned)hr);
            return false;
        }
        void* mapped = nullptr;
        D3D12_RANGE wr = {0, 0};
        hr = ctx->vb->Map(0, &wr, &mapped);
        if (SUCCEEDED(hr) && mapped) {
            memcpy(mapped, kTri, sizeof(kTri));
            D3D12_RANGE empty = {0, 0};
            ctx->vb->Unmap(0, &empty);
        }
        if (FAILED(hr)) {
            snprintf(detail, n, "vertex buffer Map hr=0x%08x", (unsigned)hr);
            return false;
        }
        ctx->vbv.BufferLocation = ctx->vb->GetGPUVirtualAddress();
        ctx->vbv.StrideInBytes = sizeof(TriVertex);
        ctx->vbv.SizeInBytes = sizeof(kTri);
    }
    // Root signature (empty) + PSO with runtime-compiled HLSL.
    {
        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        char cerr[256] = {};
        if (!CompileShader("VS", "vs_5_0", &vsBlob, cerr, sizeof(cerr))) {
            snprintf(detail, n, "VS compile failed: %s", cerr);
            return false;
        }
        if (!CompileShader("PS", "ps_5_0", &psBlob, cerr, sizeof(cerr))) {
            snprintf(detail, n, "PS compile failed: %s", cerr);
            vsBlob->Release();
            return false;
        }
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob* rsBlob = nullptr;
        ID3DBlob* rsErr = nullptr;
        hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                         &rsBlob, &rsErr);
        if (SUCCEEDED(hr))
            hr = ctx->dev->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                               rsBlob->GetBufferSize(),
                                               IID_PPV_ARGS(&ctx->rootSig));
        if (rsBlob) rsBlob->Release();
        if (rsErr) rsErr->Release();
        if (FAILED(hr)) {
            snprintf(detail, n, "root signature hr=0x%08x", (unsigned)hr);
            vsBlob->Release();
            psBlob->Release();
            return false;
        }
        D3D12_INPUT_ELEMENT_DESC elems[2] = {};
        elems[0].SemanticName = "POSITION";
        elems[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
        elems[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        elems[1].SemanticName = "COLOR";
        elems[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        elems[1].AlignedByteOffset = 12;
        elems[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = ctx->rootSig;
        pso.VS.pShaderBytecode = vsBlob->GetBufferPointer();
        pso.VS.BytecodeLength = vsBlob->GetBufferSize();
        pso.PS.pShaderBytecode = psBlob->GetBufferPointer();
        pso.PS.BytecodeLength = psBlob->GetBufferSize();
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso.SampleDesc.Count = 1;
        pso.InputLayout.pInputElementDescs = elems;
        pso.InputLayout.NumElements = 2;
        hr = ctx->dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&ctx->pso));
        vsBlob->Release();
        psBlob->Release();
        if (FAILED(hr)) {
            snprintf(detail, n, "CreateGraphicsPipelineState hr=0x%08x",
                     (unsigned)hr);
            return false;
        }
    }
    // Best-effort swapchain (descriptors 1+2 hold the back-buffer RTVs).
    // Without a compositor this fails (0x887a0022) — environmental, and
    // the frame loop falls back to the offscreen RT.
    {
        const wchar_t* kClass = L"ScarlettProofWnd";
        HINSTANCE hi = GetModuleHandleW(nullptr);
        WNDCLASSW wc = {};
        if (!GetClassInfoW(hi, kClass, &wc)) {
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = hi;
            wc.lpszClassName = kClass;
            RegisterClassW(&wc);
        }
        HWND hwnd = CreateWindowExW(0, kClass, L"ScarlettProof",
                                    WS_OVERLAPPEDWINDOW, 0, 0, 640, 360,
                                    nullptr, nullptr, hi, nullptr);
        if (hwnd) {
            IDXGIFactory2* factory = nullptr;
            HRESULT hr2 = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
            if (SUCCEEDED(hr2)) {
                DXGI_SWAP_CHAIN_DESC1 sd = {};
                sd.Width = 640;
                sd.Height = 360;
                sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                sd.SampleDesc.Count = 1;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.BufferCount = 2;
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                IDXGISwapChain1* sc1 = nullptr;
                hr2 = factory->CreateSwapChainForHwnd(ctx->q, hwnd, &sd,
                                                      nullptr, nullptr,
                                                      &sc1);
                // GetCurrentBackBufferIndex lives on IDXGISwapChain3,
                // not IDXGISwapChain1.
                if (SUCCEEDED(hr2))
                    hr2 = sc1->QueryInterface(IID_PPV_ARGS(&ctx->sc));
                if (sc1) sc1->Release();
                if (SUCCEEDED(hr2)) {
                    ctx->hasSwapchain = true;
                    D3D12_CPU_DESCRIPTOR_HANDLE base =
                        ctx->rtvHeap->GetCPUDescriptorHandleForHeapStart();
                    for (int i = 0; i < 2; ++i) {
                        ID3D12Resource* bb = nullptr;
                        if (SUCCEEDED(ctx->sc->GetBuffer(i, IID_PPV_ARGS(&bb)))) {
                            D3D12_CPU_DESCRIPTOR_HANDLE h = base;
                            h.ptr += (SIZE_T)(i + 1) * ctx->rtvSize;
                            ctx->dev->CreateRenderTargetView(bb, nullptr, h);
                            bb->Release();
                        }
                    }
                }
            }
            if (factory) factory->Release();
            // Keep the window alive for the frame loop while the
            // swapchain lives; without a swapchain there is nothing to
            // present to, so the window goes away.
            if (ctx->hasSwapchain) {
                g_proofHwnd = hwnd;
            } else {
                DestroyWindow(hwnd);
            }
        }
    }
    return true;
}

static void FreeFrameCtx(FrameCtx* ctx) {
    if (g_proofHwnd) { DestroyWindow(g_proofHwnd); g_proofHwnd = nullptr; }
    if (ctx->sc) { ctx->sc->Release(); ctx->sc = nullptr; }
    if (ctx->pso) ctx->pso->Release();
    if (ctx->rootSig) ctx->rootSig->Release();
    if (ctx->vb) ctx->vb->Release();
    if (ctx->offRT) ctx->offRT->Release();
    if (ctx->rtvHeap) ctx->rtvHeap->Release();
    if (ctx->fenceEv) CloseHandle(ctx->fenceEv);
    if (ctx->fence) ctx->fence->Release();
    if (ctx->list) ctx->list->Release();
    if (ctx->alloc) ctx->alloc->Release();
    if (ctx->q) ctx->q->Release();
}

// Record + execute one triangle draw into the given RTV, then fence-wait.
// In swapchain mode the caller passes the current back-buffer RTV and
// the needed barriers; in offscreen mode the RT is already in
// RENDER_TARGET state.
static bool DrawTriangle(FrameCtx* ctx, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                         ID3D12Resource* target, bool isSwapchain,
                         char* detail, size_t n) {
    HRESULT hr = ctx->alloc->Reset();
    if (SUCCEEDED(hr)) hr = ctx->list->Reset(ctx->alloc, ctx->pso);
    if (SUCCEEDED(hr)) {
        if (isSwapchain) {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = target;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ctx->list->ResourceBarrier(1, &b);
        }
        D3D12_VIEWPORT vp = {};
        vp.Width = 640.0f;
        vp.Height = 360.0f;
        vp.MaxDepth = 1.0f;
        D3D12_RECT sr = {0, 0, 640, 360};
        ctx->list->RSSetViewports(1, &vp);
        ctx->list->RSSetScissorRects(1, &sr);
        ctx->list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        ctx->list->ClearRenderTargetView(rtv, ctx->clearColor, 0, nullptr);
        ctx->list->SetGraphicsRootSignature(ctx->rootSig);
        ctx->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->list->IASetVertexBuffers(0, 1, &ctx->vbv);
        ctx->list->DrawInstanced(3, 1, 0, 0);
        if (isSwapchain) {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = target;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ctx->list->ResourceBarrier(1, &b);
        }
        hr = ctx->list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {ctx->list};
        ctx->q->ExecuteCommandLists(1, lists);
        ctx->q->Signal(ctx->fence, ++ctx->fenceVal);
        ctx->fence->SetEventOnCompletion(ctx->fenceVal, ctx->fenceEv);
        if (WaitForSingleObject(ctx->fenceEv, 10000) != WAIT_OBJECT_0)
            hr = E_FAIL;
    }
    if (FAILED(hr)) {
        snprintf(detail, n, "draw+fence hr=0x%08x", (unsigned)hr);
        return false;
    }
    return true;
}

// ---- stage: real rendered scene (triangle, pixel-verified) ----
// Draws the triangle into the offscreen RT, copies it to a readback
// buffer, and checks two pixels: the frame center must be the triangle's
// red (255,0,0,255), a corner must still be the clear color
// (51,102,204,255). That proves the draw actually happened — a clear
// alone would leave the center blue.
static bool TestScene(FrameCtx* ctx, char* detail, size_t n) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        ctx->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    char derr[128] = {};
    if (!DrawTriangle(ctx, rtv, ctx->offRT, false, derr, sizeof(derr))) {
        snprintf(detail, n, "triangle draw failed: %s", derr);
        return false;
    }
    // Readback: copy the RT to a CPU-visible buffer.
    D3D12_RESOURCE_DESC rtDesc = ctx->offRT->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSize = 0, totalBytes = 0;
    ctx->dev->GetCopyableFootprints(&rtDesc, 0, 1, 0, &footprint, &numRows,
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
    HRESULT hr = ctx->dev->CreateCommittedResource(
        &hpRb, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(hr)) {
        snprintf(detail, n, "readback alloc hr=0x%08x", (unsigned)hr);
        return false;
    }
    bool ok = false;
    char pixelDetail[160] = {};
    {
        HRESULT hr2 = ctx->alloc->Reset();
        if (SUCCEEDED(hr2)) hr2 = ctx->list->Reset(ctx->alloc, nullptr);
        if (SUCCEEDED(hr2)) {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = ctx->offRT;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ctx->list->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = ctx->offRT;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            ctx->list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            D3D12_RESOURCE_BARRIER b2 = {};
            b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b2.Transition.pResource = ctx->offRT;
            b2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b2.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ctx->list->ResourceBarrier(1, &b2);
            hr2 = ctx->list->Close();
        }
        if (SUCCEEDED(hr2)) {
            ID3D12CommandList* lists[] = {ctx->list};
            ctx->q->ExecuteCommandLists(1, lists);
            ctx->q->Signal(ctx->fence, ++ctx->fenceVal);
            ctx->fence->SetEventOnCompletion(ctx->fenceVal, ctx->fenceEv);
            if (WaitForSingleObject(ctx->fenceEv, 10000) != WAIT_OBJECT_0)
                hr2 = E_FAIL;
        }
        if (SUCCEEDED(hr2)) {
            void* mapped = nullptr;
            D3D12_RANGE range = {0, totalBytes};
            hr2 = readback->Map(0, &range, &mapped);
            if (SUCCEEDED(hr2) && mapped) {
                const uint8_t* base = (const uint8_t*)mapped;
                auto pxAt = [&](int x, int y) -> const uint8_t* {
                    return base + footprint.Offset +
                           (UINT64)y * footprint.Footprint.RowPitch + x * 4;
                };
                const uint8_t* c = pxAt(320, 180);
                const uint8_t* k = pxAt(10, 10);
                bool centerRed = (abs((int)c[0] - 255) <= 3) &&
                                 (abs((int)c[1] - 0) <= 3) &&
                                 (abs((int)c[2] - 0) <= 3) &&
                                 (abs((int)c[3] - 255) <= 3);
                bool cornerClear = (abs((int)k[0] - 51) <= 3) &&
                                   (abs((int)k[1] - 102) <= 3) &&
                                   (abs((int)k[2] - 204) <= 3) &&
                                   (abs((int)k[3] - 255) <= 3);
                ok = centerRed && cornerClear;
                snprintf(pixelDetail, sizeof(pixelDetail),
                         "center (%d,%d,%d,%d) want (255,0,0,255); corner "
                         "(%d,%d,%d,%d) want (51,102,204,255)",
                         c[0], c[1], c[2], c[3], k[0], k[1], k[2], k[3]);
                D3D12_RANGE empty = {0, 0};
                readback->Unmap(0, &empty);
            } else {
                snprintf(pixelDetail, sizeof(pixelDetail),
                         "Map hr=0x%08x", (unsigned)hr2);
            }
        } else {
            snprintf(pixelDetail, sizeof(pixelDetail),
                     "copy hr=0x%08x", (unsigned)hr2);
        }
    }
    readback->Release();
    snprintf(detail, n, "triangle PSO+draw via shim device, %s", pixelDetail);
    return ok;
}

// ---- stage: 60-frame loop (render + fence + Present + pacing) ----
// Each frame: ScheduleFrameEventX, draw the triangle into the current
// target (swapchain back buffer when one exists, else the offscreen RT),
// Present(0,0) when a swapchain exists, fence-wait, WaitFrameEventX.
// Present(0,0) is immediate (no vsync wait) so the pacing measurement
// is the WaitFrameEventX path, not DXGI vsync. A Present failure is
// environmental (no compositor) and is counted, not fatal.
static bool TestFrames(FrameCtx* ctx, char* detail, size_t n) {
    LARGE_INTEGER qpcFreq;
    QueryPerformanceFrequency(&qpcFreq);
    double totalMs = 0.0, minMs = 1e9, maxMs = 0.0;
    int framesOk = 0, presentsOk = 0, presentsTried = 0, slowFrames = 0;
    bool allOk = true;
    char derr[128] = {};

    D3D12_CPU_DESCRIPTOR_HANDLE rtvBase =
        ctx->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < 60; ++i) {
        LARGE_INTEGER q0, q1;
        QueryPerformanceCounter(&q0);
        HRESULT hr = SetFrameIntervalX(1, 0);
        if (SUCCEEDED(hr)) hr = ScheduleFrameEventX((UINT64)(i + 1));
        bool frameOk = SUCCEEDED(hr);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvBase;
        ID3D12Resource* target = ctx->offRT;
        bool isSwapchain = false;
        ID3D12Resource* bb = nullptr;
        if (frameOk && ctx->hasSwapchain) {
            UINT idx = ctx->sc->GetCurrentBackBufferIndex();
            if (SUCCEEDED(ctx->sc->GetBuffer(idx, IID_PPV_ARGS(&bb)))) {
                target = bb;
                isSwapchain = true;
                rtv.ptr += (SIZE_T)(idx + 1) * ctx->rtvSize;
            }
        }
        if (frameOk)
            frameOk = DrawTriangle(ctx, rtv, target, isSwapchain, derr,
                                   sizeof(derr));
        if (frameOk && ctx->hasSwapchain) {
            presentsTried++;
            HRESULT hrp = ctx->sc->Present(0, 0);
            if (SUCCEEDED(hrp)) presentsOk++;
        }
        if (bb) bb->Release();
        if (frameOk) {
            hr = WaitFrameEventX((UINT64)(i + 1), 1000);
            frameOk = SUCCEEDED(hr);
        }
        QueryPerformanceCounter(&q1);
        double ms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 /
                    (double)qpcFreq.QuadPart;
        totalMs += ms;
        if (ms < minMs) minMs = ms;
        if (ms > maxMs) maxMs = ms;
        if (ms > 50.0) slowFrames++;
        if (frameOk) framesOk++;
        else allOk = false;
    }
    double avgMs = totalMs / 60.0;
    // Pacing bar: every frame must complete (render + fence + wait), and
    // the average must be in a plausible 60 Hz window. Averages far
    // above 25 ms would mean the wait is not firing on time; far below
    // 10 ms would mean it is not waiting at all. A single slow frame
    // is NOT a failure — under a full-build load the OS/driver stalls
    // one frame (seen: 149.3 ms with avg 17.5 ms, present 60/60).
    // Fail only if more than 2 frames exceed 50 ms. (Test-robustness
    // fix, phase 5: the avg + present count still prove the loop.)
    bool timingOk = avgMs >= 10.0 && avgMs <= 25.0 && slowFrames <= 2;
    bool ok = allOk && framesOk == 60 && timingOk;
    if (ctx->hasSwapchain) {
        snprintf(detail, n,
                 "60 frames %s, avg %.1f ms min %.1f max %.1f slow %d, "
                 "present %d/%d",
                 ok ? "ok" : "FAIL", avgMs, minMs, maxMs, slowFrames,
                 presentsOk, presentsTried);
    } else {
        snprintf(detail, n,
                 "60 frames %s, avg %.1f ms min %.1f max %.1f slow %d, "
                 "no swapchain (environmental, offscreen render only)",
                 ok ? "ok" : "FAIL", avgMs, minMs, maxMs, slowFrames);
    }
    return ok;
}

// ---- stage: save round-trip (provider + container + update flow) ----
static bool TestSave(char* detail, size_t n) {
    XGameSaveProviderHandle provider = nullptr;
    HRESULT hr = XGameSaveInitializeProvider(
        g_user, "00000000-0000-0000-0000-000000000004", FALSE, &provider);
    if (FAILED(hr) || !provider) {
        snprintf(detail, n, "XGameSaveInitializeProvider hr=0x%08x",
                 (unsigned)hr);
        return false;
    }
    XGameSaveContainerHandle container = nullptr;
    hr = XGameSaveCreateContainer(provider, "proof-slot", &container);
    if (FAILED(hr) || !container) {
        snprintf(detail, n, "XGameSaveCreateContainer hr=0x%08x",
                 (unsigned)hr);
        XGameSaveCloseProvider(provider);
        return false;
    }
    XGameSaveUpdateHandle update = nullptr;
    hr = XGameSaveCreateUpdate(container, "proof-slot", &update);
    if (FAILED(hr) || !update) {
        snprintf(detail, n, "XGameSaveCreateUpdate hr=0x%08x", (unsigned)hr);
        XGameSaveCloseContainer(container);
        XGameSaveCloseProvider(provider);
        return false;
    }
    static const uint8_t kState[8] = {'P', 'R', 'O', 'O', 'F', '!', '!', '!'};
    hr = XGameSaveSubmitBlobWrite(update, "ProofState", kState, sizeof(kState));
    if (FAILED(hr)) {
        snprintf(detail, n, "XGameSaveSubmitBlobWrite hr=0x%08x",
                 (unsigned)hr);
        XGameSaveCloseUpdate(update);
        XGameSaveCloseContainer(container);
        XGameSaveCloseProvider(provider);
        return false;
    }
    hr = XGameSaveSubmitUpdate(update);
    if (FAILED(hr)) {
        snprintf(detail, n, "XGameSaveSubmitUpdate hr=0x%08x", (unsigned)hr);
        XGameSaveCloseUpdate(update);
        XGameSaveCloseContainer(container);
        XGameSaveCloseProvider(provider);
        return false;
    }
    hr = XGameSaveCloseUpdate(update);
    if (FAILED(hr)) {
        snprintf(detail, n, "XGameSaveCloseUpdate hr=0x%08x", (unsigned)hr);
        XGameSaveCloseContainer(container);
        XGameSaveCloseProvider(provider);
        return false;
    }
    uint8_t readBack[16] = {};
    size_t bytesRead = 0;
    hr = XGameSaveReadBlobData(container, "ProofState", &bytesRead,
                               sizeof(readBack), readBack);
    if (FAILED(hr) || bytesRead != sizeof(kState) ||
        memcmp(readBack, kState, sizeof(kState)) != 0) {
        snprintf(detail, n,
                 "XGameSaveReadBlobData hr=0x%08x bytes=%zu",
                 (unsigned)hr, bytesRead);
        XGameSaveCloseContainer(container);
        XGameSaveCloseProvider(provider);
        return false;
    }
    // Async submit on a second update.
    XGameSaveUpdateHandle update2 = nullptr;
    hr = XGameSaveCreateUpdate(container, "proof-slot", &update2);
    if (FAILED(hr) || !update2) {
        snprintf(detail, n, "XGameSaveCreateUpdate#2 hr=0x%08x",
                 (unsigned)hr);
        XGameSaveCloseContainer(container);
        XGameSaveCloseProvider(provider);
        return false;
    }
    static const uint8_t kAsyncBlob[4] = {'7', '7', '7', '7'};
    hr = XGameSaveSubmitBlobWrite(update2, "AsyncProof", kAsyncBlob,
                                  sizeof(kAsyncBlob));
    if (FAILED(hr)) {
        snprintf(detail, n, "XGameSaveSubmitBlobWrite#2 hr=0x%08x",
                 (unsigned)hr);
        XGameSaveCloseUpdate(update2);
        XGameSaveCloseContainer(container);
        XGameSaveCloseProvider(provider);
        return false;
    }
    XAsyncBlock submitAsync = {};
    submitAsync.callback = GenericCallback;
    g_genericCallbackRan = false;
    hr = XGameSaveSubmitUpdateAsync(update2, &submitAsync);
    if (FAILED(hr) || !g_genericCallbackRan) {
        snprintf(detail, n,
                 "XGameSaveSubmitUpdateAsync hr=0x%08x cb=%d",
                 (unsigned)hr, (int)g_genericCallbackRan);
        XGameSaveCloseUpdate(update2);
        XGameSaveCloseContainer(container);
        XGameSaveCloseProvider(provider);
        return false;
    }
    hr = XGameSaveSubmitUpdateResult(&submitAsync);
    if (FAILED(hr)) {
        snprintf(detail, n, "XGameSaveSubmitUpdateResult hr=0x%08x",
                 (unsigned)hr);
        XGameSaveCloseUpdate(update2);
        XGameSaveCloseContainer(container);
        XGameSaveCloseProvider(provider);
        return false;
    }
    hr = XGameSaveCloseUpdate(update2);
    if (FAILED(hr)) {
        snprintf(detail, n, "XGameSaveCloseUpdate#2 hr=0x%08x", (unsigned)hr);
        XGameSaveCloseContainer(container);
        XGameSaveCloseProvider(provider);
        return false;
    }
    hr = XGameSaveCloseContainer(container);
    if (FAILED(hr)) {
        snprintf(detail, n, "XGameSaveCloseContainer hr=0x%08x",
                 (unsigned)hr);
        XGameSaveCloseProvider(provider);
        return false;
    }
    hr = XGameSaveCloseProvider(provider);
    if (FAILED(hr)) {
        snprintf(detail, n, "XGameSaveCloseProvider hr=0x%08x", (unsigned)hr);
        return false;
    }
    snprintf(detail, n,
             "gamesave round-trip ok (8B sync + 4B async, read-back "
             "verified)");
    return true;
}

// ---- stage: XGameSaveFiles folder + Win32 file round-trip ----
static bool TestFiles(char* detail, size_t n) {
    XAsyncBlock filesAsync = {};
    filesAsync.callback = GenericCallback;
    g_genericCallbackRan = false;
    HRESULT hr = XGameSaveFilesGetFolderWithUiAsync(
        g_user, "00000000-0000-0000-0000-000000000005", &filesAsync);
    if (FAILED(hr) || !g_genericCallbackRan) {
        snprintf(detail, n,
                 "XGameSaveFilesGetFolderWithUiAsync hr=0x%08x cb=%d",
                 (unsigned)hr, (int)g_genericCallbackRan);
        return false;
    }
    char folder[MAX_PATH] = {};
    hr = XGameSaveFilesGetFolderWithUiResult(&filesAsync, sizeof(folder),
                                             folder);
    if (FAILED(hr) || folder[0] == 0) {
        snprintf(detail, n,
                 "XGameSaveFilesGetFolderWithUiResult hr=0x%08x",
                 (unsigned)hr);
        return false;
    }
    char filePath[MAX_PATH] = {};
    snprintf(filePath, sizeof(filePath), "%s\\proof-test.txt", folder);
    HANDLE hf = CreateFileA(filePath, GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) {
        snprintf(detail, n, "CreateFileA in save folder failed (%lu)",
                 GetLastError());
        return false;
    }
    DWORD written = 0;
    const char* kText = "proof-files";
    BOOL wok = WriteFile(hf, kText, (DWORD)strlen(kText), &written, nullptr);
    CloseHandle(hf);
    if (!wok || written != strlen(kText)) {
        snprintf(detail, n, "WriteFile in save folder failed");
        return false;
    }
    hf = CreateFileA(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) {
        snprintf(detail, n, "reopen save file failed (%lu)",
                 GetLastError());
        return false;
    }
    char back[32] = {};
    DWORD read = 0;
    BOOL rok = ReadFile(hf, back, sizeof(back) - 1, &read, nullptr);
    CloseHandle(hf);
    if (!rok || read != strlen(kText) || strcmp(back, kText) != 0) {
        snprintf(detail, n, "save file round-trip mismatch (read=%lu)",
                 read);
        return false;
    }
    snprintf(detail, n, "files folder \"%s\" Win32 round-trip ok", folder);
    return true;
}

// ---- stage: package mount (async mount + result + close) ----
static bool TestPackageMount(char* detail, size_t n) {
    char pkgId[XPACKAGE_IDENTIFIER_MAX_LENGTH] = {};
    HRESULT hr = XPackageGetCurrentProcessPackageIdentifier(sizeof(pkgId), pkgId);
    if (FAILED(hr) || pkgId[0] == 0) {
        snprintf(detail, n,
                 "XPackageGetCurrentProcessPackageIdentifier hr=0x%08x",
                 (unsigned)hr);
        return false;
    }
    XAsyncBlock mountAsync = {};
    mountAsync.callback = GenericCallback;
    g_genericCallbackRan = false;
    hr = XPackageMountWithUiAsync(pkgId, &mountAsync);
    if (FAILED(hr) || !g_genericCallbackRan) {
        snprintf(detail, n,
                 "XPackageMountWithUiAsync hr=0x%08x cb=%d",
                 (unsigned)hr, (int)g_genericCallbackRan);
        return false;
    }
    XPackageMountHandle mount = nullptr;
    hr = XPackageMountWithUiResult(&mountAsync, &mount);
    if (FAILED(hr) || !mount) {
        snprintf(detail, n, "XPackageMountWithUiResult hr=0x%08x",
                 (unsigned)hr);
        return false;
    }
    hr = XPackageCloseMountHandle(mount);
    if (FAILED(hr)) {
        snprintf(detail, n, "XPackageCloseMountHandle hr=0x%08x",
                 (unsigned)hr);
        return false;
    }
    snprintf(detail, n, "pkg \"%s\" mount ok", pkgId);
    return true;
}

// ---- stage: per-section page protections ----
static int g_protProbe = 1234;  // nonzero so it lands in .data, not .bss
static bool TestProtections(char* detail, size_t n);
static bool TestProtections(char* detail, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    DWORD textProt = 0, rodataProt = 0, dataProt = 0;
    if (VirtualQuery((void*)&TestProtections, &mbi, sizeof(mbi)) == 0) {
        snprintf(detail, n, "VirtualQuery(.text) failed (%lu)",
                 GetLastError());
        return false;
    }
    textProt = mbi.Protect;
    if (VirtualQuery("ScarlettCompat protection probe", &mbi, sizeof(mbi)) ==
        0) {
        snprintf(detail, n, "VirtualQuery(.rdata) failed (%lu)",
                 GetLastError());
        return false;
    }
    rodataProt = mbi.Protect;
    if (VirtualQuery(&g_protProbe, &mbi, sizeof(mbi)) == 0) {
        snprintf(detail, n, "VirtualQuery(.data) failed (%lu)",
                 GetLastError());
        return false;
    }
    dataProt = mbi.Protect;
    // Boot #10: an Xbox title keeps mutable globals in .rdata, so the
    // loader maps data sections READWRITE for Xbox titles (see
    // SectionProtection in pe_loader.cpp). Expect RW, not R.
    bool ok = (textProt == PAGE_EXECUTE_READ) &&
              (rodataProt == PAGE_READWRITE) && (dataProt == PAGE_READWRITE);
    snprintf(detail, n,
             "text=0x%x (want 0x20 XR) rdata=0x%x (want 0x4 RW) "
             "data=0x%x (want 0x4 RW)",
             textProt, rodataProt, dataProt);
    return ok;
}

// ---- stage: TLS on worker threads ----
static bool TestTlsThread(char* detail, size_t n) {
    g_workerSaw = -1;
    g_workerSet = 0;
    int mainBefore = g_tlsThreadVar;  // 77 on the main thread
    HANDLE h = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    if (!h) {
        snprintf(detail, n, "CreateThread failed (%lu)", GetLastError());
        return false;
    }
    DWORD w = WaitForSingleObject(h, 10000);
    CloseHandle(h);
    if (w != WAIT_OBJECT_0) {
        snprintf(detail, n, "worker thread timed out");
        return false;
    }
    int mainAfter = g_tlsThreadVar;  // must still be 77
    bool ok = (g_workerSaw == 77) && (g_workerSet == 88) &&
              (mainBefore == 77) && (mainAfter == 77);
    snprintf(detail, n, "worker saw=%ld set=%ld main=%d->%d",
             (long)g_workerSaw, (long)g_workerSet, mainBefore, mainAfter);
    return ok;
}

int main() {
    fopen_s(&g_report, "guest_report.txt", "w");
    printf("[guest] realistic proof guest starting\n");
    // TLS early read (phase 5 diagnostic): read the __declspec(thread)
    // var immediately at entry, before any stage runs. If a later
    // tls stage reads garbage while this one reads 42, the TLS block
    // was corrupted DURING the guest's run (not at load time).
    printf("[guest] tls_early init=%d (want 42)\n", g_tlsVar);
    // Keep .xbld: a volatile read forces the compiler to emit the section
    // (an unreferenced static const array is dropped at /O2); /OPT:NOREF
    // keeps it at link time.
    volatile const char* xbldKeep = kXbld;
    (void)*xbldKeep;
    bool allBoot = true;
    char detail[512];

    // Boot order mirrors an Xbox One title's boot (in GDK terms — see
    // the file header): profile first, because the profile drives the
    // save; profile save early, before the renderer; DLC enumeration;
    // graphics; audio + input before the loop.
    //
    // 1. Runtime init — the first call a GDK title makes.
    HRESULT hr = XGameRuntimeInitialize();
    bool ok = SUCCEEDED(hr);
    snprintf(detail, sizeof(detail), "hr=0x%08x", (unsigned)hr);
    Report("proof_runtime", ok, detail);
    allBoot &= ok;

    // 2. Profile sign-in (silent default user) — first, because the
    // profile drives the save. An Xbox One title signs the profile in
    // before it touches storage.
    detail[0] = 0;
    ok = TestUser(detail, sizeof(detail));
    Report("proof_user", ok, detail);
    allBoot &= ok;

    // 3. Profile save load — EARLY, before the renderer is up. An
    // Xbox One title reads the profile save (cars, credits, progress)
    // before the title screen, not after.
    detail[0] = 0;
    ok = TestSave(detail, sizeof(detail));
    Report("proof_save", ok, detail);
    allBoot &= ok;

    // 4. Package check + DLC enumeration — real titles verify they are
    // packaged and enumerate related packages (an Xbox One title's car
    // packs).
    detail[0] = 0;
    ok = TestPackageEarly(detail, sizeof(detail));
    Report("proof_package", ok, detail);
    allBoot &= ok;

    // 5. Memory.
    detail[0] = 0;
    ok = TestMemory(detail, sizeof(detail));
    Report("proof_memory", ok, detail);
    allBoot &= ok;

    // 6. Real d3d12_xs device.
    void* dev = nullptr;
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params = {};
    params.Version = 0x20004;  // SDK 2.4, as the real exe passes
    hr = D3D12XboxCreateDevice(nullptr, &params,
                               __uuidof(ID3D12Device), &dev);
    ok = false;
    if (SUCCEEDED(hr) && dev) {
        ID3D12Device* d = (ID3D12Device*)dev;
        HRESULT removed = d->GetDeviceRemovedReason();
        UINT nodes = d->GetNodeCount();
        ok = (removed == S_OK) && (nodes >= 1);
        snprintf(detail, sizeof(detail),
                 "hr=0x%08x device=%p nodes=%u removed=0x%08x (REAL)",
                 (unsigned)hr, dev, nodes, (unsigned)removed);
    } else {
        snprintf(detail, sizeof(detail), "hr=0x%08x device=%p",
                 (unsigned)hr, dev);
    }
    Report("proof_gfx_xs", ok, detail);
    allBoot &= ok;

    // 7. Device-method surface through the shim device (renderer init).
    detail[0] = 0;
    ok = false;
    if (dev) ok = TestXsSurface((ID3D12Device*)dev, detail, sizeof(detail));
    Report("proof_gfx_surface", ok, detail);
    allBoot &= ok;

    // 8. Audio init (stub — must still return S_OK). An Xbox One
    // title's audio
    // engine starts before the main loop.
    void* xa = nullptr;
    hr = XAudio2Create((IXAudio2**)&xa, 0, 0);
    ok = SUCCEEDED(hr);
    snprintf(detail, sizeof(detail), "hr=0x%08x (stub)", (unsigned)hr);
    Report("proof_audio", ok, detail);
    allBoot &= ok;

    // 9. Input init (stub). The controller is up before the title
    // screen loop so "press A" works.
    void* gi = nullptr;
    hr = GameInputCreate((IGameInput**)&gi);
    ok = SUCCEEDED(hr);
    snprintf(detail, sizeof(detail), "hr=0x%08x (stub)", (unsigned)hr);
    Report("proof_input", ok, detail);
    allBoot &= ok;

    // 10. Storage (stub). Two-arg form: (riid, ppFactory) — the stub
    // ignores riid; nullptr is fine.
    void* ds = nullptr;
    hr = DStorageGetFactory(nullptr, &ds);
    ok = SUCCEEDED(hr);
    snprintf(detail, sizeof(detail), "hr=0x%08x (stub)", (unsigned)hr);
    Report("proof_storage", ok, detail);
    allBoot &= ok;

    // 11-12. Real rendered scene + 60-frame loop (share a frame
    // context) — the title screen loop at 60 Hz.
    FrameCtx ctx = {};
    ctx.dev = (ID3D12Device*)dev;
    detail[0] = 0;
    ok = false;
    if (dev) ok = BuildFrameCtx(&ctx, detail, sizeof(detail));
    if (!ok) {
        Report("proof_scene", false, detail);
        Report("proof_frames", false, "no frame context");
        allBoot = false;
    } else {
        detail[0] = 0;
        ok = TestScene(&ctx, detail, sizeof(detail));
        Report("proof_scene", ok, detail);
        allBoot &= ok;

        detail[0] = 0;
        ok = TestFrames(&ctx, detail, sizeof(detail));
        Report("proof_frames", ok, detail);
        allBoot &= ok;
    }

    // 13. XGameSaveFiles folder + Win32 file I/O.
    detail[0] = 0;
    ok = TestFiles(detail, sizeof(detail));
    Report("proof_files", ok, detail);
    allBoot &= ok;

    // 14. Package mount.
    detail[0] = 0;
    ok = TestPackageMount(detail, sizeof(detail));
    Report("proof_pkg_mount", ok, detail);
    allBoot &= ok;

    // 15. Per-section page protections.
    detail[0] = 0;
    ok = TestProtections(detail, sizeof(detail));
    Report("proof_protections", ok, detail);
    allBoot &= ok;

    // 16. TLS (main thread).
    detail[0] = 0;
    int tlsInit = g_tlsVar;
    bool tlsOk = g_tlsCallbackRan && (tlsInit == 42);
    g_tlsVar = 123;
    tlsOk = tlsOk && (g_tlsVar == 123);
    snprintf(detail, sizeof(detail), "callback=%d init=%d set=%d",
             (int)g_tlsCallbackRan, tlsInit, g_tlsVar);
    Report("proof_tls", tlsOk, detail);
    allBoot &= tlsOk;

    // 17. TLS on worker threads.
    detail[0] = 0;
    ok = TestTlsThread(detail, sizeof(detail));
    Report("proof_tls_thread", ok, detail);
    allBoot &= ok;

    // Close the user handle, then the final summary.
    if (g_user) {
        hr = XUserCloseHandle(g_user);
        if (FAILED(hr)) {
            printf("[guest] warning: XUserCloseHandle hr=0x%08x\n",
                   (unsigned)hr);
        }
        g_user = nullptr;
    }
    FreeFrameCtx(&ctx);
    if (dev) ((ID3D12Device*)dev)->Release();

    snprintf(detail, sizeof(detail),
             "%d/%d boot stages pass, 60-frame loop, triangle "
             "pixel-verified",
             g_stagesPassed, g_stagesTotal);
    Report("proof", allBoot, detail);
    printf("[guest] proof boot %s\n", allBoot ? "ALL PASS" : "FAIL");
    if (g_report) fclose(g_report);
    return allBoot ? 0 : 1;
}
