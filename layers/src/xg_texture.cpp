// xg_texture.cpp — xg_xs texture computer + the shared texture-layout engine.
//
// WHY (boot #60): the exe called XGCreateTextureComputer, this layer answered
// S_OK (STUB) and handed back a null object; the exe ignores the HRESULT,
// takes the object from [rsp+0x38] and calls slot 8 through [r10+0x40] with
// r10 = [obj] = 0 -> 0xC0000005 at 0x1416F71FB. Build 80 made the object real.
//
// BUILD 81: the layout arithmetic was factored out of the object into
// ScXg_ComputeLayout so the device's slot 25 GetResourceAllocationInfo sizes a
// console-layout resource with the same calculator the copy uses. A tiled
// allocation can therefore never be sized by arithmetic different from the one
// that writes it.
//
// WHAT IS MODELLED, HONESTLY:
//   * Descriptor validation, bytes-per-texel from the DXGI format, per-mip /
//     per-slice subresource offsets, 64 KiB tile granularity and the packed
//     linear extent are all computed here. Overflow is checked, and every
//     unsupported shape is rejected with a status instead of a number.
//   * The texel order inside a 64 KiB tile is the project's existing Morton
//     (Z-order) model from opt_detile.cpp (ScOpt_Tile2D) — the same model the
//     rest of the layer uses. It is NOT verified against the GDK's per-mode
//     tables (GDKX-gated).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "xg_texture.h"
#include "opt_detile.h"
#include "scarlett.h"

using namespace sc;

namespace {

// 56-byte D3D12_RESOURCE_DESC as the game passes it (see header).
#pragma pack(push, 1)
struct DescView {
    uint32_t dimension;      // +0x00
    uint32_t pad04;          // +0x04
    uint64_t alignment;      // +0x08
    uint64_t width;          // +0x10
    uint32_t height;         // +0x18
    uint16_t depthOrArray;   // +0x1C
    uint16_t mips;           // +0x1E
    uint32_t format;         // +0x20
    uint32_t sampleCount;    // +0x24
    uint32_t sampleQuality;  // +0x28
    uint32_t layout;         // +0x2C
    uint32_t flags;          // +0x30
    uint32_t pad34;          // +0x34
};
#pragma pack(pop)
static_assert(sizeof(DescView) == 56, "D3D12_RESOURCE_DESC stride is 56 bytes");

const uint32_t kMaxMips = 16;
const uint32_t kMaxSlices = 16;
const uint32_t kSlotCopy = 8;
const uint32_t kVtableSlots = 16;
const uint64_t kTileBytes = 65536;
// The single tile mode this layer implements, written as 0x100|mode.
const uint32_t kOwnTileMode = 1;
const uint32_t kLayoutConsoleBit = 0x100;

struct Layout {
    uint32_t valid;
    ScXgLayoutInfo info;
};

struct XgComputer {
    const void* vtbl;      // +0x00
    volatile LONG refs;    // +0x08
    uint32_t magic;        // +0x0C
    DescView desc;         // copy of the 56-byte descriptor
    ScXgLayoutInfo layout;
};

const uint32_t kMagic = 0x58475443;  // 'XGTC'

// ---- memory probing -------------------------------------------------------

// Continuous committed span starting at p, up to n bytes. Returns how many
// bytes from p are committed and (for writes) writable. Used to refuse a
// copy that would run past the caller's allocation instead of faulting.
static uint64_t ProbedSpan(const void* p, uint64_t n, bool needWrite) {
    if (!p || !n) return 0;
    uintptr_t a = (uintptr_t)p;
    uintptr_t end = a + (uintptr_t)n;
    if (end < a) return 0;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof mbi)) return a - (uintptr_t)p;
        if (mbi.State != MEM_COMMIT) return a - (uintptr_t)p;
        DWORD prot = mbi.Protect;
        if (prot & (PAGE_NOACCESS | PAGE_GUARD)) return a - (uintptr_t)p;
        if (needWrite &&
            !(prot & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                      PAGE_EXECUTE_WRITECOPY)))
            return a - (uintptr_t)p;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return (uint64_t)n;
}

static bool Readable(const void* p, uint64_t n) { return ProbedSpan(p, n, false) == n; }

// ---- layout --------------------------------------------------------------

static bool TileGeometry(uint32_t bpe, uint32_t* tileW, uint32_t* tileH) {
    switch (bpe) {
    case 1: *tileW = 256; *tileH = 256; return true;
    case 2: *tileW = 256; *tileH = 128; return true;
    case 4: *tileW = 128; *tileH = 128; return true;
    case 8: *tileW = 128; *tileH = 64; return true;
    case 16: *tileW = 64; *tileH = 64; return true;
    default: return false;
    }
}

static uint32_t CeilDiv(uint32_t a, uint32_t b) { return b ? (a + b - 1) / b : 0; }

static bool ValidLayout(uint32_t layout) {
    if (layout == 0) return true;                    // linear
    if ((layout & kLayoutConsoleBit) == 0) return false;
    // Only the mode this layer implements is modelled; any other mode is a
    // clean rejection rather than an invented size.
    return (layout & ~kLayoutConsoleBit) == kOwnTileMode;
}

// Build the layout for a descriptor. Fills *out completely in every case.
static void BuildLayout(const DescView& d, ScXgLayoutInfo* out) {
    memset(out, 0, sizeof *out);
    out->status = SC_XG_OK;

    if (!ValidLayout(d.layout)) {
        out->status = SC_XG_UNSUPPORTED_LAYOUT;
        return;
    }
    if (d.dimension != 3) {  // 2 = buffer, 4 = texture3D (not modelled here)
        out->status = SC_XG_BAD_DIMS;
        return;
    }
    if (d.width == 0 || d.width > (1ull << 31) || d.height == 0 ||
        d.mips == 0 || d.mips > kMaxMips || d.depthOrArray == 0 ||
        d.depthOrArray > kMaxSlices) {
        out->status = SC_XG_BAD_DIMS;
        return;
    }

    uint32_t bpe = ScXg_BytesPerTexel(d.format);
    uint32_t tileW = 0, tileH = 0;
    if (bpe == 0 || !TileGeometry(bpe, &tileW, &tileH)) {
        out->status = SC_XG_UNSUPPORTED_FORMAT;
        return;
    }

    out->bpe = bpe;
    out->tileW = tileW;
    out->tileH = tileH;
    out->mipLevels = d.mips;
    out->slices = d.depthOrArray;
    out->baseAlign = kTileBytes;

    uint64_t offset = 0, packed = 0, tiles = 0;
    uint32_t count = 0;
    for (uint32_t m = 0; m < d.mips; ++m) {
        uint32_t w = (uint32_t)(d.width >> m);
        uint32_t h = d.height >> m;
        if (w == 0) w = 1;
        if (h == 0) h = 1;
        uint64_t rowPitch = (uint64_t)w * bpe;
        uint64_t slicePitch = rowPitch * h;
        uint64_t slabTiles = (uint64_t)CeilDiv(w, tileW) * CeilDiv(h, tileH);
        uint64_t slab = slabTiles * kTileBytes;
        if (slab == 0 || slabTiles > (1ull << 32)) {
            out->status = SC_XG_OVERFLOW;
            return;
        }
        for (uint32_t s = 0; s < d.depthOrArray; ++s) {
            if (count >= SC_XG_MAX_SUBRESOURCES) {
                out->status = SC_XG_TOO_MANY_SUBRESOURCES;
                return;
            }
            if (offset > UINT64_MAX - slab || packed > UINT64_MAX - slicePitch ||
                tiles > UINT64_MAX - slabTiles) {
                out->status = SC_XG_OVERFLOW;
                return;
            }
            out->subOffset[count] = offset;
            out->subBytes[count] = slab;
            ++count;
            offset += slab;
            packed += slicePitch;
            tiles += slabTiles;
        }
    }
    out->subCount = count;
    out->tileCount = (uint32_t)tiles;
    out->totalBytes = offset;
    out->packedBytes = packed;
    out->status = SC_XG_OK;
}

// ---- the object ----------------------------------------------------------

static void VtblInfo();      // slot 0
static HRESULT VtblRelease(void* self);
static HRESULT VtblAddRef(void* self);
static HRESULT VtblSlotStub(void* self, uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6);
static HRESULT VtblCopy(void* self, void* dst, uint32_t dstRowPitch,
                        uint32_t subresource, const void* src,
                        uint32_t srcRowPitch, uint32_t srcSlicePitch);

// Slot 1 is Release (measured: the exe issues `call [rax+8]` to destroy the
// computer at 0x1416F7233). Slot 0 and slots 2..15 are real functions too —
// never uninitialised memory — and they log the slot and its raw arguments
// before failing.
static void* g_vtbl[kVtableSlots] = {
    (void*)VtblInfo,      // 0
    (void*)VtblRelease,   // 1
    (void*)VtblAddRef,    // 2
    (void*)VtblSlotStub,  // 3
    (void*)VtblSlotStub,  // 4
    (void*)VtblSlotStub,  // 5
    (void*)VtblSlotStub,  // 6
    (void*)VtblSlotStub,  // 7
    (void*)VtblCopy,      // 8  measured copy slot
    (void*)VtblSlotStub,  // 9
    (void*)VtblSlotStub,  // 10 measured 5-arg call at 0x140344283
    (void*)VtblSlotStub,  // 11
    (void*)VtblSlotStub,  // 12
    (void*)VtblSlotStub,  // 13
    (void*)VtblSlotStub,  // 14
    (void*)VtblSlotStub,  // 15
};

static XgComputer* AsComputer(void* self) {
    XgComputer* c = (XgComputer*)self;
    if (!c || c->magic != kMagic) return nullptr;
    return c;
}

static void VtblInfo() { Log("xgxs", "computer slot 0 (query) -> E_NOTIMPL"); }

static HRESULT VtblAddRef(void* self) {
    XgComputer* c = AsComputer(self);
    if (!c) return E_INVALIDARG;
    LONG n = InterlockedIncrement(&c->refs);
    Log("xgxs", "computer AddRef -> %ld", (long)n);
    return S_OK;
}

static HRESULT VtblRelease(void* self) {
    XgComputer* c = AsComputer(self);
    if (!c) {
        Log("xgxs", "computer Release on unknown object %p", self);
        return E_INVALIDARG;
    }
    LONG n = InterlockedDecrement(&c->refs);
    if (n <= 0) {
        Log("xgxs", "computer Release -> 0, freeing (%llux%u fmt %u mips %u slices %u)",
            (unsigned long long)c->desc.width, c->desc.height, c->desc.format,
            c->desc.mips, c->desc.depthOrArray);
        c->magic = 0;
        HeapFree(GetProcessHeap(), 0, c);
    }
    return S_OK;
}

static HRESULT VtblSlotStub(void* self, uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    Log("xgxs",
        "computer slot reached: unknown vtable slot, args %llx %llx %llx %llx %llx %llx -> E_NOTIMPL",
        (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3,
        (unsigned long long)a4, (unsigned long long)a5, (unsigned long long)a6);
    (void)self;
    return E_NOTIMPL;
}

// Slot 8, measured argument shape (see header):
//   copy from the linear source described by (src, srcRowPitch,
//   srcSlicePitch) into the tiled subresource `subresource` at `dst`.
static HRESULT VtblCopy(void* self, void* dst, uint32_t dstRowPitch,
                        uint32_t subresource, const void* src,
                        uint32_t srcRowPitch, uint32_t srcSlicePitch) {
    XgComputer* c = AsComputer(self);
    if (!c) {
        Log("xgxs", "copy: bad computer %p", self);
        return E_INVALIDARG;
    }
    const ScXgLayoutInfo& lay = c->layout;
    Log("xgxs",
        "copy: dst %p dstRowPitch %u sub %u src %p srcRowPitch %u srcSlicePitch %u (desc %llux%u fmt %u bpe %u mips %u slices %u, layout %llu bytes)",
        dst, dstRowPitch, subresource, src, srcRowPitch, srcSlicePitch,
        (unsigned long long)c->desc.width, c->desc.height, c->desc.format,
        lay.bpe, c->desc.mips, c->desc.depthOrArray,
        (unsigned long long)lay.totalBytes);

    if (lay.status != SC_XG_OK) {
        Log("xgxs", "copy: descriptor layout was never valid (status %u) -> E_INVALIDARG",
            lay.status);
        return E_INVALIDARG;
    }
    if (!dst || !src) {
        Log("xgxs", "copy: null pointer (dst %p src %p) -> E_INVALIDARG", dst, src);
        return E_INVALIDARG;
    }
    // A tiled destination has no row pitch. The measured call passes 0; a
    // non-zero value means the argument order is not the one modelled here,
    // so refuse instead of writing through a misinterpreted pointer.
    if (dstRowPitch != 0) {
        Log("xgxs",
            "copy: destination row pitch %u != 0 for a tiled destination -> E_INVALIDARG (argument order not the modelled one)",
            dstRowPitch);
        return E_INVALIDARG;
    }
    if (subresource >= lay.subCount) {
        Log("xgxs", "copy: subresource %u out of range (%u present) -> E_INVALIDARG",
            subresource, lay.subCount);
        return E_INVALIDARG;
    }

    // Rebuild this subresource's geometry (mip, slice, packed pitches).
    uint32_t mip = subresource / lay.slices;
    uint32_t w = (uint32_t)(c->desc.width >> mip);
    uint32_t h = c->desc.height >> mip;
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    uint32_t rowPitch = w * lay.bpe;
    uint64_t needSlice = (uint64_t)rowPitch * h;
    uint64_t subBytes = lay.subBytes[subresource];

    if (srcRowPitch < rowPitch) {
        Log("xgxs",
            "copy: source row pitch %u < packed row %u (%ux%u bpe %u) -> E_INVALIDARG",
            srcRowPitch, rowPitch, w, h, lay.bpe);
        return E_INVALIDARG;
    }
    uint64_t needSpan = (uint64_t)srcRowPitch * (h ? h - 1 : 0) + rowPitch;
    if (h > 1 && srcSlicePitch < needSpan) {
        Log("xgxs",
            "copy: source slice pitch %u < needed %llu (%u rows of %u) -> E_INVALIDARG",
            srcSlicePitch, (unsigned long long)needSpan, h, srcRowPitch);
        return E_INVALIDARG;
    }
    (void)needSlice;

    // Source must be readable for exactly the rows we are about to read.
    uint64_t srcHave = ProbedSpan(src, needSpan, false);
    if (srcHave != needSpan) {
        Log("xgxs",
            "copy: source span %llu bytes only %llu committed/readable -> E_INVALIDARG",
            (unsigned long long)needSpan, (unsigned long long)srcHave);
        return E_INVALIDARG;
    }
    // Destination: our layout says this subresource occupies subBytes at
    // dst + subOffset. If the caller's allocation does not cover that, the
    // game's layout size differs from ours — refuse and say so.
    uint8_t* dstBase = (uint8_t*)dst + lay.subOffset[subresource];
    uint64_t dstHave = ProbedSpan(dstBase, subBytes, true);
    if (dstHave != subBytes) {
        Log("xgxs",
            "copy: destination subresource needs %llu bytes at offset %llu but only %llu are committed/writable -> E_INVALIDARG (layout model mismatch, nothing written)",
            (unsigned long long)subBytes,
            (unsigned long long)lay.subOffset[subresource],
            (unsigned long long)dstHave);
        return E_INVALIDARG;
    }

    // Gather the source rows into a packed linear image when the caller's
    // row pitch is wider than the packed row, then tile it in one pass.
    const uint8_t* packed = (const uint8_t*)src;
    uint8_t* temp = nullptr;
    if (srcRowPitch != rowPitch) {
        uint64_t size = (uint64_t)rowPitch * h;
        temp = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size);
        if (!temp) {
            Log("xgxs", "copy: out of memory for %llu-byte gather -> E_OUTOFMEMORY",
                (unsigned long long)size);
            return E_OUTOFMEMORY;
        }
        for (uint32_t y = 0; y < h; ++y)
            memcpy(temp + (uint64_t)y * rowPitch,
                   packed + (uint64_t)y * srcRowPitch, rowPitch);
        packed = temp;
    }

    HRESULT hr = ScOpt_Tile2D(packed, w, h, lay.bpe, lay.tileW, lay.tileH,
                              dstBase);
    if (temp) HeapFree(GetProcessHeap(), 0, temp);

    if (FAILED(hr)) {
        Log("xgxs",
            "copy: tiling %ux%u bpe %u tile %ux%u failed (hr 0x%08lx) -> nothing reported as copied",
            w, h, lay.bpe, lay.tileW, lay.tileH, (unsigned long)hr);
        return hr;
    }
    Log("xgxs", "copy: tiled %ux%u bpe %u into sub %u at %p (tile %ux%u) -> S_OK",
        w, h, lay.bpe, subresource, dstBase, lay.tileW, lay.tileH);
    return S_OK;
}

}  // namespace

// ---- shared layout engine ------------------------------------------------

extern "C" uint32_t WINAPI ScXg_BytesPerTexel(uint32_t dxgiFormat) {
    // DXGI_FORMAT ranges (uncompressed only; block-compressed formats 70+ and
    // anything newer have no tile geometry in this engine yet).
    if (dxgiFormat >= 1 && dxgiFormat <= 4) return 16;    // R32G32B32A32*
    if (dxgiFormat >= 5 && dxgiFormat <= 8) return 12;    // R32G32B32*
    if (dxgiFormat >= 9 && dxgiFormat <= 22) return 8;    // R16G16B16A16*, R32G32*, R32G8X24*
    if (dxgiFormat >= 23 && dxgiFormat <= 26) return 4;   // R10G10B10A2*, R11G11B10
    if (dxgiFormat >= 27 && dxgiFormat <= 32) return 4;   // R8G8B8A8*
    if (dxgiFormat >= 33 && dxgiFormat <= 38) return 4;   // R16G16*
    if (dxgiFormat >= 39 && dxgiFormat <= 43) return 4;   // R32*, D32*
    if (dxgiFormat >= 44 && dxgiFormat <= 47) return 4;   // R24G8*, D24S8, R24*
    if (dxgiFormat >= 48 && dxgiFormat <= 52) return 2;   // R8G8*
    if (dxgiFormat >= 53 && dxgiFormat <= 59) return 2;   // R16*, D16
    if (dxgiFormat >= 60 && dxgiFormat <= 65) return 1;   // R8*, A8
    if (dxgiFormat >= 66 && dxgiFormat <= 68) return 4;   // R9G9B9E5, R8G8_B8G8, G8R8_G8B8
    if (dxgiFormat >= 69 && dxgiFormat <= 70) return 2;   // B5G6R5, B5G5R5A1
    return 0;
}

extern "C" HRESULT WINAPI ScXg_ComputeLayout(const void* pDesc,
                                             ScXgLayoutInfo* out) {
    if (!out) return E_OUTOFMEMORY;
    memset(out, 0, sizeof *out);
    if (!pDesc) {
        out->status = SC_XG_BAD_DESC;
        return E_INVALIDARG;
    }
    if (!Readable(pDesc, sizeof(DescView))) {
        out->status = SC_XG_BAD_DESC;
        return E_INVALIDARG;
    }
    DescView d;
    memcpy(&d, pDesc, sizeof d);
    BuildLayout(d, out);
    return out->status == SC_XG_OK ? S_OK : E_INVALIDARG;
}

// ---- exports -------------------------------------------------------------

extern "C" HRESULT WINAPI XGCreateTextureComputer(const void* pDesc,
                                                  void** ppComputer) {
    if (ppComputer) *ppComputer = nullptr;
    if (!pDesc || !ppComputer) {
        Log("xgxs", "XGCreateTextureComputer(pDesc %p, ppComputer %p) -> E_POINTER",
            pDesc, ppComputer);
        return E_POINTER;
    }
    ScXgLayoutInfo lay;
    HRESULT hr = ScXg_ComputeLayout(pDesc, &lay);

    DescView desc;
    if (Readable(pDesc, sizeof desc)) memcpy(&desc, pDesc, sizeof desc);
    else memset(&desc, 0, sizeof desc);

    Log("xgxs",
        "XGCreateTextureComputer(desc %p: dim %u %llux%u depth %u mips %u fmt %u layout 0x%x flags 0x%x bpe %u) -> %s (status %u)",
        pDesc, desc.dimension, (unsigned long long)desc.width, desc.height,
        desc.depthOrArray, desc.mips, desc.format, desc.layout, desc.flags,
        lay.bpe, SUCCEEDED(hr) ? "supported" : "UNSUPPORTED", lay.status);
    if (FAILED(hr)) {
        Log("xgxs",
            "XGCreateTextureComputer -> E_INVALIDARG (no object; out stays null so a caller that ignores this cannot use a half-built one)");
        return E_INVALIDARG;
    }

    XgComputer* c = (XgComputer*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                           sizeof(XgComputer));
    if (!c) {
        Log("xgxs", "XGCreateTextureComputer -> E_OUTOFMEMORY");
        return E_OUTOFMEMORY;
    }
    c->vtbl = g_vtbl;
    c->refs = 1;
    c->magic = kMagic;
    c->desc = desc;
    c->layout = lay;
    *ppComputer = c;
    Log("xgxs",
        "XGCreateTextureComputer -> S_OK object %p (%u subresources, %llu bytes, tile %ux%u bpe %u)",
        c, lay.subCount, (unsigned long long)lay.totalBytes, lay.tileW, lay.tileH,
        lay.bpe);
    return S_OK;
}

// Picks the layout dword for a descriptor. Derived from the same table the
// copy path implements (bytes-per-texel -> 64 KiB tile geometry), so the
// answer always describes what this layer can actually copy; a descriptor
// outside the table fails instead of getting an invented mode.
extern "C" HRESULT WINAPI XGSuggestSwizzleMode(const void* pDesc, uint32_t flags,
                                               uint32_t* pOutLayout) {
    if (!pDesc) {
        Log("xgxs", "XGSuggestSwizzleMode(null desc, flags %u, out %p) -> E_POINTER",
            flags, pOutLayout);
        return E_POINTER;
    }
    ScXgLayoutInfo lay;
    HRESULT hr = ScXg_ComputeLayout(pDesc, &lay);

    DescView desc;
    if (Readable(pDesc, sizeof desc)) memcpy(&desc, pDesc, sizeof desc);
    else memset(&desc, 0, sizeof desc);

    // The descriptor may already name a console layout (0x100|mode) — echo it
    // rather than overwrite the caller's choice.
    if (SUCCEEDED(hr) && ValidLayout(desc.layout) && desc.layout != 0) {
        if (pOutLayout) {
            if (ProbedSpan(pOutLayout, sizeof(uint32_t), true) != sizeof(uint32_t)) {
                Log("xgxs", "XGSuggestSwizzleMode: out %p not writable -> E_INVALIDARG",
                    pOutLayout);
                return E_INVALIDARG;
            }
            *pOutLayout = desc.layout;
        }
        Log("xgxs",
            "XGSuggestSwizzleMode -> S_OK, kept descriptor layout 0x%x (flags %u)",
            desc.layout, flags);
        return S_OK;
    }
    if (FAILED(hr)) {
        Log("xgxs",
            "XGSuggestSwizzleMode(desc %p: dim %u %llux%u depth %u mips %u fmt %u layout 0x%x flags 0x%x, flags %u) -> E_INVALIDARG status %u, no mode written",
            pDesc, desc.dimension, (unsigned long long)desc.width, desc.height,
            desc.depthOrArray, desc.mips, desc.format, desc.layout, desc.flags,
            flags, lay.status);
        return E_INVALIDARG;
    }
    if (pOutLayout) {
        if (ProbedSpan(pOutLayout, sizeof(uint32_t), true) != sizeof(uint32_t)) {
            Log("xgxs", "XGSuggestSwizzleMode: out %p not writable -> E_INVALIDARG",
                pOutLayout);
            return E_INVALIDARG;
        }
        *pOutLayout = kLayoutConsoleBit | kOwnTileMode;
    }
    Log("xgxs",
        "XGSuggestSwizzleMode(desc %llux%u bpe %u, flags %u) -> S_OK layout 0x%x (this layer's 64 KiB model, tile %ux%u)",
        (unsigned long long)desc.width, desc.height, lay.bpe, flags,
        kLayoutConsoleBit | kOwnTileMode, lay.tileW, lay.tileH);
    return S_OK;
}
