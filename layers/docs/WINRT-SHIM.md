# WinRT activation routing + CoreApplication shim

## Why this exists

An Xbox One title boot got past the VirtualAlloc crash and died at the
title's main-loop entry: the guest's
`CoreApplication::Run(viewSource)` returned **E_INVALIDARG** from the
real desktop WinRT, which rejects an unpackaged, manually-mapped Xbox
title with no package identity. The guest's C++/CX handler threw an
uncaught `Platform::InvalidArgumentException`.

The activation itself had already **succeeded** — via vccorlib110's
`GetActivationFactoryByPCWSTR`, not a direct `RoGetActivationFactory`
import (an Xbox One title that activates via vccorlib110 has no such
import; the loader's old warning "no RoGetActivationFactory import
found (WinRT routing not installed)" was therefore misleading and is
fixed in this phase). The gap is the app-model object itself: the real
`CoreApplication` refuses to run an unpackaged title. So the layer
intercepts activation and provides its own `ICoreApplication` whose
`Run` actually drives the guest view's lifecycle.

## The critical constraint

**Returning S_OK from Run does NOT work.** The guest's `Main`
returns 0 right after `Run` returns (proven by disassembly of an Xbox
One title's activation path), so an early S_OK would end the game
without running it. `Run` must drive the main loop: it returns only
when the view exits.

## Interception points (loader, pe_loader.cpp HookWinRtActivation)

Both installed for ERA titles only (the `eraDetected` gate), after
import resolution (the FT holds the real addresses) and before section
protections:

| Guest import | Patched to | Behaviour |
|---|---|---|
| `combase!RoGetActivationFactory` — in practice recorded under the WinRT API set `api-ms-win-core-winrt-l1-1-0.dll` by runtimeobject.lib (the hook matches `combase.dll`, `api-ms-win-core-com-l1-1-0.dll` and that API-set name; the first build proved the descriptor name with dumpbin — matching only the first two missed the hook entirely, and the era guest's phase-5 WinRT stage was a false pass on the real RoGet) | `WinRtRoGetActivationFactory` | `Windows.ApplicationModel.Core.CoreApplication` → layer factory; everything else → `EraRoGetActivationFactory` (phase 5 behaviour preserved: Xbox-namespace refusal + forward to the real function) |
| `vccorlib110!GetActivationFactoryByPCWSTR` — **the path an Xbox One title actually uses**, exported by the real DLL ONLY under its C++ mangled name (`?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid@Platform@@PEAPEAX@Z`, undname: `long __cdecl GetActivationFactoryByPCWSTR(void*, Platform::Guid&, void**)`); the hook matches both the mangled name and the plain name | `ScGetActivationFactoryByPCWSTR` | CoreApplication → layer factory; everything else → the real function (stashed from the IAT at hook time; E_FAIL if no real function was stashed) |

Both hooks are logged; a missing hook is a note, not a failure (the
guest may simply not use that path). Only if *neither* hook installs
does the loader log the "WinRT routing not installed" warning.

Non-ERA (GDK) titles are NOT hooked yet — the phase 5 gate is kept.
A Game Core title also uses `CoreApplication::Run`, so extending the
hooks to non-ERA titles is a follow-up once the ERA path is proven.

## The shim (src/winrt_shim.cpp, include/winrt_shim.h)

Hand-rolled ABI interfaces — no SDK header dependency. Layouts and
GUIDs grounded in SDK 10.0.26100.0 `winrt\windows.applicationmodel.core.h`
and `winrt\windows.ui.core.h` (vtable order + GUIDs read from those
headers on the build machine; `ICoreApplication::Run` at vtable index
13 also proven independently by disassembly of an Xbox One title's
activation path). Every
ABI method returns HRESULT, as the MIDL headers declare.

### CoreApplication (singleton)

One object implements `IActivationFactory` + `ICoreApplication` +
`ICoreApplication2` + `ICoreApplication3`. This matters because an
Xbox One title guest asks `GetActivationFactoryByPCWSTR` for
`IID_ICoreApplication` and calls `Run` on the returned pointer
directly — it never calls `ActivateInstance` — so the factory pointer
MUST QI to `ICoreApplication`.

- `get_Id` → HSTRING `"ScarlettCompat.CoreApplication"` (marker).
- `get_Properties` → **E_NOTIMPL** (documented gap — no `IPropertySet`
  yet; if a boot shows the guest needs it, implement it).
- `GetCurrentView` → the singleton app view (created on demand).
- `Run(viewSource)` — the lifecycle (see below).
- `RunWithActivationFactories` → delegates to `Run`.
- Events (`Suspending`/`Resuming`/`BackgroundActivated`/
  `LeavingBackground`/`EnteredBackground`) → dummy tokens, never
  fired. `EnablePrelaunch` → S_OK.
- `RequestRestartAsync`/`RequestRestartForUserAsync` → a completed
  `IAsyncAction` (the shim never actually restarts).
- Unknown-IID QI attempts are **logged** (the boot log reveals what
  the guest asks for).

### Run — the lifecycle

```
1. viewSource->CreateView(&view)          // guest view object
2. view->Initialize(appView)              // ICoreApplicationView
3. view->SetWindow(coreWindow)            // the layer's CoreWindow
4. view->Load(L"")                        // empty entry-point string
5. view->Run()                            // THE GAME LOOP — returns
                                          // only when the view exits
6. view->Uninitialize()                   // best effort, even if Run
                                          // failed
```

A failure at steps 1–4 returns that HRESULT immediately. `Run` is
NOT wrapped in a C++ catch: if the guest's view throws, the exception
propagates to the guest's own `Main` handler — the same as real WinRT
would do (the real `Run` reports unhandled errors to the guest's
`UnhandledErrorDetected`, it does not swallow them).

### CoreWindow (real Win32 window)

`ScCoreWindow` creates a real Win32 window (1280×720 client area,
class `ScarlettCompatCoreWindow`), hidden until `Activate()` shows it
(the standard UWP pattern — the view calls `Activate` during setup).
It owns a `CoreDispatcher` and stores event handlers:

- `get_Bounds` → the client rect (updated on `WM_SIZE`).
- `get_Dispatcher` → the window's dispatcher.
- `get_Visible` → true after `Activate`.
- `Close` → `DestroyWindow`, which fires `WM_DESTROY` synchronously.
- `WM_DESTROY` → invokes the registered `Closed` handlers with an
  `ICoreWindowEventArgs`-compatible args object, then
  `PostQuitMessage(0)` — the view's exit signal to the pump.
- `SizeChanged` handlers are stored but **not invoked** (the harness
  window size is fixed); `Bounds` is updated on resize.
- All other events (`Activated`, `KeyDown`, `PointerMoved`, …) are
  stored with a token and never fired — the shim has no input source
  yet. Registration itself always succeeds, so a guest that subscribes
  and waits on an event it never gets will hang the same way it would
  on a real device with no input; input is a follow-up.

### CoreDispatcher (Win32 message pump)

`ScCoreDispatcher` runs the pump on the calling thread (the same
thread the guest's `Run` runs on — no threading):

- `ProcessEvents(ProcessOneAndAllPending)` → one `GetMessage` +
  dispatch, then drain the queued `RunAsync` handlers.
- `ProcessEvents(ProcessOneIfPresent)` → one `PeekMessage` if present,
  then the queued handlers.
- `ProcessEvents(ProcessUntilQuit)` → `GetMessage` loop until
  `WM_QUIT`, running queued handlers as they arrive.
- `ProcessEvents(ProcessAllIfPresent)` → drain all pending messages +
  queued handlers.
- `RunAsync`/`RunIdleAsync` → the handler is queued and run by the
  next `ProcessEvents`; the returned `IAsyncAction` completes when the
  handler has run (a minimal action object: `get_Status`/`get_ErrorCode`/
  `Completed` handler storage, `Cancel` is a no-op). The action object
  uses the **real** `Windows.Foundation.IAsyncAction` IID
  ({5A648006-843A-4DA9-865B-9D26E5DFAD7B}) and vtable layout
  (`get_Id`, `get_Status`, `get_ErrorCode`, `Cancel`, `Close`,
  `put_Completed`, `get_Completed`, `GetResults`) — an earlier draft
  omitted `get_ErrorCode`, which shifted every later method one slot;
  the first build caught it (C2787 + C2737).

`CoreProcessEventsOption` values in this SDK: `ProcessOneAndAllPending=0`,
`ProcessOneIfPresent=1`, `ProcessUntilQuit=2`, `ProcessAllIfPresent=3`
— this order differs from older documentation; it was read from the
SDK header, not assumed.

### CoreApplicationView

Holds the `CoreWindow`; `get_CoreWindow` returns it, `IsMain` → TRUE,
`IsHosted` → FALSE, `Activated` add/remove → dummy tokens.

## The swapchain question (open, follow-up)

How the guest's view obtains an `HWND` for its swapchain is not
solved here. `D3D11XCreateDeviceAndSwapChain`'s `OutputWindow` may be
null/dummy on Xbox; substituting the CoreWindow's HWND would need an
`era_to_gdk.cpp` change, which is off-limits for this phase.
`WinRtCoreWindowHwnd()` exposes the HWND for that follow-up. The
guest_winrt proof guest does not create a swapchain.

## Per-import failure logging (same phase)

The loader now logs the exact failed import
(`IMPORT FAILED: <module>!<name>` or `<module>:#<ordinal>`) instead of
only the count, so a boot pins down failures by name.

## Proof (tests/guest_winrt.cpp, RC8)

The guest is ERA-shaped (one `kernelx` import, `XMemAlloc`) so the
loader installs the WinRT hooks, and it links `vccorlib110.lib`
(generated from `defs/vccorlib110.def` — a **name** import on purpose:
the real DLL's export ordinal is unknown and an ordinal import would
fail to load if the number were wrong) + `runtimeobject.lib` (direct
`RoGetActivationFactory`).

Stages (`winrt_<stage>` in `guest_report.txt`):

| Stage | Proves |
|---|---|
| `winrt_era` | ERA shape: `XMemAlloc(64)` non-null through the kernelx translation |
| `winrt_roget` | Direct `RoGetActivationFactory(CoreApplication)` returns the layer factory (S_OK, non-null) — through the hooked `api-ms-win-core-winrt-l1-1-0.dll` import |
| `winrt_activation` | `vccorlib110!GetActivationFactoryByPCWSTR(CoreApplication)` returns the layer factory (the Xbox One title activation path) — imported by its mangled name, like an Xbox One title's import |
| `winrt_same_factory` | Both activation paths return the **same** singleton (identical `ICoreApplication` pointers) |
| `winrt_qi2` | The factory also exposes `ICoreApplication2` |
| `winrt_id` | `get_Id` returns `"ScarlettCompat.CoreApplication"` |
| `winrt_view` | `GetCurrentView` → CoreWindow 1280×720 + Dispatcher, `IsMain` true, `IsHosted` false |
| `winrt_run` | `Run` returns S_OK **and** took ≥ 400 ms (the test view pumps ~500 ms then closes its window — Run must drive the loop, not return early) |
| `winrt_lifecycle` | Exact sequence `Initialize,SetWindow,Load,Run,Uninitialize` + `Activate` returned S_OK and the window was visible after `Activate` |

The guest's test view records the lifecycle in a file-static string
(the view object is owned by the layer's `Run` and deleted when `Run`
returns, so the guest must not read view state after `Run`).

## Verification results

**Clean build (WinRT phase, 2026-11-23 ~05:4x EST): BUILD_EXIT=0, all 10
verify runs ALL PASS** — minimal, ordinal, proof 17/17, era 21/21,
software-rt 7/7 under off / software / hardware-fallback, opt 13/13,
physics 7/7, winrt 9/9. Full evidence: `F:\ScarlettCompat\build_winrt_final.log`
(local copy of the log is in the build dir; the run-2 log with the TLS
flake is `F:\ScarlettCompat\build_winrt_run2.log`).

WinRT guest proof numbers (clean run):
- `winrt_era` PASS — XMemAlloc(64) non-null.
- `winrt_roget` PASS — direct `RoGetActivationFactory(CoreApplication)`
  → S_OK, factory `000002AEEA7A1460`.
- `winrt_activation` PASS — `vccorlib110!GetActivationFactoryByPCWSTR
  (CoreApplication)` → S_OK, factory `000002AEEA7A1468` (imported by its
  mangled name; the import now resolves: 73 resolved, 0 failed).
- `winrt_same_factory` PASS — both paths return the IDENTICAL
  `ICoreApplication` pointer (`000002AEEA7A1468`).
- `winrt_qi2` PASS — QI to `ICoreApplication2` S_OK.
- `winrt_id` PASS — `get_Id` → `"ScarlettCompat.CoreApplication"`.
- `winrt_view` PASS — CoreWindow bounds 1280×720, Dispatcher non-null,
  `IsMain`=1, `IsHosted`=0.
- `winrt_run` PASS — `Run` → S_OK after **516 ms** (test view pumps
  ~500 ms then closes its window — Run drove the loop, did not return
  early).
- `winrt_lifecycle` PASS — exact sequence
  `Initialize,SetWindow,Load,Run,Uninitialize`; `Activate` S_OK and the
  window visible after `Activate`.

Both loader hooks confirmed installed in the clean run
(`hooked vccorlib110!GetActivationFactoryByPCWSTR…`,
`hooked combase!RoGetActivationFactory…`).

**Xbox One title boot — the end-to-end verification for this phase.**
Result: **the shim moved the boot past the Run wall.** The vccorlib110
hook installed on the title; the title's activation returned the layer
factory; `CoreApplication::Run` entered the lifecycle; `CreateView`
returned the title's view; the layer CoreWindow was created (1280×720).
The title then died **inside its own `IFrameworkView::Initialize`** —
the next log line after the CoreWindow creation would have been `view
Initialize -> S_OK`; it never appears. The title still does not run —
but the failure moved from "Run rejected" to "Initialize threw". What
Initialize calls that throws is the next diagnosis.

Side finding:
- The era guest's phase-5 WinRT stage was a **false pass**: its
  `RoGetActivationFactory` import lives under
  `api-ms-win-core-winrt-l1-1-0.dll`, which the phase-5 hook did not
  match — the real RoGet refused the Xbox class and forwarded Uri on
  its own. The WinRT phase's hook fix (matching that API-set name)
  makes the era guest's WinRT stage genuinely layer-routed now; the
  era guest still passes 21/21.

## Known gaps / follow-ups

- `get_Properties` → E_NOTIMPL (no `IPropertySet`).
- `SizeChanged` handlers stored, never invoked.
- Input events stored, never fired (no input source yet).
- `RequestRestartAsync` completes without restarting.
- WinRT hooks are ERA-only; extend to non-ERA Game Core titles once
  the ERA path is proven.
- The guest view's swapchain HWND (`WinRtCoreWindowHwnd()` is the
  accessor for the follow-up).
- The guest's `Run` throwing a C++ exception propagates to the
  guest's `Main` (by design — same as real WinRT).
