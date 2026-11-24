#pragma once
#include <string>
#include <vector>

namespace miami {

enum class FeatureStatus {
    NotImplemented,
    Research,
    Experimental,
    PartiallyImplemented,
    Implemented,
    Verified
};

const char* FeatureStatusToString(FeatureStatus status);

struct FeatureCapability {
    std::string name;
    std::string component;
    FeatureStatus status;
    std::string dependencies;
    std::string testCoverage;
    std::string knownLimitations;
    std::string verificationMethod;
};

class FeatureMatrix {
public:
    static FeatureMatrix& GetInstance();

    void RegisterFeature(const FeatureCapability& feat);
    const std::vector<FeatureCapability>& GetCapabilities() const { return m_capabilities; }
    const FeatureCapability* FindFeature(const std::string& name) const;

private:
    FeatureMatrix();
    std::vector<FeatureCapability> m_capabilities;
};

} // namespace miami
