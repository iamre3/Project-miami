// opt_detile.cpp — de-tiled texture cache (docs/OPTIMIZATION.md item 2).
// Morton (Z-order) texel order inside each 64KB tile, tiles in row-major
// order across the texture. Tile/untile are exact inverses (proven by
// round-trip); the cache keys on the full tiled input bytes.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "opt_detile.h"
#include "sc_opt_common.h"
#include "scarlett.h"

using namespace sc;

// Spread the low 16 bits of v so bit i lands at bit 2*i (Morton part).
static uint32_t MortonPart(uint32_t v) {
    v &= 0x0000FFFFu;
    v = (v | (v << 8)) & 0x00FF00FFu;
    v = (v | (v << 4)) & 0x0F0F0F0Fu;
    v = (v | (v << 2)) & 0x33333333u;
    v = (v | (v << 1)) & 0x55555555u;
    return v;
}

// Morton index of (x, y) within a tile (x -> even bits, y -> odd bits).
static uint32_t Morton2D(uint32_t x, uint32_t y) {
    return MortonPart(x) | (MortonPart(y) << 1);
}

static bool IsPow2(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

static HRESULT Validate(uint32_t w, uint32_t h, uint32_t bpp, uint32_t tileW,
                        uint32_t tileH) {
    if (w == 0 || h == 0) return E_INVALIDARG;
    if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 && bpp != 16)
        return E_INVALIDARG;
    if (!IsPow2(tileW) || !IsPow2(tileH)) return E_INVALIDARG;
    if ((uint64_t)tileW * tileH * bpp != 65536ULL) return E_INVALIDARG;
    if (tileW > 65536 || tileH > 65536) return E_INVALIDARG;
    return S_OK;
}

// Byte offset of texel (x, y) in the tiled buffer.
static size_t TiledOffset(uint32_t x, uint32_t y, uint32_t w, uint32_t bpp,
                          uint32_t tileW, uint32_t tileH) {
    uint32_t tilesPerRow = (w + tileW - 1) / tileW;
    uint32_t tileX = x / tileW, tileY = y / tileH;
    uint32_t tx = x % tileW, ty = y % tileH;
    uint64_t tileIndex = (uint64_t)tileY * tilesPerRow + tileX;
    uint64_t within = Morton2D(tx, ty);
    return (size_t)((tileIndex * (uint64_t)tileW * tileH + within) * bpp);
}

extern "C" HRESULT ScOpt_Tile2D(const uint8_t* linear, uint32_t w, uint32_t h,
                                uint32_t bpp, uint32_t tileW, uint32_t tileH,
                                uint8_t* tiled) {
    if (!linear || !tiled) return E_INVALIDARG;
    HRESULT hr = Validate(w, h, bpp, tileW, tileH);
    if (FAILED(hr)) return hr;
    size_t total = (size_t)w * h * bpp;
    memset(tiled, 0, total);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            size_t src = ((size_t)y * w + x) * bpp;
            size_t dst = TiledOffset(x, y, w, bpp, tileW, tileH);
            memcpy(tiled + dst, linear + src, bpp);
        }
    }
    return S_OK;
}

extern "C" HRESULT ScOpt_Detile2D(const uint8_t* tiled, uint32_t w, uint32_t h,
                                  uint32_t bpp, uint32_t tileW, uint32_t tileH,
                                  uint8_t* linear) {
    if (!tiled || !linear) return E_INVALIDARG;
    HRESULT hr = Validate(w, h, bpp, tileW, tileH);
    if (FAILED(hr)) return hr;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            size_t src = TiledOffset(x, y, w, bpp, tileW, tileH);
            size_t dst = ((size_t)y * w + x) * bpp;
            memcpy(linear + dst, tiled + src, bpp);
        }
    }
    return S_OK;
}

// ---- cache ----
static const size_t kDetileCap = 64;
static const uint64_t kDetileBudget = 64ULL * 1024 * 1024;  // 64 MB total

struct DetileEntry {
    bool used = false;
    bool tombstone = false;  // evicted entry: probe continues past it
    uint64_t hash = 0;
    uint8_t* tiled = nullptr;   // stored input copy (full compare)
    uint8_t* linear = nullptr;  // stored output copy
    size_t bytes = 0;           // w*h*bpp
    uint32_t w = 0, h = 0, bpp = 0, tileW = 0, tileH = 0;
    uint64_t lastUsed = 0;      // LRU tick (0 = never used)
};

static DetileEntry g_detileTable[kDetileCap];
static uint64_t g_detileBytesUsed = 0;
static CRITICAL_SECTION g_detileLock;
static bool g_detileInit = false;
static uint32_t g_detiles = 0, g_detileHits = 0, g_detileEvictions = 0;
static uint64_t g_detileTick = 0;

static void DetileEnsureInit() {
    if (!g_detileInit) {
        InitializeCriticalSection(&g_detileLock);
        memset(g_detileTable, 0, sizeof(g_detileTable));
        g_detileInit = true;
    }
}

extern "C" HRESULT ScOpt_DetileCached(const uint8_t* tiled, uint32_t w,
                                      uint32_t h, uint32_t bpp, uint32_t tileW,
                                      uint32_t tileH, uint8_t* linearOut,
                                      BOOL* hit) {
    if (!tiled || !linearOut || !hit) return E_INVALIDARG;
    HRESULT hr = Validate(w, h, bpp, tileW, tileH);
    if (FAILED(hr)) return hr;
    *hit = FALSE;
    size_t bytes = (size_t)w * h * bpp;
    uint64_t hash = ScOptFnv1a(tiled, bytes);
    hash = ScOptHashU32(hash, w);
    hash = ScOptHashU32(hash, h);
    hash = ScOptHashU32(hash, bpp);
    hash = ScOptHashU32(hash, tileW);
    hash = ScOptHashU32(hash, tileH);

    DetileEnsureInit();
    EnterCriticalSection(&g_detileLock);
    size_t slotIdx = (size_t)(hash & (kDetileCap - 1));
    // Open-addressing probe. The insertion slot is the FIRST tombstone
    // or never-used slot at-or-after the hash slot. The probe must
    // continue past tombstones — an evicted entry must not break the
    // probe chain for entries stored after it — and it stops at a
    // never-used slot (nothing past it can match, since insertion
    // always places at the first non-live slot at-or-after the hash).
    DetileEntry* slot = nullptr;
    for (size_t probe = 0; probe < kDetileCap; probe++) {
        DetileEntry* e = &g_detileTable[(slotIdx + probe) & (kDetileCap - 1)];
        if (!e->used && !e->tombstone) {
            if (!slot) slot = e;
            break;
        }
        if (e->tombstone) {
            if (!slot) slot = e;
            continue;
        }
        if (e->hash == hash && e->bytes == bytes && e->w == w && e->h == h &&
            e->bpp == bpp && e->tileW == tileW && e->tileH == tileH &&
            memcmp(e->tiled, tiled, bytes) == 0) {
            memcpy(linearOut, e->linear, bytes);
            *hit = TRUE;
            g_detileHits++;
            e->lastUsed = ++g_detileTick;  // LRU refresh
            LeaveCriticalSection(&g_detileLock);
            return S_OK;
        }
    }
    // Miss: compute into the caller's buffer, then try to store.
    LeaveCriticalSection(&g_detileLock);
    hr = ScOpt_Detile2D(tiled, w, h, bpp, tileW, tileH, linearOut);
    if (FAILED(hr)) return hr;
    EnterCriticalSection(&g_detileLock);
    g_detiles++;
    // LRU eviction: make room (a free slot AND budget headroom) by
    // evicting the least recently used entries first. Evicted entries
    // become TOMBSTONES (buffers freed, slot kept marked) so probe
    // chains stay intact. A texture whose stored pair alone exceeds
    // the budget is computed but never stored — the loop evicts
    // everything and still cannot fit it.
    const char* storeNote = "cached";
    while (true) {
        if (slot && g_detileBytesUsed + 2 * bytes <= kDetileBudget) break;
        DetileEntry* lru = nullptr;
        for (size_t i = 0; i < kDetileCap; i++) {
            if (!g_detileTable[i].used) continue;
            if (!lru || g_detileTable[i].lastUsed < lru->lastUsed)
                lru = &g_detileTable[i];
        }
        if (!lru) { storeNote = "not cached (over budget)"; break; }
        free(lru->tiled);
        free(lru->linear);
        g_detileBytesUsed -= 2 * lru->bytes;
        *lru = DetileEntry{};
        lru->tombstone = true;
        g_detileEvictions++;
        // Re-probe for the insertion slot: the eviction freed the
        // first non-live slot at-or-after the hash slot (the evicted
        // tombstone, or an earlier one).
        slot = nullptr;
        for (size_t probe = 0; probe < kDetileCap; probe++) {
            DetileEntry* e =
                &g_detileTable[(slotIdx + probe) & (kDetileCap - 1)];
            if (!e->used) { slot = e; break; }
        }
    }
    if (slot && g_detileBytesUsed + 2 * bytes <= kDetileBudget) {
        uint8_t* tiledCopy = (uint8_t*)malloc(bytes);
        uint8_t* linearCopy = (uint8_t*)malloc(bytes);
        if (tiledCopy && linearCopy) {
            memcpy(tiledCopy, tiled, bytes);
            memcpy(linearCopy, linearOut, bytes);
            slot->used = true;
            slot->tombstone = false;
            slot->hash = hash;
            slot->tiled = tiledCopy;
            slot->linear = linearCopy;
            slot->bytes = bytes;
            slot->w = w;
            slot->h = h;
            slot->bpp = bpp;
            slot->tileW = tileW;
            slot->tileH = tileH;
            slot->lastUsed = ++g_detileTick;
            g_detileBytesUsed += 2 * bytes;
        } else {
            free(tiledCopy);
            free(linearCopy);
            storeNote = "not cached (malloc failed)";
        }
    }
    LeaveCriticalSection(&g_detileLock);
    Log("opt_detile", "detile %ux%u bpp=%u (miss, %s)", w, h, bpp, storeNote);
    return S_OK;
}

extern "C" HRESULT ScOpt_DetileStats(uint32_t* detiles, uint32_t* hits,
                                     uint32_t* evictions) {
    if (!detiles || !hits) return E_INVALIDARG;
    DetileEnsureInit();
    EnterCriticalSection(&g_detileLock);
    *detiles = g_detiles;
    *hits = g_detileHits;
    if (evictions) *evictions = g_detileEvictions;
    LeaveCriticalSection(&g_detileLock);
    return S_OK;
}

extern "C" void ScOpt_DetileDestroy() {
    if (!g_detileInit) return;
    EnterCriticalSection(&g_detileLock);
    for (size_t i = 0; i < kDetileCap; i++) {
        if (g_detileTable[i].used) {
            free(g_detileTable[i].tiled);
            free(g_detileTable[i].linear);
            g_detileTable[i] = DetileEntry{};
        }
    }
    g_detileBytesUsed = 0;
    LeaveCriticalSection(&g_detileLock);
    DeleteCriticalSection(&g_detileLock);
    g_detileInit = false;
}
