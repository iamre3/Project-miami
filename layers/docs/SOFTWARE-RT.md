# Software ray tracing (DXR replacement for GPUs with no RT cores)

Why this exists: modern Series X titles use DirectX Raytracing (DXR Tier 1.1) and
mesh shaders — both need Feature Level 12_2 (DirectX 12 Ultimate).
The build machine's RX 5700 XT is RDNA1, Feature Level 12_1: no
hardware ray tracing units, no mesh shaders. Desktop D3D12 reports
`D3D12_FEATURE_D3D12_OPTIONS5::RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED`
on this card, so a real title's `ID3D12Device5::CreateRaytracingPipelineState`
call would fail outright. The game cannot render its full pipeline on
this GPU — but the ray tracing part can be *replaced*, not passed
through: ray tracing done in compute shaders, Lumen-style. Lumen's
software ray tracing path is the proof this works on RDNA1 (The Elder
Scrolls IV: Oblivion Remastered runs Lumen on an RX 5700, its minimum
AMD card, in software mode — no RT cores, slower, but it works).

This document is the design. The implementation is
`src/software_rt.cpp` + `include/software_rt.h`; the proof is
`tests/guest_software_rt.cpp` through the harness.

## What DXR is (the calls a title makes)

A DirectX Raytracing workload is four things:

1. **State object** (`CreateRaytracingPipelineState`): a pipeline
   built from shader-library subobjects — ray generation shader,
   miss shaders, hit groups (closest-hit + any-hit + intersection),
   plus the shader config (payload size, attribute size) and the
   pipeline config (max recursion depth).
2. **Acceleration structures** (`BuildRaytracingAccelerationStructure`):
   a bottom-level AS (BLAS) holds triangle geometry in a BVH; a
   top-level AS (TLAS) holds instances of BLASes with transforms.
   Both are built on the GPU (or CPU) and live in GPU memory.
3. **Shader tables** (`D3D12_DISPATCH_RAYS_DESC`): three tables of
   shader records — ray generation (1 record), miss (N records),
   hit groups (M records). Each record is a shader identifier plus
   local root arguments (constants / descriptors the shader reads).
4. **Dispatch** (`ID3D12GraphicsCommandList4::DispatchRays`): the GPU
   traces one ray per output pixel (width x height x depth rays),
   runs the raygen shader per ray, walks the TLAS/BLAS, runs the
   miss shader on a miss and the hit-group shaders on a hit, and
   writes the result (usually to a UAV texture).

On Xbox (Series X) the same concepts arrive through `d3d12_xs` with
Xbox-side parameter shapes; the layer's DXR work is the same four
things, translated.

## Mapping: each DXR call → software implementation

| DXR concept | Software implementation in this layer | Status |
|---|---|---|
| `RaytracingTier` detection (`CheckFeatureSupport` OPTIONS5) | `ScSoftwareRT_QueryPath`: reads the tier from the real device. Tier `NOT_SUPPORTED` → software path; tier ≥ 1_0 → hardware path (see below) | Implemented |
| Ray generation shader | A compute shader (`cs_5_0`, compiled at runtime with `D3DCompile`, same technique as the proof guest's triangle): one thread per output pixel, generates the ray from the camera record and writes the pixel | Implemented |
| Miss shader | The compute shader's miss branch: no intersection → the miss record's color (the clear color) | Implemented |
| Closest-hit shader | The compute shader's hit branch: the hit record's color (the triangle's material color) | Implemented (flat color; no lighting, no any-hit, no custom intersection) |
| BLAS build | CPU BVH build (median-split over triangle centroids, one triangle per leaf) + upload to a GPU `StructuredBuffer`; the compute shader walks it with a fixed stack | Implemented |
| TLAS build | A single identity instance: the scene holds one BLAS, the compute shader reads it directly. No instance transforms, no multi-BLAS TLAS | Partial — single instance only |
| Shader tables | A CPU-side table of records (raygen camera record, miss color record, one hit-group record per triangle carrying its color). The compute shader indexes the hit record by triangle id | Implemented (records, not DXR shader identifiers) |
| `DispatchRays` | `ScSoftwareRT_DispatchRays`: binds the compute PSO + root signature, dispatches `ceil(W/8) x ceil(H/8)` thread groups of 8x8, writes the caller's UAV texture | Implemented |
| Any-hit shader / alpha-tested geometry | No equivalent — any-hit runs per candidate intersection before closest-hit is known; the software walk takes the first hit as closest-hit and stops | Not implemented |
| Intersection shader (procedural primitives) | No equivalent — the software walk only intersects triangles (Möller–Trumbore) | Not implemented |
| Recursion (rays spawning rays) | No equivalent — one ray per pixel, no secondary rays, no recursion | Not implemented |
| Hardware path (tier ≥ 1_0) | `ScSoftwareRT_DispatchRays` returns `E_NOTIMPL` with a log line when the adapter reports a real RT tier. Real hardware DXR passthrough (state objects, real shader tables, real BLAS/TLAS) is future work — it cannot be verified on this card, and faking it would be worse than refusing it. When the mode is `hardware` on a tier-0 adapter, the layer falls back to software with a clear log line instead of failing (see the RT mode section below) | Stub (honest refusal) |

Counts: **7 DXR concepts implemented, 1 partial (single-instance TLAS),
4 with no software equivalent, 1 honest stub (hardware passthrough).**
0 faked.

## RT mode launch setting (item 8)

The layer reads one launch setting at startup that selects the RT
mode: the environment variable **`SCARLETT_RT_MODE`**. The harness
passes its environment through to the child process automatically
(`verify.cpp` uses `lpEnvironment=NULL`), so a mode set in
`build.bat` reaches the guest; no config file is needed — the env
var is the simplest robust mechanism. The mode is read once per
process and cached; it is a per-process launch setting, not a
per-call flag.

| Value | Meaning |
|---|---|
| `off` | RT calls succeed but do nothing (no-op). The title renders without RT effects — **no RT reflections, no RT global illumination, no RT shadows** — and relies on its rasterized lighting. `BuildScene` returns `S_OK` with a null scene handle; `DispatchRays` returns `S_OK` and writes nothing (the output texture is left exactly as the rasterizer left it). |
| `software` | The existing Lumen-style compute-shader path above. **Default** on the 5700 XT (which reports RT tier 0). Also the mode for an unset, empty, or unrecognized value (an unrecognized value logs a warning and falls back to software). |
| `hardware` | Hardware DXR when the adapter reports an RT tier above 0. On the 5700 XT (tier 0) it **falls back to software with a clear log line, NOT a failure** — the title still renders, through the software path. On a tier>0 adapter the path is hardware, and `BuildScene`/`DispatchRays` return `E_NOTIMPL` (the passthrough is future work, unverifiable on this card — see the honest stub above). |

How to set it:

```
REM cmd (and build.bat, which runs the guest three times, once per mode):
set SCARLETT_RT_MODE=off
set SCARLETT_RT_MODE=software
set SCARLETT_RT_MODE=hardware

# PowerShell:
$env:SCARLETT_RT_MODE = 'off'
```

Values are case-insensitive (`OFF`, `Software`, `HARDWARE` all work).
The selected mode is visible two ways: the harness prints it in the
software-rt report header (`--- software-rt report (RT mode: off) ---`)
and the guest reports an `srt_mode` stage (requested value vs the
layer's effective mode) plus an `srt_tier` stage that shows the
effective path and, for `hardware` on tier 0, the fallback. The
mode is also readable at runtime through the new entry point
`ScSoftwareRT_GetMode()` (see the interface below).

Mode logic lives in `src/software_rt.cpp` + `include/software_rt.h`
only (`ScRtMode` enum, `ScSoftwareRT_GetMode`,
`ScSoftwareRT_ModeName` — the latter is layer-side only, not an HLE
export). `ScSoftwareRT_QueryPath` is mode-aware: `off` → path OFF
(the tier is still reported, for information only); `software` →
path SOFTWARE regardless of tier; `hardware` → tier > 0 → path
HARDWARE, tier == 0 → path SOFTWARE with the fallback log line.
`ScSoftwareRT_Init` compiles the compute PSO only on the software
path; `BuildScene` and `DispatchRays` branch on the same effective
path, so every entry point agrees about which mode is in force.

## The layer interface (flat entry points, not COM)

The layer's established pattern is flat C shim exports (the same as
`XGameRuntimeInitialize`, `D3D12XboxCreateDevice`, ...), not COM
wrappers. The software-RT surface follows it — these are
ScarlettCompat-defined entry points in `d3d12_xs.dll`, **not** Xbox
APIs, provided so a synthetic guest can exercise the software path
through the normal loader:

- `ScSoftwareRT_QueryPath(device, &path, &rtTier)` — which path this
  adapter takes, and the tier it reported.
- `ScSoftwareRT_GetMode()` — the selected RT mode (`off` /
  `software` / `hardware`) from `SCARLETT_RT_MODE` (see the mode
  section above).
- `ScSoftwareRT_Init(device)` — compile the compute PSO once, cache
  it. S_OK on success.
- `ScSoftwareRT_BuildScene(device, tris, triCount, &scene)` — CPU BVH
  build + GPU upload; returns a scene handle.
- `ScSoftwareRT_FreeScene(scene)`.
- `ScSoftwareRT_DispatchRays(device, cmdList, scene, &camera,
  outputUav, width, height)` — run the ray tracer into the caller's
  UAV texture. The caller owns the output resource and does its own
  readback (same pattern as the proof guest's triangle scene).

The guest passes the device it got from `D3D12XboxCreateDevice` and
its own command list — the backend runs on the same real device, in
the guest process, on the COMPUTE queue.

## The remaining integration: the COM wrapper (future work)

A real title does not call flat functions — it calls
`ID3D12Device5::CreateRaytracingPipelineState` and
`ID3D12GraphicsCommandList4::DispatchRays` /
`BuildRaytracingAccelerationStructure` on the COM device the layer
hands it. Routing those calls to this backend needs a COM vtable
wrapper around the device and the command list: the layer returns a
wrapped `ID3D12Device5` instead of the raw device, the wrapper
forwards every method except the RT ones, and the RT methods call
into `src/software_rt.cpp` (state object → scene handle, BLAS/TLAS
build → `ScSoftwareRT_BuildScene`, DispatchRays → 
`ScSoftwareRT_DispatchRays` with the shader tables translated into
records). That wrapper is NOT written here: it is ~100 forwarding
methods, and a mistake in it breaks every guest that uses the device
(including the phase 4/5 proof guests). The backend it would call
into is proven here; the wrapper is the next step, after phase 5
lands and the device-creation path is stable. Do not pretend a real
title's `DispatchRays` works today — it reaches the raw device and
fails with the tier unsupported, exactly as it would without this
work.

## Performance honesty

Software ray tracing is slower than hardware RT by an order of
magnitude for a real scene — the RT cores do BVH traversal and
triangle intersection in fixed-function hardware; the compute path
does both in shader code. Expect roughly 10–100x slower for a
full-scene workload (a hardware-RT frame that takes 2 ms of GPU
time would take tens of ms in compute), so a full title frame
through software RT is not 60 FPS on this card — it is a
correctness path, not a performance path. The proof scene (one
triangle, 256x256) is far too small to measure meaningfully; its
GPU time is dominated by dispatch overhead, not tracing. Do not
quote a speedup or a frame time for the real game from this work.

What the software path DOES buy: a title that calls DXR gets a
correct image back instead of a failed device call — the same trade
Lumen's software path makes. On a GPU with real RT cores the layer
takes the hardware path instead (once that passthrough exists).

## What is still missing for a real title's DXR workload

- The COM wrapper above (real-title routing).
- Hardware DXR passthrough for tier ≥ 1_0 adapters (cannot be
  verified on the 5700 XT).
- Mesh shaders (9,282 in the reference title): a separate problem — mesh shaders
  are not ray tracing and are not covered here. They need either a
  mesh-shader-capable GPU or a compute emulation of the mesh pipeline.
- The real shaders: the reference title's 26,503 shaders are precompiled RDNA2
  programs that need the DXIL lifting route (research brief §3).
  The software path traces with its own compute shader, not the
  game's raygen/miss/hit shaders — lifting those is still open.
- Multi-BLAS TLAS with instance transforms, any-hit, procedural
  primitives, recursion — all listed above as not implemented.
- Unified memory (`MEM_GRAPHICS`): the same open question as
  everywhere else in the layer.

## Verification (bar)

`tests/guest_software_rt.cpp` — a synthetic guest through the
harness: boots like the proof guest (runtime init → device), reads
the RT mode (`srt_mode` stage: requested vs effective), queries the
RT path (`srt_tier` stage — the expected path depends on the mode
and the tier), inits the backend, builds a one-triangle scene, and
dispatches rays into a 256x256 UAV. What "correct" means depends on
the mode: `software` — center == triangle red (255,0,0,255) and
corner == miss clear color (51,102,204,255); `off` — the output is
pre-filled with the miss color and the dispatch is a no-op, so
center AND corner must still be the miss color (proving the no-op
wrote nothing); `hardware` — on this tier-0 card it falls back to
software (same check as software); on a tier>0 adapter
`BuildScene`/`DispatchRays` return `E_NOTIMPL` and the stages pass
on that honest refusal.
Stages report as `srt_*` in guest_report.txt (7 stages: runtime,
device, mode, tier, init, scene, dispatch). `build.bat` runs the
guest three times — `SCARLETT_RT_MODE=software` (RC5), `=off`
(RC5b), `=hardware` (RC5c) — and the build fails if any of them
fails. Bar: clean rebuild `BUILD_EXIT=0`;
`guest_software_rt` ALL PASS under all three modes; all existing
guests still ALL PASS (no regressions).
