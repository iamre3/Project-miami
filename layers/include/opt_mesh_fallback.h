// opt_mesh_fallback.h — mesh-shader fallback for RDNA1
// (docs/OPTIMIZATION.md item 3). The title has 9,282 mesh shaders; mesh
// shaders need FL 12_2 and the RX 5700 XT is FL 12_1 with no
// mesh-shader hardware. This module replaces the mesh pipeline with the
// classic pipeline: a meshlet (vertices + indices) is expanded into a
// plain vertex buffer + index buffer, and the guest draws it with a
// normal graphics PSO + DrawIndexed. These are ScarlettCompat-defined
// entry points, NOT Xbox APIs.
//
// SCOPE (honest): expansion is done on the CPU in this phase — exactly
// right and simple to prove, but a correctness path, not a performance
// path (expanding thousands of meshlets on the CPU per frame would be
// slow; a GPU compute expansion is the follow-up). Amplification
// shaders (meshlet selection/culling) are NOT emulated — the caller
// passes the meshlet list in.
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One meshlet: a small cluster of triangles. Positions are clip-space
// XYZ (the fallback vertex shader passes them through unchanged);
// colors are linear RGBA per vertex.
typedef struct ScOptMeshlet {
    const float* positions;  // vertexCount * 3 floats
    const float* colors;     // vertexCount * 4 floats
    const uint32_t* indices; // indexCount entries (into the vertex list)
    uint32_t vertexCount;
    uint32_t indexCount;
} ScOptMeshlet;

// Expanded mesh: GPU buffers the guest draws with DrawIndexed.
// vertexBuffer holds interleaved {float3 pos, float4 col} (28 B/vertex).
typedef struct ScOptExpandedMesh {
    ID3D12Resource* vertexBuffer;
    ID3D12Resource* indexBuffer;
    uint32_t vertexCount;
    uint32_t indexCount;
} ScOptExpandedMesh;

// Compile the fallback VS/PS once (cached per process) and create the
// internal upload queue used by Expand. Safe to call more than once.
HRESULT ScOpt_MeshFallbackInit(ID3D12Device* device);

// The compiled fallback shaders (owned by the layer — do not free or
// release). The guest builds its own PSO from these blobs (empty root
// signature, RTV format R8G8B8A8_UNORM, like the proof guest).
HRESULT ScOpt_MeshFallbackGetShaders(const void** vsBlob, SIZE_T* vsLen,
                                     const void** psBlob, SIZE_T* psLen);

// Expand a meshlet into GPU vertex + index buffers (CPU expansion,
// synchronous: returns after the upload fence completes).
HRESULT ScOpt_MeshFallbackExpand(ID3D12Device* device,
                                 const ScOptMeshlet* meshlet,
                                 ScOptExpandedMesh* expanded);

// Release an expanded mesh's GPU resources.
void ScOpt_MeshFallbackFreeExpanded(ScOptExpandedMesh* expanded);

// Release the cached shaders and the internal queue. Safe to call
// when never initialized.
void ScOpt_MeshFallbackDestroy();

#ifdef __cplusplus
}
#endif
