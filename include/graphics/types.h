#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace miami::gfx {

enum class GraphicsApi {
    Direct3D12,
    Vulkan,
    Null
};

enum class Format {
    Unknown,
    R8G8B8A8_UNORM,
    B8G8R8A8_UNORM,
    R8G8B8A8_UINT,
    R16G16B16A16_FLOAT,
    R32G32B32A32_FLOAT,
    R32G32B32_FLOAT,
    R32G32_FLOAT,
    R32_FLOAT,
    D32_FLOAT,
    D24_UNORM_S8_UINT
};

enum class BufferUsage {
    Vertex,
    Index,
    Constant,
    Readback,
    Upload,
    Storage
};

enum class TextureUsage {
    ShaderResource,
    RenderTarget,
    DepthStencil,
    CopySrc,
    CopyDst
};

enum class PrimitiveTopology {
    TriangleList,
    TriangleStrip,
    LineList,
    PointList
};

enum class CullMode {
    None,
    Front,
    Back
};

enum class FillMode {
    Solid,
    Wireframe
};

enum class ComparisonFunc {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always
};

struct Viewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float minDepth = 0.0f;
    float maxDepth = 1.0f;
};

struct ScissorRect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
};

struct DeviceInfo {
    std::string deviceName = "Unknown GPU";
    std::string driverVersion = "0.0.0";
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint64_t dedicatedVideoMemoryBytes = 0;
    uint64_t sharedSystemMemoryBytes = 0;
    GraphicsApi api = GraphicsApi::Direct3D12;
    std::string apiVersionStr = "Direct3D 12 (Feature Level 12_0+)";
    bool hardwareRaytracing = false;
    bool meshShaders = false;
    bool waveOps = false;
    uint32_t waveLaneCount = 32;
};

struct BufferDesc {
    uint64_t sizeBytes = 0;
    BufferUsage usage = BufferUsage::Vertex;
    uint32_t strideBytes = 0;
    bool cpuVisible = false;
    std::string debugName;
};

struct TextureDesc {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t mipLevels = 1;
    Format format = Format::R8G8B8A8_UNORM;
    TextureUsage usage = TextureUsage::ShaderResource;
    std::string debugName;
};

struct InputElementDesc {
    std::string semanticName;
    uint32_t semanticIndex = 0;
    Format format = Format::R32G32B32A32_FLOAT;
    uint32_t inputSlot = 0;
    uint32_t alignedByteOffset = 0;
};

struct PipelineDesc {
    std::vector<uint8_t> vertexShaderBytecode;
    std::vector<uint8_t> pixelShaderBytecode;
    std::vector<InputElementDesc> inputLayout;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    CullMode cullMode = CullMode::None;
    FillMode fillMode = FillMode::Solid;
    bool depthEnable = false;
    bool depthWrite = false;
    ComparisonFunc depthFunc = ComparisonFunc::LessEqual;
    Format renderTargetFormat = Format::R8G8B8A8_UNORM;
    Format depthStencilFormat = Format::Unknown;
    std::string debugName;
};

struct SwapChainDesc {
    void* windowHandle = nullptr; // HWND on Windows
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t bufferCount = 2;
    Format format = Format::R8G8B8A8_UNORM;
    bool vsync = true;
};

struct RenderPassDesc {
    class ITexture* renderTarget = nullptr;
    class ITexture* depthStencil = nullptr;
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    bool clearColorFlag = true;
    float clearDepth = 1.0f;
    uint8_t clearStencil = 0;
    bool clearDepthFlag = false;
};

struct FrameTimingStats {
    uint64_t frameIndex = 0;
    double cpuFrameMs = 0.0;
    double gpuDurationMs = 0.0;
    double presentDurationMs = 0.0;
    float currentFps = 0.0f;
    float targetFps = 60.0f;
};

struct GpuMemoryStats {
    uint64_t allocatedBufferBytes = 0;
    uint64_t allocatedTextureBytes = 0;
    uint32_t bufferCount = 0;
    uint32_t textureCount = 0;
    uint32_t pipelineCount = 0;
};

} // namespace miami::gfx
