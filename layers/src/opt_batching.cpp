// opt_batching.cpp — frame batching (docs/OPTIMIZATION.md item 8,
// overall item 9). Three levers: submission batching (fewer
// ExecuteCommandLists), draw/state batching (dedupe sets, merge
// contiguous same-state draws), frames in flight (queue depth 2/3/4).
// Order is never reordered: submission is FIFO, merges are consecutive
// + identical-state + contiguous only. See the doc for the design,
// the correctness conditions, and the measurement plan.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <new>  // std::nothrow (not pulled in by the other headers)
#include "opt_batching.h"
#include "sc_opt_common.h"
#include "scarlett.h"

using namespace sc;

// ---- Lever 1: submission batching ----
struct Batch {
    ID3D12CommandQueue* queue = nullptr;
    uint32_t maxLists = 0;
    ID3D12CommandList* lists[64] = {};
    uint32_t count = 0;
    uint32_t listsSubmitted = 0;
    uint32_t submitCalls = 0;
    CRITICAL_SECTION lock;
};

extern "C" HRESULT ScOpt_BatchInit(ID3D12CommandQueue* queue,
                                   uint32_t maxLists,
                                   ScOptBatchHandle* batch) {
    if (!queue || maxLists == 0 || maxLists > 64 || !batch)
        return E_INVALIDARG;
    Batch* b = new (std::nothrow) Batch();
    if (!b) return E_OUTOFMEMORY;
    b->queue = queue;
    b->maxLists = maxLists;
    InitializeCriticalSection(&b->lock);
    *batch = (ScOptBatchHandle)b;
    Log("opt_batch", "submission batcher init (maxLists=%u)", maxLists);
    return S_OK;
}

static HRESULT BatchFlushLocked(Batch* b) {
    if (b->count == 0) return S_OK;
    b->queue->ExecuteCommandLists(b->count, b->lists);
    b->listsSubmitted += b->count;
    b->submitCalls++;
    b->count = 0;
    return S_OK;
}

extern "C" HRESULT ScOpt_BatchAdd(ScOptBatchHandle batch,
                                  ID3D12CommandList* list) {
    if (!batch || !list) return E_INVALIDARG;
    Batch* b = (Batch*)batch;
    EnterCriticalSection(&b->lock);
    b->lists[b->count++] = list;
    if (b->count >= b->maxLists) BatchFlushLocked(b);  // auto-flush at cap
    LeaveCriticalSection(&b->lock);
    return S_OK;
}

extern "C" HRESULT ScOpt_BatchFlush(ScOptBatchHandle batch) {
    if (!batch) return E_INVALIDARG;
    Batch* b = (Batch*)batch;
    EnterCriticalSection(&b->lock);
    HRESULT hr = BatchFlushLocked(b);
    LeaveCriticalSection(&b->lock);
    return hr;
}

extern "C" HRESULT ScOpt_BatchStats(ScOptBatchHandle batch,
                                    uint32_t* listsSubmitted,
                                    uint32_t* submitCalls) {
    if (!batch || !listsSubmitted || !submitCalls) return E_INVALIDARG;
    Batch* b = (Batch*)batch;
    EnterCriticalSection(&b->lock);
    *listsSubmitted = b->listsSubmitted;
    *submitCalls = b->submitCalls;
    LeaveCriticalSection(&b->lock);
    return S_OK;
}

extern "C" void ScOpt_BatchDestroy(ScOptBatchHandle batch) {
    if (!batch) return;
    Batch* b = (Batch*)batch;
    EnterCriticalSection(&b->lock);
    BatchFlushLocked(b);  // never drop a pending list
    LeaveCriticalSection(&b->lock);
    DeleteCriticalSection(&b->lock);
    delete b;
}

// ---- Lever 2: draw/state batching ----
struct DrawBatch {
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12PipelineState* curPso = nullptr;
    ID3D12RootSignature* curRs = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE curRtvs[8] = {};
    UINT curNumRtvs = 0;
    BOOL curSingleHandle = FALSE;
    D3D12_CPU_DESCRIPTOR_HANDLE curDsv = {};
    bool hasDsv = false;
    bool hasRtv = false;  // a SetRenderTargets has been issued at least once
    D3D12_VERTEX_BUFFER_VIEW curVb = {};
    bool hasVb = false;
    // Pending merged draw (held, not yet issued).
    bool hasPending = false;
    UINT pendingVertexCount = 0;
    UINT pendingStartVertex = 0;
    UINT pendingInstanceCount = 0;
    UINT pendingStartInstance = 0;
    uint32_t drawsRequested = 0;
    uint32_t drawsRecorded = 0;
    uint32_t setsRequested = 0;
    uint32_t setsIssued = 0;
};

static void FlushPendingDraw(DrawBatch* b) {
    if (!b->hasPending) return;
    b->list->DrawInstanced(b->pendingVertexCount, b->pendingInstanceCount,
                           b->pendingStartVertex, b->pendingStartInstance);
    b->drawsRecorded++;
    b->hasPending = false;
}

extern "C" HRESULT ScOpt_DrawBatchBegin(ID3D12GraphicsCommandList* list,
                                        ScOptDrawBatchHandle* batch) {
    if (!list || !batch) return E_INVALIDARG;
    DrawBatch* b = new (std::nothrow) DrawBatch();
    if (!b) return E_OUTOFMEMORY;
    b->list = list;
    *batch = (ScOptDrawBatchHandle)b;
    return S_OK;
}

extern "C" HRESULT ScOpt_DrawBatchSetPSO(ScOptDrawBatchHandle batch,
                                         ID3D12PipelineState* pso) {
    if (!batch) return E_INVALIDARG;
    DrawBatch* b = (DrawBatch*)batch;
    b->setsRequested++;
    if (pso == b->curPso) return S_OK;  // identical re-set: skip
    FlushPendingDraw(b);  // the pending draw belongs under the old PSO
    b->list->SetPipelineState(pso);
    b->curPso = pso;
    b->setsIssued++;
    return S_OK;
}

extern "C" HRESULT ScOpt_DrawBatchSetRootSignature(ScOptDrawBatchHandle batch,
                                                   ID3D12RootSignature* rs) {
    if (!batch) return E_INVALIDARG;
    DrawBatch* b = (DrawBatch*)batch;
    b->setsRequested++;
    if (rs == b->curRs) return S_OK;
    FlushPendingDraw(b);
    b->list->SetGraphicsRootSignature(rs);
    b->curRs = rs;
    b->setsIssued++;
    return S_OK;
}

extern "C" HRESULT ScOpt_DrawBatchSetRenderTargets(
    ScOptDrawBatchHandle batch, UINT numRTs,
    const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL singleHandle,
    const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    if (!batch || numRTs > 8 || (numRTs > 0 && !rtvs)) return E_INVALIDARG;
    DrawBatch* b = (DrawBatch*)batch;
    b->setsRequested++;
    // Only skip when a previous set actually happened (the first set
    // is always issued — the list starts with no render targets).
    bool same = b->hasRtv && (numRTs == b->curNumRtvs) &&
                (singleHandle == b->curSingleHandle) &&
                (b->hasDsv == (dsv != nullptr));
    if (same) {
        for (UINT i = 0; i < numRTs; i++) {
            if (rtvs[i].ptr != b->curRtvs[i].ptr) {
                same = false;
                break;
            }
        }
        if (same && dsv && dsv->ptr != b->curDsv.ptr) same = false;
    }
    if (same) return S_OK;  // identical re-set: skip
    FlushPendingDraw(b);
    b->list->OMSetRenderTargets(numRTs, rtvs, singleHandle, dsv);
    b->curNumRtvs = numRTs;
    b->curSingleHandle = singleHandle;
    for (UINT i = 0; i < numRTs; i++) b->curRtvs[i] = rtvs[i];
    b->hasDsv = (dsv != nullptr);
    b->hasRtv = true;
    if (dsv) b->curDsv = *dsv;
    b->setsIssued++;
    return S_OK;
}

extern "C" HRESULT ScOpt_DrawBatchSetVertexBuffer(ScOptDrawBatchHandle batch,
                                                  UINT slot,
                                                  const D3D12_VERTEX_BUFFER_VIEW* view) {
    if (!batch || slot != 0 || !view) return E_INVALIDARG;
    DrawBatch* b = (DrawBatch*)batch;
    b->setsRequested++;
    // Slot 0 only in this phase (the merge tracker watches one buffer).
    bool same = b->hasVb &&
                b->curVb.BufferLocation == view->BufferLocation &&
                b->curVb.SizeInBytes == view->SizeInBytes &&
                b->curVb.StrideInBytes == view->StrideInBytes;
    if (same) return S_OK;
    FlushPendingDraw(b);
    b->list->IASetVertexBuffers(slot, 1, view);
    b->curVb = *view;
    b->hasVb = true;
    b->setsIssued++;
    return S_OK;
}

extern "C" HRESULT ScOpt_DrawBatchDrawInstanced(ScOptDrawBatchHandle batch,
                                                UINT vertexCount,
                                                UINT startVertex,
                                                UINT instanceCount,
                                                UINT startInstance) {
    if (!batch || vertexCount == 0) return E_INVALIDARG;
    DrawBatch* b = (DrawBatch*)batch;
    b->drawsRequested++;
    // Merge only when the pending draw is contiguous, single-instance,
    // and the new draw is single-instance starting exactly where the
    // pending one ends. (State cannot have changed since the pending
    // draw was recorded — any Set* flushes it first.)
    if (b->hasPending && b->pendingInstanceCount == 1 &&
        b->pendingStartInstance == 0 && instanceCount == 1 &&
        startInstance == 0 &&
        startVertex == b->pendingStartVertex + b->pendingVertexCount) {
        b->pendingVertexCount += vertexCount;
        return S_OK;
    }
    FlushPendingDraw(b);
    b->pendingVertexCount = vertexCount;
    b->pendingStartVertex = startVertex;
    b->pendingInstanceCount = instanceCount;
    b->pendingStartInstance = startInstance;
    b->hasPending = true;
    return S_OK;
}

extern "C" HRESULT ScOpt_DrawBatchFlushDraw(ScOptDrawBatchHandle batch) {
    if (!batch) return E_INVALIDARG;
    FlushPendingDraw((DrawBatch*)batch);
    return S_OK;
}

extern "C" HRESULT ScOpt_DrawBatchEnd(ScOptDrawBatchHandle batch) {
    if (!batch) return E_INVALIDARG;
    DrawBatch* b = (DrawBatch*)batch;
    FlushPendingDraw(b);
    delete b;
    return S_OK;
}

extern "C" HRESULT ScOpt_DrawBatchStats(ScOptDrawBatchHandle batch,
                                        uint32_t* drawsRequested,
                                        uint32_t* drawsRecorded,
                                        uint32_t* setsRequested,
                                        uint32_t* setsIssued) {
    if (!batch || !drawsRequested || !drawsRecorded || !setsRequested ||
        !setsIssued)
        return E_INVALIDARG;
    DrawBatch* b = (DrawBatch*)batch;
    *drawsRequested = b->drawsRequested;
    *drawsRecorded = b->drawsRecorded;
    *setsRequested = b->setsRequested;
    *setsIssued = b->setsIssued;
    return S_OK;
}

extern "C" void ScOpt_DrawBatchDestroy(ScOptDrawBatchHandle batch) {
    if (!batch) return;
    DrawBatch* b = (DrawBatch*)batch;
    FlushPendingDraw(b);
    delete b;
}

// ---- Lever 3: frames in flight ----
struct Flight {
    uint32_t depth = 0;
    uint32_t frameIndex = 0;
    UINT64 nextFenceValue = 0;
    UINT64 slotValues[4] = {};  // fence value per slot (0 = never used)
    uint32_t frames = 0;
    double totalWaitMs = 0.0;
    HANDLE event = nullptr;
    CRITICAL_SECTION lock;
};

extern "C" HRESULT ScOpt_FlightInit(uint32_t depth, ScOptFlightHandle* flight) {
    if (depth < 2 || depth > 4 || !flight) return E_INVALIDARG;
    Flight* f = new (std::nothrow) Flight();
    if (!f) return E_OUTOFMEMORY;
    f->depth = depth;
    f->event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!f->event) {
        delete f;
        return E_FAIL;
    }
    InitializeCriticalSection(&f->lock);
    *flight = (ScOptFlightHandle)f;
    Log("opt_batch", "frames-in-flight init (depth=%u)", depth);
    return S_OK;
}

extern "C" HRESULT ScOpt_FlightBeginFrame(ScOptFlightHandle flight,
                                          ID3D12CommandQueue* queue,
                                          ID3D12Fence* fence,
                                          double* waitMs) {
    if (!flight || !queue || !fence || !waitMs) return E_INVALIDARG;
    Flight* f = (Flight*)flight;
    *waitMs = 0.0;
    EnterCriticalSection(&f->lock);
    uint32_t slot = f->frameIndex % f->depth;
    UINT64 value = f->slotValues[slot];
    LeaveCriticalSection(&f->lock);
    if (value == 0) return S_OK;  // slot never used: nothing to wait for
    ScOptTimer t;
    t.Start();
    fence->SetEventOnCompletion(value, f->event);
    DWORD w = WaitForSingleObject(f->event, 30000);
    double waited = t.ElapsedMs();
    if (w != WAIT_OBJECT_0) {
        Log("opt_batch", "FlightBeginFrame wait timed out (slot %u value "
                         "%llu)",
            slot, (unsigned long long)value);
        return E_FAIL;
    }
    EnterCriticalSection(&f->lock);
    f->totalWaitMs += waited;
    LeaveCriticalSection(&f->lock);
    *waitMs = waited;
    return S_OK;
}

extern "C" HRESULT ScOpt_FlightEndFrame(ScOptFlightHandle flight,
                                        ID3D12CommandQueue* queue,
                                        ID3D12Fence* fence) {
    if (!flight || !queue || !fence) return E_INVALIDARG;
    Flight* f = (Flight*)flight;
    EnterCriticalSection(&f->lock);
    uint32_t slot = f->frameIndex % f->depth;
    UINT64 value = ++f->nextFenceValue;
    f->slotValues[slot] = value;
    f->frameIndex++;
    f->frames++;
    LeaveCriticalSection(&f->lock);
    return queue->Signal(fence, value);
}

extern "C" HRESULT ScOpt_FlightStats(ScOptFlightHandle flight,
                                     uint32_t* frames, double* totalWaitMs,
                                     double* avgWaitMs) {
    if (!flight || !frames || !totalWaitMs || !avgWaitMs) return E_INVALIDARG;
    Flight* f = (Flight*)flight;
    EnterCriticalSection(&f->lock);
    *frames = f->frames;
    *totalWaitMs = f->totalWaitMs;
    *avgWaitMs = f->frames > 0 ? f->totalWaitMs / f->frames : 0.0;
    LeaveCriticalSection(&f->lock);
    return S_OK;
}

extern "C" void ScOpt_FlightDestroy(ScOptFlightHandle flight) {
    if (!flight) return;
    Flight* f = (Flight*)flight;
    if (f->event) CloseHandle(f->event);
    DeleteCriticalSection(&f->lock);
    delete f;
}
