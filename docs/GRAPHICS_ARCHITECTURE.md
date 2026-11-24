# Project Miami — Native Graphics Architecture

## 1. Selected Native API: Direct3D 12
Following a comprehensive audit of Project Miami's desktop requirements, **Direct3D 12 (D3D12)** was chosen as the primary native graphics backend:
- **Windows-First Ecosystem**: Seamlessly integrates with DXGI swap chains, Windows 11 window managers, and DirectComposition.
- **Hardware Feature Alignment**: Directly exposes physical GPU capabilities on modern PC hardware (e.g. WaveOps/Wave32/Wave64, DXR Tier 1.1 raytracing, Mesh Shaders) without cross-platform translation layers.
- **Architectural Synergies with XENITH & Leonida**:
  - Leonida's root signature contract (`RootSignature`, `UserSGPR`, `D3D12_XS` descriptor sanitation) maps 1:1 to Direct3D 12 Root Signatures (`ID3D12RootSignature`).
  - GDK/Scarlett compatibility shims (`d3d12_xs.dll`) target D3D12 command lists.
- **Clean Backend Decoupling**: Designed behind an abstract graphics device interface (`IGraphicsDevice`), enabling an alternate backend (such as Vulkan) to be introduced in the future with zero changes to caller code.

---

## 2. Core Architecture & Subsystem Separation

The graphics subsystem is structured into modular layers completely separated from the launcher and game files:

```
miami::gfx
├── Types & Descriptors (include/graphics/types.h)
│   ├── Enums: GraphicsApi, Format, BufferUsage, TextureUsage, PrimitiveTopology
│   └── Descriptors: BufferDesc, TextureDesc, PipelineDesc, SwapChainDesc, RenderPassDesc
│
├── Device & Context Abstraction (include/graphics/device.h)
│   ├── IGraphicsDevice (Adapter selection, factory for resources/pipelines/swapchains)
│   ├── IGraphicsContext (Command list recording, state transitions, draw dispatch, readback)
│   ├── IBuffer (Vertex, Index, Constant, Upload, Readback)
│   ├── ITexture (2D/3D, Render Target, Depth/Stencil, Shader Resource)
│   ├── IPipeline (PSO encapsulation of VS/PS bytecode, rasterizer, blend, depth states)
│   └── ISwapChain (HWND window presentation, flip-discard backbuffers, resize handling)
│
├── Hardware Backend: Direct3D 12 (src/graphics/d3d12/)
│   ├── D3D12GraphicsDevice (Adapter enumeration, feature probing, default root signature)
│   ├── D3D12GraphicsContext (Direct command allocator & list, RTV/DSV/SRV descriptor heaps)
│   ├── D3D12Buffer & D3D12Texture (Committed GPU heaps, resource barriers, CPU mapping)
│   ├── D3D12Pipeline (ID3D12PipelineState compilation)
│   └── D3D12SwapChain (IDXGISwapChain3 flip model with fence synchronization)
│
├── Diagnostics & Shader Compiler (src/graphics/)
│   ├── ShaderCompiler (Real HLSL vs_5_1 / ps_5_1 compilation via D3DCompile)
│   └── GraphicsDiagnostics (CPU frame duration, GPU execution time, memory tracking)
│
└── Runtime Integration Interface (include/graphics/runtime_bridge.h)
    └── IGraphicsRuntimeBridge (Abstract bridge exposing device, context, swap chain, and frame loop)
```

---

## 3. GPU Resource Lifecycle & State Transitions

1. **Explicit Memory Allocation**:
   - `CreateBuffer()`: Allocates `D3D12_HEAP_TYPE_DEFAULT`, `UPLOAD`, or `READBACK` committed memory.
   - `CreateTexture()`: Allocates 2D textures with usage flags (`ALLOW_RENDER_TARGET`, `ALLOW_DEPTH_STENCIL`).
2. **Subresource Uploads**:
   - `UploadSubresource()`: Automatically manages pitch alignment using `GetCopyableFootprints` and transitions textures from `COPY_DEST` to `PIXEL_SHADER_RESOURCE`.
3. **State Management**:
   - Context tracks and automatically inserts `ID3D12GraphicsCommandList::ResourceBarrier` when binding render targets, depth buffers, shader resources, and readback sources.

---

## 4. Frame Management & Presentation

- **Presentation Model**: Uses `DXGI_SWAP_EFFECT_FLIP_DISCARD` on `IDXGISwapChain3` with 2 back buffers.
- **Synchronization**: Managed through an `ID3D12Fence` and win32 event to prevent CPU/GPU racing without frame stuttering.
- **Dynamic Resize**: Supports seamless window client area resizing (`ResizeBuffers`) with backbuffer re-acquisition.

---

## 5. Standalone Validation & Readback Testing

Project Miami provides a dedicated test suite in [`tests/test_graphics.cpp`](file:///e:/ProjectMiami/tests/test_graphics.cpp), validating all 8 required test scenes:
- **Scene A**: Clear and present on an active Win32 window.
- **Scene B**: Geometry rendering with vertex & index buffers.
- **Scene C**: Textured quad rendering with 2D checkerboard subresource upload.
- **Scene D**: Multiple draw calls with uniform tint/world constants.
- **Scene E**: Depth stencil allocation and depth-buffered rendering.
- **Scene F & H**: Off-screen render-to-texture and GPU memory readback verifying pixel color values.
- **Scene G**: Window resizing and swap chain re-allocation.
