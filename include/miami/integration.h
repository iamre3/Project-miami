#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace miami {

enum class IntegrationStatus {
    NotConfigured,
    Ready,
    Running,
    Error,
    Unavailable
};

struct LayerDiagnostic {
    std::string name;
    std::string role;
    std::string version;
    std::string path;
    bool isAvailable = false;
    std::string statusDetail;
};

struct UnifiedIntegrationStatus {
    bool allLayersReady = false;
    LayerDiagnostic xenithLayer;
    LayerDiagnostic leonidaRecomp;
    LayerDiagnostic gta6CompatLayer;
    std::string summary;

    // Live runtime metrics gathered directly from the compiled engines
    size_t xenithSubsystemsInitialized = 0;
    size_t xenithActiveExports = 0;
    size_t xenithTotalExports = 0;
    double xenithReadinessPercent = 0.0;

    size_t scarlettHleExportCount = 0;
    bool scarlettDllLoaded = false;

    bool leonidaRootSigPackerVerified = false;
    bool leonidaGfx10TilingVerified = false;
    uint32_t leonidaPackedUserSgprs = 0;
};

class ICompatibilityAdapter {
public:
    virtual ~ICompatibilityAdapter() = default;
    virtual UnifiedIntegrationStatus GetStatus() = 0;
    virtual bool PrepareGameEnvironment(const std::string& gameDir, std::string& outExecutable, std::string& outError) = 0;
};

class UnifiedRuntimeAdapter : public ICompatibilityAdapter {
public:
    UnifiedRuntimeAdapter();
    ~UnifiedRuntimeAdapter() override;

    UnifiedIntegrationStatus GetStatus() override;
    bool PrepareGameEnvironment(const std::string& gameDir, std::string& outExecutable, std::string& outError) override;

private:
    bool InitializeEngines();

    bool m_enginesInitialized = false;
    std::string m_xenithRoot = "e:\\XENITH";
    std::string m_leonidaRoot = "e:\\XENITH\\references\\Leonida";
    std::string m_compatLayerRoot = "e:\\XENITH\\references\\GTA6-COMPAT-LAYER";
};

} // namespace miami
