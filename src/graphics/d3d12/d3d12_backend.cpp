#include "graphics/d3d12/d3d12_backend.h"
#include "graphics/diagnostics.h"
#include <d3dcompiler.h>
#include <dxgidebug.h>
#include <cstdio>
#include <stdexcept>
#include <algorithm>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

namespace miami::gfx {

std::unique_ptr<IGraphicsDevice> CreateNativeGraphicsDevice(GraphicsApi api) {
    if (api == GraphicsApi::Direct3D12) {
        return std::make_unique<d3d12::D3D12GraphicsDevice>();
    }
    return nullptr;
}

} // namespace miami::gfx

namespace miami::gfx::d3d12 {

DXGI_FORMAT ConvertFormatToDxgi(Format fmt) {
    switch (fmt) {
        case Format::R8G8B8A8_UNORM:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::B8G8R8A8_UNORM:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::R8G8B8A8_UINT:      return DXGI_FORMAT_R8G8B8A8_UINT;
        case Format::R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::R32G32B32_FLOAT:    return DXGI_FORMAT_R32G32B32_FLOAT;
        case Format::R32G32_FLOAT:       return DXGI_FORMAT_R32G32_FLOAT;
        case Format::R32_FLOAT:          return DXGI_FORMAT_R32_FLOAT;
        case Format::D32_FLOAT:          return DXGI_FORMAT_D32_FLOAT;
        case Format::D24_UNORM_S8_UINT:  return DXGI_FORMAT_D24_UNORM_S8_UINT;
        default:                         return DXGI_FORMAT_UNKNOWN;
    }
}

Format ConvertDxgiToFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:     return Format::R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM:     return Format::B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UINT:      return Format::R8G8B8A8_UINT;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return Format::R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return Format::R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32_FLOAT:       return Format::R32G32_FLOAT;
        case DXGI_FORMAT_R32_FLOAT:          return Format::R32_FLOAT;
        case DXGI_FORMAT_D32_FLOAT:          return Format::D32_FLOAT;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:  return Format::D24_UNORM_S8_UINT;
        default:                             return Format::Unknown;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// D3D12Buffer Implementation
// ─────────────────────────────────────────────────────────────────────────────

D3D12Buffer::D3D12Buffer(ID3D12Device* device, const BufferDesc& desc, const void* initialData)
    : m_desc(desc) {
    
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = desc.cpuVisible ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
    if (desc.usage == BufferUsage::Readback) {
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;
    }

    D3D12_RESOURCE_DESC rDesc = {};
    rDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rDesc.Alignment = 0;
    rDesc.Width = desc.sizeBytes;
    rDesc.Height = 1;
    rDesc.DepthOrArraySize = 1;
    rDesc.MipLevels = 1;
    rDesc.Format = DXGI_FORMAT_UNKNOWN;
    rDesc.SampleDesc.Count = 1;
    rDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    if (desc.usage == BufferUsage::Readback) {
        m_currentState = D3D12_RESOURCE_STATE_COPY_DEST;
    } else if (desc.cpuVisible) {
        m_currentState = D3D12_RESOURCE_STATE_GENERIC_READ;
    } else {
        m_currentState = D3D12_RESOURCE_STATE_COMMON;
    }

    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &rDesc,
        m_currentState,
        nullptr,
        IID_PPV_ARGS(&m_resource)
    );

    if (FAILED(hr)) {
        GraphicsDiagnostics::Instance().Log(DiagnosticLogEntry::Level::Error, "D3D12Buffer", "CreateCommittedResource failed");
        return;
    }

    if (!desc.debugName.empty()) {
        std::wstring wname(desc.debugName.begin(), desc.debugName.end());
        m_resource->SetName(wname.c_str());
    }

    if (initialData && desc.sizeBytes > 0) {
        UpdateData(initialData, desc.sizeBytes, 0);
    }
}

D3D12Buffer::~D3D12Buffer() {
    Unmap();
}

void* D3D12Buffer::Map() {
    if (!m_resource) return nullptr;
    if (m_mappedCpuPtr) return m_mappedCpuPtr;

    D3D12_RANGE readRange = { 0, 0 };
    if (m_desc.usage == BufferUsage::Readback) {
        readRange.End = static_cast<SIZE_T>(m_desc.sizeBytes);
    }
    HRESULT hr = m_resource->Map(0, &readRange, &m_mappedCpuPtr);
    if (FAILED(hr)) {
        m_mappedCpuPtr = nullptr;
    }
    return m_mappedCpuPtr;
}

void D3D12Buffer::Unmap() {
    if (m_resource && m_mappedCpuPtr) {
        D3D12_RANGE writtenRange = { 0, static_cast<SIZE_T>(m_desc.sizeBytes) };
        m_resource->Unmap(0, &writtenRange);
        m_mappedCpuPtr = nullptr;
    }
}

bool D3D12Buffer::UpdateData(const void* data, uint64_t sizeBytes, uint64_t offsetBytes) {
    if (!m_resource || !data) return false;
    if (offsetBytes + sizeBytes > m_desc.sizeBytes) return false;

    if (m_desc.cpuVisible || m_desc.usage == BufferUsage::Readback) {
        void* ptr = Map();
        if (!ptr) return false;
        memcpy(static_cast<uint8_t*>(ptr) + offsetBytes, data, static_cast<size_t>(sizeBytes));
        Unmap();
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// D3D12Texture Implementation
// ─────────────────────────────────────────────────────────────────────────────

D3D12Texture::D3D12Texture(ID3D12Device* device, ID3D12CommandQueue* queue, const TextureDesc& desc, const void* initialData)
    : m_pDevice(device), m_pQueue(queue), m_desc(desc) {

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rDesc = {};
    rDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rDesc.Alignment = 0;
    rDesc.Width = desc.width;
    rDesc.Height = desc.height;
    rDesc.DepthOrArraySize = static_cast<UINT16>(desc.depth);
    rDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
    rDesc.Format = ConvertFormatToDxgi(desc.format);
    rDesc.SampleDesc.Count = 1;
    rDesc.SampleDesc.Quality = 0;
    rDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_CLEAR_VALUE clearVal = {};
    clearVal.Format = rDesc.Format;
    D3D12_CLEAR_VALUE* pClearVal = nullptr;

    if (desc.usage == TextureUsage::RenderTarget) {
        rDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        clearVal.Color[0] = 0.0f; clearVal.Color[1] = 0.0f; clearVal.Color[2] = 0.0f; clearVal.Color[3] = 1.0f;
        pClearVal = &clearVal;
        m_currentState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    } else if (desc.usage == TextureUsage::DepthStencil) {
        rDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        clearVal.DepthStencil.Depth = 1.0f;
        clearVal.DepthStencil.Stencil = 0;
        pClearVal = &clearVal;
        m_currentState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else {
        m_currentState = D3D12_RESOURCE_STATE_COMMON;
    }

    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &rDesc,
        m_currentState,
        pClearVal,
        IID_PPV_ARGS(&m_resource)
    );

    if (FAILED(hr)) {
        GraphicsDiagnostics::Instance().Log(DiagnosticLogEntry::Level::Error, "D3D12Texture", "CreateCommittedResource texture failed");
        return;
    }

    if (!desc.debugName.empty()) {
        std::wstring wname(desc.debugName.begin(), desc.debugName.end());
        m_resource->SetName(wname.c_str());
    }

    if (initialData) {
        uint32_t bpp = 4;
        UploadSubresource(initialData, desc.width * bpp, desc.width * desc.height * bpp);
    }
}

D3D12Texture::D3D12Texture(const TextureDesc& desc, ComPtr<ID3D12Resource> existingResource, D3D12_RESOURCE_STATES state)
    : m_desc(desc), m_resource(existingResource), m_currentState(state) {
}

D3D12Texture::~D3D12Texture() = default;

bool D3D12Texture::UploadSubresource(const void* srcData, uint32_t rowPitch, uint32_t slicePitch) {
    if (!m_resource || !m_pDevice || !m_pQueue || !srcData) return false;

    D3D12_RESOURCE_DESC desc = m_resource->GetDesc();
    UINT64 uploadBufferSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;

    m_pDevice->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);
    uploadBufferSize = totalBytes;

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = uploadBufferSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> uploadBuffer;
    HRESULT hr = m_pDevice->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer)
    );
    if (FAILED(hr)) return false;

    // Copy CPU memory to uploadBuffer with footprint pitch alignment
    uint8_t* pMapped = nullptr;
    uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&pMapped));
    const uint8_t* pSrc = static_cast<const uint8_t*>(srcData);
    for (UINT y = 0; y < numRows; ++y) {
        memcpy(pMapped + footprint.Offset + (y * footprint.Footprint.RowPitch),
               pSrc + (y * rowPitch),
               static_cast<size_t>(std::min<UINT64>(rowPitch, rowSizeInBytes)));
    }
    uploadBuffer->Unmap(0, nullptr);

    // Record copy command
    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList));

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_resource.Get();
    barrier.Transition.StateBefore = m_currentState;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = m_resource.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = uploadBuffer.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmdList->ResourceBarrier(1, &barrier);
    m_currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    cmdList->Close();
    ID3D12CommandList* ppCmds[] = { cmdList.Get() };
    m_pQueue->ExecuteCommandLists(1, ppCmds);

    // Wait on fence
    ComPtr<ID3D12Fence> fence;
    m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_pQueue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, hEvent);
    WaitForSingleObject(hEvent, INFINITE);
    CloseHandle(hEvent);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// D3D12Pipeline Implementation
// ─────────────────────────────────────────────────────────────────────────────

D3D12Pipeline::D3D12Pipeline(ID3D12Device* device, ID3D12RootSignature* rootSig, const PipelineDesc& desc)
    : m_desc(desc), m_rootSignature(rootSig) {

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSig;

    if (!desc.vertexShaderBytecode.empty()) {
        psoDesc.VS.pShaderBytecode = desc.vertexShaderBytecode.data();
        psoDesc.VS.BytecodeLength = desc.vertexShaderBytecode.size();
    }
    if (!desc.pixelShaderBytecode.empty()) {
        psoDesc.PS.pShaderBytecode = desc.pixelShaderBytecode.data();
        psoDesc.PS.BytecodeLength = desc.pixelShaderBytecode.size();
    }

    // Rasterizer
    psoDesc.RasterizerState.FillMode = (desc.fillMode == FillMode::Wireframe) ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = (desc.cullMode == CullMode::Back) ? D3D12_CULL_MODE_BACK :
                                       ((desc.cullMode == CullMode::Front) ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_NONE);
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    // Blend State
    psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Depth Stencil State
    psoDesc.DepthStencilState.DepthEnable = desc.depthEnable ? TRUE : FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = desc.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    // Input Layout
    std::vector<D3D12_INPUT_ELEMENT_DESC> d3dLayout;
    for (const auto& elem : desc.inputLayout) {
        D3D12_INPUT_ELEMENT_DESC d = {};
        d.SemanticName = elem.semanticName.c_str();
        d.SemanticIndex = elem.semanticIndex;
        d.Format = ConvertFormatToDxgi(elem.format);
        d.InputSlot = elem.inputSlot;
        d.AlignedByteOffset = elem.alignedByteOffset;
        d.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        d.InstanceDataStepRate = 0;
        d3dLayout.push_back(d);
    }
    psoDesc.InputLayout.pInputElementDescs = d3dLayout.data();
    psoDesc.InputLayout.NumElements = static_cast<UINT>(d3dLayout.size());

    // Topology
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if (desc.topology == PrimitiveTopology::LineList) {
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    } else if (desc.topology == PrimitiveTopology::PointList) {
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    }

    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = ConvertFormatToDxgi(desc.renderTargetFormat);
    if (desc.depthStencilFormat != Format::Unknown) {
        psoDesc.DSVFormat = ConvertFormatToDxgi(desc.depthStencilFormat);
    }
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;

    HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState));
    if (FAILED(hr)) {
        GraphicsDiagnostics::Instance().Log(DiagnosticLogEntry::Level::Error, "D3D12Pipeline", "CreateGraphicsPipelineState failed");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// D3D12SwapChain Implementation
// ─────────────────────────────────────────────────────────────────────────────

D3D12SwapChain::D3D12SwapChain(ID3D12Device* device, IDXGIFactory4* factory, ID3D12CommandQueue* queue, const SwapChainDesc& desc)
    : m_pDevice(device), m_pQueue(queue), m_desc(desc) {

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width = desc.width;
    scDesc.Height = desc.height;
    scDesc.Format = ConvertFormatToDxgi(desc.format);
    scDesc.Stereo = FALSE;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = desc.bufferCount;
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    scDesc.Flags = 0;

    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = factory->CreateSwapChainForHwnd(
        queue,
        static_cast<HWND>(desc.windowHandle),
        &scDesc,
        nullptr,
        nullptr,
        &sc1
    );

    if (SUCCEEDED(hr)) {
        sc1.As(&m_swapChain);
        CreateRtvHeapAndViews();
    } else {
        GraphicsDiagnostics::Instance().Log(DiagnosticLogEntry::Level::Error, "D3D12SwapChain", "CreateSwapChainForHwnd failed");
    }
}

D3D12SwapChain::~D3D12SwapChain() = default;

void D3D12SwapChain::CreateRtvHeapAndViews() {
    m_backBuffers.clear();

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = m_desc.bufferCount;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    m_pDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap));
    m_rtvDescriptorSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t i = 0; i < m_desc.bufferCount; ++i) {
        ComPtr<ID3D12Resource> backBuf;
        m_swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuf));
        m_pDevice->CreateRenderTargetView(backBuf.Get(), nullptr, rtvHandle);

        TextureDesc tDesc;
        tDesc.width = m_desc.width;
        tDesc.height = m_desc.height;
        tDesc.format = m_desc.format;
        tDesc.usage = TextureUsage::RenderTarget;
        tDesc.debugName = "SwapChainBackBuffer_" + std::to_string(i);

        m_backBuffers.push_back(std::make_shared<D3D12Texture>(tDesc, backBuf, D3D12_RESOURCE_STATE_PRESENT));
        rtvHandle.ptr += m_rtvDescriptorSize;
    }
    m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
}

bool D3D12SwapChain::Resize(uint32_t width, uint32_t height) {
    if (!m_swapChain || (width == 0 || height == 0)) return false;

    m_backBuffers.clear();
    m_rtvHeap.Reset();

    HRESULT hr = m_swapChain->ResizeBuffers(
        m_desc.bufferCount,
        width,
        height,
        ConvertFormatToDxgi(m_desc.format),
        0
    );

    if (SUCCEEDED(hr)) {
        m_desc.width = width;
        m_desc.height = height;
        CreateRtvHeapAndViews();
        return true;
    }
    return false;
}

bool D3D12SwapChain::Present(bool vsync) {
    if (!m_swapChain) return false;
    HRESULT hr = m_swapChain->Present(vsync ? 1 : 0, 0);
    m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    return SUCCEEDED(hr);
}

ITexture* D3D12SwapChain::GetCurrentBackBuffer() {
    if (m_backBufferIndex < m_backBuffers.size()) {
        return m_backBuffers[m_backBufferIndex].get();
    }
    return nullptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12SwapChain::GetCurrentRtvHandle() const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (m_backBufferIndex * m_rtvDescriptorSize);
    return handle;
}

// ─────────────────────────────────────────────────────────────────────────────
// D3D12GraphicsContext Implementation
// ─────────────────────────────────────────────────────────────────────────────

D3D12GraphicsContext::D3D12GraphicsContext(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12RootSignature* rootSig)
    : m_pDevice(device), m_pQueue(queue), m_pRootSig(rootSig) {

    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAlloc));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&m_cmdList));
    m_cmdList->Close();

    // Descriptor heap for SRV
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 128;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap));
    m_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // DSV heap
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.NumDescriptors = 16;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap));
}

D3D12GraphicsContext::~D3D12GraphicsContext() = default;

void D3D12GraphicsContext::BeginFrame() {
    m_cmdAlloc->Reset();
    m_cmdList->Reset(m_cmdAlloc.Get(), nullptr);

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);
    m_cmdList->SetGraphicsRootSignature(m_pRootSig);
}

void D3D12GraphicsContext::EndFrame() {
    m_cmdList->Close();
    ID3D12CommandList* ppCmds[] = { m_cmdList.Get() };
    m_pQueue->ExecuteCommandLists(1, ppCmds);
}

void D3D12GraphicsContext::TransitionResource(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after || !res) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &b);
}

void D3D12GraphicsContext::BeginRenderPass(const RenderPassDesc& pass) {
    m_currentPass = pass;
    m_inRenderPass = true;

    auto* pTex = dynamic_cast<D3D12Texture*>(pass.renderTarget);
    if (!pTex) return;

    TransitionResource(pTex->GetResource(), pTex->GetCurrentState(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    pTex->SetCurrentState(D3D12_RESOURCE_STATE_RENDER_TARGET);

    // Get RTV
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = ConvertFormatToDxgi(pTex->GetDesc().format);
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    // Use temporary CPU handle from SRV/RTV
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {};
    // Check if backbuffer from swapchain
    // We can create a quick RTV on heap start
    ComPtr<ID3D12DescriptorHeap> tempRtvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC thDesc = {};
    thDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    thDesc.NumDescriptors = 1;
    m_pDevice->CreateDescriptorHeap(&thDesc, IID_PPV_ARGS(&tempRtvHeap));
    rtvHandle = tempRtvHeap->GetCPUDescriptorHandleForHeapStart();
    m_pDevice->CreateRenderTargetView(pTex->GetResource(), &rtvDesc, rtvHandle);

    if (pass.clearColorFlag) {
        m_cmdList->ClearRenderTargetView(rtvHandle, pass.clearColor, 0, nullptr);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE* pDsv = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
    if (pass.depthStencil) {
        auto* pDepth = dynamic_cast<D3D12Texture*>(pass.depthStencil);
        if (pDepth) {
            TransitionResource(pDepth->GetResource(), pDepth->GetCurrentState(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
            pDepth->SetCurrentState(D3D12_RESOURCE_STATE_DEPTH_WRITE);

            dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
            D3D12_DEPTH_STENCIL_VIEW_DESC dDesc = {};
            dDesc.Format = ConvertFormatToDxgi(pDepth->GetDesc().format);
            dDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            m_pDevice->CreateDepthStencilView(pDepth->GetResource(), &dDesc, dsvHandle);

            if (pass.clearDepthFlag) {
                m_cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, pass.clearDepth, pass.clearStencil, 0, nullptr);
            }
            pDsv = &dsvHandle;
        }
    }

    m_cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, pDsv);

    Viewport vp;
    vp.x = 0; vp.y = 0;
    vp.width = static_cast<float>(pTex->GetDesc().width);
    vp.height = static_cast<float>(pTex->GetDesc().height);
    SetViewport(vp);

    ScissorRect sc;
    sc.left = 0; sc.top = 0;
    sc.right = static_cast<int32_t>(pTex->GetDesc().width);
    sc.bottom = static_cast<int32_t>(pTex->GetDesc().height);
    SetScissorRect(sc);
}

void D3D12GraphicsContext::EndRenderPass() {
    if (m_inRenderPass && m_currentPass.renderTarget) {
        auto* pTex = dynamic_cast<D3D12Texture*>(m_currentPass.renderTarget);
        if (pTex) {
            // Transition backbuffer to PRESENT if it's swapchain backbuffer, else to GENERIC_READ
            D3D12_RESOURCE_STATES nextState = (pTex->GetDesc().debugName.find("SwapChainBackBuffer") != std::string::npos)
                ? D3D12_RESOURCE_STATE_PRESENT
                : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            TransitionResource(pTex->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET, nextState);
            pTex->SetCurrentState(nextState);
        }
    }
    m_inRenderPass = false;
}

void D3D12GraphicsContext::SetViewport(const Viewport& vp) {
    D3D12_VIEWPORT v = { vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth };
    m_cmdList->RSSetViewports(1, &v);
}

void D3D12GraphicsContext::SetScissorRect(const ScissorRect& sc) {
    D3D12_RECT r = { sc.left, sc.top, sc.right, sc.bottom };
    m_cmdList->RSSetScissorRects(1, &r);
}

void D3D12GraphicsContext::SetPipeline(IPipeline* pipeline) {
    auto* pPipe = dynamic_cast<D3D12Pipeline*>(pipeline);
    if (pPipe) {
        m_cmdList->SetPipelineState(pPipe->GetPipelineState());
        D3D12_PRIMITIVE_TOPOLOGY top = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        if (pPipe->GetDesc().topology == PrimitiveTopology::LineList) top = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        if (pPipe->GetDesc().topology == PrimitiveTopology::TriangleStrip) top = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        m_cmdList->IASetPrimitiveTopology(top);
    }
}

void D3D12GraphicsContext::SetVertexBuffer(IBuffer* buffer, uint32_t slot, uint32_t offset) {
    auto* pBuf = dynamic_cast<D3D12Buffer*>(buffer);
    if (pBuf) {
        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        vbv.BufferLocation = pBuf->GetGpuAddress() + offset;
        vbv.SizeInBytes = static_cast<UINT>(pBuf->GetDesc().sizeBytes - offset);
        vbv.StrideInBytes = pBuf->GetDesc().strideBytes;
        m_cmdList->IASetVertexBuffers(slot, 1, &vbv);
    }
}

void D3D12GraphicsContext::SetIndexBuffer(IBuffer* buffer, uint32_t offset, bool is32Bit) {
    auto* pBuf = dynamic_cast<D3D12Buffer*>(buffer);
    if (pBuf) {
        D3D12_INDEX_BUFFER_VIEW ibv = {};
        ibv.BufferLocation = pBuf->GetGpuAddress() + offset;
        ibv.SizeInBytes = static_cast<UINT>(pBuf->GetDesc().sizeBytes - offset);
        ibv.Format = is32Bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        m_cmdList->IASetIndexBuffer(&ibv);
    }
}

void D3D12GraphicsContext::SetConstantBuffer(uint32_t rootIndex, IBuffer* buffer) {
    auto* pBuf = dynamic_cast<D3D12Buffer*>(buffer);
    if (pBuf) {
        m_cmdList->SetGraphicsRootConstantBufferView(rootIndex, pBuf->GetGpuAddress());
    }
}

void D3D12GraphicsContext::SetTexture(uint32_t rootIndex, ITexture* texture) {
    auto* pTex = dynamic_cast<D3D12Texture*>(texture);
    if (pTex) {
        // Place SRV descriptor into descriptor heap index 0
        D3D12_SHADER_RESOURCE_VIEW_DESC sDesc = {};
        sDesc.Format = ConvertFormatToDxgi(pTex->GetDesc().format);
        sDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sDesc.Texture2D.MipLevels = pTex->GetDesc().mipLevels;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        m_pDevice->CreateShaderResourceView(pTex->GetResource(), &sDesc, cpuHandle);

        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        m_cmdList->SetGraphicsRootDescriptorTable(rootIndex, gpuHandle);
    }
}

void D3D12GraphicsContext::Draw(uint32_t vertexCount, uint32_t startVertexLocation) {
    m_cmdList->DrawInstanced(vertexCount, 1, startVertexLocation, 0);
}

void D3D12GraphicsContext::DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation) {
    m_cmdList->DrawIndexedInstanced(indexCount, 1, startIndexLocation, baseVertexLocation, 0);
}

bool D3D12GraphicsContext::CopyTextureToBuffer(ITexture* srcTexture, IBuffer* dstBuffer) {
    auto* pTex = dynamic_cast<D3D12Texture*>(srcTexture);
    auto* pBuf = dynamic_cast<D3D12Buffer*>(dstBuffer);
    if (!pTex || !pBuf) return false;

    D3D12_RESOURCE_DESC tDesc = pTex->GetResource()->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSize = 0;
    UINT64 totalSize = 0;
    m_pDevice->GetCopyableFootprints(&tDesc, 0, 1, 0, &footprint, &numRows, &rowSize, &totalSize);

    TransitionResource(pTex->GetResource(), pTex->GetCurrentState(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    pTex->SetCurrentState(D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = pTex->GetResource();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = pBuf->GetResource();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint = footprint;

    m_cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    TransitionResource(pTex->GetResource(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pTex->SetCurrentState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// D3D12GraphicsDevice Implementation
// ─────────────────────────────────────────────────────────────────────────────

D3D12GraphicsDevice::D3D12GraphicsDevice() = default;

D3D12GraphicsDevice::~D3D12GraphicsDevice() {
    Shutdown();
}

bool D3D12GraphicsDevice::Initialize(bool enableDebugLayer) {
    if (enableDebugLayer) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            GraphicsDiagnostics::Instance().Log(DiagnosticLogEntry::Level::Info, "D3D12Device", "D3D12 Debug Layer enabled");
        }
    }

    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&m_dxgiFactory));
    if (FAILED(hr)) return false;

    // Pick discrete / best hardware adapter
    for (UINT i = 0; m_dxgiFactory->EnumAdapters1(i, &m_adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        m_adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        if (SUCCEEDED(D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
            char nameA[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nameA, sizeof(nameA), nullptr, nullptr);
            m_deviceInfo.deviceName = nameA;
            m_deviceInfo.vendorId = desc.VendorId;
            m_deviceInfo.deviceId = desc.DeviceId;
            m_deviceInfo.dedicatedVideoMemoryBytes = desc.DedicatedVideoMemory;
            m_deviceInfo.sharedSystemMemoryBytes = desc.SharedSystemMemory;
            break;
        }
    }

    if (!m_adapter) {
        GraphicsDiagnostics::Instance().Log(DiagnosticLogEntry::Level::Error, "D3D12Device", "No hardware D3D12 adapter found");
        return false;
    }

    hr = D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
    if (FAILED(hr)) return false;

    // Command Queue
    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = m_device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&m_commandQueue));
    if (FAILED(hr)) return false;

    // Fence & Synchronization
    m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_fenceValue = 1;

    // Query hardware features
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 opt1 = {};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opt1, sizeof(opt1)))) {
        m_deviceInfo.waveOps = opt1.WaveOps;
        m_deviceInfo.waveLaneCount = opt1.WaveLaneCountMin;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5 = {};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5)))) {
        m_deviceInfo.hardwareRaytracing = (opt5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0);
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7 = {};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7)))) {
        m_deviceInfo.meshShaders = (opt7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1);
    }

    CreateDefaultRootSignature();
    m_context = std::make_unique<D3D12GraphicsContext>(m_device.Get(), m_commandQueue.Get(), m_defaultRootSignature.Get());

    GraphicsDiagnostics::Instance().Log(DiagnosticLogEntry::Level::Info, "D3D12Device",
        "Direct3D 12 device initialized successfully on: " + m_deviceInfo.deviceName +
        " (VRAM: " + std::to_string(m_deviceInfo.dedicatedVideoMemoryBytes / (1024 * 1024)) + " MB)");

    return true;
}

void D3D12GraphicsDevice::CreateDefaultRootSignature() {
    // 0: CBV (b0) for frame constants
    // 1: CBV (b1) for object constants
    // 2: Descriptor table with 1 SRV (t0)
    D3D12_ROOT_PARAMETER params[3] = {};

    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &srvRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static sampler (s0)
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC sigDesc = {};
    sigDesc.NumParameters = 3;
    sigDesc.pParameters = params;
    sigDesc.NumStaticSamplers = 1;
    sigDesc.pStaticSamplers = &sampler;
    sigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> err;
    D3D12SerializeRootSignature(&sigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_defaultRootSignature));
}

void D3D12GraphicsDevice::WaitForGpuIdle() {
    if (!m_commandQueue || !m_fence) return;
    const uint64_t fVal = m_fenceValue++;
    m_commandQueue->Signal(m_fence.Get(), fVal);
    if (m_fence->GetCompletedValue() < fVal) {
        m_fence->SetEventOnCompletion(fVal, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void D3D12GraphicsDevice::Shutdown() {
    WaitForGpuIdle();
    m_context.reset();
    m_defaultRootSignature.Reset();
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    m_fence.Reset();
    m_commandQueue.Reset();
    m_device.Reset();
    m_adapter.Reset();
    m_dxgiFactory.Reset();
}

std::shared_ptr<ISwapChain> D3D12GraphicsDevice::CreateSwapChain(const SwapChainDesc& desc) {
    return std::make_shared<D3D12SwapChain>(m_device.Get(), m_dxgiFactory.Get(), m_commandQueue.Get(), desc);
}

std::shared_ptr<IBuffer> D3D12GraphicsDevice::CreateBuffer(const BufferDesc& desc, const void* initialData) {
    m_memStats.allocatedBufferBytes += desc.sizeBytes;
    m_memStats.bufferCount++;
    return std::make_shared<D3D12Buffer>(m_device.Get(), desc, initialData);
}

std::shared_ptr<ITexture> D3D12GraphicsDevice::CreateTexture(const TextureDesc& desc, const void* initialData) {
    uint64_t bpp = 4;
    m_memStats.allocatedTextureBytes += (desc.width * desc.height * bpp);
    m_memStats.textureCount++;
    return std::make_shared<D3D12Texture>(m_device.Get(), m_commandQueue.Get(), desc, initialData);
}

std::shared_ptr<IPipeline> D3D12GraphicsDevice::CreatePipeline(const PipelineDesc& desc) {
    m_memStats.pipelineCount++;
    return std::make_shared<D3D12Pipeline>(m_device.Get(), m_defaultRootSignature.Get(), desc);
}

} // namespace miami::gfx::d3d12
