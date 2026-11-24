// opt_texcache.h — GPU de-tiled texture cache with memory management
// (docs/OPTIMIZATION.md item 2, memory management). The CPU de-tile
// cache (opt_detile.h) holds byte buffers; THIS cache holds the GPU
// resources the game actually binds, with a VRAM budget and LRU
// eviction, so the title's gigabytes of textures cannot grow it without
// bound. These are ScarlettCompat-defined entry points, NOT Xbox APIs.
//
// Bind-time contract (the lookup path, on every texture bind/use):
//   1. ScOpt_TexCacheLookup(cache, key, &res, &hit) — hit: bind `res`
//      (the caller owns the returned reference; Release it after
//      binding). Miss: go to step 2.
//   2. ScOpt_TexCacheStore(cache, key, tiled, w, h, bpp, tileW, tileH,
//      queue, &res) — de-tiles the texture (GPU compute for bpp 4/8/16,
//      CPU fallback for bpp 1/2) and uploads it into a DEFAULT-heap
//      texture in PIXEL_SHADER_RESOURCE state, ready to bind.
//      Evicts the coldest (least recently used) entries first when
//      the budget would be exceeded. The returned resource is ready
//      to bind — the store waits for the GPU work to finish.
//   3. When the game unloads a texture, ScOpt_TexCacheEvict(cache,
//      key) drops it from the cache, so the cache works WITH the
//      game's streaming instead of holding textures the game freed.
//      (The game, not the cache, owns the streaming decision; the
//      cache never resurrects an evicted texture on its own.)
//
// Budget: the cache is created with a byte cap (0 = 512 MB default).
// bytesUsed is the sum of w*h*bpp over live entries and never exceeds
// the budget EXCEPT for a single texture larger than the budget
// itself, which is stored alone (evicting everything else) with a
// warning — it is needed right now and cannot be split. Eviction is
// strict LRU: the least recently used entry goes first, and a lookup
// hit refreshes an entry's recency.
//
// The de-tile itself is the same Morton (Z-order) swizzle as
// opt_detile.cpp (see the honesty note there: the exact XG tables are
// GDKX-gated, the shape is documented and self-consistent). The GPU
// compute de-tiler is proven byte-identical to the CPU de-tiler by
// readback in the guest (tests/guest_opt.cpp, opt_texcache stage).
// bpp 1/2 take the CPU de-tile + upload path instead: a compute
// shader copies whole 4-byte words, so sub-word formats cannot be
// de-tiled on the GPU without clobbering neighbours.
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* ScOptTexCacheHandle;

// Default budget when budgetBytes is 0 (512 MB — the 5700 XT has
// 8 GB; the cache must leave room for the frame, the PSO cache and
// the game's own resident set).
#define SCOPT_TEXCACHE_DEFAULT_BUDGET (512ULL * 1024 * 1024)

// Create a texture cache for `device` with a VRAM budget of
// `budgetBytes` (0 = default). Compiles the GPU de-tile compute
// shader once. Returns S_OK or a failure HRESULT.
HRESULT ScOpt_TexCacheInit(ID3D12Device* device, uint64_t budgetBytes,
                           ScOptTexCacheHandle* cache);

// Bind-time lookup. On a hit, *resource is the de-tiled texture
// (AddRef'd; the caller releases it) and *hit is TRUE; the entry
// becomes most-recently-used. On a miss, *resource is nullptr and
// *hit is FALSE — call ScOpt_TexCacheStore.
HRESULT ScOpt_TexCacheLookup(ScOptTexCacheHandle cache, uint64_t key,
                             ID3D12Resource** resource, BOOL* hit);

// De-tile + upload. `tiled` is w*h*bpp bytes of Xbox-tiled data.
// Returns a DEFAULT-heap texture in PIXEL_SHADER_RESOURCE state,
// ready to bind (the call waits for the GPU work). If `key` is
// already cached, returns the existing resource instead of doing
// the work again. Evicts LRU entries as needed to stay in budget.
HRESULT ScOpt_TexCacheStore(ScOptTexCacheHandle cache, uint64_t key,
                            const uint8_t* tiled, uint32_t w, uint32_t h,
                            uint32_t bpp, uint32_t tileW, uint32_t tileH,
                            ID3D12CommandQueue* queue,
                            ID3D12Resource** resource);

// Game-unload path: drop one texture from the cache. S_OK when the
// entry existed and was dropped, S_FALSE when the key was not cached.
HRESULT ScOpt_TexCacheEvict(ScOptTexCacheHandle cache, uint64_t key);

// Counts: entries live, bytesUsed (sum of w*h*bpp, <= budget except
// for a single over-budget texture), the budget, hits, misses, and
// evictions (LRU evictions + explicit Evict calls are NOT counted
// here — only budget-driven evictions).
HRESULT ScOpt_TexCacheStats(ScOptTexCacheHandle cache, uint32_t* entries,
                            uint64_t* bytesUsed, uint64_t* bytesBudget,
                            uint32_t* hits, uint32_t* misses,
                            uint32_t* evictions);

// Release the cache (releases every cached texture). Safe to call
// when empty.
void ScOpt_TexCacheDestroy(ScOptTexCacheHandle cache);

#ifdef __cplusplus
}
#endif
