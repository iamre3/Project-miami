# Architecture

## Why this shape

A Series X title is a native x86-64 PE32+ running on a Windows-derived OS
(Game OS). No CPU emulation is needed — the exe maps straight into a Windows
process and its API imports are intercepted. The work is threefold:

1. **PE loader + import interception** — map the guest, resolve its imports.
2. **HLE shims for Gaming Runtime services** — answer the GDK calls the title
   makes at boot.
3. **`d3d12_xs` → desktop D3D12 translation** — device, command lists,
   resources, presentation, plus RDNA2 shader lifting (DXIL route).

The template is ScarlettX's shim-DLL architecture with Xenia's HLE registry
pattern. The Xbox One ERA shim-DLL model proves the shim-DLL approach; Azimuth/Mezcal
prove the translation model at AAA scale for one engine family — but all of
those target Xbox One, not Series X Game Core. Nothing from them transfers
directly; only the architecture does.

## Loader (`src/pe_loader.cpp`)

- Reads the guest PE, validates MZ / PE32+ / machine `0x8664`.
- Validates the `.xbld` section and parses the GXDK edition/build strings
  (tells us which GDK the title was built against once the exe lands).
- `VirtualAlloc`s at the preferred base (`0x180000000` for Xbox binaries);
  falls back to any base and applies `IMAGE_REL_BASED_DIR64` relocations.
- Copies headers + sections. (Per-section page protections are a TODO;
  the scaffolding mapping is RWX.)
- Resolves imports per descriptor:
  - **Xbox DLLs** (`d3d12_xs`, `xmem`, `xgameruntime`, `xgameplatform`,
    `XAudio2_9`, `dstorage_xs`, `gameinput`, …) → the HLE registry
    (`src/hle_registry.cpp`). Never `LoadLibrary`'d — this also avoids the
    known trap where Windows 11 build 26100 finds its own System32
    `xgameruntime.dll` via PATH before a shim (`0xC0000139`).
  - **Everything else** (`api-ms-win-*`, `KERNEL32.dll`, `d3d12.dll`, CRT)
    → real `LoadLibrary` + `GetProcAddress`. The API-set surface forwards
    to desktop Windows, which is why most of the shipped DLLs' imports are
    already satisfiable.
  - Ordinal imports: logged, unsupported for now.
  - Missing/unimplemented imports: IAT slot gets `ScNotImplementedStub`
    (returns `E_NOTIMPL`) and is counted, so a load never fails silently.
- TLS directory: detected and logged; not handled yet.
- Entry point is invoked by the harness in a throwaway child process (a
  normal EXE entry calls `ExitProcess` on return, and a guest crash must
  not kill the harness).

## HLE registry (`src/hle_registry.cpp`)

Central table, Xenia-style: `(module, function name) -> {status, host fn,
note}`. Status tags: `kImplemented` (real host code), `kStub` (logs +
plausible success), `kNotImplemented` (no handler; E_NOTIMPL stub),
`kSketchy`, `kHighFrequency`. The table is the project's tracking surface —
see [STATUS.md](STATUS.md). New functions are added here first, then the
loader picks them up with no other change.

Boot sequence the layer must support (research brief §1):

1. PE load → CRT startup → `XGameRuntimeInitialize()`
2. User model resolution (XUser sign-in / default user)
3. Package/content mount (XPackage, MSIXVC layout)
4. Storage init (XGameSave / DirectStorage factory)
5. Graphics device (`D3D12XboxCreateDevice`) + swapchain/presentation
6. Input/audio init (GameInput, XAudio2)
7. XSAPI session (will fail offline — must not hard-abort)

## Graphics plan (`d3d12_xs` → desktop D3D12)

- **Device creation differs:** Xbox uses `D3D12XboxCreateDevice` with
  `D3D12XBOX_CREATE_DEVICE_PARAMETERS`, not `D3D12CreateDevice`. The shim
  translates this to a desktop device. The exact parameter struct is
  GDKX-gated — the stub currently assumes a D3D12CreateDevice-shaped
  signature; it must be confirmed against the real exe's imports.
- **Passthrough surface:** most core D3D12 calls (command lists, fences,
  descriptor heaps, resource barriers, PSOs, views, clears, copies,
  queries) are structurally identical between Xbox and desktop. The shim
  covers this surface thinly, mirroring what Leonida's proxy already does
  on Vulkan — minus the Vulkan translation.
- **Texture tiling:** Xbox uses hardware-specific tiled layouts. Translate
  via the DirectXTex Xbox-extension tiling tables
  (`XBOX_TILEMODE_SCARLETT`); de-tile or translate tiled resources.
- **Shaders — DXIL route, not SPIR-V.** Xbox shaders are precompiled RDNA2
  programs in DXBC containers inside `shaders.rpf`. Pipeline (per
  Xbox2Windows): carve DXBC → unpack native RDNA2 → LLVM disassembly →
  DXIL. Leonida proved the carving/disassembly method (all 26,503 shaders)
  but its SPIR-V/Vulkan output does not transfer to a D3D12 backend.
  Wave64 caveat: 21,545 shaders are wave64 — exact on 64-wide subgroups
  (AMD), approximate on 32-wide (NVIDIA/Intel) for cross-half wave ops.
- **Mesh shaders (9,282) + DXR Tier 1.1:** desktop D3D12 supports both;
  the build machine's RX 5700 XT (RDNA1) supports neither, so the full
  pipeline cannot be verified on it. Verification target must be a newer
  GPU or WARP.
- **Presentation / frame pacing:** Xbox uses `SetFrameIntervalX`,
  `ScheduleFrameEventX`, `WaitFrameEventX`. ScarlettX stubs these as
  immediate success (hence 188 FPS uncapped). A stable layer needs real
  `WaitFrameEventX` semantics once a frame exists.

## Memory model — the #1 open design question

Xbox Series X has 16 GB unified GDDR6 shared by CPU and GPU. Xbox code
uses `D3D12XBOX_MEMORY_TYPE_TITLE` and `MEM_GRAPHICS` with 64 KB / 4 MB
physical-virtual aliasing — CPU and GPU address the same memory. Desktop
Windows has separate system RAM and VRAM; there is no equivalent aliasing.
No public project has solved this. Options (undecided): a unified virtual
address space with explicit sync, or copy-on-boundary. Do not assume
bandwidth parity (console 560 GB/s vs PCIe).

## Storage / streaming

RAGE streams the world continuously; on Xbox this rides the Velocity
Architecture + DirectStorage (`dstorage_xs`, hardware BCPack decompression).
On PC the layer must serve the same streaming I/O through desktop
DirectStorage or plain async file I/O at comparable throughput — a slow
storage shim stalls the world streamer before graphics ever matters.

## Verification ladder

1. Synthetic guest boots through the loader (done — `scarlett_verify`).
2. First `d3d12_xs` frame on screen (real device + swapchain, not the stub).
3. One small real Game Core title boots (e.g. an unencrypted Unity indie).
4. Point the layer at a large real title exe; read its import table
   first, then its boot call order.
