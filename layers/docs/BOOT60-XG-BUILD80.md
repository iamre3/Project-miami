# Boot 60 → build 80: the XG texture-computer path

Everything here is measured from `game_scarlett_master.exe` (image base
`0x140000000`, 120,181,760 bytes) and from the boot-60 log. Where a reading is
an inference it is marked **inference** and comes with the measurement that
would disprove it.

Boot #60 ended at `0xc0000005` reading address `0x40` at `0x1416F71FB`. The
log's last two lines were

```
XGSuggestSwizzleMode() -> S_OK (STUB)
XGCreateTextureComputer() -> S_OK (STUB)
```

The crash is the S_OK stub: `XGCreateTextureComputer` returned success and left
the out-parameter null; the exe ignores the HRESULT (`0x1416F7146` reads the
descriptor instead of testing `eax`), takes the object from `[rsp+0x38]` into
`r10`, and does `call [r10+0x40]` — vtable slot 8 — with `r10 = 0`.

## 1. Which side is tiled (pinned)

**Conclusion: the copy is linear source → tiled destination.**

Measured facts:

* The call at `0x1416F71FB` is `call [r10+0x40]` with
  `rcx = object`, `rdx = [rsp+0x40]` (= the caller's first pointer argument),
  `r8d = 0`, `r9d = r15d` (= `rdi + r14`, the accumulating subresource index),
  `[rsp+0x20] = arg2 + byte offset`, `[rsp+0x28] = row pitch`,
  `[rsp+0x30] = slice pitch`. Six parameters plus `this`, i.e.
  `(pDst, dstRowPitch, subresource, pSrc, srcRowPitch, srcSlicePitch)`.
* `r8d = 0` is the destination row pitch. A tiled destination has no row pitch;
  a *linear* destination with a zero row pitch cannot be addressed. So the
  destination is the tiled side.
* The row pitch at `0x1416F7250` is computed as
  `f(descriptor) * g(subresource) / 8` — bits-per-pixel × width / 8, the packed
  linear row stride of the subresource. Slice pitch at `0x1416F7280` likewise.
  Pitches describe the *linear* side, and the arguments that carry them sit next
  to the source pointer.
* The destination pointer `rdx` is the buffer the caller allocated immediately
  before (`0x14193DF37` calls `0x142474690(flags, type, size, align)` with
  `size/align` from `0x1451FBDD0` → thunk → `0x140343E80`), and that helper
  computes a *tiled* surface: it rounds width and height up to 4
  (`and r15, -4`, `and r13d, -4` at `0x140343ED4`/`0x140343ED8`), switches on the
  format class and returns 64 KiB-granular sizes. A fresh allocation with the
  tiled surface size is a destination, not a source: nothing has written it yet.
* The 56-byte descriptor the object is built from comes from the translator
  `0x144ECC940` (reachable through the `jmp` thunk `0x144ECC7F0`), and the same
  normalized descriptor feeds the allocation at `0x14193DF37`. Destination,
  layout and descriptor therefore all describe one resource.

What would disprove it: a call whose `r8d` (destination row pitch) is non-zero,
or a source pointer that lies inside a known placed resource while the
destination does not. Both are logged at run time by the build-80 copy slot,
which refuses (`E_INVALIDARG`, nothing written) rather than copying through a
misread argument list.

The earlier alternative reading — tiled source → linear destination — fit the
argument *positions* but not the provenance: it required the freshly allocated
tiled-size buffer to be read before anything was written into it.

## 2. Build 80 — what `xg_xs` now does

`src/xg_texture.cpp`, `include/xg_texture.h`; both stubs removed from
`src/hle_registry.cpp` and the registry rows retagged `kSketchy`.

`XGCreateTextureComputer(desc, &object)`

* Validates the descriptor is readable, then checks `type == 3` (measured: the
  translator writes 3 for a texture), non-zero width/height/depth/mips, mips
  ≤ 16, slices ≤ 16, bytes-per-element ∈ {1, 2, 4, 8, 16}.
* Builds a per-subresource layout: per mip, `w = max(1, width>>m)`,
  `h = max(1, height>>m)`, packed row pitch `w*bpe`, packed slice pitch
  `rowPitch*h`, tiled slab `ceil(w/tileW) * ceil(h/tileH) * 65536`, subresources
  mip-major then slice, each slab contiguous.
* Tile geometry by bytes-per-element: 1 → 256×256, 2 → 256×128, 4 → 128×128,
  8 → 128×64, 16 → 64×64 (all 64 KiB).
* Allocates a zeroed object: vtable pointer, refcount 1, magic, a copy of the
  56-byte descriptor, the computed layout. **All 16 vtable slots are
  initialised** — slot 1 is `Release` (measured: `call [rax+8]` at
  `0x1416F7233`), slot 2 `AddRef`, slot 8 the copy, slot 0 and 3–15 real
  functions that log the slot and its raw arguments and return `E_NOTIMPL`.
  No slot points at uninitialised memory.
* Unsupported descriptor → logged `E_INVALIDARG` with a null out-parameter, so a
  caller that ignores the HRESULT still cannot use a half-built object.

Slot 8 copy (measured shape above)

* Rejects: null pointers, non-zero destination row pitch, subresource out of
  range, source row pitch below the packed row, source slice pitch below
  `rowPitch*(h-1) + rowPitch`, a source span that is not fully committed and
  readable, and a destination subresource span that is not fully committed and
  writable (walked with `VirtualQuery`). Each refusal is logged with the numbers.
* The destination-span test is also the layout self-check: if this layer's
  layout size disagrees with the allocation the game sized from its own helper,
  the copy is refused and the mismatch is logged instead of written through.
* Copies by gathering rows when the caller's row pitch is wider than the packed
  row, then tiling with `ScOpt_Tile2D` (the project's Morton 64 KiB model).
  Returns `S_OK` only after the whole subresource was written.

Limits, stated plainly: the texel order inside a 64 KiB tile is the project's
Morton model, **not** verified against the GDK's per-mode tables (GDKX-gated).
What is verified is that the object exists, that every vtable slot is a real
function, that the descriptor/layout arithmetic is consistent, and that no copy
is reported successful unless the full validated span was written.

`XGSuggestSwizzleMode(desc, flags, out)`

* At `0x140C4B3CC` the exe sets `rdx = 0` and calls it in the branch where the
  requested mode is `0xff` (unspecified); the other branch stores `0x100|mode`
  into `[r8]`. So the export is asked to produce the layout dword.
* Build 80 echoes the descriptor's own `0x100|mode` when the descriptor already
  names one, and otherwise writes `0x100 | 1` — the id of the single 64 KiB
  model this layer's copy implements — so the suggested layout always describes
  what the copy path can actually do. Descriptors outside the table fail with
  `E_INVALIDARG` and no write. It never returns a mode it cannot copy.

## 3. Audit of `SC_DEFINE_SOK_STUB` expansions

`src/hle_registry.cpp:3435` defines the macro: it returns `S_OK` and ignores all
arguments. That is only safe for a function with no out-parameters. Every
expansion was checked; the ones that hand a caller an out-parameter it will read
are unsafe by construction, and boot #60 is one of them.

Unsafe (return success, caller reads an output the stub never wrote):

| export | out-parameter the caller reads |
| --- | --- |
| `XGCreateTextureComputer` | computer object — **fixed in build 80** |
| `XGSuggestSwizzleMode` | layout/mode dword — **fixed in build 80** |
| `ffxCreateContext` | context handle |
| `ffxQuery` | query result struct (`GetDesc`, jitter offsets, …) |
| `ffxDispatch` | none directly, but it reports work that never ran |
| `XApuDequeueResult` | result buffer + result count |
| `XApuGetResultCount` | count |
| `XDspConnectWithMaximumStreamLimit` | stream limit out |
| `BinkGetPlatformInfo` | platform info struct |
| `BinkGetFrameBuffersInfo` | frame-buffer descriptor array |
| `HCHttpCallCreate` | call handle |
| `HCHttpCallDuplicateHandle` | duplicated handle |
| `HCHttpCallRequestGetUrl` / `GetHeaderAtIndex` / `GetNumHeaders` / `GetRequestBodyBytes` / `GetRetryAllowed` / `GetRetryCacheId` / `GetRetryDelay` / `GetTimeout` / `GetTimeoutWindow` | each writes a scalar or string the caller then uses |
| `HCHttpCallResponseGetHeader` / `GetHeaderAtIndex` / `GetNumHeaders` / `GetNetworkErrorCode` / `GetPlatformNetworkErrorMessage` / `GetResponseBodyBytes` / `GetResponseBodyBytesSize` / `GetResponseString` / `GetStatusCode` | response fields; a zeroed status code reads as a valid HTTP 0 |
| `HCSettingsGetTraceLevel` | trace level |
| `BinkGetKeyFrame` / `BinkShouldSkip` | boolean answers, safe to be `S_OK`+0 only if 0 means no |

Safe as-is (no out-parameter, or the value is never read): the `XDspActivate`
/`Deactivate`/`Disconnect`/`SubmitCommand` family, `HCInitialize`,
`HCCleanupAsync`, `HCHttpCallCloseHandle`, `HCHttpCallPerformAsync`,
`HCHttpCallRequestSet*`, `HCHttpCallSetTracing`, `HCTraceImplMessage`,
`HCTraceSetEtwEnabled`, `XApuConnect`/`Disconnect`/`EnqueueCommand`,
`ffxDestroyContext`, `BinkClose`/`BinkDoFrame`/… (the void stubs).

Ranked by what the boot actually reaches: `XGCreateTextureComputer` (fixed),
`XGSuggestSwizzleMode` (fixed), then `XApuGetResultCount`/`XApuDequeueResult`
and `BinkGetPlatformInfo` are the next out-parameter reads in the boot path.
They are not touched in build 80.

## 4. The four `XGCreateTextureComputer` call sites

Export thunk `0x145F3FF40`, IAT slot `0x146793F60`. Descriptor in `rcx`, out
object pointer in `rdx` at every site. Each site takes whatever the out
parameter holds and calls a vtable slot through it.

| # | call site | containing function | HRESULT checked? | vtable slot(s) reached | notes |
| --- | --- | --- | --- | --- | --- |
| 1 | `0x140344235` | `0x140343xxx` cluster next to the layout helper `0x140343E80` | no | slot 10 (`+0x50`) with 5 arguments | the second half of the build-80 vtable requirement: slot 10 is a logging `E_NOTIMPL` stub, so if this site runs the log records its arguments |
| 2 | `0x140979C75` | texture-upload path | no | not reached in boot #60 | object released through `+8` |
| 3 | `0x1416F7141` | `0x1416F7100..0x1416F724F`, called from `0x14193DF4B` inside `0x14193DE10` (normalizes a descriptor, allocates the tiled buffer, loops planes) | no — `0x1416F7146` reads the descriptor instead | slot 8 (`+0x40`) at `0x1416F71FB`, then release via `+8` at `0x1416F7233` | **the boot-#60 crash**; this is the site build 80 implements |
| 4 | `0x145121A5E` | texture-creation wrapper | yes — `0x144E9B660` + `js` | slot 4 (`+0x10`) then release (`+8`) | the only site that behaves like a normal COM caller |

Upload path callers of `0x14193DE10`: `0x140325141`, `0x14034BFFC`,
`0x14131A288`. In the first, the copy's source argument is a struct with
`[0]=0`, `[0x10]=pointer`, `[0x18]=index`; the third passes a stack value. The
build-80 copy slot logs the concrete pointers on the first call, which settles
which of them is the live path for boot #61.

`XGSuggestSwizzleMode` (thunk `0x145F3FF50`, IAT `0x146793F68`) has two sites:
`0x140C4B3CE` (sections above) and `0x144ECCA2` (inside the descriptor
translator `0x144ECC940`).

## 5. What was deliberately not touched

256 MB physical-page mapping cap, slot 74 registration/index, the arena
dispatcher (`kind 0x808000`), the descriptor-reservation workaround, the
D3D12.X slots 39/41/61/70/24/86/16 that answer `E_NOTIMPL`. Leonida and XCL
remain read-only references; no code was copied from either, and the XCL
handoff confirms XCL has no `XGCreateTextureComputer` of its own
("TomBox reimplements xg_xs"), so there was no reference implementation to
borrow even if that had been allowed.
