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
    CorruptedExecutable,
    MissingGameData,
    Error
};

struct InstallationInfo {
    std::string directoryPath;
    ValidationStatus status = ValidationStatus::NotValidated;
    std::string statusMessage;
    
    // Strict Executable validation
    bool executableFound = false;
    std::string executableName;
    uint64_t executableSizeBytes = 0;
    bool isPE64 = false;
    bool hasXbldSection = false;
    std::string xbldEdition;
    
    // Strict Game Data validation
    bool hasGameData = false;
    int packageCount = 0;
    uint64_t totalDataBytes = 0;
    
    void Clear() {
        directoryPath.clear();
        status = ValidationStatus::NotValidated;
        statusMessage.clear();
        executableFound = false;
        executableName.clear();
        executableSizeBytes = 0;
        isPE64 = false;
        hasXbldSection = false;
        xbldEdition.clear();
        hasGameData = false;
        packageCount = 0;
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
    bool ProbeExecutableHeaders(const std::string& exePath, InstallationInfo& outInfo);
};

} // namespace miami
