// opt_shader_cache.cpp — shader precompilation cache (docs/OPTIMIZATION.md
// item 7). Disk cache keyed on FNV-1a(source + profile + entryPoint).
// File format: magic "SCSC" (4 bytes) + sourceHash (8) + blobSize (8),
// then the compiled blob bytes. A file whose magic, hash, or size does
// not check out is treated as a miss and overwritten.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "opt_shader_cache.h"
#include "sc_opt_common.h"
#include "scarlett.h"

using namespace sc;

static char g_cacheDir[MAX_PATH] = {};
static bool g_scInit = false;
static uint64_t g_scHits = 0;
static uint64_t g_scMisses = 0;
static CRITICAL_SECTION g_scLock;

struct CacheHeader {
    char magic[4];  // "SCSC"
    uint64_t sourceHash;
    uint64_t blobSize;
    uint64_t blobHash;  // FNV-1a of the blob bytes (tamper check)
};

extern "C" HRESULT ScOpt_ShaderCacheInit(const char* dir) {
    if (g_scInit) return S_OK;
    InitializeCriticalSection(&g_scLock);
    if (dir && dir[0]) {
        strncpy_s(g_cacheDir, sizeof(g_cacheDir), dir, _TRUNCATE);
    } else {
        char localAppData[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", localAppData,
                                          sizeof(localAppData));
        if (n == 0 || n >= sizeof(localAppData)) {
            DeleteCriticalSection(&g_scLock);
            return E_FAIL;
        }
        snprintf(g_cacheDir, sizeof(g_cacheDir),
                 "%s\\ScarlettCompat\\ShaderCache", localAppData);
    }
    if (!CreateDirectoryA(g_cacheDir, nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        // The parent may not exist yet — create it, then retry.
        char parent[MAX_PATH] = {};
        strncpy_s(parent, sizeof(parent), g_cacheDir, _TRUNCATE);
        char* slash = strrchr(parent, '\\');
        if (slash) {
            *slash = 0;
            CreateDirectoryA(parent, nullptr);
        }
        if (!CreateDirectoryA(g_cacheDir, nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            DeleteCriticalSection(&g_scLock);
            return HRESULT_FROM_WIN32(GetLastError());
        }
    }
    g_scInit = true;
    Log("opt_shader", "shader cache directory: %s", g_cacheDir);
    return S_OK;
}

static void CachePath(uint64_t hash, char* out, size_t outSize) {
    snprintf(out, outSize, "%s\\%016llx.cso", g_cacheDir,
             (unsigned long long)hash);
}

static HRESULT TryLoad(uint64_t hash, uint8_t** blob, size_t* blobSize) {
    char path[MAX_PATH] = {};
    CachePath(hash, path, sizeof(path));
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return E_FAIL;  // no entry: a miss
    CacheHeader hdr = {};
    DWORD read = 0;
    BOOL ok = ReadFile(f, &hdr, sizeof(hdr), &read, nullptr);
    if (!ok || read != sizeof(hdr) || memcmp(hdr.magic, "SCSC", 4) != 0 ||
        hdr.sourceHash != hash || hdr.blobSize == 0 ||
        hdr.blobSize > 64 * 1024 * 1024) {
        CloseHandle(f);
        return E_FAIL;  // corrupt/mismatched: a miss
    }
    uint8_t* buf = (uint8_t*)malloc((size_t)hdr.blobSize);
    if (!buf) {
        CloseHandle(f);
        return E_OUTOFMEMORY;
    }
    ok = ReadFile(f, buf, (DWORD)hdr.blobSize, &read, nullptr);
    CloseHandle(f);
    if (!ok || read != hdr.blobSize) {
        free(buf);
        return E_FAIL;
    }
    // The blob must hash to the digest stored in the header: a file
    // whose bytes were tampered with after a valid header is caught here.
    if (ScOptFnv1a(buf, (size_t)hdr.blobSize) != hdr.blobHash) {
        free(buf);
        return E_FAIL;
    }
    *blob = buf;
    *blobSize = (size_t)hdr.blobSize;
    return S_OK;
}

static HRESULT Store(uint64_t hash, const uint8_t* blob, size_t blobSize) {
    char path[MAX_PATH] = {};
    CachePath(hash, path, sizeof(path));
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());
    CacheHeader hdr = {};
    memcpy(hdr.magic, "SCSC", 4);
    hdr.sourceHash = hash;
    hdr.blobSize = (uint64_t)blobSize;
    hdr.blobHash = ScOptFnv1a(blob, blobSize);
    DWORD written = 0;
    BOOL ok = WriteFile(f, &hdr, sizeof(hdr), &written, nullptr);
    if (ok && written == sizeof(hdr))
        ok = WriteFile(f, blob, (DWORD)blobSize, &written, nullptr);
    CloseHandle(f);
    if (!ok || written != blobSize) return E_FAIL;
    return S_OK;
}

extern "C" HRESULT ScOpt_CompileCached(const char* source, size_t sourceLen,
                                       const char* profile,
                                       const char* entryPoint,
                                       uint8_t** blob, size_t* blobSize) {
    if (!g_scInit || !source || sourceLen == 0 || !profile || !entryPoint ||
        !blob || !blobSize)
        return E_INVALIDARG;
    *blob = nullptr;
    *blobSize = 0;
    // Key: FNV-1a over source bytes, then profile, then entry point.
    uint64_t h = ScOptFnv1a(source, sourceLen);
    h = ScOptHashBytes(h, profile, strlen(profile));
    h = ScOptHashBytes(h, entryPoint, strlen(entryPoint));
    EnterCriticalSection(&g_scLock);
    HRESULT hr = TryLoad(h, blob, blobSize);
    if (SUCCEEDED(hr)) {
        g_scHits++;
        LeaveCriticalSection(&g_scLock);
        Log("opt_shader", "shader cache HIT (hash %016llx, %llu bytes)",
            (unsigned long long)h, (unsigned long long)*blobSize);
        return S_OK;
    }
    // Miss: compile from source.
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    hr = D3DCompile(source, sourceLen, nullptr, nullptr, nullptr, entryPoint,
                    profile, 0, 0, &code, &errors);
    if (FAILED(hr)) {
        if (errors) {
            Log("opt_shader", "D3DCompile failed for %s %s: %s", profile,
                entryPoint, (const char*)errors->GetBufferPointer());
            errors->Release();
        }
        LeaveCriticalSection(&g_scLock);
        return hr;
    }
    uint8_t* buf = (uint8_t*)malloc(code->GetBufferSize());
    if (!buf) {
        code->Release();
        LeaveCriticalSection(&g_scLock);
        return E_OUTOFMEMORY;
    }
    memcpy(buf, code->GetBufferPointer(), code->GetBufferSize());
    *blob = buf;
    *blobSize = code->GetBufferSize();
    HRESULT storeHr = Store(h, buf, *blobSize);
    code->Release();
    g_scMisses++;
    LeaveCriticalSection(&g_scLock);
    Log("opt_shader", "shader cache MISS (compiled %s %s, %llu bytes, "
                      "store %s)",
        profile, entryPoint, (unsigned long long)*blobSize,
        SUCCEEDED(storeHr) ? "ok" : "FAILED");
    return S_OK;
}

extern "C" void ScOpt_ShaderCacheStats(uint64_t* hits, uint64_t* misses) {
    if (!g_scInit) {
        if (hits) *hits = 0;
        if (misses) *misses = 0;
        return;
    }
    EnterCriticalSection(&g_scLock);
    if (hits) *hits = g_scHits;
    if (misses) *misses = g_scMisses;
    LeaveCriticalSection(&g_scLock);
}
