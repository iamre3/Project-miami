#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace miami {

enum class ValidationStatus {
    NotValidated,
    Validating,
    Valid,
    InvalidDirectory,
    MissingExecutable,
    Error
};

struct InstallationInfo {
    std::string directoryPath;
    ValidationStatus status = ValidationStatus::NotValidated;
    std::string statusMessage;
    
    bool executableFound = false;
    std::string executableName;
    uint64_t executableSizeBytes = 0;
    
    bool hasGameData = false;
    uint64_t totalDataBytes = 0;
    
    void Clear() {
        directoryPath.clear();
        status = ValidationStatus::NotValidated;
        statusMessage.clear();
        executableFound = false;
        executableName.clear();
        executableSizeBytes = 0;
        hasGameData = false;
        totalDataBytes = 0;
    }
};

class InstallationManager {
public:
    InstallationManager();
    ~InstallationManager();

    InstallationInfo ValidateDirectory(const std::string& path);
    const InstallationInfo& GetCurrentInfo() const { return m_currentInfo; }
    
    bool SetAndSaveInstallation(const std::string& path);

private:
    InstallationInfo m_currentInfo;
};

} // namespace miami
