// xg_texture.h — xg_xs.dll texture-layout exports (build 80).
//
// The two xg_xs exports the title exe imports and calls during boot:
//   XGCreateTextureComputer  -> makes a texture address computer object
//   XGSuggestSwizzleMode     -> picks a tile (swizzle) mode for a descriptor
//
// Shapes below are MEASURED from the exe, not guessed from a header we do
// not have:
//
// XGCreateTextureComputer (thunk 0x145F3FF40, IAT 0x146793F60)
//   rcx = descriptor (56-byte D3D12_RESOURCE_DESC), rdx = out object.
//   Fields as listed in the shared-engine comment below; the game's
//   translator at 0x144ECC940 fills the same struct.
//
// XGSuggestSwizzleMode (thunk 0x145F3FF50, IAT 0x146793F68)
//   rcx = descriptor, rdx = flags (0 at its call site 0x140C4B3CC),
//   r8  = out layout dword (the other branch there stores 0x100|mode to [r8]).
//
// Computer vtable use measured at the boot-#60 crash: object at [rsp+0x38],
// `call [r10+0x40]` = slot 8 with
//   rcx = object
//   rdx = destination pointer            (allocated with size/align taken
//                                         from the game's layout helper
//                                         0x140343E80 -> tiled size)
//   r8d = 0                              (destination row pitch; a tiled
//                                         destination has none)
//   r9d = subresource index              (r15d = plane loop accumulator)
//   [rsp+0x20] = source pointer          (arg2 + byte offset)
//   [rsp+0x28] = source row pitch        (computed at 0x1416F7250 from
//                                         bits-per-pixel*width/8)
//   [rsp+0x30] = source slice pitch      (0x1416F7280)
//   The object is released with `call [rax+8]` at 0x1416F7233, so slot 1 is
//   Release.
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

HRESULT WINAPI XGCreateTextureComputer(const void* pDesc, void** ppComputer);
HRESULT WINAPI XGSuggestSwizzleMode(const void* pDesc, uint32_t flags,
                                    uint32_t* pOutLayout);

// ---- shared texture-layout engine (build 81) ------------------------------
// ONE calculator for both the texture computer (slot 8's copy) and the
// device's slot 25 GetResourceAllocationInfo. A tiled allocation is sized by
// exactly the arithmetic the copy will use, so the two cannot drift apart.
// The descriptor is the 56-byte D3D12_RESOURCE_DESC the game passes:
//   0x00 dimension, 0x08 alignment, 0x10 width, 0x18 height,
//   0x1C depth/array, 0x1E mips, 0x20 format, 0x24 sampleCount,
//   0x28 sampleQuality, 0x2C layout, 0x30 flags.
// (Measured 2026-11-24: the game writes 0x105 to +0x2C at 0x1403440F7, and
// the boot-#61 slot-25 log reads layout 257 / flags 0x300000 at those
// offsets. Build 80's +0x30/+0x24 field map was wrong and is corrected here.)
// layout is 0 (linear) or 0x100|mode for a console tiled layout.

#define SC_XG_MAX_SUBRESOURCES 256

// Rejection reasons, also used verbatim in the slot-25 log record.
enum ScXgStatus {
    SC_XG_OK = 0,
    SC_XG_BAD_DESC = 1,           // null/unreadable descriptor
    SC_XG_BAD_DIMS = 2,           // zero or out-of-range width/height/mips/depth
    SC_XG_UNSUPPORTED_LAYOUT = 3, // layout that is not 0 or 0x100|mode
    SC_XG_UNSUPPORTED_FORMAT = 4, // no tile geometry for this format
    SC_XG_OVERFLOW = 5,           // size arithmetic overflowed
    SC_XG_TOO_MANY_SUBRESOURCES = 6,
};

typedef struct ScXgLayoutInfo {
    uint32_t status;      // ScXgStatus
    uint32_t bpe;         // bytes per texel, derived from format
    uint32_t tileW, tileH;
    uint32_t mipLevels, slices, subCount;
    uint32_t tileCount;   // 64 KiB tiles across every subresource
    uint64_t packedBytes; // sum of packed linear extents (all subresources)
    uint64_t totalBytes;  // contiguous tiled size of the whole resource
    uint64_t baseAlign;
    uint64_t subOffset[SC_XG_MAX_SUBRESOURCES];
    uint64_t subBytes[SC_XG_MAX_SUBRESOURCES];
} ScXgLayoutInfo;

// Fills *out completely. Returns S_OK when status is SC_XG_OK, else E_INVALIDARG
// (or E_OUTOFMEMORY for a null out). Never partially fills: on failure the
// status says why and no size is claimed.
HRESULT WINAPI ScXg_ComputeLayout(const void* pDesc, ScXgLayoutInfo* out);

// Bytes per texel for a DXGI format, 0 when this engine has no geometry for
// it. Exposed so slot 25 can report it in its log record.
uint32_t WINAPI ScXg_BytesPerTexel(uint32_t dxgiFormat);

#ifdef __cplusplus
}
#endif
