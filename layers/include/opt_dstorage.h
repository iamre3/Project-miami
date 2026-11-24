// opt_dstorage.h — DirectStorage + BCPack (docs/OPTIMIZATION.md item 6).
// RAGE streams the world continuously; a slow storage shim stalls the
// world streamer before graphics ever matters. This module is the
// async read queue (overlapped Win32 I/O) plus a pluggable
// decompression stage. These are ScarlettCompat-defined entry points,
// NOT Xbox APIs.
//
// MEASURED FACT (2026-11-23): the desktop DirectStorage runtime is
// NOT on the build machine — no dstorage.dll in System32, no
// dstorage.h/lib in SDK 10.0.26100.0. So the queue path here is
// overlapped Win32 I/O (same async semantics, CPU copy). The desktop
// DirectStorage API integration is future work for a machine with the
// runtime installed. The queue-vs-sync measurement below is real
// either way.
//
// BCPACK (honest gap): BCPack is a custom entropy coder for BCn data
// (endpoints separated from palette indices, rANS algorithm) in the
// Xbox hardware decompression block. The rANS bitstream format is not
// public, so no correct decoder can be written from public
// documentation. BCPACK modes return E_NOTIMPL here — a decoder from
// a guessed bitstream would be worse than refusing one.
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ScOptDsPath {
    SC_OPT_DS_PATH_OVERLAPPED = 0,  // overlapped Win32 I/O (this machine)
    SC_OPT_DS_PATH_DIRECTSTORAGE = 1,  // desktop DirectStorage (not here)
} ScOptDsPath;

typedef enum ScOptDsDecompress {
    SC_OPT_DS_DECOMP_NONE = 0,
    SC_OPT_DS_DECOMP_ZLIB = 1,      // RFC 1950/1951 inflate, CPU
    SC_OPT_DS_DECOMP_BCPACK_BC1 = 2,
    SC_OPT_DS_DECOMP_BCPACK_BC2 = 3,
    SC_OPT_DS_DECOMP_BCPACK_BC3 = 4,
    SC_OPT_DS_DECOMP_BCPACK_BC4 = 5,
    SC_OPT_DS_DECOMP_BCPACK_BC5 = 6,
    SC_OPT_DS_DECOMP_BCPACK_BC6H = 7,
    SC_OPT_DS_DECOMP_BCPACK_BC7 = 8,
} ScOptDsDecompress;

// Initialize the queue path. Always succeeds on this machine (the
// overlapped path needs no runtime). Reports which path was taken.
HRESULT ScOpt_DStorageInit(ScOptDsPath* path);

// Read `size` bytes at `offset` from `path` (ANSI path) through the
// async queue (one overlapped read + wait). *data is malloc'd by the
// layer — the caller frees it with free(). *bytesRead is the bytes
// actually read (short reads are an error: E_FAIL).
HRESULT ScOpt_DStorageReadFile(const char* path, uint64_t offset,
                               uint32_t size, uint8_t** data,
                               uint32_t* bytesRead);

// Read `count` chunks concurrently through the queue (one overlapped
// read per chunk, all issued before waiting). offsets/sizes are
// per-chunk; datas[i] is malloc'd per chunk (caller frees each with
// free()). This is the queue's real benefit — N outstanding reads —
// versus N sequential synchronous reads. count must be 1..64.
HRESULT ScOpt_DStorageReadFileBatch(const char* path,
                                    const uint64_t* offsets,
                                    const uint32_t* sizes, uint32_t count,
                                    uint8_t** datas, uint32_t* bytesRead);

// Decompression stage. NONE is a memcpy. ZLIB is an RFC 1950/1951
// inflate on the CPU (stored + fixed-Huffman + dynamic-Huffman
// blocks; see the doc for what the test covers). BCPACK modes return
// E_NOTIMPL (see the header note). dst must hold dstLen bytes;
// *outLen receives the decompressed length.
HRESULT ScOpt_DStorageDecompress(ScOptDsDecompress mode,
                                 const uint8_t* src, size_t srcLen,
                                 uint8_t* dst, size_t dstLen,
                                 size_t* outLen);

#ifdef __cplusplus
}
#endif
