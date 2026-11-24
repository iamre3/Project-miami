// opt_pso_cache.cpp — PSO + root-signature caching (docs/OPTIMIZATION.md
// item 1). Open-addressing hash tables keyed by a canonical serialization
// of the description. A hit compares the full canonical bytes, so a hash
// collision degrades to a miss — it can never return the wrong PSO.
// Padding bytes are never hashed: every plain struct is copied into a
// zeroed local first, and shader bytecode is hashed by contents.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "opt_pso_cache.h"
#include "sc_opt_common.h"
#include "scarlett.h"

using namespace sc;

// ---- canonical serialization builder ----
struct CanonBuf {
    uint8_t* data = nullptr;
    size_t len = 0;
    size_t cap = 0;
    bool Append(const void* p, size_t n) {
        if (n == 0) return true;
        if (len + n > cap) {
            size_t nc = cap ? cap * 2 : 1024;
            while (nc < len + n) nc *= 2;
            uint8_t* nd = (uint8_t*)realloc(data, nc);
            if (!nd) return false;
            data = nd;
            cap = nc;
        }
        memcpy(data + len, p, n);
        len += n;
        return true;
    }
    template <typename T> bool AppendStruct(const T& v) {
        T zeroed = {};
        memcpy(&zeroed, &v, sizeof(T));
        return Append(&zeroed, sizeof(T));
    }
    bool AppendShader(const D3D12_SHADER_BYTECODE& bc) {
        uint64_t n = (uint64_t)bc.BytecodeLength;
        if (!Append(&n, sizeof(n))) return false;
        if (bc.pShaderBytecode && bc.BytecodeLength > 0)
            return Append(bc.pShaderBytecode, bc.BytecodeLength);
        return true;
    }
    void Free() {
        free(data);
        data = nullptr;
        len = cap = 0;
    }
};

// Canonical bytes for a graphics PSO description. Returns false on OOM.
static bool CanonGraphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d,
                          CanonBuf* out, uint64_t* hashOut) {
    CanonBuf b;
    // Root signature identity is the pointer: two different root
    // signature objects are different PSOs even with identical blobs.
    if (!b.Append(&d->pRootSignature, sizeof(d->pRootSignature))) return false;
    if (!b.AppendShader(d->VS)) return false;
    if (!b.AppendShader(d->PS)) return false;
    if (!b.AppendShader(d->DS)) return false;
    if (!b.AppendShader(d->HS)) return false;
    if (!b.AppendShader(d->GS)) return false;
    // Stream output.
    if (!b.Append(&d->StreamOutput.NumEntries, sizeof(UINT))) return false;
    for (UINT i = 0; i < d->StreamOutput.NumEntries; i++) {
        if (!b.AppendStruct(d->StreamOutput.pSODeclaration[i])) return false;
    }
    if (!b.Append(&d->StreamOutput.NumStrides, sizeof(UINT))) return false;
    for (UINT i = 0; i < d->StreamOutput.NumStrides; i++) {
        if (!b.Append(&d->StreamOutput.pBufferStrides[i], sizeof(UINT)))
            return false;
    }
    if (!b.AppendStruct(d->BlendState)) return false;
    if (!b.Append(&d->SampleMask, sizeof(UINT))) return false;
    if (!b.AppendStruct(d->RasterizerState)) return false;
    if (!b.AppendStruct(d->DepthStencilState)) return false;
    // Input layout: hash semantic name *contents*, not the pointer.
    if (!b.Append(&d->InputLayout.NumElements, sizeof(UINT))) return false;
    for (UINT i = 0; i < d->InputLayout.NumElements; i++) {
        const D3D12_INPUT_ELEMENT_DESC& e = d->InputLayout.pInputElementDescs[i];
        D3D12_INPUT_ELEMENT_DESC z = {};
        memcpy(&z, &e, sizeof(z));
        z.SemanticName = nullptr;
        if (!b.Append(&z, sizeof(z))) return false;
        uint64_t slen = e.SemanticName ? (uint64_t)strlen(e.SemanticName) : 0;
        if (!b.Append(&slen, sizeof(slen))) return false;
        if (slen > 0 && !b.Append(e.SemanticName, (size_t)slen)) return false;
    }
    if (!b.Append(&d->IBStripCutValue, sizeof(d->IBStripCutValue))) return false;
    if (!b.Append(&d->PrimitiveTopologyType, sizeof(d->PrimitiveTopologyType)))
        return false;
    if (!b.Append(&d->NumRenderTargets, sizeof(UINT))) return false;
    for (UINT i = 0; i < d->NumRenderTargets; i++) {
        if (!b.Append(&d->RTVFormats[i], sizeof(DXGI_FORMAT))) return false;
    }
    if (!b.Append(&d->DSVFormat, sizeof(DXGI_FORMAT))) return false;
    if (!b.AppendStruct(d->SampleDesc)) return false;
    if (!b.Append(&d->NodeMask, sizeof(UINT))) return false;
    if (!b.Append(&d->Flags, sizeof(d->Flags))) return false;
    // NOTE: CachedPSO deliberately excluded (driver hint, not identity).
    *hashOut = ScOptFnv1a(b.data, b.len);
    *out = b;
    return true;
}

static bool CanonCompute(const D3D12_COMPUTE_PIPELINE_STATE_DESC* d,
                         CanonBuf* out, uint64_t* hashOut) {
    CanonBuf b;
    if (!b.Append(&d->pRootSignature, sizeof(d->pRootSignature))) return false;
    if (!b.AppendShader(d->CS)) return false;
    if (!b.Append(&d->NodeMask, sizeof(UINT))) return false;
    if (!b.Append(&d->Flags, sizeof(d->Flags))) return false;
    *hashOut = ScOptFnv1a(b.data, b.len);
    *out = b;
    return true;
}

// ---- tables ----
static const size_t kTableCap = 256;

struct PsoEntry {
    bool used = false;
    uint64_t hash = 0;
    uint8_t* canon = nullptr;
    size_t canonLen = 0;
    ID3D12PipelineState* pso = nullptr;
};

struct RsEntry {
    bool used = false;
    uint64_t hash = 0;
    uint8_t* blob = nullptr;
    size_t blobLen = 0;
    ID3D12RootSignature* rs = nullptr;
};

static PsoEntry g_psoTable[kTableCap];
static RsEntry g_rsTable[kTableCap];
static CRITICAL_SECTION g_psoLock;
static bool g_psoInit = false;
static uint32_t g_created = 0, g_hits = 0, g_misses = 0;

static size_t ProbeStart(uint64_t hash) {
    return (size_t)(hash & (kTableCap - 1));
}

extern "C" HRESULT ScOpt_PsoCacheInit(ID3D12Device* device) {
    (void)device;
    if (g_psoInit) return S_OK;
    InitializeCriticalSection(&g_psoLock);
    memset(g_psoTable, 0, sizeof(g_psoTable));
    memset(g_rsTable, 0, sizeof(g_rsTable));
    g_created = g_hits = g_misses = 0;
    g_psoInit = true;
    Log("opt_pso", "PSO/root-signature cache initialized (256 slots)");
    return S_OK;
}

extern "C" void ScOpt_PsoCacheDestroy() {
    if (!g_psoInit) return;
    EnterCriticalSection(&g_psoLock);
    for (size_t i = 0; i < kTableCap; i++) {
        if (g_psoTable[i].used) {
            if (g_psoTable[i].pso) g_psoTable[i].pso->Release();
            free(g_psoTable[i].canon);
            g_psoTable[i] = PsoEntry{};
        }
        if (g_rsTable[i].used) {
            if (g_rsTable[i].rs) g_rsTable[i].rs->Release();
            free(g_rsTable[i].blob);
            g_rsTable[i] = RsEntry{};
        }
    }
    LeaveCriticalSection(&g_psoLock);
    DeleteCriticalSection(&g_psoLock);
    g_psoInit = false;
    Log("opt_pso", "PSO/root-signature cache destroyed");
}

// Shared lookup/insert for PSOs. `create` is only called on a miss,
// outside the lock's held span is not possible here (creation happens
// under the lock; PSO creation is not reentrant into this cache).
static HRESULT PsoGetOrCreate(ID3D12Device* device, CanonBuf* canon,
                              uint64_t hash, bool isCompute,
                              const D3D12_GRAPHICS_PIPELINE_STATE_DESC* gdesc,
                              const D3D12_COMPUTE_PIPELINE_STATE_DESC* cdesc,
                              ID3D12PipelineState** out, BOOL* hitOut) {
    if (!g_psoInit || !device || !out || !hitOut) return E_INVALIDARG;
    *out = nullptr;
    *hitOut = FALSE;
    EnterCriticalSection(&g_psoLock);
    size_t idx = ProbeStart(hash);
    PsoEntry* slot = nullptr;
    for (size_t probe = 0; probe < kTableCap; probe++) {
        PsoEntry* e = &g_psoTable[(idx + probe) & (kTableCap - 1)];
        if (!e->used) {
            slot = e;
            break;
        }
        if (e->hash == hash && e->canonLen == canon->len &&
            memcmp(e->canon, canon->data, canon->len) == 0) {
            // Full canonical match: a real hit (a hash collision would
            // fail the memcmp and fall through to the empty slot).
            e->pso->AddRef();
            *out = e->pso;
            *hitOut = TRUE;
            g_hits++;
            LeaveCriticalSection(&g_psoLock);
            return S_OK;
        }
    }
    if (!slot) {
        // Table full: do not cache, create uncached so the caller still
        // gets a working PSO.
        LeaveCriticalSection(&g_psoLock);
        HRESULT hr = isCompute
            ? device->CreateComputePipelineState(cdesc, IID_PPV_ARGS(out))
            : device->CreateGraphicsPipelineState(gdesc, IID_PPV_ARGS(out));
        if (SUCCEEDED(hr)) g_misses++;
        return hr;
    }
    // Miss: create, then store.
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = isCompute
        ? device->CreateComputePipelineState(cdesc, IID_PPV_ARGS(&pso))
        : device->CreateGraphicsPipelineState(gdesc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        LeaveCriticalSection(&g_psoLock);
        return hr;
    }
    slot->used = true;
    slot->hash = hash;
    slot->canon = canon->data;
    slot->canonLen = canon->len;
    slot->pso = pso;
    canon->data = nullptr;  // ownership moved to the entry
    canon->len = canon->cap = 0;
    g_misses++;
    g_created++;
    pso->AddRef();
    *out = pso;
    *hitOut = FALSE;
    LeaveCriticalSection(&g_psoLock);
    Log("opt_pso", "PSO cache miss -> created %p (hash 0x%016llx)",
        pso, (unsigned long long)hash);
    return S_OK;
}

extern "C" HRESULT ScOpt_PsoCacheGetGraphics(
    ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
    ID3D12PipelineState** pso, BOOL* hit) {
    if (!desc) return E_INVALIDARG;
    CanonBuf canon;
    uint64_t hash = 0;
    if (!CanonGraphics(desc, &canon, &hash)) return E_OUTOFMEMORY;
    HRESULT hr = PsoGetOrCreate(device, &canon, hash, false, desc, nullptr,
                                pso, hit);
    canon.Free();
    return hr;
}

extern "C" HRESULT ScOpt_PsoCacheGetCompute(
    ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
    ID3D12PipelineState** pso, BOOL* hit) {
    if (!desc) return E_INVALIDARG;
    CanonBuf canon;
    uint64_t hash = 0;
    if (!CanonCompute(desc, &canon, &hash)) return E_OUTOFMEMORY;
    HRESULT hr = PsoGetOrCreate(device, &canon, hash, true, nullptr, desc,
                                pso, hit);
    canon.Free();
    return hr;
}

extern "C" HRESULT ScOpt_PsoCacheGetRootSignature(ID3D12Device* device,
                                                  const void* blob,
                                                  SIZE_T blobLen,
                                                  ID3D12RootSignature** rs,
                                                  BOOL* hit) {
    if (!g_psoInit || !device || !blob || blobLen == 0 || !rs || !hit)
        return E_INVALIDARG;
    *rs = nullptr;
    *hit = FALSE;
    uint64_t hash = ScOptFnv1a(blob, blobLen);
    EnterCriticalSection(&g_psoLock);
    size_t idx = ProbeStart(hash);
    RsEntry* slot = nullptr;
    for (size_t probe = 0; probe < kTableCap; probe++) {
        RsEntry* e = &g_rsTable[(idx + probe) & (kTableCap - 1)];
        if (!e->used) {
            slot = e;
            break;
        }
        if (e->hash == hash && e->blobLen == blobLen &&
            memcmp(e->blob, blob, blobLen) == 0) {
            e->rs->AddRef();
            *rs = e->rs;
            *hit = TRUE;
            g_hits++;
            LeaveCriticalSection(&g_psoLock);
            return S_OK;
        }
    }
    if (!slot) {
        LeaveCriticalSection(&g_psoLock);
        HRESULT hr = device->CreateRootSignature(0, blob, blobLen,
                                                 IID_PPV_ARGS(rs));
        if (SUCCEEDED(hr)) g_misses++;
        return hr;
    }
    ID3D12RootSignature* out = nullptr;
    HRESULT hr = device->CreateRootSignature(0, blob, blobLen,
                                             IID_PPV_ARGS(&out));
    if (FAILED(hr)) {
        LeaveCriticalSection(&g_psoLock);
        return hr;
    }
    uint8_t* stored = (uint8_t*)malloc(blobLen);
    if (!stored) {
        out->Release();
        LeaveCriticalSection(&g_psoLock);
        return E_OUTOFMEMORY;
    }
    memcpy(stored, blob, blobLen);
    slot->used = true;
    slot->hash = hash;
    slot->blob = stored;
    slot->blobLen = blobLen;
    slot->rs = out;
    g_misses++;
    g_created++;
    out->AddRef();
    *rs = out;
    *hit = FALSE;
    LeaveCriticalSection(&g_psoLock);
    Log("opt_pso", "root-signature cache miss -> created %p", out);
    return S_OK;
}

extern "C" HRESULT ScOpt_PsoCacheStats(uint32_t* created, uint32_t* hits,
                                       uint32_t* misses) {
    if (!created || !hits || !misses) return E_INVALIDARG;
    if (!g_psoInit) {
        *created = *hits = *misses = 0;
        return S_OK;
    }
    EnterCriticalSection(&g_psoLock);
    *created = g_created;
    *hits = g_hits;
    *misses = g_misses;
    LeaveCriticalSection(&g_psoLock);
    return S_OK;
}
