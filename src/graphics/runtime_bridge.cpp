#include "graphics/runtime_bridge.h"
#include "graphics/diagnostics.h"
#include <chrono>

namespace miami::gfx {

class GraphicsRuntimeBridge : public IGraphicsRuntimeBridge {
public:
    GraphicsRuntimeBridge() = default;
    ~GraphicsRuntimeBridge() override {
        Shutdown();
    }

    bool Initialize(const RuntimeGraphicsInitDesc& desc) override {
        m_device = CreateNativeGraphicsDevice(desc.preferredApi);
        if (!m_device || !m_device->Initialize(desc.enableValidation)) {
            GraphicsDiagnostics::Instance().Log(DiagnosticLogEntry::Level::Error, "RuntimeBridge", "Failed to initialize graphics device");
            return false;
        }

        if (desc.hostWindowHandle) {
            SwapChainDesc scDesc;
            scDesc.windowHandle = desc.hostWindowHandle;
            scDesc.width = desc.initialWidth;
            scDesc.height = desc.initialHeight;
            scDesc.bufferCount = 2;
            scDesc.format = Format::R8G8B8A8_UNORM;
            scDesc.vsync = desc.enableVsync;
            m_swapChain = m_device->CreateSwapChain(scDesc);
            if (!m_swapChain) {
                GraphicsDiagnostics::Instance().Log(DiagnosticLogEntry::Level::Error, "RuntimeBridge", "Failed to create swap chain");
                return false;
            }
        }

        m_context = m_device->GetImmediateContext();
        GraphicsDiagnostics::Instance().Log(DiagnosticLogEntry::Level::Info, "RuntimeBridge", "Graphics runtime bridge operational");
        return true;
    }

    void Shutdown() override {
        if (m_device) {
            m_device->WaitForGpuIdle();
        }
        m_swapChain.reset();
        m_device.reset();
    }

    IGraphicsDevice* GetDevice() override { return m_device.get(); }
    ISwapChain* GetSwapChain() override { return m_swapChain.get(); }
    IGraphicsContext* GetContext() override { return m_context; }

    bool Resize(uint32_t width, uint32_t height) override {
        if (m_device) m_device->WaitForGpuIdle();
        if (m_swapChain) return m_swapChain->Resize(width, height);
        return false;
    }

    void BeginFrame() override {
        m_frameStartTime = std::chrono::steady_clock::now();
        if (m_context) m_context->BeginFrame();
    }

    void EndAndPresentFrame() override {
        auto gpuEnd = std::chrono::steady_clock::now();
        if (m_context) m_context->EndFrame();

        auto presentStart = std::chrono::steady_clock::now();
        if (m_swapChain) m_swapChain->Present(true);
        auto presentEnd = std::chrono::steady_clock::now();

        double cpuMs = std::chrono::duration<double, std::milli>(presentEnd - m_frameStartTime).count();
        double gpuMs = std::chrono::duration<double, std::milli>(gpuEnd - m_frameStartTime).count();
        double presMs = std::chrono::duration<double, std::milli>(presentEnd - presentStart).count();

        GraphicsDiagnostics::Instance().UpdateFrameTiming(cpuMs, gpuMs, presMs);
    }

    const DeviceInfo& GetDeviceInfo() const override {
        static DeviceInfo s_null;
        return m_device ? m_device->GetDeviceInfo() : s_null;
    }

    const FrameTimingStats& GetTimingStats() const override {
        return GraphicsDiagnostics::Instance().GetTimingStats();
    }

    const GpuMemoryStats& GetMemoryStats() const override {
        static GpuMemoryStats s_empty;
        return m_device ? m_device->GetMemoryStats() : s_empty;
    }

private:
    std::unique_ptr<IGraphicsDevice> m_device;
    std::shared_ptr<ISwapChain> m_swapChain;
    IGraphicsContext* m_context = nullptr;
    std::chrono::steady_clock::time_point m_frameStartTime;
};

std::unique_ptr<IGraphicsRuntimeBridge> CreateGraphicsRuntimeBridge() {
    return std::make_unique<GraphicsRuntimeBridge>();
}

} // namespace miami::gfx
