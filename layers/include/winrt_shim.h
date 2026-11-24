// winrt_shim.h — WinRT activation routing + CoreApplication shim.
//
// An Xbox One title boot diagnosis proved the guest dies at the UWP
// app-model entry point: its CoreApplication::Run(viewSource) gets
// E_INVALIDARG from the real desktop WinRT, because a manually-mapped,
// unpackaged Xbox title has no package identity. The layer therefore
// intercepts activation and provides its own ICoreApplication whose
// Run actually drives the guest view's lifecycle (Initialize ->
// SetWindow -> Load -> Run -> Uninitialize) with a message pump.
//
// Interception points (both installed by the loader, pe_loader.cpp):
//   - vccorlib110!GetActivationFactoryByPCWSTR — the path an Xbox One
//     title actually uses (the guest imports it from vccorlib110, not
//     RoGetActivationFactory directly). Patched in the guest IAT to
//     ScGetActivationFactoryByPCWSTR.
//   - combase!RoGetActivationFactory — patched in the guest IAT to
//     WinRtRoGetActivationFactory, which routes CoreApplication to the
//     shim and delegates everything else to EraRoGetActivationFactory
//     (phase 5 behaviour preserved).
//
// All interface layouts below are grounded in the Windows SDK
// 10.0.26100.0 MIDL headers windows.applicationmodel.core.h and
// windows.ui.core.h (vtable order + GUIDs read from those headers on
// the build machine; ICoreApplication::Run at vtable index 13 also
// proven independently by disassembly of an Xbox One title's
// activation path). The ABI
// interfaces are hand-rolled here (the SDK headers are not included)
// so the layer has no new header dependency; every method returns
// HRESULT at the ABI level, as the MIDL headers declare.
#pragma once
#include "scarlett.h"
#include <inspectable.h>  // IInspectable
#include <hstring.h>      // HSTRING
#include <cwchar>         // _wcsicmp

namespace sc {

// ---- IIDs (SDK 10.0.26100.0 winrt headers) ----
// ICoreApplication {0AACF7A4-5E1D-49DF-8034-FB6A68BC5ED1} — also proven
// from an Xbox One title guest (it builds this IID on the stack before
// calling GetActivationFactoryByPCWSTR).
static const IID IID_ScCoreApplication = {
    0x0AACF7A4, 0x5E1D, 0x49DF,
    {0x80, 0x34, 0xFB, 0x6A, 0x68, 0xBC, 0x5E, 0xD1}};
// IFrameworkViewSource {CD770614-65C4-426C-9494-34FC43554862}
static const IID IID_ScFrameworkViewSource = {
    0xCD770614, 0x65C4, 0x426C,
    {0x94, 0x94, 0x34, 0xFC, 0x43, 0x55, 0x48, 0x62}};
// IFrameworkView {FAAB5CD0-8924-45AC-AD0F-A08FAE5D0324}
static const IID IID_ScFrameworkView = {
    0xFAAB5CD0, 0x8924, 0x45AC,
    {0xAD, 0x0F, 0xA0, 0x8F, 0xAE, 0x5D, 0x03, 0x24}};
// ICoreApplicationView {638BB2DB-451D-4661-B099-414F34FFB9F1}
static const IID IID_ScCoreApplicationView = {
    0x638BB2DB, 0x451D, 0x4661,
    {0xB0, 0x99, 0x41, 0x4F, 0x34, 0xFF, 0xB9, 0xF1}};
// ICoreWindow {79B9D5F2-879E-4B89-B798-79E47598030C}
static const IID IID_ScCoreWindow = {
    0x79B9D5F2, 0x879E, 0x4B89,
    {0xB7, 0x98, 0x79, 0xE4, 0x75, 0x98, 0x03, 0x0C}};
// ICoreDispatcher {60DB2FA8-B705-4FDE-A7D6-EBBB1891D39E}
static const IID IID_ScCoreDispatcher = {
    0x60DB2FA8, 0xB705, 0x4FDE,
    {0xA7, 0xD6, 0xEB, 0xBB, 0x18, 0x91, 0xD3, 0x9E}};
// IActivationFactory {00000035-0000-0000-C000-000000000046}
static const IID IID_ScActivationFactory = {
    0x00000035, 0x0000, 0x0000,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
// ICoreApplication2 {998681FB-1AB6-4B7F-BE4A-9A0645224C04}
static const IID IID_ScCoreApplication2 = {
    0x998681FB, 0x1AB6, 0x4B7F,
    {0xBE, 0x4A, 0x9A, 0x06, 0x45, 0x22, 0x4C, 0x04}};
// ICoreApplication3 {FEEC0D39-598B-4507-8A67-772632580A57}
static const IID IID_ScCoreApplication3 = {
    0xFEEC0D39, 0x598B, 0x4507,
    {0x8A, 0x67, 0x77, 0x26, 0x32, 0x58, 0x0A, 0x57}};
// IAsyncAction {5A648006-843A-4DA9-865B-9D26E5DFAD7B} — the REAL
// Windows.Foundation.IAsyncAction IID. IScAsyncAction is the layer's
// stand-in for it (real vtable layout, see below), so a guest QI'ing
// for IAsyncAction gets a working object instead of E_NOINTERFACE.
static const IID IID_ScAsyncAction = {
    0x5A648006, 0x843A, 0x4DA9,
    {0x86, 0x5B, 0x9D, 0x26, 0xE5, 0xDF, 0xAD, 0x7B}};

// ---- ABI value types (layout-identical to the SDK structs) ----
struct ScRect {
    float X, Y, Width, Height;
};
struct ScPoint {
    float X, Y;
};
struct ScEventToken {
    UINT64 value;
};

// CoreProcessEventsOption (SDK 10.0.26100.0 values — note
// ProcessOneAndAllPending=0 and ProcessUntilQuit=2, which differ from
// the older documented order).
enum ScProcessEventsOption {
    kProcessOneAndAllPending = 0,
    kProcessOneIfPresent = 1,
    kProcessUntilQuit = 2,
    kProcessAllIfPresent = 3,
};
// CoreDispatcherPriority.
enum ScDispatcherPriority {
    kPriorityIdle = -2,
    kPriorityLow = -1,
    kPriorityNormal = 0,
    kPriorityHigh = 1,
};
// AsyncStatus.
enum ScAsyncStatus {
    kAsyncStarted = 0,
    kAsyncCompleted = 1,
    kAsyncCanceled = 2,
    kAsyncError = 3,
};

// ---- ABI interfaces (hand-rolled; vtable order per the SDK headers) ----
struct IScFrameworkView;
struct IScCoreWindow;
struct IScCoreApplicationView;
struct IScCoreDispatcher;
struct IScAsyncAction;

// IFrameworkViewSource: IInspectable + CreateView (6).
struct IScFrameworkViewSource : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateView(
        IScFrameworkView** view) = 0;
};
// IFrameworkView: IInspectable + Initialize (6), SetWindow (7),
// Load (8), Run (9), Uninitialize (10).
struct IScFrameworkView : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Initialize(
        IScCoreApplicationView* applicationView) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetWindow(IScCoreWindow* window) = 0;
    virtual HRESULT STDMETHODCALLTYPE Load(HSTRING entryPoint) = 0;
    virtual HRESULT STDMETHODCALLTYPE Run() = 0;
    virtual HRESULT STDMETHODCALLTYPE Uninitialize() = 0;
};
// ICoreApplicationView: IInspectable + get_CoreWindow (6),
// add_Activated (7), remove_Activated (8), get_IsMain (9),
// get_IsHosted (10).
struct IScCoreApplicationView : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_CoreWindow(
        IScCoreWindow** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Activated(
        IUnknown* handler, ScEventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Activated(
        ScEventToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsMain(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsHosted(boolean* value) = 0;
};
// IDispatchedHandler: IInspectable + Invoke (6).
struct IScDispatchedHandler : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Invoke() = 0;
};
// IAsyncActionCompletedHandler: IInspectable + Invoke (6).
struct IScAsyncActionCompletedHandler : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Invoke(IScAsyncAction* asyncInfo,
                                             int status) = 0;
};
// IAsyncAction: IInspectable + IAsyncInfo get_Id (6), get_Status (7),
// Cancel (8), Close (9) + put_Completed (10), get_Completed (11),
// GetResults (12).
struct IScAsyncAction : public IInspectable {
    // Windows.Foundation.IAsyncAction, real vtable layout (the
    // IAsyncInfo part first): get_Id(6), get_Status(7),
    // get_ErrorCode(8), Cancel(9), Close(10), put_Completed(11),
    // get_Completed(12), GetResults(13). An earlier draft omitted
    // get_ErrorCode, which shifted every later method one slot — fixed
    // 2026-11-23 (first build caught it: C2787 + C2737).
    virtual HRESULT STDMETHODCALLTYPE get_Id(unsigned int* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int* status) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(
        HRESULT* errorCode) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Completed(
        IScAsyncActionCompletedHandler* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(
        IScAsyncActionCompletedHandler** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults() = 0;
};
// ICoreDispatcher: IInspectable + get_HasThreadAccess (6),
// ProcessEvents (7), RunAsync (8), RunIdleAsync (9).
struct IScCoreDispatcher : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_HasThreadAccess(
        boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE ProcessEvents(int options) = 0;
    virtual HRESULT STDMETHODCALLTYPE RunAsync(
        int priority, IScDispatchedHandler* agileCallback,
        IScAsyncAction** asyncInfo) = 0;
    virtual HRESULT STDMETHODCALLTYPE RunIdleAsync(
        IScDispatchedHandler* agileCallback,
        IScAsyncAction** asyncInfo) = 0;
};
// ICoreWindow: IInspectable + 6..57 in SDK order (see
// docs/WINRT-SHIM.md for the full table). Handler parameters are
// TypedEventHandler<CoreWindow*, ...>* — pointer-sized, declared here
// as IUnknown* because the shim stores (and for Closed/SizeChanged
// invokes) them through the handler's Invoke slot.
struct IScCoreWindow : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_AutomationHostProvider(
        IUnknown** value) = 0;                                    // 6
    virtual HRESULT STDMETHODCALLTYPE get_Bounds(ScRect* value) = 0;  // 7
    virtual HRESULT STDMETHODCALLTYPE get_CustomProperties(
        IUnknown** value) = 0;                                    // 8
    virtual HRESULT STDMETHODCALLTYPE get_Dispatcher(
        IScCoreDispatcher** value) = 0;                           // 9
    virtual HRESULT STDMETHODCALLTYPE get_FlowDirection(int* value) = 0;  // 10
    virtual HRESULT STDMETHODCALLTYPE put_FlowDirection(int value) = 0;  // 11
    virtual HRESULT STDMETHODCALLTYPE get_IsInputEnabled(
        boolean* value) = 0;                                      // 12
    virtual HRESULT STDMETHODCALLTYPE put_IsInputEnabled(
        boolean value) = 0;                                       // 13
    virtual HRESULT STDMETHODCALLTYPE get_PointerCursor(
        IUnknown** value) = 0;                                    // 14
    virtual HRESULT STDMETHODCALLTYPE put_PointerCursor(
        IUnknown* value) = 0;                                     // 15
    virtual HRESULT STDMETHODCALLTYPE get_PointerPosition(
        ScPoint* value) = 0;                                      // 16
    virtual HRESULT STDMETHODCALLTYPE get_Visible(boolean* value) = 0;  // 17
    virtual HRESULT STDMETHODCALLTYPE Activate() = 0;             // 18
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;                // 19
    virtual HRESULT STDMETHODCALLTYPE GetAsyncKeyState(
        int virtualKey, int* state) = 0;                          // 20
    virtual HRESULT STDMETHODCALLTYPE GetKeyState(
        int virtualKey, int* state) = 0;                          // 21
    virtual HRESULT STDMETHODCALLTYPE ReleasePointerCapture() = 0;  // 22
    virtual HRESULT STDMETHODCALLTYPE SetPointerCapture() = 0;    // 23
    virtual HRESULT STDMETHODCALLTYPE add_Activated(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 24
    virtual HRESULT STDMETHODCALLTYPE remove_Activated(
        ScEventToken cookie) = 0;                                 // 25
    virtual HRESULT STDMETHODCALLTYPE add_AutomationProviderRequested(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 26
    virtual HRESULT STDMETHODCALLTYPE remove_AutomationProviderRequested(
        ScEventToken cookie) = 0;                                 // 27
    virtual HRESULT STDMETHODCALLTYPE add_CharacterReceived(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 28
    virtual HRESULT STDMETHODCALLTYPE remove_CharacterReceived(
        ScEventToken cookie) = 0;                                 // 29
    virtual HRESULT STDMETHODCALLTYPE add_Closed(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 30
    virtual HRESULT STDMETHODCALLTYPE remove_Closed(
        ScEventToken cookie) = 0;                                 // 31
    virtual HRESULT STDMETHODCALLTYPE add_InputEnabled(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 32
    virtual HRESULT STDMETHODCALLTYPE remove_InputEnabled(
        ScEventToken cookie) = 0;                                 // 33
    virtual HRESULT STDMETHODCALLTYPE add_KeyDown(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 34
    virtual HRESULT STDMETHODCALLTYPE remove_KeyDown(
        ScEventToken cookie) = 0;                                 // 35
    virtual HRESULT STDMETHODCALLTYPE add_KeyUp(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 36
    virtual HRESULT STDMETHODCALLTYPE remove_KeyUp(
        ScEventToken cookie) = 0;                                 // 37
    virtual HRESULT STDMETHODCALLTYPE add_PointerCaptureLost(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 38
    virtual HRESULT STDMETHODCALLTYPE remove_PointerCaptureLost(
        ScEventToken cookie) = 0;                                 // 39
    virtual HRESULT STDMETHODCALLTYPE add_PointerEntered(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 40
    virtual HRESULT STDMETHODCALLTYPE remove_PointerEntered(
        ScEventToken cookie) = 0;                                 // 41
    virtual HRESULT STDMETHODCALLTYPE add_PointerExited(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 42
    virtual HRESULT STDMETHODCALLTYPE remove_PointerExited(
        ScEventToken cookie) = 0;                                 // 43
    virtual HRESULT STDMETHODCALLTYPE add_PointerMoved(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 44
    virtual HRESULT STDMETHODCALLTYPE remove_PointerMoved(
        ScEventToken cookie) = 0;                                 // 45
    virtual HRESULT STDMETHODCALLTYPE add_PointerPressed(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 46
    virtual HRESULT STDMETHODCALLTYPE remove_PointerPressed(
        ScEventToken cookie) = 0;                                 // 47
    virtual HRESULT STDMETHODCALLTYPE add_PointerReleased(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 48
    virtual HRESULT STDMETHODCALLTYPE remove_PointerReleased(
        ScEventToken cookie) = 0;                                 // 49
    virtual HRESULT STDMETHODCALLTYPE add_TouchHitTesting(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 50
    virtual HRESULT STDMETHODCALLTYPE remove_TouchHitTesting(
        ScEventToken cookie) = 0;                                 // 51
    virtual HRESULT STDMETHODCALLTYPE add_PointerWheelChanged(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 52
    virtual HRESULT STDMETHODCALLTYPE remove_PointerWheelChanged(
        ScEventToken cookie) = 0;                                 // 53
    virtual HRESULT STDMETHODCALLTYPE add_SizeChanged(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 54
    virtual HRESULT STDMETHODCALLTYPE remove_SizeChanged(
        ScEventToken cookie) = 0;                                 // 55
    virtual HRESULT STDMETHODCALLTYPE add_VisibilityChanged(
        IUnknown* handler, ScEventToken* cookie) = 0;             // 56
    virtual HRESULT STDMETHODCALLTYPE remove_VisibilityChanged(
        ScEventToken cookie) = 0;                                 // 57
};

// ---- Layer API (called by the loader) ----
// vccorlib110's activation helper (the function guests import).
// Declared here so guests can call it without the C++/CX headers;
// the import lib is generated from defs/vccorlib110.def.
extern "C" HRESULT __stdcall GetActivationFactoryByPCWSTR(
    PCWSTR activatableClassId, REFIID iid, void** factory);
typedef HRESULT(__stdcall* WinRtGetActivationFactoryByPCWSTRFn)(
    PCWSTR activatableClassId, REFIID iid, void** factory);

// IAT hook target for vccorlib110!GetActivationFactoryByPCWSTR.
// Returns the layer CoreApplication factory for
// "Windows.ApplicationModel.Core.CoreApplication"; forwards every
// other class to the real vccorlib110 function stashed by the loader.
extern "C" HRESULT ScGetActivationFactoryByPCWSTR(
    PCWSTR activatableClassId, REFIID iid, void** factory);

// IAT hook target for combase!RoGetActivationFactory. Routes
// CoreApplication to the layer factory; delegates everything else to
// EraRoGetActivationFactory (phase 5 behaviour preserved).
extern "C" HRESULT WinRtRoGetActivationFactory(HSTRING activatableClassId,
                                               REFIID iid, void** factory);

// Stash the real vccorlib110 function (called by the loader when it
// installs the IAT hook).
void WinRtSetRealGetActivationFactoryByPCWSTR(
    WinRtGetActivationFactoryByPCWSTRFn realFn);

// The layer CoreWindow's Win32 HWND (null before the first Run).
// The swapchain path needs a real presentable window; see
// docs/WINRT-SHIM.md.
HWND WinRtCoreWindowHwnd();

}  // namespace sc
