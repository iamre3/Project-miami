#pragma once
#include "graphics/device.h"
#include <memory>
#include <string>

namespace miami::gfx {

struct RuntimeGraphicsInitDesc {
    void* hostWindowHandle = nullptr;
    uint32_t initialWidth = 1280;
    uint32_t initialHeight = 720;
    bool enableVsync = true;
    bool enableValidation = true;
    GraphicsApi preferredApi = GraphicsApi::Direct3D12;
};

class IGraphicsRuntimeBridge {
public:
    virtual ~IGraphicsRuntimeBridge() = default;

    virtual bool Initialize(const RuntimeGraphicsInitDesc& desc) = 0;
    virtual void Shutdown() = 0;

    virtual IGraphicsDevice* GetDevice() = 0;
    virtual ISwapChain* GetSwapChain() = 0;
    virtual IGraphicsContext* GetContext() = 0;

    virtual bool Resize(uint32_t width, uint32_t height) = 0;
    virtual void BeginFrame() = 0;
    virtual void EndAndPresentFrame() = 0;

    virtual const DeviceInfo& GetDeviceInfo() const = 0;
    virtual const FrameTimingStats& GetTimingStats() const = 0;
    virtual const GpuMemoryStats& GetMemoryStats() const = 0;
};

std::unique_ptr<IGraphicsRuntimeBridge> CreateGraphicsRuntimeBridge();

} // namespace miami::gfx
