#pragma once
#include "graphics/types.h"

namespace miami::gfx {

class IBuffer {
public:
    virtual ~IBuffer() = default;
    virtual const BufferDesc& GetDesc() const = 0;
    virtual bool UpdateData(const void* data, uint64_t sizeBytes, uint64_t offsetBytes = 0) = 0;
    virtual void* Map() = 0;
    virtual void Unmap() = 0;
};

class ITexture {
public:
    virtual ~ITexture() = default;
    virtual const TextureDesc& GetDesc() const = 0;
    virtual bool UploadSubresource(const void* srcData, uint32_t rowPitch, uint32_t slicePitch) = 0;
};

class IPipeline {
public:
    virtual ~IPipeline() = default;
    virtual const PipelineDesc& GetDesc() const = 0;
};

class ISwapChain {
public:
    virtual ~ISwapChain() = default;
    virtual const SwapChainDesc& GetDesc() const = 0;
    virtual bool Resize(uint32_t width, uint32_t height) = 0;
    virtual bool Present(bool vsync) = 0;
    virtual ITexture* GetCurrentBackBuffer() = 0;
    virtual uint32_t GetCurrentBackBufferIndex() const = 0;
};

class IGraphicsContext {
public:
    virtual ~IGraphicsContext() = default;

    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;

    virtual void BeginRenderPass(const RenderPassDesc& pass) = 0;
    virtual void EndRenderPass() = 0;

    virtual void SetViewport(const Viewport& vp) = 0;
    virtual void SetScissorRect(const ScissorRect& sc) = 0;
    virtual void SetPipeline(IPipeline* pipeline) = 0;

    virtual void SetVertexBuffer(IBuffer* buffer, uint32_t slot = 0, uint32_t offset = 0) = 0;
    virtual void SetIndexBuffer(IBuffer* buffer, uint32_t offset = 0, bool is32Bit = false) = 0;
    virtual void SetConstantBuffer(uint32_t rootIndex, IBuffer* buffer) = 0;
    virtual void SetTexture(uint32_t rootIndex, ITexture* texture) = 0;

    virtual void Draw(uint32_t vertexCount, uint32_t startVertexLocation = 0) = 0;
    virtual void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation = 0, int32_t baseVertexLocation = 0) = 0;

    // Readback helpers for automated test assertions
    virtual bool CopyTextureToBuffer(ITexture* srcTexture, IBuffer* dstBuffer) = 0;
};

class IGraphicsDevice {
public:
    virtual ~IGraphicsDevice() = default;

    virtual bool Initialize(bool enableDebugLayer = true) = 0;
    virtual void Shutdown() = 0;

    virtual const DeviceInfo& GetDeviceInfo() const = 0;
    virtual const GpuMemoryStats& GetMemoryStats() const = 0;

    virtual std::shared_ptr<ISwapChain> CreateSwapChain(const SwapChainDesc& desc) = 0;
    virtual std::shared_ptr<IBuffer> CreateBuffer(const BufferDesc& desc, const void* initialData = nullptr) = 0;
    virtual std::shared_ptr<ITexture> CreateTexture(const TextureDesc& desc, const void* initialData = nullptr) = 0;
    virtual std::shared_ptr<IPipeline> CreatePipeline(const PipelineDesc& desc) = 0;

    virtual IGraphicsContext* GetImmediateContext() = 0;
    virtual void WaitForGpuIdle() = 0;
};

// Factory entry
std::unique_ptr<IGraphicsDevice> CreateNativeGraphicsDevice(GraphicsApi api = GraphicsApi::Direct3D12);

} // namespace miami::gfx
