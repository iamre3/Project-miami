// opt_async_compute.h — async compute mapping (docs/OPTIMIZATION.md
// item 4). Xbox titles lean on async compute (graphics on DIRECT,
// compute on COMPUTE, overlapping). On RDNA1 this is fiddly — graphics
// and compute contend for the same CUs — so this module is a
// *measurement*, not an assumed win: it runs a fixed synthetic
// workload both ways and reports the numbers on the 5700 XT. These are
// ScarlettCompat-defined entry points, NOT Xbox APIs.
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ScOptAsyncMode {
    SC_OPT_ASYNC_SERIAL = 0,  // compute + draw, both on DIRECT, one list
    SC_OPT_ASYNC_ASYNC = 1,   // compute on COMPUTE, draw on DIRECT, overlap
} ScOptAsyncMode;

// Compile the workload shaders and create the queues, PSOs, buffers
// and fences once (per process). Safe to call more than once.
HRESULT ScOpt_AsyncInit(ID3D12Device* device);

// Run the fixed workload N times in the given mode and report the
// average wall time in ms (QPC around execute + fence completion).
// The workload is a compute dispatch (256K threads, xorshift loop)
// plus a triangle draw to a 256x256 target. Both modes produce the
// same compute output and the same draw pixels — timing is the only
// difference. Returns E_INVALIDARG for a bad mode.
HRESULT ScOpt_AsyncRunTimed(ID3D12Device* device, ScOptAsyncMode mode,
                             double* avgMs);

// FNV-1a checksum of the compute output buffer from the last RunTimed
// call in the given mode (read back after the fences). The guest
// compares serial vs async checksums to prove async did not change
// the result.
HRESULT ScOpt_AsyncChecksum(ScOptAsyncMode mode, uint64_t* checksum);

// Release the queues, PSOs, buffers and fences. Safe to call when
// never initialized.
void ScOpt_AsyncDestroy();

#ifdef __cplusplus
}
#endif
