// guest_minimal.cpp — synthetic guest title for the ScarlettCompat harness.
// A minimal Xbox-style PE: image base 0x180000000, a .xbld section with
// GXDK build strings, and imports from the Xbox DLLs (xgameruntime,
// xmem, d3d12_xs, XAudio2_9, dstorage_xs, gameinput) that the layer shims.
// It exercises the boot sequence from the research brief §1:
//   XGameRuntimeInitialize -> XMemAlloc -> D3D12XboxCreateDevice ->
//   frame-pacing calls -> XAudio2Create -> GameInputCreate ->
//   DStorageGetFactory -> real desktop D3D12 device + clear + fence.
// The desktop D3D12 check (gfx_real) proves the translation target works;
// the d3d12_xs call (gfx_xs) proves the boot path reaches the shim.
// Results go to guest_report.txt (read by scarlett_verify) and stdout.
// This file contains NO game code — it is a test program written for the
// layer, and it is never run through the Windows loader directly (its
// Xbox imports only resolve inside the layer).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <d3d12.h>
#include <dxgi1_3.h>  // IDXGIFactory2 + CreateDXGIFactory2
#include <comdef.h>
#include "xbox_shims.h"

// Xbox build-info section, like the shipped DLLs carry.
// (Matches the October 2026 GXDK stamp on amd_fidelityfx_upscaler_xbox.dll.)
#pragma section(".xbld", read)
__declspec(allocate(".xbld")) static const char kXbld[] =
    "_xbld_edition_name=GXDK,October 2026 GXDK\0"
    "_xbld_edition_full_productbuild=GXDK,10.0.26100.9605\0"
    "_xbld_edition_sdktype=GXDK,261000\0"
    "_xbld_edition_mscver=GXDK,195237025.0\0"
    "_xbld_edition_build=GXDK,0x65F42585\0";

// TLS test (phase 2, milestone D): a __declspec(thread) variable and a
// .CRT$XLB callback. The loader must allocate a TLS slot + block, copy
// the template (init 42), and run the callback before the entry point.
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

static FILE* g_report = nullptr;

static void Report(const char* name, bool pass, const char* detail) {
    printf("[guest] %-12s %s %s\n", name, pass ? "PASS" : "FAIL",
           detail ? detail : "");
    if (g_report) {
        fprintf(g_report, "%s=%s\n", name, pass ? "PASS" : "FAIL");
        if (detail) fprintf(g_report, "%s_detail=%s\n", name, detail);
        fflush(g_report);
    }
}

// Services (phase 2, milestone E): XUser sign-in (silent default user),
// XAsync completion, XGameSave provider + container, XPackage mount path.
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
    // The synthetic package must identify itself consistently.
    if (strcmp(details->packageIdentifier,
               "ScarlettCompat-Guest-00000001") != 0)
        return false;
    if (details->kind != XPackageKind_Game) return false;
    if (strcmp(details->titleID, "00000001") != 0) return false;
    return true;  // continue enumerating
}

// Services part 2 (phase 3): XGameSave update flow (sync + async),
// XGameSaveFiles folder + Win32 file I/O, XPackage identifier /
// enumerate / mount.
static bool TestServices2(char* detail, size_t n) {
    // A user for the XGameSaveFiles call.
    XAsyncBlock userAsync = {};
    userAsync.callback = GenericCallback;
    g_genericCallbackRan = false;
    HRESULT hr = XUserAddAsync(XUserAddOptions_AddDefaultUserSilently,
                               &userAsync);
    if (FAILED(hr) || !g_genericCallbackRan) {
        snprintf(detail, n, "XUserAddAsync hr=0x%08x cb=%d",
                 (unsigned)hr, (int)g_genericCallbackRan);
        return false;
    }
    XUserHandle user = nullptr;
    hr = XUserAddResult(&userAsync, &user);
    if (FAILED(hr) || !user) {
        snprintf(detail, n, "XUserAddResult hr=0x%08x", (unsigned)hr);
        return false;
    }

    // XGameSave update flow, sync submit + read-back.
    XGameSaveProviderHandle provider = nullptr;
    hr = XGameSaveInitializeProvider(
        user, "00000000-0000-0000-0000-000000000002", FALSE, &provider);
    if (FAILED(hr) || !provider) {
        snprintf(detail, n, "XGameSaveInitializeProvider hr=0x%08x",
                 (unsigned)hr);
        return false;
    }
    XGameSaveContainerHandle container = nullptr;
    hr = XGameSaveCreateContainer(provider, "save-slot-1", &container);
    if (FAILED(hr) || !container) {
        snprintf(detail, n, "XGameSaveCreateContainer hr=0x%08x",
                 (unsigned)hr);
        XGameSaveCloseProvider(provider);
        return false;
    }
    XGameSaveUpdateHandle update = nullptr;
    hr = XGameSaveCreateUpdate(container, "save-slot-1", &update);
    if (FAILED(hr) || !update) {
        snprintf(detail, n, "XGameSaveCreateUpdate hr=0x%08x", (unsigned)hr);
        XGameSaveCloseContainer(container);
        XGameSaveCloseProvider(provider);
        return false;
    }
    static const uint8_t kPlayerState[8] = {'A', 'B', 'C', 'D',
                                            'E', 'F', 'G', 'H'};
    hr = XGameSaveSubmitBlobWrite(update, "PlayerState", kPlayerState,
                                  sizeof(kPlayerState));
    if (FAILED(hr)) {
        snprintf(detail, n, "XGameSaveSubmitBlobWrite hr=0x%08x",
                 (unsigned)hr);
        XGameSaveCloseUpdate(update);
        XGameSaveCloseContainer(container);
        XGameSaveCloseProvider(provider);
        return false;
    }
    hr = XGameSaveSubmitBlobDelete(update, "OldBlob");
    if (FAILED(hr)) {
        snprintf(detail, n, "XGameSaveSubmitBlobDelete hr=0x%08x",
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
    // Read the blob back and verify the bytes.
    uint8_t readBack[16] = {};
    size_t bytesRead = 0;
    hr = XGameSaveReadBlobData(container, "PlayerState", &bytesRead,
                               sizeof(readBack), readBack);
    if (FAILED(hr) || bytesRead != sizeof(kPlayerState) ||
        memcmp(readBack, kPlayerState, sizeof(kPlayerState)) != 0) {
        snprintf(detail, n,
                 "XGameSaveReadBlobData hr=0x%08x bytes=%zu",
                 (unsigned)hr, bytesRead);
        XGameSaveCloseContainer(container);
        XGameSaveCloseProvider(provider);
        return false;
    }
    // Async submit path on a second update.
    XGameSaveUpdateHandle update2 = nullptr;
    hr = XGameSaveCreateUpdate(container, "save-slot-1", &update2);
    if (FAILED(hr) || !update2) {
        snprintf(detail, n, "XGameSaveCreateUpdate#2 hr=0x%08x",
                 (unsigned)hr);
        XGameSaveCloseContainer(container);
        XGameSaveCloseProvider(provider);
        return false;
    }
    static const uint8_t kAsyncBlob[4] = {'1', '2', '3', '4'};
    hr = XGameSaveSubmitBlobWrite(update2, "AsyncBlob", kAsyncBlob,
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
        snprintf(detail, n, "XGameSaveCloseUpdate#2 hr=0x%08x",
                 (unsigned)hr);
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
        snprintf(detail, n, "XGameSaveCloseProvider hr=0x%08x",
                 (unsigned)hr);
        return false;
    }

    // XGameSaveFiles: get the folder, write a file with Win32 FileIO,
    // read it back.
    XAsyncBlock filesAsync = {};
    filesAsync.callback = GenericCallback;
    g_genericCallbackRan = false;
    hr = XGameSaveFilesGetFolderWithUiAsync(
        user, "00000000-0000-0000-0000-000000000003", &filesAsync);
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
    snprintf(filePath, sizeof(filePath), "%s\\files-test.txt", folder);
    HANDLE hf = CreateFileA(filePath, GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) {
        snprintf(detail, n, "CreateFileA in save folder failed (%lu)",
                 GetLastError());
        return false;
    }
    DWORD written = 0;
    const char* kText = "hello-files";
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

    // XPackage: identifier, enumerate, mount, close.
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
    hr = XUserCloseHandle(user);
    if (FAILED(hr)) {
        snprintf(detail, n, "XUserCloseHandle#2 hr=0x%08x", (unsigned)hr);
        return false;
    }
    snprintf(detail, n,
             "gamesave round-trip ok (8B + 4B async), files folder "
             "\"%s\", pkg \"%s\" enum=1 mount ok",
             folder, pkgId);
    return true;
}

static void CALLBACK TestAsyncWork(XAsyncBlock* async) {
    async->internal[1] = (void*)42;
}

static bool TestServices(char* detail, size_t n) {
    // XUser: silent add -> status -> result -> id/gamertag/localid.
    XAsyncBlock async = {};
    async.callback = UserAddCallback;
    HRESULT hr = XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &async);
    if (FAILED(hr)) {
        snprintf(detail, n, "XUserAddAsync hr=0x%08x", (unsigned)hr);
        return false;
    }
    if (!g_userCallbackRan) {
        snprintf(detail, n, "XUserAddAsync callback did not run");
        return false;
    }
    hr = XAsyncGetStatus(&async, FALSE);
    if (FAILED(hr)) {
        snprintf(detail, n, "XAsyncGetStatus hr=0x%08x", (unsigned)hr);
        return false;
    }
    XUserHandle user = nullptr;
    hr = XUserAddResult(&async, &user);
    if (FAILED(hr) || !user) {
        snprintf(detail, n, "XUserAddResult hr=0x%08x", (unsigned)hr);
        return false;
    }
    uint64_t xuid = 0;
    hr = XUserGetId(user, &xuid);
    if (FAILED(hr) || xuid == 0) {
        snprintf(detail, n, "XUserGetId hr=0x%08x", (unsigned)hr);
        return false;
    }
    char tag[32] = {};
    size_t tagSize = sizeof(tag);
    hr = XUserGetGamertag(user, XUserGamertagOptions_UniqueModern, tag,
                          &tagSize);
    if (FAILED(hr) || strcmp(tag, "Player") != 0) {
        snprintf(detail, n, "XUserGetGamertag hr=0x%08x tag=%s",
                 (unsigned)hr, tag);
        return false;
    }
    XUserLocalId localId = {};
    hr = XUserGetLocalId(user, &localId);
    if (FAILED(hr)) {
        snprintf(detail, n, "XUserGetLocalId hr=0x%08x", (unsigned)hr);
        return false;
    }
    // XAsyncRun: work runs synchronously and completes the block.
    XAsyncBlock runBlk = {};
    hr = XAsyncRun(&runBlk, TestAsyncWork);
    size_t resultSize = 0;
    HRESULT hr2 = XAsyncGetResultSize(&runBlk, &resultSize);
    if (FAILED(hr) || FAILED(hr2) || resultSize != 42) {
        snprintf(detail, n, "XAsyncRun hr=0x%08x resultSize=%zu",
                 (unsigned)hr, resultSize);
        return false;
    }
    // XGameSave: initialize provider + create a container, then close.
    XGameSaveProviderHandle provider = nullptr;
    hr = XGameSaveInitializeProvider(
        user, "00000000-0000-0000-0000-000000000001", FALSE, &provider);
    if (FAILED(hr) || !provider) {
        snprintf(detail, n, "XGameSaveInitializeProvider hr=0x%08x",
                 (unsigned)hr);
        return false;
    }
    XGameSaveContainerHandle container = nullptr;
    hr = XGameSaveCreateContainer(provider, "test-container", &container);
    if (FAILED(hr) || !container) {
        snprintf(detail, n, "XGameSaveCreateContainer hr=0x%08x",
                 (unsigned)hr);
        XGameSaveCloseProvider(provider);
        return false;
    }
    hr = XGameSaveCloseProvider(provider);
    if (FAILED(hr)) {
        snprintf(detail, n, "XGameSaveCloseProvider hr=0x%08x", (unsigned)hr);
        return false;
    }
    // XPackage: mount path + packaged flag.
    uint32_t mountSize = 0;
    hr = XPackageGetMountPathSize(&mountSize);
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
    BOOL packaged = XPackageIsPackagedProcess();
    if (!packaged) {
        snprintf(detail, n, "XPackageIsPackagedProcess returned FALSE");
        return false;
    }
    hr = XUserCloseHandle(user);
    if (FAILED(hr)) {
        snprintf(detail, n, "XUserCloseHandle hr=0x%08x", (unsigned)hr);
        return false;
    }
    snprintf(detail, n,
             "user xuid=0x%llx tag=%s local=%llu mount=%lu chars "
             "packaged=%d",
             (unsigned long long)xuid, tag,
             (unsigned long long)localId.value, mountCap, (int)packaged);
    return true;
}

// TLS on worker threads (phase 3): a second __declspec(thread)
// variable, untouched by the main-thread TLS test, so the worker
// thread's fresh template copy (77) can be told apart from the main
// thread's value — per-thread isolation, not shared state.
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

// Per-section page protections (phase 2, milestone C): the loader must
// have applied each section's characteristics — .text executable+read,
// .rdata read-only, .data read/write. VirtualQuery on an address in each
// section tells us the page protection. (We do NOT attempt a write to
// .text — that would fault if protections are correct.)
static bool TestRealD3D12(char* detail, size_t n);
static int g_protProbe = 1234;  // nonzero so it lands in .data, not .bss
static bool TestProtections(char* detail, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    DWORD textProt = 0, rodataProt = 0, dataProt = 0;
    if (VirtualQuery((void*)&TestRealD3D12, &mbi, sizeof(mbi)) == 0) {
        snprintf(detail, n, "VirtualQuery(.text) failed (%lu)",
                 GetLastError());
        return false;
    }
    textProt = mbi.Protect;
    // String literal lives in .rdata.
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

// Real desktop D3D12 device + clear + fence. This is the translation
// target for the d3d12_xs shim: if the desktop device path works, the shim
// has something real to translate to. Guarded so a failure here does not
// mask the boot-path result.
static bool TestRealD3D12(char* detail, size_t n) {
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS(&dev));
    if (FAILED(hr)) {
        snprintf(detail, n, "D3D12CreateDevice hr=0x%08x", (unsigned)hr);
        return false;
    }
    ID3D12CommandQueue* q = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    if (FAILED(hr)) {
        snprintf(detail, n, "CreateCommandQueue hr=0x%08x", (unsigned)hr);
        dev->Release();
        return false;
    }
    ID3D12CommandAllocator* alloc = nullptr;
    hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                     IID_PPV_ARGS(&alloc));
    ID3D12GraphicsCommandList* list = nullptr;
    if (SUCCEEDED(hr))
        hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc,
                                    nullptr, IID_PPV_ARGS(&list));
    ID3D12Resource* rt = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    if (SUCCEEDED(hr))
        hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
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
        D3D12_CLEAR_VALUE cv = {};
        cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt));
    }
    bool ok = false;
    if (SUCCEEDED(hr)) {
        D3D12_CPU_DESCRIPTOR_HANDLE h =
            rtvHeap->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(rt, nullptr, h);
        FLOAT color[4] = {0.1f, 0.2f, 0.4f, 1.0f};
        list->ClearRenderTargetView(h, color, 0, nullptr);
        hr = list->Close();
        if (SUCCEEDED(hr)) {
            ID3D12CommandList* lists[] = {list};
            q->ExecuteCommandLists(1, lists);
            ID3D12Fence* fence = nullptr;
            hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&fence));
            if (SUCCEEDED(hr)) {
                HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                q->Signal(fence, 1);
                fence->SetEventOnCompletion(1, ev);
                DWORD w = WaitForSingleObject(ev, 10000);
                ok = (w == WAIT_OBJECT_0);
                if (!ok)
                    snprintf(detail, n, "fence wait timed out");
                else
                    snprintf(detail, n,
                             "device+queue+clear+fence ok (FL 11_0)");
                CloseHandle(ev);
                fence->Release();
            } else {
                snprintf(detail, n, "CreateFence hr=0x%08x", (unsigned)hr);
            }
        } else {
            snprintf(detail, n, "Close hr=0x%08x", (unsigned)hr);
        }
    } else {
        snprintf(detail, n, "setup hr=0x%08x", (unsigned)hr);
    }
    if (rt) rt->Release();
    if (rtvHeap) rtvHeap->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (q) q->Release();
    dev->Release();
    return ok;
}

// First real d3d12_xs frame: render a frame with the shim device and
// VERIFY the pixels on the CPU via a readback buffer — that is the proof
// the frame was actually rendered by the GPU through the shim device.
// The swapchain Present is then attempted as a best-effort second step:
// in this environment (SSH-launched harness, session without a compositor)
// DXGI presentation is unavailable — a standalone probe program fails
// identically (0x887a0022), so a present failure here is environmental,
// not a layer bug, and does not fail the subsystem. When the session is
// interactive, the present path is exercised and reported in the detail.
static bool TestXsFrame(ID3D12Device* dev, char* detail, size_t n) {
    // ---- core proof: offscreen render + readback pixel check ----
    ID3D12CommandQueue* q = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    if (FAILED(hr)) {
        snprintf(detail, n, "CreateCommandQueue hr=0x%08x", (unsigned)hr);
        return false;
    }
    ID3D12Resource* rt = nullptr;
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
        hr = dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt));
    }
    if (FAILED(hr)) {
        snprintf(detail, n, "CreateCommittedResource(rt) hr=0x%08x",
                 (unsigned)hr);
        q->Release();
        return false;
    }
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEv = nullptr;
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    }
    if (SUCCEEDED(hr))
        hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(hr))
        hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc,
                                    nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr))
        hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (SUCCEEDED(hr)) {
        fenceEv = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEv) hr = E_FAIL;
    }
    if (FAILED(hr)) {
        snprintf(detail, n, "frame setup hr=0x%08x", (unsigned)hr);
        if (fence) fence->Release();
        if (list) list->Release();
        if (alloc) alloc->Release();
        if (rtvHeap) rtvHeap->Release();
        rt->Release();
        q->Release();
        return false;
    }
    // Record + execute the clear.
    D3D12_CPU_DESCRIPTOR_HANDLE h =
        rtvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt, nullptr, h);
    FLOAT color[4] = {0.2f, 0.4f, 0.8f, 1.0f};
    list->ClearRenderTargetView(h, color, 0, nullptr);
    hr = list->Close();
    UINT64 fenceVal = 0;
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        q->ExecuteCommandLists(1, lists);
        q->Signal(fence, ++fenceVal);
        fence->SetEventOnCompletion(fenceVal, fenceEv);
        if (WaitForSingleObject(fenceEv, 10000) != WAIT_OBJECT_0)
            hr = E_FAIL;
    }
    // Readback: copy the RT to a CPU-visible buffer and check the pixel.
    bool pixelOk = false;
    char pixelDetail[128] = {};
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC rtDesc = rt->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT numRows = 0;
        UINT64 rowSize = 0, totalBytes = 0;
        dev->GetCopyableFootprints(&rtDesc, 0, 1, 0, &footprint, &numRows,
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
        hr = dev->CreateCommittedResource(
            &hpRb, D3D12_HEAP_FLAG_NONE, &rbDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
        if (SUCCEEDED(hr)) {
            alloc->Reset();
            list->Reset(alloc, nullptr);
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = rt;
            barrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &barrier);
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
            if (SUCCEEDED(hr)) {
                ID3D12CommandList* lists[] = {list};
                q->ExecuteCommandLists(1, lists);
                q->Signal(fence, ++fenceVal);
                fence->SetEventOnCompletion(fenceVal, fenceEv);
                if (WaitForSingleObject(fenceEv, 10000) != WAIT_OBJECT_0)
                    hr = E_FAIL;
            }
            if (SUCCEEDED(hr)) {
                void* mapped = nullptr;
                D3D12_RANGE range = {0, totalBytes};
                hr = readback->Map(0, &range, &mapped);
                if (SUCCEEDED(hr) && mapped) {
                    // Center pixel (320,180). Clear color (0.2,0.4,0.8,1)
                    // -> UNORM (51,102,204,255).
                    const uint8_t* px =
                        (const uint8_t*)mapped + footprint.Offset +
                        (UINT64)180 * footprint.Footprint.RowPitch + 320 * 4;
                    int r = px[0], g = px[1], b = px[2], a = px[3];
                    pixelOk = (abs(r - 51) <= 2) && (abs(g - 102) <= 2) &&
                              (abs(b - 204) <= 2) && (abs(a - 255) <= 2);
                    snprintf(pixelDetail, sizeof(pixelDetail),
                             "pixel (%d,%d,%d,%d) expect (51,102,204,255)",
                             r, g, b, a);
                    D3D12_RANGE empty = {0, 0};
                    readback->Unmap(0, &empty);
                } else {
                    snprintf(pixelDetail, sizeof(pixelDetail),
                             "Map hr=0x%08x", (unsigned)hr);
                }
            }
            readback->Release();
        } else {
            snprintf(pixelDetail, sizeof(pixelDetail),
                     "readback alloc hr=0x%08x", (unsigned)hr);
        }
    }
    if (!pixelOk) {
        snprintf(detail, n, "pixel check FAILED (%s)", pixelDetail);
        CloseHandle(fenceEv);
        fence->Release();
        list->Release();
        alloc->Release();
        rtvHeap->Release();
        rt->Release();
        q->Release();
        return false;
    }
    // ---- best-effort: swapchain present (environmental if unavailable) ----
    char presentDetail[160] = {};
    {
        const wchar_t* kClass = L"ScarlettGuestWnd";
        HINSTANCE hi = GetModuleHandleW(nullptr);
        WNDCLASSW wc = {};
        if (!GetClassInfoW(hi, kClass, &wc)) {
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = hi;
            wc.lpszClassName = kClass;
            RegisterClassW(&wc);
        }
        HWND hwnd = CreateWindowExW(0, kClass, L"ScarlettGuest",
                                    WS_OVERLAPPEDWINDOW, 0, 0, 640, 360,
                                    nullptr, nullptr, hi, nullptr);
        if (hwnd) {
            IDXGIFactory2* factory = nullptr;
            HRESULT hr2 = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
            IDXGISwapChain1* sc = nullptr;
            if (SUCCEEDED(hr2)) {
                DXGI_SWAP_CHAIN_DESC1 sd = {};
                sd.Width = 640;
                sd.Height = 360;
                sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                sd.SampleDesc.Count = 1;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.BufferCount = 2;
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                hr2 = factory->CreateSwapChainForHwnd(q, hwnd, &sd, nullptr,
                                                      nullptr, &sc);
            }
            if (SUCCEEDED(hr2)) {
                hr2 = sc->Present(1, 0);
                if (SUCCEEDED(hr2))
                    snprintf(presentDetail, sizeof(presentDetail),
                             "swapchain present ok");
                else
                    snprintf(presentDetail, sizeof(presentDetail),
                             "swapchain present hr=0x%08x", (unsigned)hr2);
                sc->Release();
            } else {
                snprintf(presentDetail, sizeof(presentDetail),
                         "swapchain unavailable hr=0x%08x (environmental; "
                         "standalone probe fails identically)",
                         (unsigned)hr2);
            }
            if (factory) factory->Release();
            DestroyWindow(hwnd);
        } else {
            snprintf(presentDetail, sizeof(presentDetail),
                     "no window for swapchain (%lu)", GetLastError());
        }
    }
    snprintf(detail, n,
             "640x360 frame rendered via shim device, %s; %s",
             pixelDetail, presentDetail);
    CloseHandle(fenceEv);
    fence->Release();
    list->Release();
    alloc->Release();
    rtvHeap->Release();
    rt->Release();
    q->Release();
    return true;
}

// d3d12_xs surface (phase 3): exercise the full D3D12 device-method
// surface THROUGH the shim device (the one D3D12XboxCreateDevice
// returned) — queues of all three types, allocators/lists, committed +
// placed resources, descriptor heaps, root signature, feature query,
// and a GPU copy round-trip verified on the CPU. Xbox titles call these
// as COM methods on the device, so a real desktop device behind the
// shim already answers them; this test proves the path end to end.
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
    // Three committed buffers: DEFAULT / UPLOAD / READBACK, 256 bytes.
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
    // Descriptor heap (CBV_SRV_UAV) + a CBV on the upload buffer.
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
    // Empty root signature (no shaders needed).
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
    // Feature query.
    if (SUCCEEDED(hr)) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS opts = {};
        hr = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts));
        if (SUCCEEDED(hr)) bindingTier = opts.ResourceBindingTier;
    }
    // Heap + placed resource.
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
    // GPU copy round-trip: fill the upload buffer, copy upload -> default
    // -> readback, fence-wait, verify bytes on the CPU.
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

int main() {
    fopen_s(&g_report, "guest_report.txt", "w");
    printf("[guest] synthetic Xbox guest starting\n");
    // TLS early read (phase 5 diagnostic): read the __declspec(thread)
    // var immediately at entry, before any stage runs. If a later
    // tls stage reads garbage while this one reads 42, the TLS block
    // was corrupted DURING the guest's run (not at load time).
    printf("[guest] tls_early init=%d (want 42)\n", g_tlsVar);
    // Keep .xbld: a volatile read forces the compiler to emit the section
    // (an unreferenced static const array is dropped at /O2); /OPT:NOREF
    // keeps it at link time. This exercises the loader's .xbld detection.
    volatile const char* xbldKeep = kXbld;
    (void)*xbldKeep;
    bool allBoot = true;
    char detail[256];

    HRESULT hr = XGameRuntimeInitialize();
    bool ok = SUCCEEDED(hr);
    snprintf(detail, sizeof(detail), "hr=0x%08x", (unsigned)hr);
    Report("runtime", ok, detail);
    allBoot &= ok;

    void* mem = XMemAlloc(1024, 0);
    ok = mem != nullptr;
    Report("memory", ok, ok ? "1024 bytes allocated+freed" : "alloc NULL");
    if (mem) XMemFree(mem);
    allBoot &= ok;

    // The device from the shim must be a REAL ID3D12Device, not the old
    // fake pointer: prove it by calling methods on it.
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
    Report("gfx_xs", ok, detail);
    allBoot &= ok;

    // First real d3d12_xs frame: swapchain off the shim device, clear,
    // present, fence-wait.
    detail[0] = 0;
    ok = false;
    if (dev) ok = TestXsFrame((ID3D12Device*)dev, detail, sizeof(detail));
    Report("frame_xs", ok, detail);
    allBoot &= ok;

    // d3d12_xs surface (phase 3): full device-method surface through the
    // shim device.
    detail[0] = 0;
    ok = false;
    if (dev) ok = TestXsSurface((ID3D12Device*)dev, detail, sizeof(detail));
    Report("gfx_xs_surface", ok, detail);
    allBoot &= ok;

    hr = SetFrameIntervalX(1, 0);
    ok = SUCCEEDED(hr);
    Report("present", ok, ok ? "frame interval set" : "SetFrameIntervalX failed");
    allBoot &= ok;
    hr = ScheduleFrameEventX(1);
    ok = SUCCEEDED(hr);
    Report("frame_evt", ok, nullptr);
    allBoot &= ok;
    // Real pacing: WaitFrameEventX must actually wait ~one frame period
    // (interval 1 = 16.7 ms). Measure it; >= 8 ms proves a real wait.
    LARGE_INTEGER qpcFreq, qpc0, qpc1;
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpc0);
    hr = WaitFrameEventX(1, 1000);
    QueryPerformanceCounter(&qpc1);
    double waitedMs = (double)(qpc1.QuadPart - qpc0.QuadPart) * 1000.0 /
                      (double)qpcFreq.QuadPart;
    ok = SUCCEEDED(hr) && waitedMs >= 8.0;
    snprintf(detail, sizeof(detail), "waited %.1f ms (interval 1 = 16.7 ms)",
             waitedMs);
    Report("frame_wait", ok, detail);
    allBoot &= ok;

    void* xa = nullptr;
    hr = XAudio2Create((IXAudio2**)&xa, 0, 0);
    ok = SUCCEEDED(hr);
    snprintf(detail, sizeof(detail), "hr=0x%08x", (unsigned)hr);
    Report("audio", ok, detail);
    allBoot &= ok;

    void* gi = nullptr;
    hr = GameInputCreate((IGameInput**)&gi);
    ok = SUCCEEDED(hr);
    Report("input", ok, nullptr);
    allBoot &= ok;

    void* ds = nullptr;
    hr = DStorageGetFactory(nullptr, &ds);
    ok = SUCCEEDED(hr);
    Report("storage", ok, nullptr);
    allBoot &= ok;

    // Translation-target proof (separate from the boot path).
    detail[0] = 0;
    ok = TestRealD3D12(detail, sizeof(detail));
    Report("gfx_real", ok, detail);

    // Per-section page protections (loader, milestone C).
    detail[0] = 0;
    ok = TestProtections(detail, sizeof(detail));
    Report("protections", ok, detail);
    allBoot &= ok;

    // TLS (loader, milestone D): callback ran at load, __declspec(thread)
    // variable reads its template value (42) and is writable per-thread.
    detail[0] = 0;
    int tlsInit = g_tlsVar;
    bool tlsOk = g_tlsCallbackRan && (tlsInit == 42);
    g_tlsVar = 123;
    tlsOk = tlsOk && (g_tlsVar == 123);
    snprintf(detail, sizeof(detail), "callback=%d init=%d set=%d",
             (int)g_tlsCallbackRan, tlsInit, g_tlsVar);
    Report("tls", tlsOk, detail);
    allBoot &= tlsOk;

    // TLS on worker threads (phase 3): the guest's CreateThread import
    // is hooked by the loader, so the worker gets its own TLS block.
    detail[0] = 0;
    ok = TestTlsThread(detail, sizeof(detail));
    Report("tls_thread", ok, detail);
    allBoot &= ok;

    // Services (milestone E): XUser / XAsync / XGameSave / XPackage.
    detail[0] = 0;
    ok = TestServices(detail, sizeof(detail));
    Report("services", ok, detail);
    allBoot &= ok;

    // Services part 2 (phase 3): XGameSave update flow, XGameSaveFiles,
    // XPackage identifier / enumerate / mount.
    detail[0] = 0;
    ok = TestServices2(detail, sizeof(detail));
    Report("services2", ok, detail);
    allBoot &= ok;

    printf("[guest] boot path %s\n", allBoot ? "ALL PASS" : "FAIL");
    if (g_report) fclose(g_report);
    return allBoot ? 0 : 1;
}
