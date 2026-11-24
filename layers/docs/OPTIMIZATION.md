# Optimizations — every reasonable AMD benefit

Why this exists: the layer works (phases 1–4 proven, phase 5 and the
software-RT path in progress), but "works" is not "fast". A compat
layer adds a translation step to every call a title makes, and on the
build machine — an RX 5700 XT (RDNA1, FL 12_1: no hardware RT, no
mesh shaders) — the layer has to earn its overhead back. Seth's rule
from the DX11 work applies here too: **no speculative change
presented as a fix — every optimization needs its source location,
measured cost, correctness conditions, and a before/after
comparison.** An item with no measured win is reported as no win,
not claimed as one.

This document is the design. The implementation is `src/opt_*.cpp` +
`include/opt_*.h`; the proof is `tests/guest_opt.cpp` through the
harness (stages report as `opt_*`).

## What "reasonable" means

An optimization is reasonable here when it is:

1. **Implementable and measurable on this card with synthetic
   workloads, without the exe.** Nothing here needs real game data.
2. **A real cost in the layer's own path**, not a guess about the
   game's behaviour. The cost is measured in the harness, before
   and after, on the 5700 XT.
3. **Correctness-preserving.** Every item carries a correctness
   check (byte compare, pixel compare, round-trip) next to its
   timing numbers. A faster wrong answer is a regression.

What is NOT reasonable here (documented as gaps, not attempted):
the COM device/command-list wrapper that would route a *real*
title's raw D3D12 calls through these caches (same "remaining
integration" as docs/SOFTWARE-RT.md — ~100 forwarding methods, a
mistake breaks every guest); hardware-exact Xbox swizzle tables
and the BCPack rANS bitstream (both GDKX-gated — see items 2 and 6).

## The items (1–7 plus frame batching, item 9 overall)

### 1. PSO + root-signature caching (`src/opt_pso_cache.cpp`)

**Source location / cost.** A title creates graphics/compute PSOs
and root signatures as it loads levels and shaders. Each
`CreateGraphicsPipelineState` / `CreateRootSignature` is a driver
compile: shader bytecode validation + code generation + state
validation. A title that re-creates the same PSO (same shaders,
same blend/raster/depth state) pays the compile cost every time.
The layer cannot intercept the raw COM calls (no device wrapper
yet), so the cache is a layer-side component the title-side code
calls instead of the raw methods — the same flat-entry-point shape
as the software-RT backend.

**Design.** `ScOpt_PsoCacheInit(device)` creates one cache per
device (process lifetime). `ScOpt_PsoCacheGetGraphics(device, desc,
&pso, &hit)` hashes the pipeline description, looks it up, and on
a miss calls `device->CreateGraphicsPipelineState` and stores the
result; on a hit it `AddRef`s and returns the stored PSO.
`ScOpt_PsoCacheGetCompute` is the same for compute PSOs.
`ScOpt_PsoCacheGetRootSignature(device, blob, blobLen, &rs, &hit)`
hashes the serialized root-signature blob. `ScOpt_PsoCacheStats`
reports created/hits/misses. Hash is FNV-1a 64-bit over a canonical
serialization: shader bytecode *contents* (never the pointers),
plus every state field that affects the compiled output — the
desc struct is hashed field by field, not as raw struct bytes
(padding bytes are uninitialized and would make identical PSOs
hash differently). The cache is a fixed-size open-addressing hash
table (no per-lookup allocation); entries hold a COM pointer and
are released when the cache is destroyed.

**Correctness conditions.** Two descriptions hash equal only when
every field that changes the compiled PSO is equal (shader
bytecode bytes, blend/rasterizer/depth-stencil state, render-target
formats, sample desc, root-signature pointer identity). A hash
collision would return the wrong PSO — the table stores the full
canonical serialization alongside the hash and compares it on a
hit, so a collision degrades to a miss, never a wrong PSO.

**Measurement.** The guest creates the same graphics PSO 100
times through the cache (first = miss/compile, 99 = hits) and
times it; it also creates the same PSO 100 times through the raw
device call and times that. Reported: both totals, both per-call
averages, hit count. Same for one root signature.

### 2. De-tiled texture cache (`src/opt_detile.cpp`, `src/opt_texcache.cpp`)

**Source location / cost.** Xbox textures are stored tiled
(`XBOX_TILEMODE_SCARLETT`, 64KB tiles). Every tiled texture the
title touches must be de-tiled into a linear layout the desktop
GPU can sample. De-tiling the same texture every frame is pure
wasted CPU work; doing it once and caching it is the win. But the
cache itself costs VRAM, and a large title has gigabytes of textures — an
unbounded cache would eat the card. So the cache is built with
memory management, not just the de-tiling (see below).

**Design — two layers.** The tile/untile pair works on CPU buffers:
`ScOpt_Tile2D(linear, w, h, bpp, tileW, tileH, tiled)` and
`ScOpt_Detile2D(tiled, w, h, bpp, tileW, tileH, linear)`. Within a
64KB tile, texels are stored in Morton (Z-order) — bit-interleaved
x/y — which is the documented shape of the Xbox swizzle
(D3D12's `D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE` is the
same idea on desktop: a tile holds nearly the same number of
texels in each cardinal dimension). Tile dimensions are
parameterized (tileW x tileH texels, tileW*tileH*bpp = 65536) so
the table can be corrected per tile mode. `ScOpt_DetileCached(key,
tiled, w, h, bpp, tileW, tileH, &linear, &hit)` hashes the tiled
input (FNV-1a over the bytes + dimensions + mode) and returns the
cached linear buffer on a hit instead of recomputing.

On top of that sits the **GPU texture cache**
(`src/opt_texcache.cpp`) — the layer the game actually binds.
It holds DEFAULT-heap textures (the de-tiled resource, in
`PIXEL_SHADER_RESOURCE` state), keyed by a caller-provided
uint64 texture id, under a VRAM budget with LRU eviction.

**Memory management — the budget.** The GPU cache is created
with a byte cap: `ScOpt_TexCacheInit(device, budgetBytes, &cache)`.
`budgetBytes == 0` means the default, **512 MB** (the 5700 XT has
8 GB; the cache must leave room for the frame, the PSO cache and
the game's own resident set). `bytesUsed` is the sum of `w*h*bpp`
over live entries and is reported by `ScOpt_TexCacheStats`
alongside the budget, so a run can prove the cap held. The CPU
buffer cache has its own separate 64 MB cap (it stores a tiled +
a linear copy per entry, so 2×`w*h*bpp` per entry).

**Memory management — LRU eviction.** When a store would push
`bytesUsed` over the budget, the cache evicts the **least
recently used** entries first (a doubly-linked LRU list, head =
most recently used, tail = least recently used) until the new
texture fits. Every lookup hit refreshes an entry's recency.
Evictions are counted and reported. Two edge cases are handled
honestly, not silently: (a) a single texture larger than the
budget is stored alone (everything else evicted) with a warning
log line — it is needed right now and cannot be split; (b) the
CPU cache has a fixed 64-slot table, so a texture whose stored
pair alone exceeds its 64 MB budget is computed but never stored.
The CPU cache also evicts LRU entries (per-entry `lastUsed` tick)
when its table or budget fills — the same policy, at both layers.

**Memory management — the lookup path (on every bind/use).**
The bind-time contract, in order:
1. `ScOpt_TexCacheLookup(cache, key, &res, &hit)` — hit: bind
   `res` (the caller owns the returned reference; `Release` it
   after binding) and the entry becomes most-recently-used.
   Miss: go to step 2.
2. `ScOpt_TexCacheStore(cache, key, tiled, w, h, bpp, tileW,
   tileH, queue, &res)` — de-tile + upload, evicting the coldest
   textures first if the budget would be exceeded, and return a
   texture that is ready to bind (the store waits for the GPU
   work to finish, so the caller never binds a half-built
   texture). A store of a key that is already cached returns the
   existing resource instead of doing the work twice.
3. When the game unloads a texture,
   `ScOpt_TexCacheEvict(cache, key)` drops it from the cache
   (`S_OK` when it was there, `S_FALSE` when it was not). The
   cache works WITH the game's streaming: it never holds a
   texture the game freed, and it never resurrects an evicted
   texture on its own. (The game, not the cache, owns the
   streaming decision; the cache is the mechanism the game's
   texture-load path calls.)

**Memory management — de-tile (GPU compute) + upload.** On a
miss the store de-tiles with a GPU compute shader (one thread
per texel, the same Morton mapping as the CPU pair — proven
byte-identical to the CPU de-tiler by full-texture readback in
the guest) for bpp 4/8/16, and uploads the result into a
DEFAULT-heap texture. bpp 1/2 take the CPU de-tile + upload path
instead: the compute shader copies whole 4-byte words, so
sub-word formats cannot be de-tiled on the GPU without
clobbering neighbouring texels. The GPU work (upload → DEFAULT
buffer → compute dispatch → texture in `PIXEL_SHADER_RESOURCE`)
is done OUTSIDE the cache lock, so a store never blocks lookups
while the GPU is busy. The GPU de-tile shader is compiled once
at cache init; the per-store command allocator/list is created
per call (a streaming-load path, not a per-frame hot path).

**Correctness conditions.** Round-trip exactness: tile then detile
must reproduce the input byte-for-byte for every tested size and
bpp. The cache key includes the full input hash, so a hit can only
return data de-tiled from identical input. The GPU de-tiler must
reproduce the CPU de-tiler byte-for-byte (readback comparison).
The LRU order must be exactly the recency order: after a known
sequence of stores and lookups, the evicted key must be the one
that was least recently used, and a lookup of an evicted key
must miss. `bytesUsed` must never exceed the budget except for
a single over-budget texture stored alone.

**What is NOT claimed.** The exact XG swizzle tables live in the
GDK XG library (GDKX-gated) — DirectXTex's Xbox `Tile`/`Detile`
require XG at runtime. The Morton swizzle implemented here is the
documented *shape*; whether its bit layout matches the hardware's
exact per-mode tables is **unverified until real Xbox tiled data
lands**. The cache itself — the actual optimization — is fully
proven by the round-trip + hit test regardless of the swizzle
table: when the table is corrected, the cache keeps working.
Also not claimed: the cache is not yet hooked into the game's
actual texture bind — the lookup/store/evict contract is the
bind-time API the game's texture-load path calls, and the
interception of the game's own bind happens when the layer
translates the game's texture calls (future work, needs real
texture traffic). The budget default (512 MB) is a reasoned
starting point for an 8 GB card, not a measured optimum — the
right budget is whatever the game's working set turns out to be,
and the stats make that measurable.

**Measurement.** The guest de-tiles a synthetic 256x256 RGBA8
texture twice through the CPU cache and times both calls; it also
round-trips (tile → detile) and byte-compares. The guest's
`opt_texcache` stage then measures the GPU cache: on a 1 MB
budget (4 textures fit) it stores keys 1–4, stores 5 (evicts 1,
the LRU), looks up 1 (miss) / 2, 3 (hits), stores 6 (evicts 4,
now the LRU), looks up 4 (miss) / 5 (hit), reads back a stored
texture and byte-compares it against the CPU de-tile, and
exercises the game-unload path (evict key 2, then it misses).
Then a streaming workload on a 2 MB budget: 60 accesses, 8 of
10 to the hot set {1,2,3} and 2 of 10 to cold keys {4,5} —
reported as the hit rate plus `bytesUsed` vs the budget and the
eviction count. Reported: first (compute) time, second (hit)
time, round-trip exact or not, GPU de-tile byte-exact or not,
LRU eviction order as expected or not, streaming hit rate,
`bytesUsed`/budget, evictions.

### 3. Mesh-shader fallback for RDNA1 (`src/opt_mesh_fallback.cpp`)

**Source location / cost.** The reference title has 9,282 mesh shaders. Mesh
shaders (amplification + mesh) need FL 12_2; the 5700 XT is FL
12_1 and has no mesh-shader hardware. A title's mesh-shader draw
would fail outright on this card. The fallback replaces the mesh
pipeline with the classic pipeline the card does support.

**Design.** A meshlet is a small cluster of triangles: vertices
(position + color), indices, and a primitive list. The fallback
expands a meshlet into a plain vertex buffer + index buffer and
the guest draws it with a normal graphics PSO + `DrawIndexed`
(the same triangle path the proof guest already proves).
`ScOpt_MeshFallbackInit(device)` compiles the fallback vertex
shader once. `ScOpt_MeshFallbackExpand(device, meshlet,
&expanded)` uploads the expanded buffers and returns them
(`expanded` owns the GPU resources; freed with
`ScOpt_MeshFallbackFreeExpanded`). Expansion is done on the CPU
in this phase: it is exactly right and simple to prove. A GPU
compute expansion is the performance follow-up (documented
below), not this phase.

**Correctness conditions.** The expanded draw must produce the
same pixels as the same geometry drawn directly: the guest draws
one triangle meshlet through the fallback and checks center red
(255,0,0,255) / corner clear (51,102,204,255), the same pixels
the proof guest and the software-RT guest check.

**What is NOT covered.** Amplification shaders (which select and
cull meshlets) are not emulated — the caller passes the meshlet
list in; culling is the caller's job. GPU compute expansion
(faster, per-frame) is future work. This is a correctness path,
not a performance path: expanding 9,282 meshlets on the CPU per
frame would be slow, and the doc says so plainly.

**Measurement.** The guest expands one triangle meshlet and
draws it; reported: expand time, draw + readback pixel check
pass/fail. (Boundary: software RT is the other agent's job —
mesh shaders are this item's.)

### 4. Async compute mapping (`src/opt_async_compute.cpp`)

**Source location / cost.** Xbox titles lean on async compute:
graphics work on the DIRECT queue, compute work on the COMPUTE
queue, overlapping. On RDNA1 this is fiddly — graphics and
compute contend for the same CUs, and if the balance is wrong
async compute hurts frame time instead of helping. RDNA2's
scheduling is more forgiving. So this item is a *measurement*,
not an assumed win: map the work, time both ways, report the
numbers on the 5700 XT.

**Design.** `ScOpt_AsyncRunTimed(device, mode, &ms)` runs a fixed
synthetic workload N times and reports the average wall time
(QPC around fence completion): a compute dispatch of known size
(a compute shader that writes a buffer) plus a graphics draw.
Mode `serial` runs compute on the DIRECT queue, then the draw on
DIRECT. Mode `async` runs compute on the COMPUTE queue
concurrently with the draw on DIRECT, fence-synced at the end.
The layer provides the workload, the queues, and the timing; the
guest calls it in both modes and reports both numbers.

**Correctness conditions.** Both modes must produce the same
compute output (byte-compared) and the same draw pixels — async
must not change the result, only the timing. The fence sync
guarantees the compute output is visible before readback.

**Measurement.** Reported: serial average ms, async average ms,
and the verdict (async faster / slower / within noise) *on this
card, for this workload*. No claim is made about a real title's actual
async mix — that needs the real title's workload.

### 5. Descriptor heap management (`src/opt_desc_heap.cpp`)

**Source location / cost.** Xbox and D3D12 model descriptors
differently; creating a fresh descriptor heap per allocation (or
per frame) is heap-creation churn — a CPU cost paid every frame
and a source of fragmentation. A pooled allocator removes it.

**Design.** `ScOpt_DescHeapInit(device, heapType, capacity)` creates
one shader-visible descriptor heap of `capacity` slots and manages
it with a free-list (a stack of free indices; allocate pops, free
pushes). `ScOpt_DescAlloc(heap, &cpuHandle, &gpuHandle, &index)`
and `ScOpt_DescFree(heap, index)`. When the heap is full the
allocator grows by chaining a second heap of the same capacity
(the chain is transparent to the caller: handles carry the heap
they came from). Descriptor size comes from
`GetDescriptorHandleIncrementSize`, so the allocator is correct
for every heap type.

**Correctness conditions.** A freed index is never handed out
twice while still in use (free-list discipline); handles are
computed from the real increment size, so they point at real
slots. The guest test proves reuse: allocate N, free every other
one, allocate N/2 again — all must succeed without growth, and
every returned handle must be unique among live allocations.

**Measurement.** The guest allocates/frees K descriptors in a
loop through the allocator and times it, versus creating a fresh
heap per descriptor (the naive path) and times that. Reported:
both totals, allocator heap count after the reuse test.

### 6. DirectStorage + BCPack (`src/opt_dstorage.cpp`)

**Source location / cost.** RAGE streams the world continuously;
on Xbox that rides DirectStorage with hardware decompression. A
slow storage shim stalls the world streamer before graphics ever
matters. The question this item answers is: where does
decompression happen (GPU vs CPU), and does the queue path beat
synchronous reads?

**Design.** `ScOpt_DStorageInit()` creates a DirectStorage
factory + queue from the *desktop* DirectStorage API
(`dstorage.dll`, in the Windows SDK) when it is present; when it
is not, the layer falls back to overlapped Win32 file I/O
(`ReadFile` with `OVERLAPPED` + event), which has the same async
semantics with a CPU copy. `ScOpt_DStorageReadFile(path, offset,
size, &data, &bytesRead)` enqueues an async read through the
queue and waits on its fence/event. The decompression stage is a
pluggable interface: `ScOpt_DStorageDecompress(mode, src, srcLen,
dst, dstLen)` with modes `NONE`, `ZLIB` (RFC 1950, CPU — the
format the Xbox hardware also supports, and the one whose
bitstream is public), and `BCPACK_BC1..BC7` (the Xbox per-format
modes, from the public `DSTORAGE_BCPACK_MODE` enum).

**What is NOT implemented — stated plainly.** BCPack decode is
**not implemented**. BCPack is a custom entropy coder designed
specifically for BCn data (color endpoints separated from palette
indices, compressed with a rANS algorithm), implemented in the
Xbox hardware decompression block. The rANS bitstream format is
not public — the GDK docs describe what BCPack *is*, not the
stream layout — so no correct decoder can be written from public
documentation. PC DirectStorage uses GDeflate (Deflate/Huffman),
not BCPack, so there is no desktop decoder to reuse. Writing a
decoder from a guessed bitstream would be worse than refusing
one. The queue path, the stage interface, the mode enum, the
zlib CPU reference stage, and the GPU-vs-CPU measurement are
real; BCPack decode is the documented gap, waiting on the rANS
stream format (or real BCPack data to validate against).

**Measurement.** The guest writes a synthetic file, reads it
back through the queue path and through synchronous `ReadFile`,
and times both; it also round-trips a zlib block through the
decompression stage and byte-compares. Reported: queue-read time
vs sync-read time, zlib round-trip exact or not, and the
GPU-vs-CPU decision (for this card: the queue path with CPU zlib
— GPU GDeflate is a desktop format the Xbox data does not use).

### 7. Shader precompilation cache (`src/opt_shader_cache.cpp`)

**Source location / cost.** Every shader a title uses is
compiled the first time it runs — a hitch the first time each
shader appears (level load, first sight of an effect). A disk
cache keyed by the shader source means the compile happens once,
ever, and later runs load the blob from disk.

**Design.** `ScOpt_ShaderCacheInit(dir)` sets the cache
directory (`%LOCALAPPDATA%\ScarlettCompat\ShaderCache` by
default). `ScOpt_CompileCached(source, sourceLen, profile, entryPoint,
&blob, &blobLen, &hit)` hashes the source + profile + entry point (FNV-1a),
looks for the blob on disk, and on a hit reads it; on a miss it
compiles with `D3DCompile`, writes the blob to disk under the
hash name, and returns it. `ScOpt_ShaderCacheStats` reports hits/misses. A blob is only trusted when its stored hash
matches the requested hash (the file format is magic "SCSC" + sourceHash + blobSize + blobHash,
and the blob is re-hashed on load — a corrupt or mismatched file is
treated as a miss and recompiled, never trusted).

**Correctness conditions.** The blob returned on a hit must be
byte-identical to the blob from a fresh compile of the same
source + profile (the guest byte-compares), and it must create a
valid PSO (the guest creates one from the cached blob).

**Measurement.** The guest compiles the same HLSL shader twice
through the cache and times both calls; reported: first
(compile) time, second (hit) time, byte-identical or not.

### 8. Frame batching (`src/opt_batching.cpp`) — overall item 9

Three levers, built and measured separately. The hard limits
apply to all three: the game's draw/submission order is never
reordered (transparency and depth depend on it), and the 16.7 ms
frame pacing is preserved — batching that adds latency reports
the latency cost alongside any win.

**Lever 1 — command-list submission batching.**

**Source location / cost.** The layer translates the game's calls
into D3D12 command lists. Each `ExecuteCommandLists` call is a
driver/kernel transition with validation and queue bookkeeping;
submitting one list at a time pays that cost per list. A title
that records many small lists per frame (one per draw group, one
per compute dispatch) pays it many times per frame.

**Design.** `ScOpt_BatchInit(queue, maxLists)` creates a
submission batcher for one queue. `ScOpt_BatchAdd(list)` appends
a closed command list to the pending batch (FIFO — submission
order is exactly the order lists were added, never reordered).
`ScOpt_BatchFlush()` submits the whole pending batch in a single
`ExecuteCommandLists` call, or does nothing when the batch is
empty. `ScOpt_BatchStats` reports lists submitted and
`ExecuteCommandLists` calls made, so the guest can verify the
batching actually reduced the call count. A flush happens
automatically when the batch reaches `maxLists`, so a caller that
forgets to flush still submits (at the batch boundary, not at
frame end — the batcher never holds a list past the caller's
flush point).

**Correctness conditions.** Lists execute in the order added;
a flush submits exactly the lists added since the last flush,
once each; an empty flush is a no-op. The GPU work is identical
to submitting the same lists one at a time — only the number of
submission calls changes.

**Measurement.** The guest records K small lists (each a trivial
clear) and times K separate `ExecuteCommandLists` calls versus
one batched call with all K lists. Reported: both CPU times, the
per-call overhead saved, and the call counts from
`ScOpt_BatchStats`.

**Lever 2 — draw/state batching.**

**Source location / cost.** The DX11 work measured ~2,556
redundant `OMSetRenderTargets` calls and ~10,000 redundant
SRV/sampler sets per frame in the Pinyon backend — the same
shape of waste exists in any translated stream: the game sets
the same PSO, root signature, and render targets again and again
because each draw is recorded independently. Each redundant set
is a (cheap but real) CPU call during recording, and consecutive
draws with identical state are separate `DrawInstanced` calls
that could be one.

**Design.** `ScOpt_DrawBatchBegin(list)` starts a recording
session on one command list. `ScOpt_DrawBatchSetPSO(list, pso)`,
`ScOpt_DrawBatchSetRootSignature(list, rs)`, and
`ScOpt_DrawBatchSetRenderTargets(list, ...)` forward the set to
the list only when the value differs from the one already set —
identical re-sets are skipped and counted. `ScOpt_DrawBatchDraw-
Instanced(list, vertexCount, startVertex)` records a draw; when
the next draw has the same PSO, root signature, vertex buffer,
and a `startVertex` exactly equal to the previous draw's end, the
two are merged into a single pending `DrawInstanced` (same
vertices, same order, one call). A state change or a gap flushes
the pending draw first. `ScOpt_DrawBatchEnd(list)` flushes any
pending draw. `ScOpt_DrawBatchStats` reports draws requested vs
draws recorded, and state sets requested vs sets issued. This
builds on item 1 (the PSO cache): the batcher dedupes *sets* of
cached PSOs, it does not cache PSOs itself.

**Correctness conditions.** Merging only happens for consecutive
draws with identical state and contiguous vertex ranges — the
merged draw renders exactly the vertices the two draws rendered,
in the same order. Any state change, any gap in the vertex range,
or any non-draw command between two draws breaks the merge (the
pending draw is flushed first). State-set deduping never changes
the *effective* state at any draw — the list always holds the
last value the game set.

**Measurement.** The guest issues a synthetic sequence (draws
with repeated state sets and runs of contiguous same-state draws)
through the batcher and times the recording, versus recording
the same sequence with raw calls. Reported: draws requested vs
recorded, state sets requested vs issued, and both recording
times.

**Lever 3 — frames in flight / queue depth.**

**Source location / cost.** A frame's CPU work (record +
submit) and the GPU's work overlap only as far as the queue
lets them: with N frames in flight, the CPU can be recording
frame F+N while the GPU renders frame F. Too few frames in
flight and the GPU starves waiting for the CPU (the DX11 work's
10 FPS ceiling had a component of this shape — the title thread
waiting on the GPU fence). Too many and input latency grows
(each queued frame is ~16.7 ms of latency the player feels).

**Design.** `ScOpt_FlightInit(depth)` creates a frame-slot
manager for a depth of 2, 3, or 4 (other depths are
`E_INVALIDARG` — the tested range is 2–4). One fence value per
slot; `ScOpt_FlightBeginFrame()` waits for the slot being reused
(frame index mod depth) to complete before the CPU records into
it, and reports how long it waited; `ScOpt_FlightEndFrame(queue,
fence)` signals the frame's fence value. `ScOpt_FlightStats`
reports frames run, total CPU wait time, and average wait per
frame. The workload per frame is fixed (a compute dispatch +
a triangle draw to a 256x256 target — the same shape as the
async item's workload) so the depths are compared on identical
work.

**Correctness conditions.** A slot's resources are never
recorded into while the GPU is still reading them (the wait
guards the reuse). Frame order is preserved (slot N is always
frame F, F+depth, F+2*depth, ...). The 16.7 ms pacing is a
measurement input, not something the manager enforces — the
manager reports the wait, and the guest reports the frame time,
so a depth that starves the GPU or adds latency shows up in the
numbers.

**Measurement.** The guest runs 60 frames of the fixed workload
at each depth (2, 3, 4) and reports, per depth: average frame
time (wall time per frame), average CPU wait-for-slot per frame,
and the implied input latency (depth x average frame time).
The tradeoff is read off the table, not assumed: if depth 4 is
not faster than depth 3, the doc says so.

## The layer interface (flat entry points, not COM)

Same pattern as the software-RT backend: ScarlettCompat-defined
entry points in `d3d12_xs.dll`, **not** Xbox APIs, so a synthetic
guest can exercise each optimization through the normal loader.
Registered in `src/hle_registry.cpp` (table + `HleScOpt*`
wrappers), declared in `include/opt_*.h`, exported in
`defs/d3d12_xs.def`:

- PSO cache: `ScOpt_PsoCacheInit`, `ScOpt_PsoCacheGetGraphics`,
  `ScOpt_PsoCacheGetCompute`, `ScOpt_PsoCacheGetRootSignature`,
  `ScOpt_PsoCacheStats`
- Detile cache: `ScOpt_Tile2D`, `ScOpt_Detile2D`,
  `ScOpt_DetileCached`
- Mesh fallback: `ScOpt_MeshFallbackInit`,
  `ScOpt_MeshFallbackExpand`, `ScOpt_MeshFallbackFreeExpanded`
- Async compute: `ScOpt_AsyncRunTimed`
- Descriptor heap: `ScOpt_DescHeapInit`, `ScOpt_DescAlloc`,
  `ScOpt_DescFree`, `ScOpt_DescHeapDestroy`
- DirectStorage: `ScOpt_DStorageInit`, `ScOpt_DStorageReadFile`,
  `ScOpt_DStorageDecompress`
- Shader cache: `ScOpt_ShaderCacheInit`, `ScOpt_CompileCached`,
  `ScOpt_ShaderCacheStats`

## The remaining integration: the COM wrapper (future work)

A real title does not call flat functions — it calls
`ID3D12Device::CreateGraphicsPipelineState`,
`ID3D12GraphicsCommandList::DispatchMesh`, `IDStorageQueue::EnqueueRequest`,
and so on, on the COM device the layer hands it. Routing those
calls through these caches needs the COM vtable wrapper around
the device and the command list (the same wrapper
docs/SOFTWARE-RT.md names for DXR). That wrapper is NOT written
here: it is ~100 forwarding methods, and a mistake in it breaks
every guest that uses the device. The components it would call
into are proven here; the wrapper is the next step, after phase 5
lands and the device-creation path is stable. Do not pretend a
real title's PSO creation hits this cache today — it reaches the
raw device, exactly as it would without this work.

## Performance honesty

Every number in the verification section is a measurement of a
*synthetic* workload on the RX 5700 XT, not a claim about any specific title.
A cache hit avoids a driver compile; it does not make the game
faster by a quoted percentage. Async compute may be slower on
RDNA1 — if the measurement says so, the doc reports slower.
Software expansion of meshlets is a correctness path. Nothing
here quotes a frame time or a speedup for the real game.

## Verification (bar)

`tests/guest_opt.cpp` — a synthetic guest through the harness:
boots like the proof guest (runtime init → device), then runs
one stage group per item (see the measurement paragraphs above)
and reports each as `opt_<item>` plus a per-stage detail line
carrying the measured times. Bar: clean rebuild `BUILD_EXIT=0`;
`guest_opt` ALL PASS; all existing guests still ALL PASS (no
regressions). An item whose measurement shows no win is reported
as no win in the guest detail and in docs/STATUS.md — the guest
still passes, because the item is *correct*; it just did not
measure faster.

## Measured results (2026-11-23, RX 5700 XT, build 8 — BUILD_EXIT=0)

**Re-verified in build 9 after the guest cleanup** (diagnostic
markers removed, texcache detail string fixed): BUILD_EXIT=0,
all 9 verify runs ALL PASS, **62 stage PASS lines / 0 FAIL**.
Per-run timings moved slightly, conclusions unchanged (async
15.7% faster this run; batch_draw batched recording 0.048 ms vs
raw 0.065 ms — faster this run, where build 8 had it slower on
the same 8-draw sequence; the draw-count reduction 8→3 is the
stable win, not the recording time on a tiny sequence).

`guest_opt` 13/13 stages pass; all other guests still ALL PASS
(minimal, ordinal, proof, era 21/21, software-rt 7/7 under all
three RT modes, physics 7/7). Every number below is from the
harness log `F:\ScarlettOpt\build_opt.log` (build 8), synthetic
workloads only — not a claim about any specific title.

- **1. PSO + root-signature cache:** raw 10x
  `CreateGraphicsPipelineState` 2.072 ms; cached 10x
  `ScOpt_PsoCacheGetGraphics` 0.425 ms; created=3 hits=10
  misses=3. A ~5x cut on repeated PSO creation.
- **2. De-tiled texture cache (CPU):** 256x256 RGBA8 round-trip
  byte-exact; miss (de-tile) 1.169 ms, hit (cache copy) 0.257 ms;
  detiles=1 hits=1 evictions=0. GPU cache (item 2 memory
  management): de-tile byte-exact vs the CPU de-tiler by
  full-texture readback; 1 MB budget, LRU order verified
  (store 1–4, store 5 evicts 1, store 6 evicts 4, unload evict
  drops key 2 — final entries=3, used=768 KB, evictions=2);
  streaming workload 55 hits / 5 misses = **91.7%** (want
  >= 80%), entries=5, used=1,310,720/2,097,152, evictions=0.
- **3. Mesh-shader fallback:** meshlet 4 verts / 6 indices
  expanded + drawn; centre (255,0,0,255), corner
  (51,102,204,255). Correctness path (CPU expansion), not a
  performance claim.
- **4. Async compute:** serial 0.292 ms, async 0.259 ms —
  **async FASTER, 11.6%** on this fixed workload (262,144
  threads x 512 xorshift rounds + a triangle draw); checksums
  equal (0xd370c8f7d551c72b both modes). A measured win on
  RDNA1 for this workload shape, not a blanket claim — a
  different mix could contend differently.
- **5. Descriptor heap:** pool alloc/free 10,000x 0.280 ms vs
  heap-create 10,000x 8,355.408 ms; growth 16 -> 32 slots
  (2 heaps), reuse ok, live=8 heaps=2.
- **6. DirectStorage + BCPack:** batch 16x4 MB 26.998 ms, sync
  16x4 MB 70.993 ms (overlapped Win32 I/O — the desktop
  DirectStorage runtime is not on the build machine, so the
  queue path is overlapped I/O with the same async semantics);
  single/batch hashes match; NONE ok; ZLIB stored/fixed/dynamic
  all ok; BCPack BC7 E_NOTIMPL (rANS bitstream not public).
- **7. Shader precompilation cache:** compile (miss) 2.764 ms,
  load (hit) 0.223 ms; hit/miss blobs byte-identical; 2 cache
  files corrupted then re-compiled to original bytes; hits=2
  misses=4.
- **9. Frame batching, lever 1 (submission):** 16 lists: 16
  separate submits 0.260 ms vs 1 batched submit 0.040 ms;
  batcher lists=16 calls=1. ~6.5x fewer submission calls.
- **9. Frame batching, lever 2 (draw/state):** draws 8
  requested -> 3 recorded; sets 10 requested -> 6 issued;
  batched record 0.068 ms vs raw 0.042 ms (recording is
  slightly slower — the merge bookkeeping costs more than it
  saves on 8 draws; the win is fewer draw calls issued, which
  matters at title scale, not at this size). Centre red,
  corner clear — the merged draws render the same triangles in
  the same order.
- **9. Frame batching, lever 3 (frames in flight):** depth 2:
  frame 0.416 ms, CPU wait 0.384 ms/frame, input latency
  ~0.8 ms; depth 3: frame 0.213 ms, CPU wait 0.183 ms/frame,
  latency ~0.6 ms; depth 4: frame 0.201 ms, CPU wait
  0.176 ms/frame, latency ~0.8 ms. Depth 3 is the sweet spot
  on this workload — depth 2 starves the GPU (0.384 ms wait),
  depth 4 adds nothing over depth 3.

### Bugs found by the verification runs (fixed, all in the tree)

- **Batch submit crash (builds 2–5):** the opt guest crashed
  (0xc0000409 fastfail, then 0xc0000005 in `amdxc64.dll`) in
  the batch-submit recording loop. Root cause was a guest bug:
  the stage created an RTV heap but never called
  `CreateRenderTargetView`, so `ClearRenderTargetView` read a
  garbage descriptor and the AMD driver faulted on it. Fixed
  by writing the RTV descriptor before the clear (same fix in
  the batch-draw stage). Also added a setup guard so a failed
  setup skips the loop instead of recording with a null handle.
- **Batch draw counts (build 6):** the stage read Stats before
  flushing the pending merged draw, so it counted 2 recorded
  draws instead of 3, and the drawn triangles did not cover the
  centre pixel the stage checks. Fixed by calling
  `ScOpt_DrawBatchFlushDraw` before Stats, and by moving the
  gapped pair to triangles 44/45 (vertices 132/135) with a
  half-step grid offset so triangle 44 is centred on the
  origin.
- **GPU texcache device removal (build 7):** the second
  `ScOpt_TexCacheStore` failed with `DXGI_ERROR_DEVICE_REMOVED`
  (0x887A0005), and the next cache Init failed the same way.
  Root cause: the compute shader's `RWByteAddressBuffer` UAV
  was bound to the TEXTURE — a raw-buffer UAV must be a buffer.
  Fixed by dispatching into a linear staging buffer (256-byte
  aligned rows) and copying it into the texture with
  `CopyTextureRegion`; the shader now takes a `rowPitch`
  constant (7 constants, was 6).

### Known issues

- **CPU de-tile cache under heavy churn (stress test only):**
  with 128 distinct textures churned through the 64-slot
  table, the least-recently-used entry's lookup can fail
  (the LRU key misses while the other 63 resident keys hit).
  Reproduced locally (`/tmp/detiletest/`); the guest-stage
  scenario (1 texture, 2 calls) and the GPU texcache (a
  separate implementation) are unaffected. Documented, not
  blocking; revisit after the tree is green.
- **Oversized single texture (CPU cache):** a texture larger
  than the 64 MB CPU budget is computed but never stored (the
  second call re-computes). The GPU cache stores an oversized
  texture alone instead — the two layers should be aligned
  (store-alone) in a later pass.
