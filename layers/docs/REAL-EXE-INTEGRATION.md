# Real-exe integration (2026-11-23)

How the layer was fixed for the REAL Series X exe,
`game_scarlett_master.exe` (120,181,760 bytes, PE32+ x64, image base
0x140000000, entry 0x1456A1EC8, timestamp Fri Nov 13 16:40:36 2026).
The exe was read READ-ONLY (dumpbin /headers + /imports + /exports);
it was never executed directly, never copied into the repo, and never
modified. SHA-256 verified by the layer-fix agent with Get-FileHash:
SHA-256 verified against a known-good value before any run; the exe is
treated as untrusted code in a sandbox.

## The import table (587 imports, 68 modules)

- The exe imports BY NAME, not by ordinal — except `GameInput.dll`
  ordinal 1. There are no real ordinals to replace the layer's
  synthetic tables with; the synthetic tables stay test-only
  (guest_ordinal exercises them). (Total corrected 2026-11-23 from the
  RE report's PE parse: 587 = 586 named + 1 ordinal. An earlier
  dumpbin-based count of 597 was wrong.)
- **GameInput ordinal 1 is CONFIRMED real**: it matches the layer's
  synthetic assignment (gameinput 1 = GameInputCreate). This is the
  only real ordinal import from an Xbox DLL.
- `.xbld`: TWO sections (0xAF40000 and 0xB06C000). The loader's
  ParseXbld takes the FIRST match (`GRDK,April 2025 GRDK Update 2`,
  `10.0.26100.4061`); the latest record in the section is `GXDK,April
  2026 GXDK Update 5`, `10.0.26100.7901`. Validation passes; the logged
  edition is the oldest record.
- Exports exactly one function: `__XMemAllocationHooks__` (ordinal 1).
- Debug PDB:
  `X:/americas/src/sub_story/game/vs_project/Gaming.Xbox.Scarlett.x64/Build/master/game_scarlett_master.pdb`.
- NOT statically imported: xgameruntime.dll, dstorage_xs.dll,
  XAudio2_9.dll, d3d11_x.dll, d3d12_x.dll, kernelx.dll, combase.dll,
  vccorlib110.dll. The exe imports LoadLibraryW — dynamic loading
  happens at runtime; what it loads is unknown until boot. The loader
  does NOT hook LoadLibrary, so a LoadLibrary("xgameruntime.dll")
  would hit the real Win11 System32 xgameruntime.dll — a boot-time
  question, not fixed blind.

## True D3D12XboxCreateDevice signature (call-site proof)

The exe's code uses LLD import thunks (D3D12XboxCreateDevice thunk at
0x145f3fd80, IAT slot 0x146793C18). Exactly ONE caller, at VA
0x1424785b7. Disassembly shows the argument setup immediately before
the call: `xor ecx, ecx` (rcx=0), `lea rdx, [rsp+0x1b0]` (rdx = params
struct), `lea r8, [rip+...]` (r8 = IID in .rdata), `lea r9, [rip+...]`
(r9 = ppDevice global in .data).

```c
HRESULT D3D12XboxCreateDevice(IUnknown* pAdapter,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* pParameters,
    REFIID riid, void** ppDevice);
```

- rcx=NULL (default adapter) — NOT the layer's old assumed
  D3D12CreateDevice shape (adapter, minFL, riid, ppDevice).
- Corroborated by a public Xbox SDK guide showing the same 4-arg
  form (`D3D12XboxCreateDevice(nullptr, &params, IID_PPV_ARGS(&device))`
  with `params.Version = D3D12_SDK_VERSION; params.GraphicsSwitchingMode
  = D3D12XBOX_MODE_PROCESS; params.ProcessDebugFlags =
  D3D12_PROCESS_DEBUG_FLAG_NONE;`).
- Struct (80 bytes, zero-initialized): +0x00 DWORD 0x20004 (= SDK 2.4:
  (2 << 16) | 4), +0x08 DWORD 0x100000, +0x20/+0x24 bytes from
  globals, +0x28 DWORD 1, +0x3c DWORD 0x20 (32). The struct carries NO
  feature level — on Xbox the level is fixed at 12_2 (DirectX 12
  Ultimate). The layer tries desktop feature levels from highest to
  lowest and takes the best the hardware offers (RX 5700 XT: 12_1).
  Field offsets beyond Version are from the guide's declaration order
  + the call-site writes; the layer reads Version and logs the rest.

### The ERA D3D12XCreateDevice is a different function (regression fixed 2026-11-23)

The ERA `d3d12_x.dll!D3D12XCreateDevice` and the GDK
`d3d12_xs.dll!D3D12XboxCreateDevice` are DIFFERENT functions with
DIFFERENT signatures. They shared one implementation until the GDK
form was corrected to the real 4-arg params signature — but the ERA
guest calls D3D12XCreateDevice as (adapter, minFL, riid, ppDevice), so
sharing the GDK function made it interpret the feature level (0xb000)
as a params pointer and dereference it, crashing the era guest with
0xC0000005 in era_gfx12 (two build runs). The fix splits them:
HleD3D12XboxCreateDevice keeps the real GDK params form;
HleEraD3D12XCreateDevice is a separate D3D12CreateDevice-shaped
passthrough for the ERA entry. The ERA form is ASSUMED (phase 5) — a
D3D11.x-only title does not import d3d12_x at all.

### Boot #1 (2026-11-23 ~10:43 EST) — loader PASS, title crashed in its own early init

The first boot of the real exe: **loader PASS, 68 modules, 587 imports resolved, 0 failed** (all 97 new entries hit). The title ran its entry point: D3D12ConfigureVirtualMemoryX returned S_OK, then it made 7 XMemVirtualAlloc arena allocations (two 24 GB reservations, 1.23 GB, 2.31 GB, 590 MB, 625 MB, 20 MB — all succeeded, via the VirtualAlloc flag-translation retry). It then crashed: harness reports "guest crashed in entry: exception 0xc0000005", child exit 0x80000003, minidump exception EXCEPTION_BREAKPOINT (0x80000003) at game offset 0x22106e0 — an `int3` padding byte immediately after the vtable-jump thunk `mov rax,[rcx]; jmp [rax+0xe0]` at 0x22106d0. The title called a virtual method whose vtable entry pointed to padding (an uninitialized/stub object), or hit a deliberate __debugbreak() on a failure path. **No D3D12XboxCreateDevice call was made — the title died before device creation, so the Xbox IID mapping is still unproven against the real exe.** Most likely cause: the exe is alone in its folder (no data files beside it); its early init likely loads a file, gets a NULL/placeholder, and calls a method on it. Log `F:\ScarlettCompat\boot1.log`, dump in the local boot-diagnosis notes.

### Boot #2 (2026-11-23 ~10:52 EST) — identical crash, ERA flag ruled out

Re-ran after the AcpHal ERA-detection fix: the loader no longer flags the title as ERA (AcpHal imports now resolve as "shim", no WinRT warning), and the boot is otherwise byte-identical to boot #1 — same 7 XMemVirtualAlloc calls, same crash at the same point ("guest crashed in entry: exception 0xc0000005", child exit 0x80000003). The ERA flag was NOT the cause. The crash is in the title's own early init, before device creation. Next step for the boot: find what the title accesses just before the fault (file I/O trace, or the object whose vtable entry points to padding) — likely a missing-data failure, since the exe has no data files beside it. Log `F:\ScarlettCompat\boot2.log`.

### ERA mis-detection (found by boot #1, fixed)

The loader flagged the title exe as an ERA title because it imports AcpHal.dll — but AcpHal.dll is the audio DSP HAL, shared across Xbox One and Series X (the real GDK exe imports it 9 times). The ERA-only signal is the `_x` modules (d3d11_x, d3d12_x, kernelx, xg_x). AcpHal.dll was removed from `kEraModules` (src/era_to_gdk.cpp); it still resolves through the HLE registry (kShimModules), which is separate from detection. The false ERA flag did not cause the boot #1 crash (the ERA translation only routes ERA imports, which the title does not have), but it produced a spurious WinRT warning and could have interfered. Boot #2 will confirm.

### The REFIID is an Xbox IID, not a desktop IID (CRITICAL)

The REFIID the game passes to D3D12XboxCreateDevice is
**{5405C344-D457-444E-B4DD-2366E45AEE39}** — NOT desktop
IID_ID3D12Device (189819F1-1DB6-4B57-BE54-1821339B85F7). The desktop
driver does not know that GUID, so passing it through to
D3D12CreateDevice fails with E_NOINTERFACE and the game would die at
device creation. The layer maps it: it tries the requested IID first,
and on E_NOINTERFACE retries with the desktop IID_ID3D12Device (the
base interface — the Xbox device interface extends it, so the base
methods are at the same vtable offsets). The title's XboxBvhBuilder
imports reference ID3D12Device11 / ID3D12GraphicsCommandList6/8, so it
wants newer interfaces — but the game will QueryInterface for those
later, and QI with an Xbox IID needs the COM wrapper (future work);
device creation itself must not fail here.

## Other call-site proofs

- **D3D12ConfigureVirtualMemoryX** — 1 caller at 0x141e1cfe0: ONE arg
  (rcx = pointer to a 32-byte zeroed config struct, struct+4 = a
  computed value). Returns HRESULT; the game hits `int3` on failure,
  so the layer MUST return S_OK.
- **D3D12SerializeVersionedRootSignature** — 2 callers (0x14247fc80,
  0x1455ac80a). The desktop form is 3-arg (pRootSignature, ppBlob,
  ppErrorBlob) — NO riid (proven from the SDK header d3d12.h:4442, SDK
  10.0.26100.0; the 4-arg riid form belongs to the older non-versioned
  D3D12SerializeRootSignature). The Xbox form is ASSUMED to match (the
  GDK _xs functions mirror the desktop D3D12 signatures); the two call
  sites did not decode cleanly enough to prove the arg count, so this
  is flagged — if the boot crashes at this call, the call-site
  disassembly is the next step. The layer forwards it to desktop
  D3D12SerializeVersionedRootSignature.
- **XMemVirtualAlloc** — 23 callers (heavily used). Call site at
  0x14193e220: 5 args — rcx=0 (address), rdx=size, r8=0x20003000
  (alloc type, the same Xbox bits as kernelx!VirtualAlloc), r9=0x283
  (protect — an Xbox encoding, not a desktop PAGE_* value), stack
  [rsp+0x20]=0x2004 (memory attributes). Returns a pointer in rax.
  The layer strips the Xbox alloc-type bits and retries with
  PAGE_READWRITE when the Xbox protect value fails.
- **GameInput ordinal 1** — 1 caller at 0x1459258c3: rcx = &out handle,
  HRESULT checked. The layer's HleGameInputCreate matches.

## The 97 missing registry entries (all added)

| Module | Count | What the layer does |
|---|---|---|
| xmem.dll | 10 | Real: XMemAllocDefault / XMemFreeDefault (heap), XMemVirtualAlloc (VirtualAlloc wrapper), XMemMapPhysicalPages / Scatter (ordinary memory as the mapped VA, guarded size), XMemTransferMemory (memcpy), XMemIsToolsMemAvailable (FALSE). S_OK stubs: XMemGetWorkingSetStatistics, XMemAllocatePhysicalPages, XMemFreePhysicalPages |
| d3d12_xs.dll | 2 | Real: D3D12ConfigureVirtualMemoryX (S_OK), D3D12SerializeVersionedRootSignature (desktop forward) |
| chad.dll | 5 | S_OK stubs (Xbox audio DSP) |
| xg_xs.dll | 2 | S_OK stubs (Scarlett texture computer — `xg_xs`, NOT ERA `xg_x`) |
| XboxBvhBuilder.dll | 7 | C++ method stubs (mangled names; return types read off the mangling: void except Rehydrate = HRESULT → S_OK) |
| XCRDAPI.dll | 1 | DStorageGetFactory — same dummy-factory stub as dstorage_xs.dll (the real exe uses XCRDAPI.dll, not dstorage_xs.dll) |
| amd_fidelityfx_loader_xbox.dll | 4 | Return 0 (FFX_OK) — FSR loader |
| libHttpClient.GDK.dll | 38 | S_OK stubs (HTTP client) |
| xapu.dll | 5 | S_OK stubs (audio APU) |
| Bink2Scarlett.dll | 23 | Void stubs; BinkOpenWithOptions returns a dummy non-NULL handle; BinkGetKeyFrame / BinkShouldSkip return 0 |
| **Total** | **97** | **13 real implementations + 84 stubs** |

8 new shim modules added to `kShimModules` (chad, xg_xs,
XboxBvhBuilder, XCRDAPI, amd_fidelityfx_loader_xbox, libHttpClient.GDK,
xapu, Bink2Scarlett) — without them the loader falls through to real
LoadLibraryA, which fails for Xbox DLLs, and every import from them
gets the E_NOTIMPL stub.

Desktop/API-set modules (MSVCP140, VCRUNTIME140, MFPlat, WS2_32,
WINHTTP, bcrypt, api-ms-win-core-*, api-ms-win-crt-*) resolve via
real LoadLibraryA — verified present in System32.

## What the layer still does NOT do for the real exe

- **Boot call order** — unknown until the boot runs; the boot log
  (per-import resolution + per-call [hle] lines) will show it.
- **Shader lifting** (PSOs/DXIL) — design delivered, not built.
- **DirectStorage queues** — now via XCRDAPI.dll, not dstorage_xs.dll.
- **Texture de-tiling** — exact Scarlett tile layout unverified;
  xg_xs.dll XGCreateTextureComputer/XGSuggestSwizzleMode are the real
  entries (stubs for now).
- **Unified memory** — the 0% / #1 open question from the research
  brief; XMem* physical-page functions are stubs/ordinary-memory.
- **Software-RT and optimizations COM wrappers** for real titles —
  components proven, wrapper is future work; a real title today
  reaches the raw device.
- **Hardware DXR passthrough** — E_NOTIMPL (unverifiable on the
  5700 XT, FL 12_1, no RT cores).
- **BCPack decode** — E_NOTIMPL (rANS format not public).
- **LoadLibrary hooking** — the loader does not hook LoadLibrary;
  dynamic loads of Xbox DLLs at runtime would hit System32.
- **The 5700 XT cannot verify the full pipeline** (no RT/mesh
  shaders; the reference title has 9,282 mesh shaders + DXR). The device the layer
  creates is FL 12_1, not the Xbox fixed 12_2. A boot proves loading
  and early init, not a playable game.

## Boot #4 — vtable slot-6 patch (2026-11-23 ~11:44 EST)

- **The patch** (`src/pe_loader.cpp`, `ApplyScarlettVtablePatch`,
  called from `LoadXboxPe` while the image is still RWX, before
  section protections): overwrite the qword at VA `0x1465f5450`
  (derived vtable `0x1465f5420` + `0x30`, slot 6) with
  `0x140004810` (the base class's slot-6 stub: `xor eax, eax; ret`).
  Fingerprint-gated on all three values — slot-6 qword ==
  trap address, trap bytes `cc c3`, stub bytes `31 c0 c3` —
  verified against the exe bytes on disk before building, so
  proof guests are untouched. Log line on fire:
  `scarlett vtable patch: slot6 0x1422106e0 -> base stub
  0x140004810 (was int3 trap)`. Build 8: BUILD_EXIT=0, all 10
  verify runs ALL PASS (era 21/21), mirror==PC SHA-256 verified.
- **Boot #4 result: the fault MOVED.** Loader PASS (68 modules,
  587/587 imports), entry ran, `D3D12ConfigureVirtualMemoryX` →
  S_OK, 7 `XMemVirtualAlloc` arenas all OK, then
  EXCEPTION_BREAKPOINT at **`0x142219650`** — an `int3` inside F
  itself (`0x2219620`, immediately after the slot-6 call at
  `0x221964d`). F is a noreturn path by design: lock → call slot
  6 → `int3` → (unlock tail-jump, unreachable if slot 6 returns).
  The boot #3 claim that "the caller ignores slot 6's return"
  was wrong about F — only the *guard* ignores it (it returns
  the slot-0 result, NULL, either way). Both traps exist because
  this path is designed never to be reached on Xbox: slot 0 is
  supposed to find a block, so the guard never calls F.
- **What this proves:** the real blocker is upstream of F —
  slot 0 returns NULL because the allocator is empty (capacity
  `[object+0x1240]` = 0, nothing registered). Next steps: (1)
  neutralize F's `int3` (1-byte patch, RVA `0x2219650`,
  `0xcc` → `0x90`) and see whether the caller survives the
  NULL block; (2) H1 — seed/register the allocator so slot 0
  finds a block (requires finding the registration path).
  Full write-up in the local boot-diagnosis notes (boot #4);
  log `F:\ScarlettCompat\boot4.log`, dump in the local
  boot-diagnosis notes.
- **Still no `D3D12XboxCreateDevice` call in any boot** — the title
  dies in static init, before device creation, so the Xbox IID
  mapping remains unproven against the real exe.

## Boot #5 — F int3 NOP (2026-11-23 ~11:59 EST)

- **The patch** (`src/pe_loader.cpp`, `ApplyScarlettVtablePatch`,
  same RWX window as the vtable patch): NOP F's own `int3` at RVA
  `0x2219650` (`0xcc` → `0x90`). Fingerprint-gated on the byte
  being `0xcc` plus the call before it (`FF 50 30` = `call qword
  ptr [rax+0x30]`) and the mov after it (`4C 89 F1` = `mov rcx,
  r14`) — verified against the exe bytes on disk first (RVA
  `0x221964d`: `ff 50 30 cc 4c 89 f1`). Correction to the boot #4
  write-up: the mov is `4C 89 F1`, not `49 8B CE` (both encode
  `mov rcx, r14`; the file holds `4C 89 F1`). Log line on fire:
  `scarlett F int3 patch: 0xcc -> 0x90 at RVA 0x2219650 (NOP)`.
  Build 9: BUILD_EXIT=0, all 10 verify runs ALL PASS (era 21/21),
  mirror==PC MD5 verified.
- **Boot #5 result: the fault MOVED again — and changed kind.**
  Loader PASS (68 modules, 587/587 imports), entry ran,
  `D3D12ConfigureVirtualMemoryX` → S_OK, 7 `XMemVirtualAlloc`
  arenas all OK, then `guest crashed in entry: exception
  0xc0000005`, child exit `0xe0000004 (TIMEOUT)`, verdict FAIL.
  No `D3D12XboxCreateDevice` call. The exception changed from
  boot #4's breakpoint (`0x80000003` at F's int3) to an access
  violation — the fault is past F.
- **Mechanism (proven from disassembly):** F now unlocks and
  returns; the guard returns NULL; the caller `0x1cf0d60`
  (called from the pool ctor `0x2219b60`, return address
  `0x2219b97` — the boot #4 stack address) does NOT null-check
  the first guard result and passes it unconditionally to
  `memset` (`VCRUNTIME140.dll!memset`, via the import thunk at
  IAT RVA `0x6794368`). The second guard result IS null-checked
  (loop skipped on NULL). Slot 28 returns `[object+0x1230]`,
  slot 31 returns `[object+0x20]`; the memset size is derived
  from them and is 0 only if both are 0. **The caller crashes
  on the NULL block — slot 0 must return a real block.**
- **Caveats:** boot #5 produced NO minidump and no WER report
  (the child hangs after the SEH catch, so WER never writes
  one), so the exact fault address is unproven and the memset
  size's runtime value is unknown. The `memset(NULL)` mechanism
  is the strongest hypothesis with a proven code path, not a
  dump-proven fact. Next boot should log the exception record
  address (and ideally use MiniDumpWithFullMemory).
- **Next work is H1:** find the registration path that writes
  `[object+0x1240]` / the free list, so slot 0 finds a block
  and F is never called. Patching around the guard/F traps is
  exhausted. Full write-up in the local boot-diagnosis notes
  (boot #5); log `F:\ScarlettCompat\boot5.log`, build log
  `F:\ScarlettCompat\build_realexe9.log`.
