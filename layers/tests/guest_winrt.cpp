// guest_winrt.cpp — WinRT shim proof guest (WinRT phase).
//
// Proves the layer's CoreApplication shim end to end, through the
// exact activation path an Xbox One title guest uses:
//   1. The guest imports GetActivationFactoryByPCWSTR from vccorlib110
//      (the real VS2012 C++/CX runtime) and calls it for
//      "Windows.ApplicationModel.Core.CoreApplication" — the loader
//      hooks that IAT entry to ScGetActivationFactoryByPCWSTR.
//   2. The guest also calls RoGetActivationFactory directly (combase
//      path) — the loader hooks that IAT entry to
//      WinRtRoGetActivationFactory. Both paths must return the SAME
//      layer CoreApplication singleton.
//   3. The guest drives the view lifecycle through the layer's Run:
//      CreateView -> Initialize -> SetWindow -> Load -> Run ->
//      Uninitialize. The test view's Run pumps the CoreDispatcher for
//      ~500 ms, then closes the window; Run must return only after the
//      view exits (elapsed >= 400 ms) — an early S_OK would end the
//      game without running it (the WinRT app-model failure mode).
//
// The guest is ERA-shaped (one kernelx import, XMemAlloc) so the
// loader installs the WinRT hooks (eraDetected gate). It links
// vccorlib110.lib (generated from defs/vccorlib110.def — the MANGLED
// export name, so the import resolves against the real DLL exactly
// like an Xbox One title's) + runtimeobject.lib (direct
// RoGetActivationFactory, whose import descriptor is
// api-ms-win-core-winrt-l1-1-0.dll).
//
// Every stage reports as winrt_<stage> in guest_report.txt; the
// harness verdict requires all PASS. The guest links NO layer headers
// for the WinRT interfaces: the guest-side ABI interfaces below are
// declared in the SDK vtable order (grounded in SDK 10.0.26100.0
// windows.applicationmodel.core.h / windows.ui.core.h), truncated to
// the highest method the guest calls — COM vtable layout makes that
// ABI-valid.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winstring.h>  // HSTRING
#include <roapi.h>      // RoInitialize / RoGetActivationFactory
#include <cwchar>       // wcslen
#include <stdio.h>
#include <string.h>
#include "xbox_shims.h"  // XMemAlloc / XMemFree (ERA shape)

// vccorlib110!GetActivationFactoryByPCWSTR — the Xbox One title
// activation path.
// The real vccorlib110.dll exports it ONLY under its C++ mangled name
// (dumpbin /EXPORTS on System32\vccorlib110.dll; undname:
// long __cdecl GetActivationFactoryByPCWSTR(void*, Platform::Guid&,
// void**)). The guest declares it exactly so — Platform::Guid as a
// CLASS (the mangling encodes the class tag V, not struct U) with the
// GUID layout — so the linker emits the mangled reference and the
// import resolves against the real DLL exactly like an Xbox One
// title's does. A plain extern "C" declaration does NOT resolve
// (there is no plain-name export); the first build caught that as
// IMPORT FAILED.
namespace Platform {
class Guid {
public:
    unsigned long Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char Data4[8];
};
}  // namespace Platform
long __cdecl GetActivationFactoryByPCWSTR(void* activatableClassId,
                                          Platform::Guid& iid,
                                          void** factory);

// ---- guest-side ABI interfaces (SDK vtable order, truncated) ----
struct GRect {
    float X, Y, Width, Height;
};
struct GPoint {
    float X, Y;
};
// CoreProcessEventsOption (SDK 10.0.26100.0 — note this order differs
// from older docs): ProcessOneAndAllPending=0, ProcessOneIfPresent=1,
// ProcessUntilQuit=2, ProcessAllIfPresent=3.
enum GProcessEventsOption {
    kProcessOneAndAllPending = 0,
    kProcessOneIfPresent = 1,
    kProcessUntilQuit = 2,
    kProcessAllIfPresent = 3,
};

static const IID IID_GCoreApplication = {
    0x0AACF7A4, 0x5E1D, 0x49DF,
    {0x80, 0x34, 0xFB, 0x6A, 0x68, 0xBC, 0x5E, 0xD1}};
static const IID IID_GCoreApplication2 = {
    0x998681FB, 0x1AB6, 0x4B7F,
    {0xBE, 0x4A, 0x9A, 0x06, 0x45, 0x22, 0x4C, 0x04}};
static const IID IID_GCoreApplicationView = {
    0x638BB2DB, 0x451D, 0x4661,
    {0xB0, 0x99, 0x41, 0x4F, 0x34, 0xFF, 0xB9, 0xF1}};
static const IID IID_GCoreWindow = {
    0x79B9D5F2, 0x879E, 0x4B89,
    {0xB7, 0x98, 0x79, 0xE4, 0x75, 0x98, 0x03, 0x0C}};
static const IID IID_GCoreDispatcher = {
    0x60DB2FA8, 0xB705, 0x4FDE,
    {0xA7, 0xD6, 0xEB, 0xBB, 0x18, 0x91, 0xD3, 0x9E}};
static const IID IID_GFrameworkView = {
    0xFAAB5CD0, 0x8924, 0x45AC,
    {0xAD, 0x0F, 0xA0, 0x8F, 0xAE, 0x5D, 0x03, 0x24}};
static const IID IID_GFrameworkViewSource = {
    0xCD770614, 0x65C4, 0x426C,
    {0x94, 0x94, 0x34, 0xFC, 0x43, 0x55, 0x48, 0x62}};

struct GEventToken {
    UINT64 value;
};

struct GCoreDispatcher : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_HasThreadAccess(
        boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE ProcessEvents(
        int option, HRESULT* result) = 0;
};

struct GCoreWindow : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_AutomationHostProvider(
        IUnknown** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Bounds(GRect* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CustomProperties(
        IUnknown** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Dispatcher(
        GCoreDispatcher** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_FlowDirection(int* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_FlowDirection(int value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsInputEnabled(
        boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsInputEnabled(
        boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PointerCursor(
        IUnknown** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PointerCursor(
        IUnknown* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PointerPosition(
        GPoint* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Visible(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE Activate() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

struct GCoreApplicationView : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_CoreWindow(
        GCoreWindow** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Activated(
        IUnknown* handler, GEventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Activated(
        GEventToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsMain(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsHosted(boolean* value) = 0;
};

struct GFrameworkView : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Initialize(
        GCoreApplicationView* applicationView) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetWindow(GCoreWindow* window) = 0;
    virtual HRESULT STDMETHODCALLTYPE Load(HSTRING entryPoint) = 0;
    virtual HRESULT STDMETHODCALLTYPE Run() = 0;
    virtual HRESULT STDMETHODCALLTYPE Uninitialize() = 0;
};

struct GFrameworkViewSource : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateView(
        GFrameworkView** view) = 0;
};

struct GCoreApplication : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Suspending(
        IUnknown* handler, GEventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Suspending(
        GEventToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Resuming(
        IUnknown* handler, GEventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Resuming(
        GEventToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Properties(IUnknown** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentView(
        GCoreApplicationView** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE Run(
        GFrameworkViewSource* viewSource) = 0;
    virtual HRESULT STDMETHODCALLTYPE RunWithActivationFactories(
        GFrameworkViewSource* viewSource, IUnknown* factories) = 0;
};

// ---- report ----
static FILE* g_report = nullptr;
static int g_pass = 0, g_fail = 0;
static void Report(const char* name, bool pass, const char* detail) {
    fprintf(g_report, "%s=%s\n", name, pass ? "PASS" : "FAIL");
    if (detail) fprintf(g_report, "%s_detail=%s\n", name, detail);
    fflush(g_report);
    if (pass) g_pass++; else g_fail++;
}

// ---- test view: records the lifecycle the layer drives ----
static char g_sequence[256] = {};
static bool g_activatedOk = false;  // set by the view's Load (the view
// object is owned by the layer's Run and deleted when Run returns, so
// the guest must not read view state after Run — a static flag instead).
static void NoteStep(const char* step) {
    if (g_sequence[0]) strcat_s(g_sequence, sizeof(g_sequence), ",");
    strcat_s(g_sequence, sizeof(g_sequence), step);
}

class TestView : public GFrameworkView {
public:
    TestView() : m_ref(1) {}
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                             void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IInspectable) ||
            riid == IID_GFrameworkView) {
            *ppv = static_cast<GFrameworkView*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_ref);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount,
                                      IID** iids) override {
        if (!iidCount || !iids) return E_POINTER;
        *iidCount = 0;
        *iids = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(
        HSTRING* className) override {
        if (!className) return E_POINTER;
        *className = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(
        TrustLevel* trustLevel) override {
        if (!trustLevel) return E_POINTER;
        *trustLevel = TrustLevel::BaseTrust;
        return S_OK;
    }
    // IFrameworkView
    HRESULT STDMETHODCALLTYPE Initialize(
        GCoreApplicationView* applicationView) override {
        NoteStep("Initialize");
        m_appView = applicationView;
        if (m_appView) m_appView->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetWindow(GCoreWindow* window) override {
        NoteStep("SetWindow");
        m_window = window;
        if (m_window) {
            m_window->AddRef();
            HRESULT hr = m_window->get_Dispatcher(&m_dispatcher);
            if (FAILED(hr) || !m_dispatcher) m_dispatcher = nullptr;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Load(HSTRING entryPoint) override {
        (void)entryPoint;
        NoteStep("Load");
        if (m_window) {
            HRESULT hr = m_window->Activate();
            boolean vis = FALSE;
            if (SUCCEEDED(hr) &&
                SUCCEEDED(m_window->get_Visible(&vis)) && vis)
                g_activatedOk = true;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Run() override {
        NoteStep("Run");
        // Pump the dispatcher for ~500 ms (the game loop would run
        // here), then close the window. Close posts WM_QUIT through
        // the Closed handler; a final ProcessUntilQuit drains it.
        // Run must NOT return before the view exits.
        if (!m_dispatcher) return E_FAIL;
        DWORD start = GetTickCount();
        while (GetTickCount() - start < 500) {
            HRESULT hr = S_OK;
            m_dispatcher->ProcessEvents(kProcessAllIfPresent, &hr);
            if (FAILED(hr)) return hr;
            Sleep(16);
        }
        if (m_window) m_window->Close();
        HRESULT hr = S_OK;
        m_dispatcher->ProcessEvents(kProcessUntilQuit, &hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Uninitialize() override {
        NoteStep("Uninitialize");
        if (m_dispatcher) {
            m_dispatcher->Release();
            m_dispatcher = nullptr;
        }
        if (m_window) {
            m_window->Release();
            m_window = nullptr;
        }
        if (m_appView) {
            m_appView->Release();
            m_appView = nullptr;
        }
        return S_OK;
    }

    LONG m_ref;
    GCoreApplicationView* m_appView = nullptr;
    GCoreWindow* m_window = nullptr;
    GCoreDispatcher* m_dispatcher = nullptr;
};

class TestViewSource : public GFrameworkViewSource {
public:
    TestViewSource() : m_ref(1) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                             void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IInspectable) ||
            riid == IID_GFrameworkViewSource) {
            *ppv = static_cast<GFrameworkViewSource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_ref);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount,
                                      IID** iids) override {
        if (!iidCount || !iids) return E_POINTER;
        *iidCount = 0;
        *iids = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(
        HSTRING* className) override {
        if (!className) return E_POINTER;
        *className = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(
        TrustLevel* trustLevel) override {
        if (!trustLevel) return E_POINTER;
        *trustLevel = TrustLevel::BaseTrust;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateView(GFrameworkView** view) override {
        if (!view) return E_POINTER;
        *view = new TestView();
        // CreateView transfers a +1 reference (the caller releases).
        return S_OK;
    }

private:
    LONG m_ref;
};

int main() {
    fopen_s(&g_report, "guest_report.txt", "w");
    if (!g_report) return 1;
    char detail[512];

    // Stage 0: ERA shape — XMemAlloc through the kernelx translation.
    // (Also the import that makes the loader install the WinRT hooks.)
    void* mem = XMemAlloc(64, 0);
    bool eraOk = mem != nullptr;
    if (mem) XMemFree(mem);
    snprintf(detail, sizeof(detail), "XMemAlloc(64) %s",
             eraOk ? "non-null" : "NULL");
    Report("winrt_era", eraOk, detail);
    if (!eraOk) {
        fprintf(g_report, "winrt_note=era shape failed, aborting\n");
        fclose(g_report);
        return 1;
    }

    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        snprintf(detail, sizeof(detail), "RoInitialize hr=0x%08x",
                 (unsigned)hr);
        Report("winrt_roget", false, detail);
        fclose(g_report);
        return 1;
    }

    // Stage 1: direct RoGetActivationFactory (combase path).
    HSTRING hsCore = nullptr;
    hr = WindowsCreateString(
        L"Windows.ApplicationModel.Core.CoreApplication",
        (UINT32)wcslen(L"Windows.ApplicationModel.Core.CoreApplication"),
        &hsCore);
    if (FAILED(hr) || !hsCore) {
        snprintf(detail, sizeof(detail),
                 "WindowsCreateString hr=0x%08x", (unsigned)hr);
        Report("winrt_roget", false, detail);
        fclose(g_report);
        return 1;
    }
    IUnknown* fRoGet = nullptr;
    HRESULT hrRoGet = RoGetActivationFactory(hsCore, IID_IUnknown,
                                             (void**)&fRoGet);
    snprintf(detail, sizeof(detail),
             "RoGetActivationFactory(CoreApplication) -> 0x%08x "
             "factory=%p (want S_OK, non-null)",
             (unsigned)hrRoGet, (void*)fRoGet);
    Report("winrt_roget", SUCCEEDED(hrRoGet) && fRoGet != nullptr, detail);

    // Stage 2: vccorlib110 GetActivationFactoryByPCWSTR (the Xbox One
    // title path).
    // Platform::Guid has the GUID layout, so the ICoreApplication IID
    // is copied into a non-const Guid (the C++/CX signature takes a
    // non-const reference).
    IUnknown* fVccorlib = nullptr;
    Platform::Guid iidApp;
    memcpy(&iidApp, &IID_GCoreApplication, sizeof(iidApp));
    HRESULT hrVcc = GetActivationFactoryByPCWSTR(
        (void*)L"Windows.ApplicationModel.Core.CoreApplication", iidApp,
        (void**)&fVccorlib);
    snprintf(detail, sizeof(detail),
             "GetActivationFactoryByPCWSTR(CoreApplication) -> 0x%08x "
             "factory=%p (want S_OK, non-null)",
             (unsigned)hrVcc, (void*)fVccorlib);
    Report("winrt_activation", SUCCEEDED(hrVcc) && fVccorlib != nullptr,
           detail);
    WindowsDeleteString(hsCore);

    if (!fRoGet || !fVccorlib) {
        fclose(g_report);
        return 1;
    }

    // Stage 3: both activation paths return the same singleton.
    GCoreApplication* appRoGet = nullptr;
    GCoreApplication* appVcc = nullptr;
    HRESULT hrQ1 = fRoGet->QueryInterface(IID_GCoreApplication,
                                          (void**)&appRoGet);
    HRESULT hrQ2 = fVccorlib->QueryInterface(IID_GCoreApplication,
                                             (void**)&appVcc);
    bool same = SUCCEEDED(hrQ1) && SUCCEEDED(hrQ2) && appRoGet &&
                appVcc && appRoGet == appVcc;
    snprintf(detail, sizeof(detail),
             "RoGet app=%p (hr 0x%08x), vccorlib app=%p (hr 0x%08x) "
             "(want identical pointers)",
             (void*)appRoGet, (unsigned)hrQ1, (void*)appVcc,
             (unsigned)hrQ2);
    Report("winrt_same_factory", same, detail);
    fRoGet->Release();
    fVccorlib->Release();
    if (!same) {
        if (appRoGet) appRoGet->Release();
        if (appVcc) appVcc->Release();
        fclose(g_report);
        return 1;
    }
    if (appVcc) appVcc->Release();  // keep appRoGet as the app pointer

    // Stage 4: the factory also exposes ICoreApplication2.
    IUnknown* app2 = nullptr;
    HRESULT hrQ3 = appRoGet->QueryInterface(IID_GCoreApplication2,
                                            (void**)&app2);
    snprintf(detail, sizeof(detail), "QI ICoreApplication2 -> 0x%08x "
                                     "(want S_OK)",
             (unsigned)hrQ3);
    Report("winrt_qi2", SUCCEEDED(hrQ3) && app2 != nullptr, detail);
    if (app2) app2->Release();

    // Stage 5: get_Id returns the layer marker.
    HSTRING hsId = nullptr;
    HRESULT hrId = appRoGet->get_Id(&hsId);
    char idNarrow[128] = {};
    if (SUCCEEDED(hrId) && hsId) {
        UINT32 len = 0;
        PCWSTR raw = WindowsGetStringRawBuffer(hsId, &len);
        if (raw && len < sizeof(idNarrow)) {
            for (UINT32 i = 0; i < len; ++i)
                idNarrow[i] = (char)(raw[i] & 0x7F);
        }
        WindowsDeleteString(hsId);
    }
    bool idOk = SUCCEEDED(hrId) &&
                strcmp(idNarrow, "ScarlettCompat.CoreApplication") == 0;
    snprintf(detail, sizeof(detail), "get_Id -> \"%s\" (hr 0x%08x, want "
                                     "\"ScarlettCompat.CoreApplication\")",
             idNarrow, (unsigned)hrId);
    Report("winrt_id", idOk, detail);

    // Stage 6: GetCurrentView -> CoreWindow (1280x720) + Dispatcher.
    GCoreApplicationView* view = nullptr;
    HRESULT hrView = appRoGet->GetCurrentView(&view);
    bool viewOk = false;
    if (SUCCEEDED(hrView) && view) {
        GCoreWindow* window = nullptr;
        HRESULT hrWin = view->get_CoreWindow(&window);
        if (SUCCEEDED(hrWin) && window) {
            GRect bounds = {};
            HRESULT hrB = window->get_Bounds(&bounds);
            GCoreDispatcher* disp = nullptr;
            HRESULT hrD = window->get_Dispatcher(&disp);
            boolean isMain = FALSE, isHosted = TRUE;
            view->get_IsMain(&isMain);
            view->get_IsHosted(&isHosted);
            viewOk = SUCCEEDED(hrB) && bounds.Width == 1280 &&
                     bounds.Height == 720 && SUCCEEDED(hrD) && disp &&
                     isMain && !isHosted;
            snprintf(detail, sizeof(detail),
                     "CoreWindow bounds %.0fx%.0f (want 1280x720), "
                     "dispatcher=%p, IsMain=%d IsHosted=%d",
                     bounds.Width, bounds.Height, (void*)disp,
                     (int)isMain, (int)isHosted);
            if (disp) disp->Release();
            window->Release();
        } else {
            snprintf(detail, sizeof(detail),
                     "get_CoreWindow -> 0x%08x (want S_OK)",
                     (unsigned)hrWin);
        }
        view->Release();
    } else {
        snprintf(detail, sizeof(detail),
                 "GetCurrentView -> 0x%08x (want S_OK)", (unsigned)hrView);
    }
    Report("winrt_view", viewOk, detail);

    // Stages 7+8: Run the view lifecycle. The test view pumps ~500 ms
    // then closes its window; Run must return only after the view
    // exits, in the exact lifecycle order.
    TestViewSource* source = new TestViewSource();
    g_sequence[0] = 0;
    g_activatedOk = false;
    ULONGLONG t0 = GetTickCount64();
    HRESULT hrRun = appRoGet->Run(source);
    ULONGLONG elapsed = GetTickCount64() - t0;
    bool runOk = SUCCEEDED(hrRun) && elapsed >= 400;
    snprintf(detail, sizeof(detail),
             "Run -> 0x%08x after %llu ms (want S_OK, >= 400 ms — "
             "Run must drive the loop, not return early)",
             (unsigned)hrRun, elapsed);
    Report("winrt_run", runOk, detail);

    bool lifeOk =
        strcmp(g_sequence,
               "Initialize,SetWindow,Load,Run,Uninitialize") == 0 &&
        g_activatedOk;
    snprintf(detail, sizeof(detail),
             "sequence=[%s] (want "
             "Initialize,SetWindow,Load,Run,Uninitialize); "
             "Activate S_OK + window visible after Activate: %s",
             g_sequence, g_activatedOk ? "yes" : "no");
    Report("winrt_lifecycle", lifeOk, detail);

    source->Release();
    appRoGet->Release();
    fclose(g_report);
    return g_fail == 0 ? 0 : 1;
}
