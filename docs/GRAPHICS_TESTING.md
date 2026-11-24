# Project Miami — Native Graphics Validation & Testing

## 1. Overview
The native graphics subsystem includes an automated test executable: `out/test_graphics.exe`.
This test suite executes in native Windows console mode, initializes hardware D3D12 devices, compiles real HLSL shaders, submits draw commands, presents swap chain frames, and executes GPU readback assertions.

---

## 2. Test Scenes Executed

| Scene | Category | Description | Verification Method | Status |
|---|---|---|---|---|
| **Scene 1** | Adapter Management | Discrete GPU discovery and capabilities | Probes `NVIDIA GeForce RTX 3050` (6002 MB VRAM, WaveOps, DXR, Mesh) | **PASS** |
| **Scene 2** | Shader Toolchain | Real HLSL shader compilation | Compiles `basic.hlsl` (VSMain, PSColor, PSTexture) and tests negative syntax error | **PASS** |
| **Scene 3** | Buffers & Resources | GPU Buffer Allocation | Allocates Vertex, Index, Frame Constant, and Object Constant buffers | **PASS** |
| **Scene 4** | Texture Subresources | 2D Texture Creation & Upload | Generates 4x4 checkerboard texture, copies via upload buffer with footprint pitch | **PASS** |
| **Scene 5** | Pipeline State | D3D12 Pipeline Construction | Creates Color PSO, Textured PSO, and Depth-enabled PSO | **PASS** |
| **Scene 6** | Render Target & Readback | Off-Screen Render & Readback | Draws triangle onto 64x64 offscreen RT, reads back pixel (0,0), matches clear color | **PASS** |
| **Scene 7** | Presentation | Swap Chain Presentation & Resize | Presents 3 frames to Win32 HWND, executes live resize from 640x480 to 800x600 | **PASS** |
| **Scene 8** | Runtime Bridge | High-Level Engine Boundary | Validates `IGraphicsRuntimeBridge` frame lifecycle, timing stats & memory tracking | **PASS** |

---

## 3. GPU Readback Validation Details
In **Scene 6 (Off-Screen Render Target & GPU Pixel Readback)**:
1. An offscreen 64x64 texture with format `R8G8B8A8_UNORM` is allocated with `ALLOW_RENDER_TARGET`.
2. A companion 64x64 texture with format `D32_FLOAT` is allocated with `ALLOW_DEPTH_STENCIL`.
3. The render target is cleared to cobalt blue: `RGBA(0.1, 0.2, 0.4, 1.0)`.
4. A draw call is issued with `vBuffer` and `iBuffer`.
5. The texture is transitioned from `RENDER_TARGET` to `COPY_SOURCE`.
6. A readback buffer allocated in `D3D12_HEAP_TYPE_READBACK` receives the data via `CopyTextureRegion`.
7. The fence is signaled and synchronized with the CPU.
8. The readback buffer is mapped and sampled at pixel `(0, 0)`.
9. The read values are: `R=25, G=51, B=102, A=255`, perfectly matching expected IEEE clear values.
