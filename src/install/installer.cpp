#include "miami/installer.h"
#include "miami/config.h"
#include <windows.h>

namespace miami {

InstallationManager::InstallationManager() {
}

InstallationManager::~InstallationManager() {
}

bool InstallationManager::ProbeExecutableHeaders(const std::string& exePath, InstallationInfo& outInfo) {
    HANDLE hFile = CreateFileA(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER liSize;
    if (GetFileSizeEx(hFile, &liSize)) {
        outInfo.executableSizeBytes = static_cast<uint64_t>(liSize.QuadPart);
    }

    // Must be at least 10MB to be a legitimate Game Core master binary
    if (outInfo.executableSizeBytes < 10 * 1024 * 1024) {
        CloseHandle(hFile);
        return false;
    }

    IMAGE_DOS_HEADER dosHeader;
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, &dosHeader, sizeof(dosHeader), &bytesRead, NULL) || bytesRead != sizeof(dosHeader)) {
        CloseHandle(hFile);
        return false;
    }

    if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE) {
        CloseHandle(hFile);
        return false;
    }

    SetFilePointer(hFile, dosHeader.e_lfanew, NULL, FILE_BEGIN);
    IMAGE_NT_HEADERS64 ntHeaders;
    if (!ReadFile(hFile, &ntHeaders, sizeof(ntHeaders), &bytesRead, NULL) || bytesRead != sizeof(ntHeaders)) {
        CloseHandle(hFile);
        return false;
    }

    if (ntHeaders.Signature != IMAGE_NT_SIGNATURE) {
        CloseHandle(hFile);
        return false;
    }

    if (ntHeaders.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64) {
        outInfo.isPE64 = true;
    } else {
        CloseHandle(hFile);
        return false;
    }

    // Scan for required .xbld build metadata section
    WORD numSections = ntHeaders.FileHeader.NumberOfSections;
    for (WORD i = 0; i < numSections; ++i) {
        IMAGE_SECTION_HEADER sec;
        if (ReadFile(hFile, &sec, sizeof(sec), &bytesRead, NULL) && bytesRead == sizeof(sec)) {
            char secName[9] = {0};
            memcpy(secName, sec.Name, 8);
            if (strncmp(secName, ".xbld", 5) == 0) {
                outInfo.hasXbldSection = true;
                outInfo.xbldEdition = "Xbox Scarlett Master (GXDK Build Verified)";
                break;
            }
        }
    }

    CloseHandle(hFile);
    return outInfo.isPE64 && outInfo.hasXbldSection;
}

InstallationInfo InstallationManager::ValidateDirectory(const std::string& path) {
    m_currentInfo.Clear();
    m_currentInfo.directoryPath = path;
    m_currentInfo.status = ValidationStatus::Validating;

    LogInfo("Strictly validating GTA 6 installation files in: " + path);

    // 1. Check directory existence
    DWORD dwAttrib = GetFileAttributesA(path.c_str());
    if (dwAttrib == INVALID_FILE_ATTRIBUTES || !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
        m_currentInfo.status = ValidationStatus::InvalidDirectory;
        m_currentInfo.statusMessage = "Folder does not exist. A valid Grand Theft Auto VI directory is required.";
        LogWarn("Validation failed: folder does not exist: " + path);
        return m_currentInfo;
    }

    // 2. Strict Check for game_scarlett_master.exe
    std::string candidateExe = path + "\\game_scarlett_master.exe";
    DWORD exeAttrib = GetFileAttributesA(candidateExe.c_str());
    if (exeAttrib != INVALID_FILE_ATTRIBUTES && !(exeAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
        m_currentInfo.executableFound = true;
        m_currentInfo.executableName = "game_scarlett_master.exe";
        bool headerOk = ProbeExecutableHeaders(candidateExe, m_currentInfo);
        if (!headerOk) {
            m_currentInfo.status = ValidationStatus::CorruptedExecutable;
            m_currentInfo.statusMessage = "game_scarlett_master.exe found, but headers or .xbld section are invalid.";
            LogError("Validation: Corrupted or invalid game_scarlett_master.exe in " + path);
            return m_currentInfo;
        }
    } else {
        m_currentInfo.status = ValidationStatus::MissingExecutable;
        m_currentInfo.statusMessage = "Missing required game_scarlett_master.exe (GTA 6 master binary).";
        LogWarn("Validation: game_scarlett_master.exe not found in " + path);
        return m_currentInfo;
    }

    // 3. Strict Check for GTA 6 Data Packages (.rar / .fdmdownload / .rpf)
    WIN32_FIND_DATAA fdData;
    HANDLE hData = FindFirstFileA((path + "\\*GTA6*").c_str(), &fdData);
    if (hData != INVALID_HANDLE_VALUE) {
        do {
            ULARGE_INTEGER uli;
            uli.LowPart = fdData.nFileSizeLow;
            uli.HighPart = fdData.nFileSizeHigh;
            m_currentInfo.totalDataBytes += uli.QuadPart;
            m_currentInfo.packageCount++;
        } while (FindNextFileA(hData, &fdData));
        FindClose(hData);
    }

    // Also scan for level packages (levels.rpf)
    DWORD levelsAttr = GetFileAttributesA((path + "\\levels.rpf").c_str());
    if (levelsAttr != INVALID_FILE_ATTRIBUTES) {
        m_currentInfo.hasGameData = true;
    } else if (m_currentInfo.totalDataBytes > 1024ULL * 1024 * 1024) { // At least 1 GB of GTA 6 package archives
        m_currentInfo.hasGameData = true;
    }

    if (!m_currentInfo.hasGameData) {
        m_currentInfo.status = ValidationStatus::MissingGameData;
        m_currentInfo.statusMessage = "Executable verified, but Grand Theft Auto VI game packages / data archives are missing.";
        LogWarn("Validation: GTA 6 game data packages missing in " + path);
        return m_currentInfo;
    }

    // Both executable with .xbld and package data verified
    m_currentInfo.status = ValidationStatus::Valid;
    uint64_t dataGb = m_currentInfo.totalDataBytes / (1024 * 1024 * 1024);
    m_currentInfo.statusMessage = "Verified GTA 6 installation (" + m_currentInfo.executableName + 
                                 ", " + std::to_string(dataGb) + " GB game data)";
    LogInfo("Validation SUCCESS: " + m_currentInfo.statusMessage);

    return m_currentInfo;
}

bool InstallationManager::SetAndSaveInstallation(const std::string& path) {
    InstallationInfo info = ValidateDirectory(path);
    if (info.status == ValidationStatus::Valid) {
        m_currentInfo = info;
        AppConfig& cfg = GetGlobalConfig();
        cfg.installationPath = path;
        cfg.activeExecutable = info.executableName;
        cfg.Save();
        return true;
    }
    m_currentInfo = info;
    return false;
}

} // namespace miami
