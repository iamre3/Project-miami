#pragma once
#include "graphics/device.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vector>
#include <string>

using Microsoft::WRL::ComPtr;

namespace miami::gfx::d3d12 {

DXGI_FORMAT ConvertFormatToDxgi(Format fmt);
Format ConvertDxgiToFormat(DXGI_FORMAT fmt);

class D3D12Buffer : public IBuffer {
public:
    D3D12Buffer(ID3D12Device* device, const BufferDesc& desc, const void* initialData = nullptr);
    ~D3D12Buffer() override;

    const BufferDesc& GetDesc() const override { return m_desc; }
    bool UpdateData(const void* data, uint64_t sizeBytes, uint64_t offsetBytes = 0) override;
    void* Map() override;
    void Unmap() override;

    ID3D12Resource* GetResource() const { return m_resource.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GetGpuAddress() const { return m_resource ? m_resource->GetGPUVirtualAddress() : 0; }
    D3D12_RESOURCE_STATES GetCurrentState() const { return m_currentState; }
    void SetCurrentState(D3D12_RESOURCE_STATES s) { m_currentState = s; }

private:
    BufferDesc m_desc;
    ComPtr<ID3D12Resource> m_resource;
    D3D12_RESOURCE_STATES m_currentState = D3D12_RESOURCE_STATE_COMMON;
    void* m_mappedCpuPtr = nullptr;
};

class D3D12Texture : public ITexture {
public:
    D3D12Texture(ID3D12Device* device, ID3D12CommandQueue* queue, const TextureDesc& desc, const void* initialData = nullptr);
    D3D12Texture(const TextureDesc& desc, ComPtr<ID3D12Resource> existingResource, D3D12_RESOURCE_STATES state);
    ~D3D12Texture() override;

    const TextureDesc& GetDesc() const override { return m_desc; }
    bool UploadSubresource(const void* srcData, uint32_t rowPitch, uint32_t slicePitch) override;

    ID3D12Resource* GetResource() const { return m_resource.Get(); }
    D3D12_RESOURCE_STATES GetCurrentState() const { return m_currentState; }
    void SetCurrentState(D3D12_RESOURCE_STATES s) { m_currentState = s; }

private:
    ID3D12Device* m_pDevice = nullptr;
    ID3D12CommandQueue* m_pQueue = nullptr;
    TextureDesc m_desc;
    ComPtr<ID3D12Resource> m_resource;
    D3D12_RESOURCE_STATES m_currentState = D3D12_RESOURCE_STATE_COMMON;
};

class D3D12Pipeline : public IPipeline {
public:
    D3D12Pipeline(ID3D12Device* device, ID3D12RootSignature* rootSig, const PipelineDesc& desc);
    ~D3D12Pipeline() override = default;

    const PipelineDesc& GetDesc() const override { return m_desc; }
    ID3D12PipelineState* GetPipelineState() const { return m_pipelineState.Get(); }
    ID3D12RootSignature* GetRootSignature() const { return m_rootSignature; }

private:
    PipelineDesc m_desc;
    ID3D12RootSignature* m_rootSignature = nullptr;
    ComPtr<ID3D12PipelineState> m_pipelineState;
};

class D3D12SwapChain : public ISwapChain {
public:
    D3D12SwapChain(ID3D12Device* device, IDXGIFactory4* factory, ID3D12CommandQueue* queue, const SwapChainDesc& desc);
    ~D3D12SwapChain() override;

    const SwapChainDesc& GetDesc() const override { return m_desc; }
    bool Resize(uint32_t width, uint32_t height) override;
    bool Present(bool vsync) override;
    ITexture* GetCurrentBackBuffer() override;
    uint32_t GetCurrentBackBufferIndex() const override { return m_backBufferIndex; }

    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRtvHandle() const;

private:
    void CreateRtvHeapAndViews();

    ID3D12Device* m_pDevice = nullptr;
    ID3D12CommandQueue* m_pQueue = nullptr;
    SwapChainDesc m_desc;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    uint32_t m_rtvDescriptorSize = 0;
    std::vector<std::shared_ptr<D3D12Texture>> m_backBuffers;
    uint32_t m_backBufferIndex = 0;
};

class D3D12GraphicsContext : public IGraphicsContext {
public:
    D3D12GraphicsContext(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12RootSignature* rootSig);
    ~D3D12GraphicsContext() override;

    void BeginFrame() override;
    void EndFrame() override;

    void BeginRenderPass(const RenderPassDesc& pass) override;
    void EndRenderPass() override;

    void SetViewport(const Viewport& vp) override;
    void SetScissorRect(const ScissorRect& sc) override;
    void SetPipeline(IPipeline* pipeline) override;

    void SetVertexBuffer(IBuffer* buffer, uint32_t slot = 0, uint32_t offset = 0) override;
    void SetIndexBuffer(IBuffer* buffer, uint32_t offset = 0, bool is32Bit = false) override;
    void SetConstantBuffer(uint32_t rootIndex, IBuffer* buffer) override;
    void SetTexture(uint32_t rootIndex, ITexture* texture) override;

    void Draw(uint32_t vertexCount, uint32_t startVertexLocation = 0) override;
    void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation = 0, int32_t baseVertexLocation = 0) override;

    bool CopyTextureToBuffer(ITexture* srcTexture, IBuffer* dstBuffer) override;

    void TransitionResource(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    ID3D12GraphicsCommandList* GetCommandList() const { return m_cmdList.Get(); }

private:
    ID3D12Device* m_pDevice = nullptr;
    ID3D12CommandQueue* m_pQueue = nullptr;
    ID3D12RootSignature* m_pRootSig = nullptr;

    ComPtr<ID3D12CommandAllocator> m_cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> m_cmdList;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    uint32_t m_srvDescriptorSize = 0;

    RenderPassDesc m_currentPass;
    bool m_inRenderPass = false;
};

class D3D12GraphicsDevice : public IGraphicsDevice {
public:
    D3D12GraphicsDevice();
    ~D3D12GraphicsDevice() override;

    bool Initialize(bool enableDebugLayer = true) override;
    void Shutdown() override;

    const DeviceInfo& GetDeviceInfo() const override { return m_deviceInfo; }
    const GpuMemoryStats& GetMemoryStats() const override { return m_memStats; }

    std::shared_ptr<ISwapChain> CreateSwapChain(const SwapChainDesc& desc) override;
    std::shared_ptr<IBuffer> CreateBuffer(const BufferDesc& desc, const void* initialData = nullptr) override;
    std::shared_ptr<ITexture> CreateTexture(const TextureDesc& desc, const void* initialData = nullptr) override;
    std::shared_ptr<IPipeline> CreatePipeline(const PipelineDesc& desc) override;

    IGraphicsContext* GetImmediateContext() override { return m_context.get(); }
    void WaitForGpuIdle() override;

    ID3D12Device* GetDevice() const { return m_device.Get(); }
    ID3D12CommandQueue* GetQueue() const { return m_commandQueue.Get(); }
    ID3D12RootSignature* GetDefaultRootSignature() const { return m_defaultRootSignature.Get(); }

private:
    void CreateDefaultRootSignature();

    DeviceInfo m_deviceInfo;
    GpuMemoryStats m_memStats;

    ComPtr<IDXGIFactory4> m_dxgiFactory;
    ComPtr<IDXGIAdapter1> m_adapter;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;

    ComPtr<ID3D12RootSignature> m_defaultRootSignature;
    std::unique_ptr<D3D12GraphicsContext> m_context;
};

} // namespace miami::gfx::d3d12
