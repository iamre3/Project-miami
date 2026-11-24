// winrt_shim.cpp — WinRT activation routing + CoreApplication shim.
//
// See include/winrt_shim.h and docs/WINRT-SHIM.md. An Xbox One title
// boot diagnosis proved the guest's CoreApplication::Run gets
// E_INVALIDARG from the real desktop WinRT (no package identity for a
// manually mapped, unpackaged title), so the layer intercepts
// activation and drives the view lifecycle itself. Returning S_OK from
// Run would NOT work — the guest's Main returns 0 right after Run
// returns — so Run blocks inside the view's own Run loop until the
// view exits.
#include "winrt_shim.h"
#include "era_to_gdk.h"  // EraRoGetActivationFactory (delegate target)
#include <cstring>       // _stricmp
#include <mutex>
#include <queue>
#include <roapi.h>     // RoGetActivationFactoryFn (via era_to_gdk.h too)
#include <vector>
#include <windows.h>
#include <winstring.h>  // WindowsCreateString / WindowsGetStringRawBuffer

namespace sc {
namespace {

// ---- small COM refcount base (single refcount shared by every
// interface a class implements; the most-derived class defines
// QueryInterface/AddRef/Release once, which lands in every base
// vtable with the right this-adjustment — the standard C++ COM
// pattern). ----
class ComBase {
public:
    ULONG AddRef() { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG Release() {
        ULONG r = (ULONG)InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

protected:
    virtual ~ComBase() = default;

private:
    volatile LONG m_ref = 1;
};

// IInspectable helpers shared by every shim object.
HRESULT StdGetIids(const IID* iids, ULONG count, ULONG* iidCount,
                   IID** iidsOut) {
    if (!iidCount || !iidsOut) return E_POINTER;
    IID* out = (IID*)CoTaskMemAlloc(sizeof(IID) * count);
    if (!out) return E_OUTOFMEMORY;
    for (ULONG i = 0; i < count; ++i) out[i] = iids[i];
    *iidCount = count;
    *iidsOut = out;
    return S_OK;
}
HRESULT StdGetRuntimeClassName(PCWSTR name, HSTRING* className) {
    if (!className) return E_POINTER;
    return WindowsCreateString(name, (UINT32)wcslen(name), className);
}
HRESULT StdGetTrustLevel(TrustLevel* trustLevel) {
    if (!trustLevel) return E_POINTER;
    *trustLevel = BaseTrust;
    return S_OK;
}

// ---- ICoreApplication / IActivationFactory / ICoreApplication2/3 ----
// (ABI interfaces; vtable order per the SDK headers — see winrt_shim.h.)
struct IScActivationFactory : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE ActivateInstance(
        IInspectable** instance) = 0;
};
struct IScCoreApplication : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Suspending(
        IUnknown* handler, ScEventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Suspending(
        ScEventToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Resuming(
        IUnknown* handler, ScEventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Resuming(
        ScEventToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Properties(IUnknown** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentView(
        IScCoreApplicationView** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE Run(
        IScFrameworkViewSource* viewSource) = 0;
    virtual HRESULT STDMETHODCALLTYPE RunWithActivationFactories(
        IScFrameworkViewSource* viewSource, IUnknown* factories) = 0;
};
struct IScCoreApplication2 : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE add_BackgroundActivated(
        IUnknown* handler, ScEventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_BackgroundActivated(
        ScEventToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_LeavingBackground(
        IUnknown* handler, ScEventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_LeavingBackground(
        ScEventToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_EnteredBackground(
        IUnknown* handler, ScEventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_EnteredBackground(
        ScEventToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnablePrelaunch(boolean value) = 0;
};
struct IScCoreApplication3 : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RequestRestartAsync(
        IScAsyncAction** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE RequestRestartForUserAsync(
        IUnknown* user, IScAsyncAction** operation) = 0;
};

// ICoreWindowEventArgs {272B1EF3-C633-4DA5-A26C-C6D0F56B29DA} — empty
// interface (no methods past IInspectable); the Closed-event args
// object implements it so a C++/CX handler's QI succeeds.
static const IID IID_ScCoreWindowEventArgs = {
    0x272B1EF3, 0xC633, 0x4DA5,
    {0xA2, 0x6C, 0xC6, 0xD0, 0xF5, 0x6B, 0x29, 0xDA}};
struct IScCoreWindowEventArgs : public IInspectable {};
// TypedEventHandler<CoreWindow*, ...>::Invoke(sender, args).
struct IScCoreWindowEventHandler : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Invoke(IScCoreWindow* sender,
                                             IInspectable* args) = 0;
};

// Event args object for Closed (and any other window event the shim
// fires): QI-able for ICoreWindowEventArgs + IInspectable.
class EmptyEventArgs : public ComBase, public IScCoreWindowEventArgs {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                             void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IInspectable) ||
            riid == IID_ScCoreWindowEventArgs) {
            *ppv = static_cast<IScCoreWindowEventArgs*>(this);
            AddRef();
            return S_OK;
        }
        Log("winrt", "EmptyEventArgs QI unknown IID "
                     "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x} "
                     "-> E_NOINTERFACE",
            riid.Data1, riid.Data2, riid.Data3, riid.Data4[0],
            riid.Data4[1], riid.Data4[2], riid.Data4[3], riid.Data4[4],
            riid.Data4[5], riid.Data4[6], riid.Data4[7]);
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ComBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override {
        return ComBase::Release();
    }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount,
                                      IID** iids) override {
        static const IID kIids[] = {__uuidof(IUnknown),
                                    __uuidof(IInspectable),
                                    IID_ScCoreWindowEventArgs};
        return StdGetIids(kIids, 3, iidCount, iids);
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(
        HSTRING* className) override {
        return StdGetRuntimeClassName(
            L"Windows.UI.Core.CoreWindowEventArgs", className);
    }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(
        TrustLevel* trustLevel) override {
        return StdGetTrustLevel(trustLevel);
    }
};

// ---- IAsyncAction (minimal, completed on dispatch) ----
class ScAsyncAction : public ComBase, public IScAsyncAction {
public:
    ScAsyncAction() : m_id(InterlockedIncrement(&s_nextId)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                             void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IInspectable) ||
            riid == IID_ScAsyncAction) {
            *ppv = static_cast<IScAsyncAction*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ComBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override {
        return ComBase::Release();
    }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount,
                                      IID** iids) override {
        static const IID kIids[] = {__uuidof(IUnknown),
                                    __uuidof(IInspectable),
                                    IID_ScAsyncAction};
        return StdGetIids(kIids, 3, iidCount, iids);
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(
        HSTRING* className) override {
        return StdGetRuntimeClassName(
            L"Windows.Foundation.IAsyncAction", className);
    }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(
        TrustLevel* trustLevel) override {
        return StdGetTrustLevel(trustLevel);
    }

    HRESULT STDMETHODCALLTYPE get_Id(unsigned int* id) override {
        if (!id) return E_POINTER;
        *id = m_id;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Status(int* status) override {
        if (!status) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_mu);
        *status = m_status;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* errorCode) override {
        if (!errorCode) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_mu);
        *errorCode = m_errorCode;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Cancel() override {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_status == kAsyncStarted) m_status = kAsyncCanceled;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE put_Completed(
        IScAsyncActionCompletedHandler* handler) override {
        IScAsyncActionCompletedHandler* toInvoke = nullptr;
        int status = kAsyncStarted;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (m_completed) m_completed->Release();
            m_completed = handler;
            if (m_completed) m_completed->AddRef();
            status = m_status;
            if (status != kAsyncStarted && m_completed) {
                toInvoke = m_completed;
                toInvoke->AddRef();
            }
        }
        if (toInvoke) {
            toInvoke->Invoke(this, status);
            toInvoke->Release();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Completed(
        IScAsyncActionCompletedHandler** handler) override {
        if (!handler) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_mu);
        *handler = m_completed;
        if (*handler) (*handler)->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetResults() override { return S_OK; }

    // Called by the dispatcher after the queued work ran.
    void Complete() {
        IScAsyncActionCompletedHandler* toInvoke = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (m_status == kAsyncStarted) m_status = kAsyncCompleted;
            if (m_completed) {
                toInvoke = m_completed;
                toInvoke->AddRef();
            }
        }
        if (toInvoke) {
            toInvoke->Invoke(this, kAsyncCompleted);
            toInvoke->Release();
        }
    }

private:
    static volatile LONG s_nextId;
    unsigned int m_id;
    std::mutex m_mu;
    int m_status = kAsyncStarted;
    HRESULT m_errorCode = S_OK;
    IScAsyncActionCompletedHandler* m_completed = nullptr;
};
volatile LONG ScAsyncAction::s_nextId = 0;

// ---- CoreDispatcher ----
struct QueuedWork {
    IScDispatchedHandler* handler = nullptr;
    ScAsyncAction* action = nullptr;
};

class ScCoreDispatcher : public ComBase, public IScCoreDispatcher {
public:
    explicit ScCoreDispatcher(HWND hwnd)
        : m_hwnd(hwnd), m_threadId(GetCurrentThreadId()) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                             void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IInspectable) ||
            riid == IID_ScCoreDispatcher) {
            *ppv = static_cast<IScCoreDispatcher*>(this);
            AddRef();
            return S_OK;
        }
        Log("winrt", "CoreDispatcher QI unknown IID -> E_NOINTERFACE");
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ComBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override {
        return ComBase::Release();
    }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount,
                                      IID** iids) override {
        static const IID kIids[] = {__uuidof(IUnknown),
                                    __uuidof(IInspectable),
                                    IID_ScCoreDispatcher};
        return StdGetIids(kIids, 3, iidCount, iids);
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(
        HSTRING* className) override {
        return StdGetRuntimeClassName(L"Windows.UI.Core.CoreDispatcher",
                                      className);
    }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(
        TrustLevel* trustLevel) override {
        return StdGetTrustLevel(trustLevel);
    }

    HRESULT STDMETHODCALLTYPE get_HasThreadAccess(
        boolean* value) override {
        if (!value) return E_POINTER;
        *value = (GetCurrentThreadId() == m_threadId) ? TRUE : FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ProcessEvents(int options) override {
        switch (options) {
            case kProcessAllIfPresent:  // 3
                PumpMessages(false);
                RunQueued(INT_MAX);
                return S_OK;
            case kProcessOneIfPresent:  // 1
                PumpMessages(true);
                RunQueued(1);
                return S_OK;
            case kProcessOneAndAllPending:  // 0
                if (!m_quit) {
                    MSG msg = {};
                    BOOL got = GetMessageW(&msg, nullptr, 0, 0);
                    if (got > 0) {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    } else if (got == 0) {
                        m_quit = true;  // WM_QUIT
                    }
                }
                PumpMessages(false);
                RunQueued(INT_MAX);
                return S_OK;
            case kProcessUntilQuit:  // 2
                while (!m_quit) {
                    MSG msg = {};
                    BOOL got = GetMessageW(&msg, nullptr, 0, 0);
                    if (got <= 0) {
                        m_quit = true;
                        break;
                    }
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                    RunQueued(INT_MAX);
                }
                return S_OK;
            default:
                Log("winrt",
                    "CoreDispatcher::ProcessEvents unknown option %d -> "
                    "E_INVALIDARG",
                    options);
                return E_INVALIDARG;
        }
    }
    HRESULT STDMETHODCALLTYPE RunAsync(
        int priority, IScDispatchedHandler* agileCallback,
        IScAsyncAction** asyncInfo) override {
        if (!agileCallback || !asyncInfo) return E_POINTER;
        *asyncInfo = nullptr;
        ScAsyncAction* action = new ScAsyncAction();
        {
            std::lock_guard<std::mutex> lock(m_mu);
            QueuedWork w;
            w.handler = agileCallback;
            w.handler->AddRef();
            w.action = action;
            action->AddRef();  // queue's reference
            m_queue.push(w);
        }
        *asyncInfo = action;  // caller owns this reference
        Log("winrt", "CoreDispatcher::RunAsync(priority=%d) queued", priority);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RunIdleAsync(
        IScDispatchedHandler* agileCallback,
        IScAsyncAction** asyncInfo) override {
        // Best-effort: queued like RunAsync (no idle-priority
        // scheduling in the shim); documented in docs/WINRT-SHIM.md.
        Log("winrt",
            "CoreDispatcher::RunIdleAsync queued as normal work "
            "(best-effort)");
        return RunAsync(kPriorityIdle, agileCallback, asyncInfo);
    }

    bool QuitRequested() const { return m_quit; }

private:
    // Pump Win32 messages. oneOnly: dispatch at most one message.
    void PumpMessages(bool oneOnly) {
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                m_quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (oneOnly) break;
        }
    }
    void RunQueued(int maxCount) {
        for (int i = 0; i < maxCount; ++i) {
            QueuedWork w;
            {
                std::lock_guard<std::mutex> lock(m_mu);
                if (m_queue.empty()) break;
                w = m_queue.front();
                m_queue.pop();
            }
            if (w.handler) {
                HRESULT hr = w.handler->Invoke();
                if (FAILED(hr))
                    Log("winrt",
                        "CoreDispatcher queued handler -> 0x%08x",
                        (unsigned)hr);
                w.handler->Release();
            }
            if (w.action) {
                w.action->Complete();
                w.action->Release();  // queue's reference
            }
        }
    }

    HWND m_hwnd;
    DWORD m_threadId;
    std::mutex m_mu;
    std::queue<QueuedWork> m_queue;
    bool m_quit = false;
};

// ---- CoreWindow ----
// A real Win32 window (1280x720 client area) behind the ICoreWindow
// surface, so the guest has a presentable window and a message queue
// the dispatcher can pump. Created hidden; shown by Activate() (the
// standard UWP pattern — the view calls Activate during setup).
// Event handlers are stored; Closed is invoked on WM_DESTROY (the
// view's exit signal), SizeChanged updates Bounds (handlers stored,
// not invoked — see docs/WINRT-SHIM.md).
enum WinRtEventKind {
    kEvActivated = 0,
    kEvAutomationProviderRequested = 1,
    kEvCharacterReceived = 2,
    kEvClosed = 3,
    kEvInputEnabled = 4,
    kEvKeyDown = 5,
    kEvKeyUp = 6,
    kEvPointerCaptureLost = 7,
    kEvPointerEntered = 8,
    kEvPointerExited = 9,
    kEvPointerMoved = 10,
    kEvPointerPressed = 11,
    kEvPointerReleased = 12,
    kEvTouchHitTesting = 13,
    kEvPointerWheelChanged = 14,
    kEvSizeChanged = 15,
    kEvVisibilityChanged = 16,
};
struct StoredEvent {
    UINT64 token;
    int kind;
    IUnknown* handler;
};

class ScCoreWindow : public ComBase, public IScCoreWindow {
public:
    ScCoreWindow() {
        RegisterWindowClass();
        RECT rc = {0, 0, 1280, 720};
        AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
        m_hwnd = CreateWindowExW(0, L"ScarlettCompatCoreWindow",
                                 L"ScarlettCompat", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT,
                                 rc.right - rc.left, rc.bottom - rc.top,
                                 nullptr, nullptr,
                                 GetModuleHandleW(nullptr), this);
        if (!m_hwnd) {
            Log("winrt", "CoreWindow CreateWindowEx failed (%lu)",
                GetLastError());
        } else {
            Log("winrt", "CoreWindow created hwnd=%p (1280x720 client)",
                (void*)m_hwnd);
        }
        m_dispatcher = new ScCoreDispatcher(m_hwnd);
        m_bounds = {0, 0, 1280, 720};
    }

    HWND Hwnd() const { return m_hwnd; }
    ScCoreDispatcher* Dispatcher() const { return m_dispatcher; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                             void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IInspectable) ||
            riid == IID_ScCoreWindow) {
            *ppv = static_cast<IScCoreWindow*>(this);
            AddRef();
            return S_OK;
        }
        Log("winrt", "CoreWindow QI unknown IID -> E_NOINTERFACE");
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ComBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override {
        return ComBase::Release();
    }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount,
                                      IID** iids) override {
        static const IID kIids[] = {__uuidof(IUnknown),
                                    __uuidof(IInspectable),
                                    IID_ScCoreWindow};
        return StdGetIids(kIids, 3, iidCount, iids);
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(
        HSTRING* className) override {
        return StdGetRuntimeClassName(L"Windows.UI.Core.CoreWindow",
                                      className);
    }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(
        TrustLevel* trustLevel) override {
        return StdGetTrustLevel(trustLevel);
    }

    HRESULT STDMETHODCALLTYPE get_AutomationHostProvider(
        IUnknown** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        Log("winrt", "CoreWindow::get_AutomationHostProvider -> "
                     "E_NOTIMPL");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_Bounds(ScRect* value) override {
        if (!value) return E_POINTER;
        *value = m_bounds;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_CustomProperties(
        IUnknown** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        Log("winrt",
            "CoreWindow::get_CustomProperties -> E_NOTIMPL");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_Dispatcher(
        IScCoreDispatcher** value) override {
        if (!value) return E_POINTER;
        *value = m_dispatcher;
        if (*value) (*value)->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_FlowDirection(int* value) override {
        if (!value) return E_POINTER;
        *value = 0;  // LeftToRight
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_FlowDirection(int value) override {
        (void)value;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_IsInputEnabled(
        boolean* value) override {
        if (!value) return E_POINTER;
        *value = m_inputEnabled ? TRUE : FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_IsInputEnabled(boolean value) override {
        m_inputEnabled = value != FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_PointerCursor(IUnknown** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        Log("winrt", "CoreWindow::get_PointerCursor -> E_NOTIMPL");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE put_PointerCursor(IUnknown* value) override {
        (void)value;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_PointerPosition(ScPoint* value) override {
        if (!value) return E_POINTER;
        value->X = 0;
        value->Y = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Visible(boolean* value) override {
        if (!value) return E_POINTER;
        *value = m_visible ? TRUE : FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Activate() override {
        if (m_hwnd) {
            ShowWindow(m_hwnd, SW_SHOW);
            SetForegroundWindow(m_hwnd);
            m_visible = true;
        }
        Log("winrt", "CoreWindow::Activate (hwnd=%p)", (void*)m_hwnd);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Close() override {
        Log("winrt", "CoreWindow::Close (hwnd=%p)", (void*)m_hwnd);
        if (m_hwnd) DestroyWindow(m_hwnd);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAsyncKeyState(int virtualKey,
                                               int* state) override {
        (void)virtualKey;
        if (!state) return E_POINTER;
        *state = 0;  // CoreVirtualKeyStates_None
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetKeyState(int virtualKey,
                                          int* state) override {
        (void)virtualKey;
        if (!state) return E_POINTER;
        *state = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleasePointerCapture() override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPointerCapture() override { return S_OK; }

    // Event registration: every add_* stores the handler and returns a
    // token; remove_* drops it. Only Closed is ever fired (on
    // WM_DESTROY); the rest are stored-only — see docs/WINRT-SHIM.md.
    HRESULT AddEvent(int kind, IUnknown* handler, ScEventToken* cookie) {
        if (!handler || !cookie) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_evMu);
        ScEventToken tok;
        tok.value = ++m_nextToken;
        StoredEvent ev;
        ev.token = tok.value;
        ev.kind = kind;
        ev.handler = handler;
        ev.handler->AddRef();
        m_events.push_back(ev);
        *cookie = tok;
        return S_OK;
    }
    HRESULT RemoveEvent(ScEventToken cookie) {
        std::lock_guard<std::mutex> lock(m_evMu);
        for (auto it = m_events.begin(); it != m_events.end(); ++it) {
            if (it->token == cookie.value) {
                it->handler->Release();
                m_events.erase(it);
                return S_OK;
            }
        }
        return S_OK;  // unknown token: tolerate
    }
    HRESULT STDMETHODCALLTYPE add_Activated(IUnknown* h,
                                            ScEventToken* c) override {
        return AddEvent(kEvActivated, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_Activated(ScEventToken c) override {
        return RemoveEvent(c);
    }
    HRESULT STDMETHODCALLTYPE add_AutomationProviderRequested(
        IUnknown* h, ScEventToken* c) override {
        return AddEvent(kEvAutomationProviderRequested, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_AutomationProviderRequested(
        ScEventToken c) override {
        return RemoveEvent(c);
    }
    HRESULT STDMETHODCALLTYPE add_CharacterReceived(
        IUnknown* h, ScEventToken* c) override {
        return AddEvent(kEvCharacterReceived, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_CharacterReceived(
        ScEventToken c) override {
        return RemoveEvent(c);
    }
    HRESULT STDMETHODCALLTYPE add_Closed(IUnknown* h,
                                         ScEventToken* c) override {
        return AddEvent(kEvClosed, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_Closed(ScEventToken c) override {
        return RemoveEvent(c);
    }
    HRESULT STDMETHODCALLTYPE add_InputEnabled(IUnknown* h,
                                               ScEventToken* c) override {
        return AddEvent(kEvInputEnabled, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_InputEnabled(
        ScEventToken c) override {
        return RemoveEvent(c);
    }
    HRESULT STDMETHODCALLTYPE add_KeyDown(IUnknown* h,
                                          ScEventToken* c) override {
        return AddEvent(kEvKeyDown, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_KeyDown(ScEventToken c) override {
        return RemoveEvent(c);
    }
    HRESULT STDMETHODCALLTYPE add_KeyUp(IUnknown* h,
                                        ScEventToken* c) override {
        return AddEvent(kEvKeyUp, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_KeyUp(ScEventToken c) override {
        return RemoveEvent(c);
    }
    HRESULT STDMETHODCALLTYPE add_PointerCaptureLost(
        IUnknown* h, ScEventToken* c) override {
        return AddEvent(kEvPointerCaptureLost, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_PointerCaptureLost(
        ScEventToken c) override {
        return RemoveEvent(c);
    }
    HRESULT STDMETHODCALLTYPE add_PointerEntered(
        IUnknown* h, ScEventToken* c) override {
        return AddEvent(kEvPointerEntered, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_PointerEntered(
        ScEventToken c) override {
        return RemoveEvent(c);
    }
    HRESULT STDMETHODCALLTYPE add_PointerExited(
        IUnknown* h, ScEventToken* c) override {
        return AddEvent(kEvPointerExited, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_PointerExited(
        ScEventToken c) override {
        return RemoveEvent(c);
    }
    HRESULT STDMETHODCALLTYPE add_PointerMoved(
        IUnknown* h, ScEventToken* c) override {
        return AddEvent(kEvPointerMoved, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_PointerMoved(
        ScEventToken c) override {
        return RemoveEvent(c);
    }
    HRESULT STDMETHODCALLTYPE add_PointerPressed(
        IUnknown* h, ScEventToken* c) override {
        return AddEvent(kEvPointerPressed, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_PointerPressed(
        ScEventToken c) override {
        return RemoveEvent(c);
    }
    HRESULT STDMETHODCALLTYPE add_PointerReleased(
        IUnknown* h, ScEventToken* c) override {
        return AddEvent(kEvPointerReleased, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_PointerReleased(
        ScEventToken c) override {
        return RemoveEvent(c);
    }
    HRESULT STDMETHODCALLTYPE add_TouchHitTesting(
        IUnknown* h, ScEventToken* c) override {
        return AddEvent(kEvTouchHitTesting, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_TouchHitTesting(
        ScEventToken c) override {
        return RemoveEvent(c);
    }
    HRESULT STDMETHODCALLTYPE add_PointerWheelChanged(
        IUnknown* h, ScEventToken* c) override {
        return AddEvent(kEvPointerWheelChanged, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_PointerWheelChanged(
        ScEventToken c) override {
        return RemoveEvent(c);
    }
    HRESULT STDMETHODCALLTYPE add_SizeChanged(
        IUnknown* h, ScEventToken* c) override {
        return AddEvent(kEvSizeChanged, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_SizeChanged(
        ScEventToken c) override {
        return RemoveEvent(c);
    }
    HRESULT STDMETHODCALLTYPE add_VisibilityChanged(
        IUnknown* h, ScEventToken* c) override {
        return AddEvent(kEvVisibilityChanged, h, c);
    }
    HRESULT STDMETHODCALLTYPE remove_VisibilityChanged(
        ScEventToken c) override {
        return RemoveEvent(c);
    }

    // Called by the window proc on WM_DESTROY: fire the Closed
    // handlers (the view's exit signal), then quit the message pump.
    void OnDestroy() {
        std::vector<StoredEvent> closed;
        {
            std::lock_guard<std::mutex> lock(m_evMu);
            for (const StoredEvent& ev : m_events) {
                if (ev.kind == kEvClosed) closed.push_back(ev);
            }
        }
        if (!closed.empty()) {
            EmptyEventArgs* args = new EmptyEventArgs();
            for (const StoredEvent& ev : closed) {
                auto* handler =
                    static_cast<IScCoreWindowEventHandler*>(ev.handler);
                HRESULT hr = handler->Invoke(this, args);
                Log("winrt", "CoreWindow Closed handler -> 0x%08x",
                    (unsigned)hr);
            }
            args->Release();
        }
        m_visible = false;
        PostQuitMessage(0);
    }
    void OnSize(int width, int height) {
        m_bounds.Width = (float)width;
        m_bounds.Height = (float)height;
        Log("winrt", "CoreWindow resized to %dx%d (Bounds updated)",
            width, height);
    }

private:
    static void RegisterWindowClass() {
        static bool registered = false;
        if (registered) return;
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &ScCoreWindow::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ScarlettCompatCoreWindow";
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        RegisterClassExW(&wc);
        registered = true;
    }
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp,
                                    LPARAM lp) {
        ScCoreWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = (CREATESTRUCTW*)lp;
            self = (ScCoreWindow*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        } else {
            self = (ScCoreWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        }
        if (self) {
            if (msg == WM_DESTROY) {
                self->OnDestroy();
                return 0;
            }
            if (msg == WM_SIZE) {
                self->OnSize(LOWORD(lp), HIWORD(lp));
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    HWND m_hwnd = nullptr;
    ScCoreDispatcher* m_dispatcher = nullptr;
    ScRect m_bounds = {0, 0, 1280, 720};
    bool m_visible = false;
    bool m_inputEnabled = true;
    std::mutex m_evMu;
    std::vector<StoredEvent> m_events;
    UINT64 m_nextToken = 0;
};

// ---- CoreApplicationView ----
class ScCoreApplicationView : public ComBase, public IScCoreApplicationView {
public:
    explicit ScCoreApplicationView(ScCoreWindow* window)
        : m_window(window) {
        if (m_window) m_window->AddRef();
    }
    ~ScCoreApplicationView() override {
        if (m_window) m_window->Release();
    }
    ScCoreWindow* Window() const { return m_window; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                             void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IInspectable) ||
            riid == IID_ScCoreApplicationView) {
            *ppv = static_cast<IScCoreApplicationView*>(this);
            AddRef();
            return S_OK;
        }
        Log("winrt", "CoreApplicationView QI unknown IID -> "
                     "E_NOINTERFACE");
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ComBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override {
        return ComBase::Release();
    }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount,
                                      IID** iids) override {
        static const IID kIids[] = {__uuidof(IUnknown),
                                    __uuidof(IInspectable),
                                    IID_ScCoreApplicationView};
        return StdGetIids(kIids, 3, iidCount, iids);
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(
        HSTRING* className) override {
        return StdGetRuntimeClassName(
            L"Windows.ApplicationModel.Core.CoreApplicationView",
            className);
    }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(
        TrustLevel* trustLevel) override {
        return StdGetTrustLevel(trustLevel);
    }
    HRESULT STDMETHODCALLTYPE get_CoreWindow(
        IScCoreWindow** value) override {
        if (!value) return E_POINTER;
        *value = m_window;
        if (*value) (*value)->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE add_Activated(IUnknown* handler,
                                            ScEventToken* token) override {
        if (!handler || !token) return E_POINTER;
        token->value = 1;  // stored-only; never fired by the shim
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE remove_Activated(
        ScEventToken token) override {
        (void)token;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_IsMain(boolean* value) override {
        if (!value) return E_POINTER;
        *value = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_IsHosted(boolean* value) override {
        if (!value) return E_POINTER;
        *value = FALSE;
        return S_OK;
    }

private:
    ScCoreWindow* m_window = nullptr;
};

// ---- CoreApplication (singleton) ----
// One object implements IActivationFactory + ICoreApplication (+ 2/3).
// An Xbox One title guest asks GetActivationFactoryByPCWSTR for
// IID_ICoreApplication and calls Run on the returned pointer directly
// (never ActivateInstance), so the factory pointer MUST QI to
// ICoreApplication.
class ScCoreApplication : public ComBase,
                          public IScActivationFactory,
                          public IScCoreApplication,
                          public IScCoreApplication2,
                          public IScCoreApplication3 {
public:
    static ScCoreApplication* GetInstance() {
        static ScCoreApplication* instance = new ScCoreApplication();
        return instance;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                             void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IInspectable)) {
            *ppv = static_cast<IScActivationFactory*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_ScActivationFactory) {
            *ppv = static_cast<IScActivationFactory*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_ScCoreApplication) {
            *ppv = static_cast<IScCoreApplication*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_ScCoreApplication2) {
            *ppv = static_cast<IScCoreApplication2*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_ScCoreApplication3) {
            *ppv = static_cast<IScCoreApplication3*>(this);
            AddRef();
            return S_OK;
        }
        Log("winrt", "CoreApplication QI unknown IID "
                     "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x} "
                     "-> E_NOINTERFACE",
            riid.Data1, riid.Data2, riid.Data3, riid.Data4[0],
            riid.Data4[1], riid.Data4[2], riid.Data4[3], riid.Data4[4],
            riid.Data4[5], riid.Data4[6], riid.Data4[7]);
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ComBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override {
        return ComBase::Release();
    }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* iidCount,
                                      IID** iids) override {
        static const IID kIids[] = {
            __uuidof(IUnknown), __uuidof(IInspectable),
            IID_ScActivationFactory, IID_ScCoreApplication,
            IID_ScCoreApplication2, IID_ScCoreApplication3};
        return StdGetIids(kIids, 6, iidCount, iids);
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(
        HSTRING* className) override {
        return StdGetRuntimeClassName(
            L"Windows.ApplicationModel.Core.CoreApplication", className);
    }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(
        TrustLevel* trustLevel) override {
        return StdGetTrustLevel(trustLevel);
    }

    // IActivationFactory
    HRESULT STDMETHODCALLTYPE ActivateInstance(
        IInspectable** instance) override {
        if (!instance) return E_POINTER;
        *instance = static_cast<IScCoreApplication*>(this);
        AddRef();
        Log("winrt", "CoreApplication::ActivateInstance -> singleton");
        return S_OK;
    }

    // ICoreApplication
    HRESULT STDMETHODCALLTYPE get_Id(HSTRING* value) override {
        if (!value) return E_POINTER;
        return WindowsCreateString(L"ScarlettCompat.CoreApplication",
                                   30, value);
    }
    HRESULT STDMETHODCALLTYPE add_Suspending(IUnknown* handler,
                                             ScEventToken* token) override {
        if (!handler || !token) return E_POINTER;
        token->value = 1;  // stored-only; the shim never suspends
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE remove_Suspending(
        ScEventToken token) override {
        (void)token;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE add_Resuming(IUnknown* handler,
                                           ScEventToken* token) override {
        if (!handler || !token) return E_POINTER;
        token->value = 2;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE remove_Resuming(ScEventToken token) override {
        (void)token;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Properties(IUnknown** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        // Documented gap: no IPropertySet yet. If the boot shows the
        // guest needs it, implement it (see docs/WINRT-SHIM.md).
        Log("winrt", "CoreApplication::get_Properties -> E_NOTIMPL");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentView(
        IScCoreApplicationView** value) override {
        if (!value) return E_POINTER;
        ScCoreApplicationView* view = GetOrCreateView();
        *value = view;
        if (*value) (*value)->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Run(
        IScFrameworkViewSource* viewSource) override {
        if (!viewSource) return E_INVALIDARG;
        Log("winrt", "CoreApplication::Run(viewSource=%p) — driving "
                     "the view lifecycle",
            (void*)viewSource);
        // 1. CreateView.
        IScFrameworkView* view = nullptr;
        HRESULT hr = viewSource->CreateView(&view);
        if (FAILED(hr) || !view) {
            Log("winrt", "CreateView -> 0x%08x (view=%p)",
                (unsigned)hr, (void*)view);
            return FAILED(hr) ? hr : E_FAIL;
        }
        Log("winrt", "CreateView -> view=%p", (void*)view);
        // 2. Initialize (with the application view).
        ScCoreApplicationView* appView = GetOrCreateView();
        hr = view->Initialize(appView);
        if (FAILED(hr)) {
            Log("winrt", "view Initialize -> 0x%08x", (unsigned)hr);
            view->Release();
            return hr;
        }
        Log("winrt", "view Initialize -> S_OK");
        // 3. SetWindow.
        IScCoreWindow* window = nullptr;
        appView->get_CoreWindow(&window);
        hr = view->SetWindow(window);
        if (FAILED(hr)) {
            Log("winrt", "view SetWindow -> 0x%08x", (unsigned)hr);
            if (window) window->Release();
            view->Release();
            return hr;
        }
        Log("winrt", "view SetWindow -> S_OK (hwnd=%p)",
            (void*)WinRtCoreWindowHwnd());
        // 4. Load (empty entry-point string).
        HSTRING hsEntry = nullptr;
        WindowsCreateString(L"", 0, &hsEntry);
        hr = view->Load(hsEntry);
        if (hsEntry) WindowsDeleteString(hsEntry);
        if (FAILED(hr)) {
            Log("winrt", "view Load -> 0x%08x", (unsigned)hr);
            if (window) window->Release();
            view->Release();
            return hr;
        }
        Log("winrt", "view Load -> S_OK");
        // 5. Run — the view's own loop. THIS is the game's main loop:
        // Run must NOT return until the view exits (the guest's Main
        // returns 0 right after Run returns, so an early S_OK would
        // end the game without running it).
        Log("winrt", "entering view Run (game loop)");
        hr = view->Run();
        Log("winrt", "view Run returned 0x%08x", (unsigned)hr);
        // 6. Uninitialize (best effort even if Run failed).
        HRESULT hrUninit = view->Uninitialize();
        Log("winrt", "view Uninitialize -> 0x%08x", (unsigned)hrUninit);
        if (window) window->Release();
        view->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE RunWithActivationFactories(
        IScFrameworkViewSource* viewSource, IUnknown* factories) override {
        (void)factories;  // activation factories unused by the shim
        Log("winrt", "RunWithActivationFactories -> Run");
        return Run(viewSource);
    }

    // ICoreApplication2 (stubs: registered, never fired).
    HRESULT STDMETHODCALLTYPE add_BackgroundActivated(
        IUnknown* h, ScEventToken* t) override {
        if (!h || !t) return E_POINTER;
        t->value = 3;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE remove_BackgroundActivated(
        ScEventToken t) override {
        (void)t;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE add_LeavingBackground(
        IUnknown* h, ScEventToken* t) override {
        if (!h || !t) return E_POINTER;
        t->value = 4;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE remove_LeavingBackground(
        ScEventToken t) override {
        (void)t;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE add_EnteredBackground(
        IUnknown* h, ScEventToken* t) override {
        if (!h || !t) return E_POINTER;
        t->value = 5;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE remove_EnteredBackground(
        ScEventToken t) override {
        (void)t;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnablePrelaunch(boolean value) override {
        (void)value;
        return S_OK;
    }

    // ICoreApplication3 (restart: a completed action — the shim never
    // actually restarts; documented in docs/WINRT-SHIM.md).
    HRESULT STDMETHODCALLTYPE RequestRestartAsync(
        IScAsyncAction** operation) override {
        if (!operation) return E_POINTER;
        ScAsyncAction* action = new ScAsyncAction();
        action->Complete();
        *operation = action;
        Log("winrt", "RequestRestartAsync -> completed action "
                     "(no real restart)");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RequestRestartForUserAsync(
        IUnknown* user, IScAsyncAction** operation) override {
        (void)user;
        return RequestRestartAsync(operation);
    }

    HWND WindowHwnd() {
        std::lock_guard<std::mutex> lock(m_mu);
        if (!m_appView) return nullptr;
        ScCoreWindow* w = m_appView->Window();
        return w ? w->Hwnd() : nullptr;
    }

private:
    ScCoreApplication() = default;
    ScCoreApplicationView* GetOrCreateView() {
        std::lock_guard<std::mutex> lock(m_mu);
        if (!m_appView) {
            ScCoreWindow* window = new ScCoreWindow();
            m_appView = new ScCoreApplicationView(window);
            window->Release();  // view holds the reference now
            Log("winrt", "CoreApplicationView + CoreWindow created");
        }
        return m_appView;
    }

    std::mutex m_mu;
    ScCoreApplicationView* m_appView = nullptr;
};

}  // namespace

// ---- Activation wrappers (the IAT hook targets) ----
static WinRtGetActivationFactoryByPCWSTRFn
    g_realGetActivationFactoryByPCWSTR = nullptr;

void WinRtSetRealGetActivationFactoryByPCWSTR(
    WinRtGetActivationFactoryByPCWSTRFn realFn) {
    g_realGetActivationFactoryByPCWSTR = realFn;
}

HWND WinRtCoreWindowHwnd() {
    return ScCoreApplication::GetInstance()->WindowHwnd();
}

extern "C" HRESULT ScGetActivationFactoryByPCWSTR(
    PCWSTR activatableClassId, REFIID iid, void** factory) {
    if (!factory) return E_POINTER;
    *factory = nullptr;
    char narrow[256] = {};
    if (activatableClassId) {
        for (int i = 0; i < 255 && activatableClassId[i]; ++i)
            narrow[i] = (char)(activatableClassId[i] & 0x7F);
    }
    if (activatableClassId &&
        _wcsicmp(activatableClassId,
                 L"Windows.ApplicationModel.Core.CoreApplication") == 0) {
        Log("winrt", "GetActivationFactoryByPCWSTR(CoreApplication) -> "
                     "layer CoreApplication factory");
        return ScCoreApplication::GetInstance()->QueryInterface(iid,
                                                                factory);
    }
    if (!g_realGetActivationFactoryByPCWSTR) {
        Log("winrt", "GetActivationFactoryByPCWSTR(\"%s\") -> E_FAIL (no "
                     "real function stashed)",
            narrow);
        return E_FAIL;
    }
    HRESULT hr = g_realGetActivationFactoryByPCWSTR(activatableClassId,
                                                    iid, factory);
    Log("winrt", "GetActivationFactoryByPCWSTR(\"%s\") -> forwarded "
                 "hr=0x%08x",
        narrow, (unsigned)hr);
    return hr;
}

extern "C" HRESULT WinRtRoGetActivationFactory(HSTRING activatableClassId,
                                               REFIID iid, void** factory) {
    if (!factory) return E_POINTER;
    *factory = nullptr;
    char className[256] = {};
    UINT32 len = 0;
    PCWSTR raw = nullptr;
    if (activatableClassId)
        raw = WindowsGetStringRawBuffer(activatableClassId, &len);
    if (raw && len > 0 && len < sizeof(className)) {
        for (UINT32 i = 0; i < len; ++i)
            className[i] = (char)(raw[i] & 0x7F);
    }
    if (_stricmp(className,
                 "Windows.ApplicationModel.Core.CoreApplication") == 0) {
        Log("winrt", "RoGetActivationFactory(CoreApplication) -> layer "
                     "CoreApplication factory");
        return ScCoreApplication::GetInstance()->QueryInterface(iid,
                                                                factory);
    }
    // Everything else keeps the phase 5 behaviour (Xbox-namespace
    // refusal + forward to the real RoGetActivationFactory).
    return EraRoGetActivationFactory(activatableClassId, iid, factory);
}

}  // namespace sc
