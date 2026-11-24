// software_rt.cpp — software ray tracing backend (see docs/SOFTWARE-RT.md).
// Compute-shader ray tracer: ray generation + stack BVH walk +
// Moller-Trumbore intersection + closest-hit / miss shading, writing the
// caller's UAV texture. No RT cores needed; runs on the RX 5700 XT
// (RaytracingTier NOT_SUPPORTED). Compiles its HLSL at runtime with
// D3DCompile (same technique as the proof guest's triangle shaders).
//
// Self-contained: no dependency on hle_registry.cpp (its own log line),
// so it links both into ScarlettCompat.dll and standalone into the
// host-side test. Camera is passed as 18 root constants (b0) — no
// cbuffer upload buffer to keep alive across dispatches. Scene buffers
// live in upload heaps (simple, correct for small scenes; a DEFAULT-heap
// + copy path is a future optimization). The output UAV descriptor goes
// into a cached 1-slot shader-visible heap: the caller must fence-wait
// between dispatches that use different output textures.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "software_rt.h"
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <algorithm>

namespace {

// ---- logging (local; no hle_registry dependency) ----
void SrtLog(const char* fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[srt] %s\n", body);
}

// ---- BVH node: 32 bytes, shared layout with the HLSL struct ----
// mn(12) + left(4) + mx(12) + right(4). Interior node: left/right are
// child indices. Leaf: left = triangle index, right = 0xFFFFFFFF
// (one triangle per leaf, so no count field is needed).
struct BvhNode {
    float mn[3];
    uint32_t left;
    float mx[3];
    uint32_t right;
};
static_assert(sizeof(BvhNode) == 32, "BvhNode must be 32 bytes");
static const uint32_t kLeafSentinel = 0xFFFFFFFF;

// ---- cached per-process state (set by Init) ----
ID3D12RootSignature* g_rootSig = nullptr;
ID3D12PipelineState* g_pso = nullptr;
ID3D12DescriptorHeap* g_uavHeap = nullptr;
UINT g_uavHeapSize = 0;
ScRtPath g_path = SC_RT_PATH_SOFTWARE;
bool g_pathKnown = false;
ScRtMode g_mode = SC_RT_MODE_SOFTWARE;
bool g_modeKnown = false;

// ---- the compute shader ----
// Root signature (declared in HLSL via root signature syntax is NOT used
// here — the C++ side builds it): b0 = 18 root constants (camera),
// t0 = verts, t1 = colors, t2 = BVH nodes, u0 = output texture.
// cbuffer packing: each float3 starts a 16-byte row, so the 18 dwords
// are origin(3)+pad, lowerLeft(3)+pad, horizontal(3)+pad,
// vertical(3)+pad, width, height.
const char* kRayTraceHLSL =
    "struct BvhNode { float3 mn; uint left; float3 mx; uint right; };\n"
    "cbuffer CameraCB : register(b0) {\n"
    "  float3 origin; float _p0;\n"
    "  float3 lowerLeft; float _p1;\n"
    "  float3 horizontal; float _p2;\n"
    "  float3 vertical; float _p3;\n"
    "  float width; float height; float2 _p4;\n"
    "};\n"
    "StructuredBuffer<float3> g_verts : register(t0);\n"
    "StructuredBuffer<float4> g_colors : register(t1);\n"
    "StructuredBuffer<BvhNode> g_nodes : register(t2);\n"
    "RWTexture2D<float4> g_out : register(u0);\n"
    "bool RayBox(float3 o, float3 invD, float3 mn, float3 mx,\n"
    "            float tMin, float tMax) {\n"
    "  float3 t0 = (mn - o) * invD;\n"
    "  float3 t1 = (mx - o) * invD;\n"
    "  float3 tsm = min(t0, t1);\n"
    "  float3 tbg = max(t0, t1);\n"
    "  tMin = max(tMin, max(tsm.x, max(tsm.y, tsm.z)));\n"
    "  tMax = min(tMax, min(tbg.x, min(tbg.y, tbg.z)));\n"
    "  return tMax >= tMin;\n"
    "}\n"
    "bool RayTri(float3 o, float3 d, float3 v0, float3 v1, float3 v2,\n"
    "            inout float t) {\n"
    "  float3 e1 = v1 - v0;\n"
    "  float3 e2 = v2 - v0;\n"
    "  float3 p = cross(d, e2);\n"
    "  float det = dot(e1, p);\n"
    "  if (abs(det) < 1e-8) return false;\n"
    "  float invDet = 1.0 / det;\n"
    "  float3 tv = o - v0;\n"
    "  float u = dot(tv, p) * invDet;\n"
    "  if (u < 0.0 || u > 1.0) return false;\n"
    "  float3 q = cross(tv, e1);\n"
    "  float v = dot(d, q) * invDet;\n"
    "  if (v < 0.0 || u + v > 1.0) return false;\n"
    "  t = dot(e2, q) * invDet;\n"
    "  return t > 0.001;\n"
    "}\n"
    "[numthreads(8, 8, 1)]\n"
    "void CS(uint3 tid : SV_DispatchThreadID) {\n"
    "  if (tid.x >= (uint)width || tid.y >= (uint)height) return;\n"
    "  float u = (tid.x + 0.5) / width;\n"
    "  float v = (tid.y + 0.5) / height;\n"
    "  float3 dir = normalize(lowerLeft + u * horizontal +\n"
    "                        v * vertical - origin);\n"
    "  float3 invD = 1.0 / dir;\n"
    "  float closest = 1e30;\n"
    "  int hitTri = -1;\n"
    "  uint stack[32];\n"
    "  int sp = 0;\n"
    "  stack[sp++] = 0;\n"
    "  while (sp > 0) {\n"
    "    BvhNode node = g_nodes[stack[--sp]];\n"
    "    if (!RayBox(origin, invD, node.mn, node.mx, 0.001, closest))\n"
    "      continue;\n"
    "    if (node.right == 0xFFFFFFFF) {\n"
    "      uint tri = node.left;\n"
    "      float t = 0.0;\n"
    "      if (RayTri(origin, dir, g_verts[tri * 3 + 0],\n"
    "                 g_verts[tri * 3 + 1], g_verts[tri * 3 + 2], t) &&\n"
    "          t < closest) { closest = t; hitTri = (int)tri; }\n"
    "    } else {\n"
    "      stack[sp++] = node.left;\n"
    "      stack[sp++] = node.right;\n"
    "    }\n"
    "  }\n"
    "  float4 col = (hitTri >= 0)\n"
    "      ? g_colors[hitTri]\n"
    "      : float4(0.2, 0.4, 0.8, 1.0);\n"
    "  g_out[tid.xy] = col;\n"
    "}\n";

// ---- CPU BVH builder (median-split over centroids) ----
struct BuildItem {
    ScRtTriangle tri;
    float cx, cy, cz;
    uint32_t index;
};

void TriBounds(const ScRtTriangle& t, float mn[3], float mx[3]) {
    mn[0] = (std::min)((std::min)(t.v0[0], t.v1[0]), t.v2[0]);
    mn[1] = (std::min)((std::min)(t.v0[1], t.v1[1]), t.v2[1]);
    mn[2] = (std::min)((std::min)(t.v0[2], t.v1[2]), t.v2[2]);
    mx[0] = (std::max)((std::max)(t.v0[0], t.v1[0]), t.v2[0]);
    mx[1] = (std::max)((std::max)(t.v0[1], t.v1[1]), t.v2[1]);
    mx[2] = (std::max)((std::max)(t.v0[2], t.v1[2]), t.v2[2]);
}

uint32_t BuildNode(std::vector<BvhNode>& nodes, std::vector<BuildItem>& items,
                   uint32_t begin, uint32_t end) {
    float mn[3] = {1e30f, 1e30f, 1e30f};
    float mx[3] = {-1e30f, -1e30f, -1e30f};
    float cmn[3] = {1e30f, 1e30f, 1e30f};
    float cmx[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = begin; i < end; i++) {
        float tmn[3], tmx[3];
        TriBounds(items[i].tri, tmn, tmx);
        for (int a = 0; a < 3; a++) {
            mn[a] = (std::min)(mn[a], tmn[a]);
            mx[a] = (std::max)(mx[a], tmx[a]);
            float c = (a == 0) ? items[i].cx : (a == 1) ? items[i].cy : items[i].cz;
            cmn[a] = (std::min)(cmn[a], c);
            cmx[a] = (std::max)(cmx[a], c);
        }
    }
    // Flat triangles have zero thickness on one axis; pad the bounds so
    // the slab test never degenerates.
    for (int a = 0; a < 3; a++) {
        mn[a] -= 1e-4f;
        mx[a] += 1e-4f;
    }
    uint32_t nodeIdx = (uint32_t)nodes.size();
    nodes.push_back(BvhNode());
    if (end - begin == 1) {
        BvhNode leaf = {};
        memcpy(leaf.mn, mn, sizeof(mn));
        memcpy(leaf.mx, mx, sizeof(mx));
        leaf.left = items[begin].index;
        leaf.right = kLeafSentinel;
        nodes[nodeIdx] = leaf;
        return nodeIdx;
    }
    // Split along the longest centroid axis at the median.
    int axis = 0;
    float spread = cmx[0] - cmn[0];
    for (int a = 1; a < 3; a++) {
        float s = cmx[a] - cmn[a];
        if (s > spread) {
            spread = s;
            axis = a;
        }
    }
    uint32_t mid = begin + (end - begin) / 2;
    std::nth_element(items.begin() + begin, items.begin() + mid,
                     items.begin() + end,
                     [axis](const BuildItem& a, const BuildItem& b) {
                         float ca = (axis == 0) ? a.cx : (axis == 1) ? a.cy : a.cz;
                         float cb = (axis == 0) ? b.cx : (axis == 1) ? b.cy : b.cz;
                         return ca < cb;
                     });
    uint32_t left = BuildNode(nodes, items, begin, mid);
    uint32_t right = BuildNode(nodes, items, mid, end);
    BvhNode interior = {};
    memcpy(interior.mn, mn, sizeof(mn));
    memcpy(interior.mx, mx, sizeof(mx));
    interior.left = left;
    interior.right = right;
    nodes[nodeIdx] = interior;
    return nodeIdx;
}

ID3D12Resource* MakeUploadBuffer(ID3D12Device* device, uint64_t bytes,
                                 const void* data) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buf = nullptr;
    HRESULT hr = device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&buf));
    if (FAILED(hr)) {
        SrtLog("MakeUploadBuffer %llu bytes hr=0x%08x", bytes, (unsigned)hr);
        return nullptr;
    }
    if (data) {
        void* mapped = nullptr;
        D3D12_RANGE wr = {0, 0};
        if (SUCCEEDED(buf->Map(0, &wr, &mapped)) && mapped) {
            memcpy(mapped, data, (size_t)bytes);
            D3D12_RANGE empty = {0, 0};
            buf->Unmap(0, &empty);
        }
    }
    return buf;
}

}  // namespace

struct ScSoftwareRTScene {
    ID3D12Resource* verts = nullptr;   // 3N float3 (upload heap)
    ID3D12Resource* colors = nullptr;  // N float4 (upload heap)
    ID3D12Resource* nodes = nullptr;   // M BvhNode (upload heap)
    uint32_t triCount = 0;
    uint32_t nodeCount = 0;
};

extern "C" const char* ScSoftwareRT_ModeName(ScRtMode mode) {
    switch (mode) {
        case SC_RT_MODE_OFF: return "off";
        case SC_RT_MODE_HARDWARE: return "hardware";
        case SC_RT_MODE_SOFTWARE:
        default: return "software";
    }
}

extern "C" ScRtMode ScSoftwareRT_GetMode(void) {
    if (g_modeKnown) return g_mode;
    g_modeKnown = true;
    char value[64] = {};
    DWORD n = GetEnvironmentVariableA("SCARLETT_RT_MODE", value,
                                      sizeof(value));
    if (n == 0 || n >= sizeof(value)) {
        // Unset (or unreadably long): the default is software.
        g_mode = SC_RT_MODE_SOFTWARE;
        SrtLog("RT mode: SCARLETT_RT_MODE unset -> software (default)");
        return g_mode;
    }
    // Case-insensitive match; anything else is a typo, not a mode.
    if (_stricmp(value, "off") == 0) {
        g_mode = SC_RT_MODE_OFF;
    } else if (_stricmp(value, "software") == 0) {
        g_mode = SC_RT_MODE_SOFTWARE;
    } else if (_stricmp(value, "hardware") == 0) {
        g_mode = SC_RT_MODE_HARDWARE;
    } else {
        SrtLog("RT mode: SCARLETT_RT_MODE='%s' unrecognized -> software "
               "(want off / software / hardware)",
               value);
        g_mode = SC_RT_MODE_SOFTWARE;
    }
    SrtLog("RT mode selected: %s (SCARLETT_RT_MODE='%s')",
           ScSoftwareRT_ModeName(g_mode), value);
    return g_mode;
}

extern "C" HRESULT ScSoftwareRT_QueryPath(ID3D12Device* device,
                                          ScRtPath* path, uint32_t* rtTier) {
    if (!device || !path || !rtTier) return E_INVALIDARG;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5 = {};
    HRESULT hr = device->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5));
    uint32_t tier = 0;
    if (SUCCEEDED(hr)) tier = (uint32_t)opts5.RaytracingTier;
    *rtTier = tier;
    ScRtMode mode = ScSoftwareRT_GetMode();
    if (mode == SC_RT_MODE_OFF) {
        // RT is off: the path is off regardless of what the adapter
        // could do. The tier is still reported (information only).
        *path = SC_RT_PATH_OFF;
        SrtLog("QueryPath rtTier=%u mode=off -> path=off (RT calls are "
               "no-ops)",
               tier);
    } else if (mode == SC_RT_MODE_SOFTWARE) {
        // Explicit software: the compute path, whatever the tier is.
        *path = SC_RT_PATH_SOFTWARE;
        SrtLog("QueryPath rtTier=%u mode=software -> path=software", tier);
    } else {
        // Hardware asked for: only real when the adapter has a tier.
        // Tier 0 (this card) falls back to software with a clear log
        // line — never a failure.
        if (tier == 0) {
            *path = SC_RT_PATH_SOFTWARE;
            SrtLog("QueryPath rtTier=0 mode=hardware -> FALLBACK to "
                   "software (adapter has no RT cores; hardware DXR "
                   "cannot be verified on this card)");
        } else {
            *path = SC_RT_PATH_HARDWARE;
            SrtLog("QueryPath rtTier=%u mode=hardware -> path=hardware "
                   "(passthrough not yet implemented; DispatchRays will "
                   "return E_NOTIMPL)",
                   tier);
        }
    }
    g_path = *path;
    g_pathKnown = true;
    return S_OK;
}

extern "C" HRESULT ScSoftwareRT_Init(ID3D12Device* device) {
    if (!device) return E_INVALIDARG;
    if (g_pso) return S_OK;  // already initialized (per process)
    if (!g_pathKnown) {
        ScRtPath path = SC_RT_PATH_SOFTWARE;
        uint32_t tier = 0;
        ScSoftwareRT_QueryPath(device, &path, &tier);
    }
    if (g_path == SC_RT_PATH_OFF) {
        // RT is off: there is no PSO to compile. S_OK, nothing built.
        SrtLog("Init off mode: no ray-tracing PSO (RT calls are no-ops)");
        return S_OK;
    }
    if (g_path == SC_RT_PATH_HARDWARE) {
        // Hardware path: the compute PSO is not used (real DXR state
        // objects are the passthrough's job, still future work).
        SrtLog("Init hardware mode: no software PSO (hardware passthrough "
               "not yet implemented)");
        return S_OK;
    }
    // Compile the compute shader at runtime (same technique as the proof
    // guest's triangle shaders).
    ID3DBlob* csBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    HRESULT hr = D3DCompile(kRayTraceHLSL, strlen(kRayTraceHLSL), nullptr,
                            nullptr, nullptr, "CS", "cs_5_0", 0, 0, &csBlob,
                            &errBlob);
    if (FAILED(hr)) {
        const char* msg = errBlob && errBlob->GetBufferPointer()
                              ? (const char*)errBlob->GetBufferPointer()
                              : "(no error text)";
        SrtLog("D3DCompile CS hr=0x%08x: %.200s", (unsigned)hr, msg);
        if (errBlob) errBlob->Release();
        return hr;
    }
    if (errBlob) errBlob->Release();

    // Root signature: [0] 18 root constants (camera, b0), [1] SRV t0
    // (verts), [2] SRV t1 (colors), [3] SRV t2 (BVH nodes), [4] UAV
    // descriptor table u0 (1 descriptor, the output texture).
    D3D12_ROOT_PARAMETER params[5] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 18;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (int i = 1; i <= 3; i++) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[i].Descriptor.ShaderRegister = (UINT)(i - 1);
        params[i].Descriptor.RegisterSpace = 0;
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = 0;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges = &uavRange;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 5;
    rs.pParameters = params;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ID3DBlob* rsBlob = nullptr;
    ID3DBlob* rsErr = nullptr;
    hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                     &rsBlob, &rsErr);
    if (SUCCEEDED(hr)) {
        hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                         rsBlob->GetBufferSize(),
                                         IID_PPV_ARGS(&g_rootSig));
    }
    if (rsBlob) rsBlob->Release();
    if (rsErr) rsErr->Release();
    if (FAILED(hr)) {
        SrtLog("CreateRootSignature hr=0x%08x", (unsigned)hr);
        if (csBlob) csBlob->Release();
        return hr;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = g_rootSig;
    pso.CS.pShaderBytecode = csBlob->GetBufferPointer();
    pso.CS.BytecodeLength = csBlob->GetBufferSize();
    hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pso));
    csBlob->Release();
    if (FAILED(hr)) {
        SrtLog("CreateComputePipelineState hr=0x%08x", (unsigned)hr);
        return hr;
    }

    // Cached 1-slot shader-visible heap for the output UAV descriptor.
    // See the header: the caller fence-waits between dispatches that use
    // different output textures.
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_uavHeap));
    if (FAILED(hr)) {
        SrtLog("UAV heap hr=0x%08x", (unsigned)hr);
        return hr;
    }
    g_uavHeapSize =
        device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    SrtLog("Init OK (path=%s)",
           (g_path == SC_RT_PATH_SOFTWARE) ? "software" : "hardware");
    return S_OK;
}

extern "C" HRESULT ScSoftwareRT_BuildScene(ID3D12Device* device,
                                           const ScRtTriangle* tris,
                                           uint32_t triCount,
                                           ScSoftwareRTSceneHandle* scene) {
    if (!device || !scene) return E_INVALIDARG;
    *scene = nullptr;
    if (!g_pathKnown) {
        ScRtPath path = SC_RT_PATH_SOFTWARE;
        uint32_t tier = 0;
        ScSoftwareRT_QueryPath(device, &path, &tier);
    }
    if (g_path == SC_RT_PATH_OFF) {
        // RT is off: no scene is built; the caller gets a null handle
        // and DispatchRays is a no-op. S_OK, not an error.
        SrtLog("BuildScene off mode: no scene (null handle)");
        return S_OK;
    }
    if (g_path == SC_RT_PATH_HARDWARE) {
        // Real BLAS/TLAS construction is part of the hardware
        // passthrough (future work, unverifiable on this card).
        SrtLog("BuildScene hardware mode: E_NOTIMPL (passthrough not yet "
               "implemented)");
        return E_NOTIMPL;
    }
    if (!tris || triCount == 0) return E_INVALIDARG;
    std::vector<BuildItem> items(triCount);
    std::vector<float> verts(3 * 3 * triCount);
    std::vector<float> colors(4 * triCount);
    for (uint32_t i = 0; i < triCount; i++) {
        items[i].tri = tris[i];
        items[i].cx = (tris[i].v0[0] + tris[i].v1[0] + tris[i].v2[0]) / 3.0f;
        items[i].cy = (tris[i].v0[1] + tris[i].v1[1] + tris[i].v2[1]) / 3.0f;
        items[i].cz = (tris[i].v0[2] + tris[i].v1[2] + tris[i].v2[2]) / 3.0f;
        items[i].index = i;
        memcpy(&verts[i * 9 + 0], tris[i].v0, 3 * sizeof(float));
        memcpy(&verts[i * 9 + 3], tris[i].v1, 3 * sizeof(float));
        memcpy(&verts[i * 9 + 6], tris[i].v2, 3 * sizeof(float));
        memcpy(&colors[i * 4], tris[i].color, 4 * sizeof(float));
    }
    std::vector<BvhNode> nodes;
    nodes.reserve(2 * triCount);
    BuildNode(nodes, items, 0, triCount);
    SrtLog("BuildScene tris=%u bvhNodes=%u (root=0)", triCount,
           (unsigned)nodes.size());

    ScSoftwareRTScene* s = new ScSoftwareRTScene();
    s->triCount = triCount;
    s->nodeCount = (uint32_t)nodes.size();
    s->verts = MakeUploadBuffer(device, (uint64_t)verts.size() * sizeof(float),
                                verts.data());
    s->colors = MakeUploadBuffer(device, (uint64_t)colors.size() * sizeof(float),
                                 colors.data());
    s->nodes = MakeUploadBuffer(device, (uint64_t)nodes.size() * sizeof(BvhNode),
                                nodes.data());
    if (!s->verts || !s->colors || !s->nodes) {
        if (s->verts) s->verts->Release();
        if (s->colors) s->colors->Release();
        if (s->nodes) s->nodes->Release();
        delete s;
        return E_OUTOFMEMORY;
    }
    *scene = s;
    return S_OK;
}

extern "C" void ScSoftwareRT_FreeScene(ScSoftwareRTSceneHandle scene) {
    ScSoftwareRTScene* s = (ScSoftwareRTScene*)scene;
    if (!s) return;
    if (s->verts) s->verts->Release();
    if (s->colors) s->colors->Release();
    if (s->nodes) s->nodes->Release();
    delete s;
}

extern "C" HRESULT ScSoftwareRT_DispatchRays(ID3D12Device* device,
                                             ID3D12GraphicsCommandList* cmdList,
                                             ScSoftwareRTSceneHandle scene,
                                             const ScRtCamera* camera,
                                             ID3D12Resource* outputUav,
                                             uint32_t width, uint32_t height) {
    if (!device || !cmdList) return E_INVALIDARG;
    if (!g_pathKnown) {
        ScRtPath path = SC_RT_PATH_SOFTWARE;
        uint32_t tier = 0;
        ScSoftwareRT_QueryPath(device, &path, &tier);
    }
    if (g_path == SC_RT_PATH_OFF) {
        // RT is off: the dispatch is a no-op that succeeds. The scene
        // may be null (BuildScene returned a null handle); the output
        // texture is left exactly as the rasterizer left it — the
        // title renders with its rasterized lighting only (no RT
        // reflections / GI / shadows). Nothing is recorded, nothing is
        // written.
        SrtLog("DispatchRays off mode: no-op (output left untouched)");
        return S_OK;
    }
    if (g_path == SC_RT_PATH_HARDWARE) {
        // The adapter has real RT cores: hardware DXR passthrough is
        // future work (it cannot be verified on this card). Refuse
        // honestly instead of running the software path where hardware
        // was asked for. This check comes BEFORE the scene null check
        // on purpose: in hardware mode BuildScene returns E_NOTIMPL
        // and no valid scene can ever exist, so a null scene must not
        // mask the path refusal with E_INVALIDARG.
        SrtLog("hardware path requested but passthrough not implemented");
        return E_NOTIMPL;
    }
    if (!scene || !camera || !outputUav || width == 0 || height == 0)
        return E_INVALIDARG;
    if (!g_pso) {
        SrtLog("DispatchRays before Init");
        return E_FAIL;
    }
    ScSoftwareRTScene* s = (ScSoftwareRTScene*)scene;
    // Camera as 18 root constants (b0): float3s start a 16-byte row, so
    // dwords 3, 7, 11, 15 are padding; width/height are dwords 16, 17.
    float constants[18] = {};
    constants[0] = camera->origin[0];
    constants[1] = camera->origin[1];
    constants[2] = camera->origin[2];
    constants[4] = camera->lowerLeft[0];
    constants[5] = camera->lowerLeft[1];
    constants[6] = camera->lowerLeft[2];
    constants[8] = camera->horizontal[0];
    constants[9] = camera->horizontal[1];
    constants[10] = camera->horizontal[2];
    constants[12] = camera->vertical[0];
    constants[13] = camera->vertical[1];
    constants[14] = camera->vertical[2];
    constants[16] = (float)width;
    constants[17] = (float)height;

    // Output UAV descriptor into the cached heap slot (caller must
    // fence-wait before a later dispatch reuses the slot).
    D3D12_CPU_DESCRIPTOR_HANDLE cpuH =
        g_uavHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateUnorderedAccessView(outputUav, nullptr, nullptr, cpuH);
    ID3D12DescriptorHeap* heaps[] = {g_uavHeap};

    cmdList->SetComputeRootSignature(g_rootSig);
    cmdList->SetPipelineState(g_pso);
    cmdList->SetComputeRoot32BitConstants(0, 18, constants, 0);
    cmdList->SetComputeRootShaderResourceView(1, s->verts->GetGPUVirtualAddress());
    cmdList->SetComputeRootShaderResourceView(2, s->colors->GetGPUVirtualAddress());
    cmdList->SetComputeRootShaderResourceView(3, s->nodes->GetGPUVirtualAddress());
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootDescriptorTable(
        4, g_uavHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    SrtLog("DispatchRays %ux%u scene tris=%u nodes=%u", width, height,
           s->triCount, s->nodeCount);
    return S_OK;
}
