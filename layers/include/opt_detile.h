// opt_detile.h — de-tiled texture cache (docs/OPTIMIZATION.md item 2).
// Xbox textures are stored tiled (XBOX_TILEMODE_SCARLETT, 64KB tiles);
// the desktop GPU needs a linear layout. This module tiles / de-tiles
// 2D textures on the CPU and caches de-tiled results so the same texture
// is only de-tiled once. These are ScarlettCompat-defined entry points,
// NOT Xbox APIs.
//
// HONESTY NOTE: the exact XG swizzle tables live in the GDK XG library
// (GDKX-gated). The swizzle implemented here is the documented shape —
// Morton (Z-order) texel order inside a 64KB tile — and the pair is
// proven self-consistent by exact round-trips. Whether its bit layout
// matches the hardware's per-mode tables is UNVERIFIED until real Xbox
// tiled data lands. The cache (the actual optimization) is proven
// regardless: when the table is corrected, the cache keeps working.
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Linear -> tiled. linear and tiled are w*h*bpp bytes each. tileW/tileH
// are texels per tile side (tileW*tileH*bpp must be 65536 for a 64KB
// tile). Partial edge tiles are handled: texels outside the texture
// bounds are left as zero in the tiled buffer. Returns S_OK, or
// E_INVALIDARG for bad dimensions (non-power-of-two tile sides,
// tileW*tileH*bpp != 65536, bpp not 1/2/4/8/16).
HRESULT ScOpt_Tile2D(const uint8_t* linear, uint32_t w, uint32_t h,
                     uint32_t bpp, uint32_t tileW, uint32_t tileH,
                     uint8_t* tiled);

// Tiled -> linear. Inverse of Tile2D.
HRESULT ScOpt_Detile2D(const uint8_t* tiled, uint32_t w, uint32_t h,
                       uint32_t bpp, uint32_t tileW, uint32_t tileH,
                       uint8_t* linear);

// De-tile with caching. On a cache hit the stored linear buffer is
// copied into linearOut and *hit is TRUE (no recompute, and the entry
// becomes most-recently-used). On a miss the texture is de-tiled, the
// result is stored (subject to the 64MB total cache budget — when the
// budget or the 64-slot table is full, the LEAST RECENTLY USED entries
// are evicted first; a texture whose stored pair alone exceeds the
// budget is computed but not stored), and *hit is FALSE. The cache key
// is the full tiled input (hash + stored copy comparison), so a hit can
// only return data de-tiled from identical input.
HRESULT ScOpt_DetileCached(const uint8_t* tiled, uint32_t w, uint32_t h,
                           uint32_t bpp, uint32_t tileW, uint32_t tileH,
                           uint8_t* linearOut, BOOL* hit);

// Counts: detiles = DetileCached calls that computed (misses);
// hits = DetileCached calls served from the cache;
// evictions = LRU evictions performed to make room (may be NULL).
HRESULT ScOpt_DetileStats(uint32_t* detiles, uint32_t* hits,
                          uint32_t* evictions);

// Release the cache (frees stored buffers). Safe to call when empty.
void ScOpt_DetileDestroy();

#ifdef __cplusplus
}
#endif
