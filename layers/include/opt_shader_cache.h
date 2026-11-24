// opt_shader_cache.h — shader precompilation cache (docs/OPTIMIZATION.md
// item 7). The first time a shader runs, compiling it from source is a
// hitch. This module hashes the source + profile + entry point and
// stores the compiled blob on disk; a later run with the same source
// loads the blob instead of compiling. Corrupt or mismatched files are
// treated as a miss, never trusted. These are ScarlettCompat-defined
// entry points, NOT Xbox APIs.
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the cache directory. If dir is null, the default is used:
// %LOCALAPPDATA%\ScarlettCompat\ShaderCache. The directory is created
// if it does not exist.
HRESULT ScOpt_ShaderCacheInit(const char* dir);

// Compile `source` (sourceLen bytes, HLSL) for `profile` (e.g. "vs_5_0")
// at `entryPoint`, or load the compiled blob from disk if the source
// hashes to an entry already cached. *blob is malloc'd by the layer —
// the caller frees it with free(). *blobSize is the blob byte count.
// A cache hit returns S_OK without compiling; a miss compiles with
// D3DCompile, stores the blob, and returns S_OK. A compile error
// returns the D3DCompile HRESULT.
HRESULT ScOpt_CompileCached(const char* source, size_t sourceLen,
                            const char* profile, const char* entryPoint,
                            uint8_t** blob, size_t* blobSize);

// Counts: hits = loads from disk without compiling; misses = compiles
// (including recompiles after a corrupt/mismatched file).
void ScOpt_ShaderCacheStats(uint64_t* hits, uint64_t* misses);

#ifdef __cplusplus
}
#endif
