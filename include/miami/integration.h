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

struct IntegrationInfo {
    std::string name = "Leonida PC Runtime";
    std::string version = "0.2.0-preview";
    IntegrationStatus status = IntegrationStatus::NotConfigured;
    std::string statusMessage = "Runtime integration ready for executable execution.";
    bool isAvailable = true;
};

class ICompatibilityAdapter {
public:
    virtual ~ICompatibilityAdapter() = default;
    virtual IntegrationInfo GetInfo() const = 0;
    virtual bool ValidateTarget(const std::string& path, std::string& outError) = 0;
};

class LeonidaRuntimeAdapter : public ICompatibilityAdapter {
public:
    LeonidaRuntimeAdapter();
    IntegrationInfo GetInfo() const override;
    bool ValidateTarget(const std::string& path, std::string& outError) override;
};

} // namespace miami
