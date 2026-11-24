// opt_batching.h — frame batching (docs/OPTIMIZATION.md item 8,
// overall item 9). Three levers, measured separately:
//   1. Command-list submission batching (fewer ExecuteCommandLists).
//   2. Draw/state batching (dedupe redundant state sets; merge
//      consecutive same-state contiguous draws).
//   3. Frames in flight / queue depth (2, 3, 4 — latency vs
//      GPU-starvation tradeoff, measured).
// Hard limits (enforced by design, see the doc): draw/submission
// order is never reordered — lists submit FIFO, draws merge only
// when consecutive with identical state and contiguous vertices.
// These are ScarlettCompat-defined entry points, NOT Xbox APIs.
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Lever 1: command-list submission batching ----
typedef void* ScOptBatchHandle;

// Create a submission batcher for `queue`. maxLists caps the pending
// batch (1..64); a flush happens automatically at the cap.
HRESULT ScOpt_BatchInit(ID3D12CommandQueue* queue, uint32_t maxLists,
                        ScOptBatchHandle* batch);

// Append a closed command list to the pending batch (FIFO). Flushes
// automatically when the batch reaches maxLists.
HRESULT ScOpt_BatchAdd(ScOptBatchHandle batch, ID3D12CommandList* list);

// Submit the whole pending batch in one ExecuteCommandLists call.
// A no-op when the batch is empty.
HRESULT ScOpt_BatchFlush(ScOptBatchHandle batch);

// Counts: listsSubmitted = lists handed to ExecuteCommandLists;
// submitCalls = ExecuteCommandLists calls made (the number batching
// is trying to reduce).
HRESULT ScOpt_BatchStats(ScOptBatchHandle batch, uint32_t* listsSubmitted,
                         uint32_t* submitCalls);

// Release the batcher (flushes any pending lists first).
void ScOpt_BatchDestroy(ScOptBatchHandle batch);

// ---- Lever 2: draw/state batching ----
typedef void* ScOptDrawBatchHandle;

// Start a recording session on `list`. Within the session, route all
// draw-related state sets and draws through this handle; direct
// commands on the list (barriers, clears, copies) must be preceded
// by ScOpt_DrawBatchFlushDraw, or they will land before a pending
// merged draw that belongs before them.
HRESULT ScOpt_DrawBatchBegin(ID3D12GraphicsCommandList* list,
                             ScOptDrawBatchHandle* batch);

// Set the PSO / root signature / render targets / vertex buffer,
// forwarded to the list only when the value differs from the one
// already set (identical re-sets are skipped and counted).
HRESULT ScOpt_DrawBatchSetPSO(ScOptDrawBatchHandle batch,
                              ID3D12PipelineState* pso);
HRESULT ScOpt_DrawBatchSetRootSignature(ScOptDrawBatchHandle batch,
                                        ID3D12RootSignature* rs);
HRESULT ScOpt_DrawBatchSetRenderTargets(
    ScOptDrawBatchHandle batch, UINT numRTs,
    const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL singleHandle,
    const D3D12_CPU_DESCRIPTOR_HANDLE* dsv);
HRESULT ScOpt_DrawBatchSetVertexBuffer(ScOptDrawBatchHandle batch, UINT slot,
                                       const D3D12_VERTEX_BUFFER_VIEW* view);

// Record a draw. Consecutive draws with identical state and
// contiguous vertex ranges are merged into one pending DrawInstanced
// (same vertices, same order, one call). A state change, a gap, or
// End flushes the pending draw first.
HRESULT ScOpt_DrawBatchDrawInstanced(ScOptDrawBatchHandle batch,
                                     UINT vertexCount, UINT startVertex,
                                     UINT instanceCount, UINT startInstance);

// Issue the pending merged draw now (no-op when there is none).
// Call this before any direct command on the list.
HRESULT ScOpt_DrawBatchFlushDraw(ScOptDrawBatchHandle batch);

// End the session: flush the pending draw. The handle is released.
HRESULT ScOpt_DrawBatchEnd(ScOptDrawBatchHandle batch);

// Counts: drawsRequested vs drawsRecorded (the merge reduction);
// setsRequested vs setsIssued (the dedupe reduction).
HRESULT ScOpt_DrawBatchStats(ScOptDrawBatchHandle batch,
                             uint32_t* drawsRequested,
                             uint32_t* drawsRecorded, uint32_t* setsRequested,
                             uint32_t* setsIssued);
void ScOpt_DrawBatchDestroy(ScOptDrawBatchHandle batch);

// ---- Lever 3: frames in flight / queue depth ----
typedef void* ScOptFlightHandle;

// Create a frame-slot manager for `depth` frames in flight (2, 3, or
// 4; anything else is E_INVALIDARG). One fence value per slot.
HRESULT ScOpt_FlightInit(uint32_t depth, ScOptFlightHandle* flight);

// Wait for the slot being reused this frame (frame index mod depth)
// to complete before the CPU records into it. *waitMs receives the
// milliseconds spent waiting (0 when the slot was already free).
// The caller passes the queue + fence it signals per frame.
HRESULT ScOpt_FlightBeginFrame(ScOptFlightHandle flight,
                               ID3D12CommandQueue* queue, ID3D12Fence* fence,
                               double* waitMs);

// Signal this frame's fence value on `queue`. Call once per frame,
// after executing the frame's lists.
HRESULT ScOpt_FlightEndFrame(ScOptFlightHandle flight,
                             ID3D12CommandQueue* queue, ID3D12Fence* fence);

// Counts: frames run, total CPU wait-for-slot time, average wait per
// frame (the latency cost of the queue depth).
HRESULT ScOpt_FlightStats(ScOptFlightHandle flight, uint32_t* frames,
                          double* totalWaitMs, double* avgWaitMs);
void ScOpt_FlightDestroy(ScOptFlightHandle flight);

#ifdef __cplusplus
}
#endif
