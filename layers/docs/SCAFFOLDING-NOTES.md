# ScarlettCompat — scaffolding notes (2026-11-22)

**Repo:** `SSTGxf/ScarlettCompat` (public, main). Work folder on the PC:
`F:\ScarlettCompat`. **Do NOT push from the PC** (no git there) — the parent
agent pulls the tree and pushes. No game code in the repo; no exe fetching
or reconstruction; read-only on `D:\game\game`.

**Deadline:** 15 hours to verify a good stable layer (~12:46 EST Nov 23).
Realistic bar (research brief): loader + shimmed boot path for a synthetic
guest title and a first `d3d12_xs` frame — NOT a playable title.

## 1. Package inventory — removed

No package inventory is kept in the repo. The layer is verified
against synthetic guest titles and real-title boots; no game
package metadata (identities, file lists, sizes, or build stamps)
is documented here.

## 2. Shipped DLLs — PE analysis (measured)

All PE32+ x86-64, base `0x180000000`, chars `0x2022`. Xbox-built DLLs
carry a `.xbld` section with GXDK/GRDK strings:

- `Bink2Scarlett.dll` — Xbox Bink video.
- `XboxBvhBuilder.dll` — Xbox BVH builder.
- `amd_fidelityfx_upscaler_xbox.dll` — AMD FSR upscaler (Xbox).
- `XCurl.dll` — cURL (Xbox).
- Key imports: `d3d12_xs.dll` (XboxBvhBuilder, FSR upscaler),
  `xmem.dll` (Bink2, BVH, FSR), `XAudio2_9.dll` (Bink2),
  `WINHTTP.dll`/`CRYPT32.dll` (XCurl, libHttpClient.GDK),
  `KERNEL32.dll`/`ADVAPI32.dll` (libHttpClient.GDK), ~30 `api-ms-win-*`
  API sets (all DLLs — forward to real Windows on PC).
- No `xgameruntime`/`xgameplatform` imports in the shipped DLLs — those
  are linked by the exe. Its import list is open question #1.

## 3. Loader design (`src/pe_loader.cpp`)

Native x86-64 PE32+ → map straight into the host process (no CPU
emulation). Validate MZ/PE32+/x64, parse `.xbld` (GXDK edition + build),
`VirtualAlloc` at preferred base `0x180000000` (reloc `DIR64` on
fallback), copy headers/sections, resolve imports: Xbox DLLs → HLE
registry (never `LoadLibrary` — avoids the Win11 26100 System32
`xgameruntime.dll` collision, `0xC0000139`); api-ms-win-*/system →
real `LoadLibrary` + `GetProcAddress`. Missing imports → E_NOTIMPL stub
+ counted. TLS detected, not handled. Entry runs in a throwaway child
process (harness) so a guest crash can't kill the harness.

## 4. Service surface (`src/hle_registry.cpp`, Xenia-style tags)

Implemented/stubbed now: `XGameRuntimeInitialize` (stub, S_OK),
`XMemAlloc`/`XMemFree` (real heap), `D3D12XboxCreateDevice` (stub;
signature assumed D3D12CreateDevice-shaped — confirm vs exe),
`SetFrameIntervalX` / `ScheduleFrameEventX` / `WaitFrameEventX` (stubs,
no real pacing), `XAudio2Create` (stub), `GameInputCreate` (stub),
`DStorageGetFactory` (stub). Not implemented: `XUserAddAsync`,
`XUserGetId`. Tracked in docs/STATUS.md, not yet in the registry:
XGameSave, XPackage, XAsync/XTaskQueue, XSAPI (fail-soft only), XGameUI,
DirectStorage queues, AcpHal/XMA.

## 5. Graphics plan (DXIL route, not SPIR-V)

`d3d12_xs` → desktop D3D12. Device creation via `D3D12XboxCreateDevice`
→ `D3D12CreateDevice`. Core D3D12 surface (lists, fences, heaps,
barriers, PSOs, views, clears, copies, queries) is structurally
identical — thin passthrough. Tiling via DirectXTex Xbox tables
(`XBOX_TILEMODE_SCARLETT`). Shaders: DXBC carve → RDNA2 → LLVM → DXIL
(per Xbox2Windows; Leonida's SPIR-V path does not transfer to D3D12).
Mesh shaders (9,282) + DXR: desktop D3D12 supports both, but the RX 5700
XT (RDNA1) has neither — full pipeline unverifiable on the build
machine. **Unified memory (`MEM_GRAPHICS` aliasing) is the #1 open
design question — 0% in every public project.**

## 6. Verification harness (`src/verify.cpp` + `tests/guest_minimal.cpp`)

`scarlett_verify.exe <guest> [timeout_ms]` spawns itself as a child
(`--run`), loads the guest through the layer, runs it, reads
`load_report.txt` + `guest_report.txt`, prints PASS/FAIL per subsystem
(loader, runtime, memory, gfx_xs, present, frame_evt, frame_wait, audio,
input, storage, gfx_real). The synthetic guest is an Xbox-style PE (base
`0x180000000`, `.xbld` with Oct 2026 GXDK strings, Xbox imports) that
runs the boot sequence plus a real desktop D3D12 device + clear + fence
as the translation-target proof. Build + run: `build.bat` (VS 2022
BuildTools, MSVC 14.44, Win SDK 10.0.26100.0).

**Verified 2026-11-22 ~21:55 EST — verdict ALL PASS (exit 0):** loader
PASS (14 modules, 63 imports resolved, 0 failed — including ordinal
import `d3d12.dll:#101` = `D3D12CreateDevice`, fixed in the loader during
this session); guest subsystems runtime/memory/gfx_xs/present/frame_evt/
frame_wait/audio/input/storage all PASS; `gfx_real` PASS (desktop D3D12
device + queue + clear + fence OK on the RX 5700 XT at FL 11_0). The
`d3d12_xs` device call itself is still a stub — real translation is the
next step after this snapshot.

## 7. Blocked on the exe

Exact import list; GDK/GDKX version; boot call order + first fatal
dependency; shader inventory used at boot; `MEM_GRAPHICS` usage pattern;
storage pattern (DirectStorage queues, BCPack/GDeflate); threading model;
runtime integrity checks (XCI / package-hash validation). **Do not run
the harness against an exe from an unofficial source** — treat any
third-party exe as untrusted code in a sandbox, and verify its SHA-256
against a known-good value before running it.

## 8. Research base

The GDK research notes — GDK/Game Core
vs XDK/ERA split; ScarlettX as closest template (64.4%, boots one indie
Unity game); Leonida shader pipeline; Xbox2Windows DXIL route; RAGE as a
cross-platform abstraction layer (platform surface concentrated in RAGE's
platform layer — exactly what a compat layer replaces).
