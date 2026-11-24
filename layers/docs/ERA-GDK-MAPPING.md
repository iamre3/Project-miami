# ERA → GDK mapping (phase 5)

What an Xbox One (ERA) title actually imports, and what this layer does
with each entry point. ERA and GDK share lineage (XGameRuntime, XUser,
XGameSave, D3D), so much of the surface maps — but the two runtimes are
NOT the same API, and the gaps are documented here instead of faked.

Sources: the public Xbox One shim-DLL d3d11.x.cpp (XDK-derived
signatures for D3D11XCreateDeviceX, D3DMapEsramMemory,
DXGIXPresentArray, D3DAllocateGraphicsMemory); the Xbox One shim-DLL
exports for the ordinal layout; Microsoft Learn / docs.xbox.com GDK
docs (public); the GDK research notes (§1, §3).
Anything marked ASSUMED is not public: the real Xbox One XDK headers
and the real ERA ordinals are NDA-gated. No real ERA title, exe, or
data is IN this repo.

## The generation split (why a translation exists at all)

- ERA is the Xbox One runtime (XDK). Game Core / GDK is the Series X|S
  runtime. Microsoft's own GDK docs state: **"On Xbox, Microsoft Game
  Development Kit (GDK) supports only D3D12.x. D3D11.x is not
  supported. If you have a game that uses D3D11.x, you'll need to move
  to D3D12.x as part of moving to the GDK."** An ERA title therefore
  cannot run on the GDK path as-is — its D3D11.x calls have no GDK
  equivalent on Xbox, and its services arrive as WinRT classes, not
  flat C functions.
- The GDK porting-guides index lists the ERA→GDK migration as four
  things: **WinRT-style API replacements**, **async pattern migration
  (`IAsyncOperation` → `XAsyncBlock` or `XTaskQueue`)**, **memory model
  changes (ERA memory manager → `XMemAlloc`)**, and **packaging
  migration to MSIXVC**. This layer implements the same four, in that
  order of feasibility (see below).
- ERA saves are the ConnectedStorage API in the `Windows.Xbox.Storage`
  namespace; GDK saves are the flat C `XGameSave` API in `xgamesave.h`
  (XR-052). Same split for users (`Windows.Xbox.System.User` → `XUser`)
  and packages (`Windows.ApplicationModel.Package` / XVD → `XPackage` /
  MSIXVC).
- **ESRAM vs EDRAM (terminology):** an Xbox One title uses **ESRAM** — 32 MB of embedded SRAM on the Xbox One APU
  for render targets (see "ESRAM" below). **EDRAM** is the Xbox 360's
  10 MB embedded DRAM — that is the Pinyon/FH1 resolve problem, not
  this one. The two are different hardware on different consoles; the
  layer handles ESRAM here and has no EDRAM path.

## Classes

- **1:1 map** — same signature, same semantics, different module name.
  The translation is a rename.
- **Adaptation** — same concept, different shape. The translation does
  real work (different parameters, different async model, different API
  generation) and the difference is stated.
- **Shared** — identical module + name in ERA and GDK; the existing GDK
  implementation already serves it.
- **No GDK equivalent** — the ERA thing has no counterpart on the GDK
  path. The layer refuses it explicitly (E_NOTIMPL, logged) instead of
  pretending it works.

## The ERA import surface

An Xbox One (ERA) title reaches the layer through the ERA DLL set:
`kernelx.dll` (Win32 forwarding + Xbox memory functions), `d3d11_x.dll`
(`D3D11XCreateDeviceX`, `D3DMapEsramMemory`, `DXGIXPresentArray`),
`d3d12_x.dll`, `xg_x.dll` (texture-tiling), `AcpHal.dll` (audio-DSP),
plus the CRT / COM support DLLs (`MSVCR110`, `MSVCP110`,
`vccorlib110`, `combase`, `WS2_32`). ERA detection uses the import
set, not a `.xbld` section — an ERA title has no `.xbld`.

A D3D11.x-only ERA title imports no `d3d12_x.dll` and no
`xgameruntime.dll`; it reaches WinRT services via vccorlib110
`GetActivationFactoryByPCWSTR` + `CoCreateInstance` (C++/CX), not a
direct `RoGetActivationFactory` import — so the layer's
`RoGetActivationFactory` hook cannot fire for such a title. Services
arrive as WinRT classes activated at runtime, not static imports.

### Ordinal layout

ERA titles import by name, so the ordinal rows in the registry and the
`.def` files use the Xbox One shim-DLL export ordinals — the only real
ordinal layout available; the real console ordinals are unknown. The
shim-DLL numbers its exports sequentially from 1 in alphabetical order:

| Function | Xbox One shim-DLL ordinal |
|---|---|
| `d3d11_x!D3D11XCreateDeviceX` | 4 |
| `d3d11_x!D3DMapEsramMemory` | 9 |
| `d3d11_x!DXGIXPresentArray` | 11 |
| `xg_x!XGComputeOptimalDepthStencilTileModes` | 2 |
| `xg_x!XGComputeOptimalTileMode` | 3 |
| `xg_x!XGComputeTexture2DLayout` | 5 |
| `xg_x!XGComputeTexture3DLayout` | 6 |
| `xg_x!XGCreateTexture1DComputer` | 8 |
| `xg_x!XGCreateTexture2DComputer` | 9 |
| `xg_x!XGCreateTexture3DComputer` | 10 |
| `AcpHal!AcpHalAllocateShapeContexts` | 1 |
| `AcpHal!AcpHalCreate` | 2 |
| `AcpHal!AcpHalReleaseShapeContexts` | 3 |
| `AcpHal!ApuAlloc` | 4 |
| `AcpHal!ApuCreateHeap` | 5 |
| `AcpHal!ApuFree` | 6 |
| `AcpHal!ApuIsVirtualAddressValid` | 8 |
| `AcpHal!ApuMapApuAddress` | 9 |
| `AcpHal!ApuMapVirtualAddress` | 10 |
| `kernelx!TitleMemoryStatus` | 463 |
| `kernelx!XMemAlloc` | 514 |
| `kernelx!XMemAllocDefault` | 515 |
| `kernelx!XMemFree` | 517 |
| `kernelx!XMemFreeDefault` | 518 |
| `kernelx!XMemSetAllocationHooks` | 524 |

An earlier phase 5 pass wrote wrong ordinals into the registry's
ordinal rows and the `.def` files by mistake. They are corrected now
to the Xbox One shim-DLL ordinals; the real console ordinals are
unknown. The name imports resolve through the name table and never
touch the ordinal rows — the rows only matter for a title that imports
by ordinal. (The same correction retires the old
"AcpHalAllocateShapeContexts is ordinal 0 and cannot be expressed in
a .def" note: its Xbox One shim-DLL ordinal is 1.)

## Inventory — flat C entry points (grounded)

| ERA entry point | Class | GDK target / what the layer does |
|---|---|---|
| `kernelx.dll!XMemAlloc` | 1:1 map | `xmem.dll!XMemAlloc` (`HleXMemAlloc`, real HeapAlloc). |
| `kernelx.dll!XMemFree` | 1:1 map | `xmem.dll!XMemFree` (`HleXMemFree`). |
| `kernelx.dll!XMemAllocDefault` | 1:1 map | Same real HeapAlloc (`EraXMemAllocDefault`). |
| `kernelx.dll!XMemFreeDefault` | 1:1 map | Same real HeapFree (`EraXMemFreeDefault`). |
| `kernelx.dll!XMemSetAllocationHooks` | Adaptation | Accepted + logged, returns S_OK, **not routed** — the layer's allocations do not call title hooks. |
| `kernelx.dll!TitleMemoryStatus` | Adaptation | ASSUMED one-pointer form: zero-fills 64 bytes, returns 0. Zeros mean "unknown", not a fabricated budget. |
| `kernelx.dll!` + 106 Win32 names | Forwarding | `GetProcAddress(kernel32.dll, name)` at load time (cached handle). The era guest exercises the forwards it imports. |
| `d3d11_x.dll!D3D11XCreateDeviceX` | Adaptation | Desktop `D3D11CreateDevice` (real D3D11 device). **3-arg form** (the public Xbox One shim-DLL d3d11.x.cpp, XDK-derived): `(D3D11X_CREATE_DEVICE_PARAMETERS*, ppDevice, ppImmediateContext)` — NOT the 11-arg D3D11CreateDevice-shaped form phase 5 first assumed (that was wrong; an 11-arg callee would read stack garbage for the device out-pointer). The params struct's ESRAM/display/ring fields are logged, not acted on. |
| `d3d11_x.dll!D3D11XCreateDevice` | Adaptation | Desktop `D3D11CreateDevice` (10-arg form, matching the public Xbox One shim-DLL `EraD3D11CreateDevice`). Supporting entry; the proof guest does not import it. |
| `d3d11_x.dll!D3D11XCreateDeviceAndSwapChain` | Adaptation | Desktop `D3D11CreateDeviceAndSwapChain`. Same note as above; the proof guest does not import it. |
| `d3d12_x.dll!D3D12XCreateDevice` | Adaptation | `d3d12_xs.dll!D3D12XboxCreateDevice` path (same real-device passthrough). A D3D11.x-only ERA title does not import d3d12_x. |
| `d3d11_x.dll!D3DMapEsramMemory` | Adaptation | **Implemented** (see "ESRAM" below): maps real committed memory at the requested VA, 32 MB arena budget enforced. Signature from the public Xbox One shim-DLL d3d11.x.cpp. |
| `d3d11_x.dll!DXGIXPresentArray` | Adaptation | **Implemented**: presents every swapchain in the array through the real desktop swapchains. Signature from the public Xbox One shim-DLL d3d11.x.cpp. Per-swapchain present parameters (source rect, scale) are accepted and ignored — the full back buffer is presented. |
| `d3d11_x.dll!D3DAllocateGraphicsMemory` | Adaptation | **Implemented** (supporting): VirtualAlloc-backed graphics-memory allocator, shape from the public Xbox One shim-DLL d3d11.x.cpp. Supporting entry; exercised by the era guest's `era_esram` stage. |
| `d3d11_x.dll!D3DFreeGraphicsMemory` | Adaptation | **Implemented** (supporting): `VirtualFree` of a graphics-memory allocation. Same source as above. |
| `XAudio2_9.dll!XAudio2Create` | Shared | Same module + name in ERA and GDK; the existing stub serves both. Real audio (XMA2 decode) is 0% in both. |
| `xg_x.dll` (all 7) | No GDK equivalent | Xbox One texture-tiling / graphics helpers (`XG*`). Explicit E_NOTIMPL + log. The proof guest uses linear (untiled) resources, so tiling is not exercised. |
| `AcpHal.dll` (all 9) | No GDK equivalent | Audio Co-Processor hardware abstraction layer — console hardware, no host counterpart. Same E_NOTIMPL + log treatment as `xg_x.dll`. |

Counts (flat C, ERA entry points the layer translates): **4 mapped
1:1, 10 adapted, 1 shared, 16 with no equivalent** (7 XG + 9 AcpHal).
Plus the kernelx Win32 forwarding rule (106 names, one rule, not
per-function translation). 13 ERA entry points are real
implementations; 0 are faked.

## ESRAM — how Xbox One ESRAM translates to desktop D3D11/D3D12

**The hardware.** The Xbox One APU carries **32 MB of ESRAM**
(embedded SRAM, ~102 GB/s) alongside 8 GB of DDR3 system memory
(68 GB/s). ESRAM is a separate, explicitly managed pool: render
targets and depth buffers that fit live there; anything bigger is
tiled and resolved to DRAM (the same tiling/resolve pattern as the
Xbox 360's 10 MB EDRAM, but ESRAM is a different, larger, SRAM pool
on a different console — the two must not be conflated). 32 MB fits
roughly a 1080p 32bpp 2xMSAA target or a 720p 4xMSAA target; larger
targets spill to DDR3. The Xbox One X dropped ESRAM entirely for
unified memory; the Series X has no embedded memory
either — unified GDDR6 — so this section is ERA-specific.

**The API surface.** An ERA title maps ESRAM through one entry
point: `D3DMapEsramMemory` (Xbox One shim-DLL ordinal 9). Its
signature (the public Xbox One shim-DLL d3d11.x.cpp) is:

```c
HRESULT D3DMapEsramMemory(UINT Flags, void* pVirtualAddress,
                          UINT NumPages, const UINT* pPageList);
```

It maps ESRAM pages into the title's virtual address space: `Flags`
bit 0 = large pages, bit 1 = 4 MB pages; `pPageList` holds the ESRAM
page numbers (4 KB pages) to map at `pVirtualAddress`. On the console
it delegates to `kernelx!MapTitleEsramPages` (Xbox One shim-DLL
ordinal 318). There is no `D3DUnmapEsramMemory` in the ERA surface. ESRAM *placement* of
render targets (which target lives in ESRAM) is a driver-side
decision on the console, not a separate import — the title creates
D3D11.x resources and the driver places them.

**The translation (what the layer does).** On a PC there is no
ESRAM, so the layer translates ESRAM placements to regular GPU
memory:

1. **Render targets / resources:** an ERA D3D11.x render target is
   created as an ordinary desktop D3D11 texture in GPU memory
   (`D3D11_USAGE_DEFAULT`, the real device). There is no 32 MB limit
   on the host, so the console's spill path (tile + resolve to DRAM)
   is unnecessary — but the *resolve semantics* are preserved: the
   data is already where the resolve reads it, so `ResolveSubresource`
   / `CopyResource` on the texture is a real copy with exactly the
   console resolve's result. The era guest proves this with a
   single-sample target (so the resolve is `CopyResource`); an MSAA
   `ResolveSubresource` is the same D3D11 call on the same resource
   and is unexercised.
2. **CPU mapping (`D3DMapEsramMemory`):** the layer maps REAL
   committed memory at the requested VA (`VirtualAlloc`) and enforces
   the 32 MB arena budget with a counter — a mapping that would push
   the total past 32 MB fails with `E_OUTOFMEMORY`, the same wall the
   console has. Page numbers are validated against the arena
   (0..8191) and logged. Large-page flag bits are accepted and
   logged; the mapping itself is 4 KB pages.
3. **Correctness issue — CPU writes vs GPU visibility:** the mapped
   window is system RAM, not GPU memory. A title that writes through
   the window and then expects the GPU to see the data must copy it
   into a D3D11 resource itself (`UpdateSubresource` / `CopyResource`)
   — exactly as the era guest proves (ESRAM window → texture →
   staging → readback, center pixel red). The layer does not
   auto-upload: it cannot know when the title is done writing.
4. **Honest limits:** (a) two VAs mapping the same ESRAM page are
   independent copies on the host (no VA aliasing), where the console
   aliases them to one physical page — a title that relies on aliasing
   would see two copies; (b) the mapped memory is never unmapped
   (there is no unmap import), so the arena counter only grows within
   a process; (c) the D3D12 route is the same principle — an ESRAM
   placement would land in a `D3D12` DEFAULT-heap committed resource,
   and the era guest's `era_gfx12` stage proves the D3D12.x device
   path, but no D3D12 ESRAM mapping is implemented because a D3D11.x-
   only ERA title does not use D3D12.

**Verification (era guest, `era_esram` stage).** Maps 4 ESRAM pages
(16 KB) at a reserved VA, writes a red RGBA pattern through the
window and reads it back; maps a second 4 KB window at a different
VA; proves the 32 MB budget (an 8193-page request fails
`E_OUTOFMEMORY`) and page-range validation (page 8192 fails
`E_INVALIDARG`); exercises `D3DAllocateGraphicsMemory` /
`D3DFreeGraphicsMemory` (alloc, write, read back, free); uploads the
ESRAM window into a render-target texture, copies it to staging, and
pixel-verifies the center is red. See docs/STATUS.md for the run
evidence.

## Inventory — WinRT services (the part that is NOT flat C)

ERA titles reach users, saves, packages, and input through WinRT
classes, activated with `RoGetActivationFactory` — not through flat
imports. There is no ERA flat entry point to translate for these; the
translation is a WinRT → flat C adaptation, and it comes in two pieces:

| ERA side (WinRT) | GDK target (flat C, already implemented) | Phase 5 status |
|---|---|---|
| `Windows.Xbox.System.User` (sign-in, identity) | `XUserAddAsync` / `XUserAddResult` / `XUserGetId` / `XUserGetGamertag` / `XUserGetLocalId` / `XUserCloseHandle` (+ `XAsyncBlock` sync completion) | **Correspondence documented; activation plumbing future work.** The proof guest exercises the GDK target directly and says so in its stage detail. |
| `Windows.Xbox.Storage` ConnectedStorage (saves) | `XGameSave*` provider/container/update flow + `XGameSaveFiles*` | Same as above. |
| `Windows.ApplicationModel.Package` / XVD mount (packaging) | `XPackage*` (mount path = guest dir, synthetic package id) | Same as above. Packaging migration (XVD/AppX → MSIXVC) is not implemented; the layer presents the guest dir as the mount, exactly as on the GDK side. |
| `Windows.Xbox.Input` (+ XInput) | `GameInputCreate` (stub) | Same as above. |
| Async pattern `IAsyncOperation` | `XAsyncBlock` (sync completion model) | Documented as the migration; the layer's sync model already matches what a title observes from the GDK side. |

**What phase 5 DOES implement for WinRT:** the activation routing.
The loader hooks the guest's `combase.dll!RoGetActivationFactory`
import (same IAT-hook technique as the `CreateThread` hook) and the
layer's `EraRoGetActivationFactory` intercepts Xbox-namespace class
names (`Windows.Xbox.*`, `Windows.ApplicationModel.Package`): they are
refused with `REGDB_E_CLASSNOTREG` and logged, because the layer has
no COM objects for them yet. Every other class name forwards to the
real `RoGetActivationFactory`, so normal WinRT keeps working. This
proves the routing exists; it does not fake the objects.

**What blocks the COM objects:** the WinRT interface definitions
(IIDs, vtable layouts) for the `Windows.Xbox.*` classes are XDK-gated.
Implementing a COM object with a guessed vtable would be faking it.
The objects need either the XDK headers or a real ERA exe's WinRT
class references to define — same class of blocker as the real GDK
ordinals needing a real title exe.

**ERA caveat:** an ERA title that activates WinRT classes via
vccorlib110 `GetActivationFactoryByPCWSTR` + `CoCreateInstance`
(C++/CX) does NOT import `RoGetActivationFactory` at all, so the
activation hook cannot fire for it; the era guest still proves the
routing exists (its `era_winrt` stage passes with the hook installed,
and the same stage passes with the real function — the outcomes are
identical for the two classes tested, so the stage does not prove the
hook works; see docs/STATUS.md).

## Inventory — things with no host equivalent at all

- **ESRAM capacity / spill:** the host has no 32 MB pool, so the
  console's spill path (tile + resolve to DRAM when a target does not
  fit) never triggers on PC — the layer enforces the budget only on
  the `D3DMapEsramMemory` mapping path (see "ESRAM" above). A title
  that sizes its targets to fit 32 MB on console gets the same pixels
  on PC; a title that depends on the spill *timing* (not the pixels)
  will not see it.
- **ERA packaging (XVD/AppX):** the layer does not parse XVD
  containers. Mount = guest directory, package id synthetic — same
  treatment as the GDK side.
- **Unified memory / `MEM_GRAPHICS`:** the same open question as on
  the Series X side (see `docs/ARCHITECTURE.md`). ERA titles share
  CPU/GPU memory on the console APU; desktop has separate RAM/VRAM.
- **Precompiled ERA shaders:** ERA D3D11.x shaders are DXBC (SM 5.0)
  and are directly consumable by desktop D3D11 — that is an advantage
  of the D3D11.x path over the D3D12 path, where precompiled shaders
  need the DXIL carving/lifting route (same as Series X). The proof
  guest compiles HLSL at runtime (`D3DCompile`), so no lifting is
  exercised in phase 5.

## Loader detection

A title is ERA when it imports any of `d3d11_x.dll`, `d3d12_x.dll`,
`kernelx.dll`, `xg_x.dll`, `AcpHal.dll`. The loader logs
`era <module>!<fn> -> <target>` for translated imports,
`era <module>!<fn> -> no GDK equivalent (E_NOTIMPL stub)` for refused
ones, and `ERA title detected` once. ERA modules are in the shim list,
so they never go through `LoadLibrary` (none of them exist on desktop
Windows). A mixed title (ERA graphics + GDK service imports, like the
synthetic proof guest) is detected as ERA and its GDK imports resolve
through the normal registry path. The loader also forwards kernelx
Win32 names to kernel32.dll (see the inventory table) and installs
the WinRT activation hook for ERA titles only.

## Verification (phase 5 bar)

`tests/guest_era.cpp` — a synthetic Xbox One-shaped PE (ERA DLL import
set, an Xbox One title's boot shape: runtime init → sign-in → save load early →
package/DLC check → memory → device → audio/input → storage → triangle
scene → frame loop → ESRAM → present array → save round-trip →
package mount). It boots THROUGH the translation into the GDK layer
and renders a real frame. Harness: `era` subsystem report in
`src/verify.cpp` (same style as the `proof` report block). Bar: clean
rebuild `BUILD_EXIT=0`; `guest_era` ALL PASS end to end (20/20 boot
stages, including `era_esram` and `era_presentarray`); all 3 existing
guests still ALL PASS (no regressions). The synthetic guest is the
regression proof that the translation itself works.
