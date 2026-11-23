#include "miami/installer.h"
#include "miami/config.h"
#include <windows.h>

namespace miami {

InstallationManager::InstallationManager() {
}

InstallationManager::~InstallationManager() {
}

InstallationInfo InstallationManager::ValidateDirectory(const std::string& path) {
    m_currentInfo.Clear();
    m_currentInfo.directoryPath = path;
    m_currentInfo.status = ValidationStatus::Validating;

    LogInfo("Validating game installation directory: " + path);

    DWORD dwAttrib = GetFileAttributesA(path.c_str());
    if (dwAttrib == INVALID_FILE_ATTRIBUTES || !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
        m_currentInfo.status = ValidationStatus::InvalidDirectory;
        m_currentInfo.statusMessage = "Directory not found. Please choose a valid game folder.";
        LogWarn("Validation: Path does not exist: " + path);
        return m_currentInfo;
    }

    // Check for target executable
    std::string candidateExe = path + "\\game_scarlett_master.exe";
    DWORD exeAttrib = GetFileAttributesA(candidateExe.c_str());
    if (exeAttrib != INVALID_FILE_ATTRIBUTES && !(exeAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
        m_currentInfo.executableFound = true;
        m_currentInfo.executableName = "game_scarlett_master.exe";
        
        HANDLE hFile = CreateFileA(candidateExe.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li;
            if (GetFileSizeEx(hFile, &li)) {
                m_currentInfo.executableSizeBytes = static_cast<uint64_t>(li.QuadPart);
            }
            CloseHandle(hFile);
        }
    } else {
        // Look for any .exe in the selected folder
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA((path + "\\*.exe").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            m_currentInfo.executableFound = true;
            m_currentInfo.executableName = fd.cFileName;
            std::string foundPath = path + "\\" + fd.cFileName;
            HANDLE hFile = CreateFileA(foundPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER li;
                if (GetFileSizeEx(hFile, &li)) {
                    m_currentInfo.executableSizeBytes = static_cast<uint64_t>(li.QuadPart);
                }
                CloseHandle(hFile);
            }
            FindClose(hFind);
        }
    }

    // Check game data packages
    WIN32_FIND_DATAA fdData;
    HANDLE hData = FindFirstFileA((path + "\\*.rar*").c_str(), &fdData);
    if (hData != INVALID_HANDLE_VALUE) {
        m_currentInfo.hasGameData = true;
        do {
            ULARGE_INTEGER uli;
            uli.LowPart = fdData.nFileSizeLow;
            uli.HighPart = fdData.nFileSizeHigh;
            m_currentInfo.totalDataBytes += uli.QuadPart;
        } while (FindNextFileA(hData, &fdData));
        FindClose(hData);
    }

    if (!m_currentInfo.executableFound) {
        m_currentInfo.status = ValidationStatus::MissingExecutable;
        m_currentInfo.statusMessage = "Folder located, but no game executable was found.";
        LogWarn("Validation: No executable found in " + path);
    } else {
        m_currentInfo.status = ValidationStatus::Valid;
        m_currentInfo.statusMessage = "Installation verified (" + m_currentInfo.executableName + ", " + 
                                     std::to_string(m_currentInfo.executableSizeBytes / (1024*1024)) + " MB)";
        LogInfo("Validation succeeded for: " + path);
    }

    return m_currentInfo;
}

bool InstallationManager::SetAndSaveInstallation(const std::string& path) {
    InstallationInfo info = ValidateDirectory(path);
    if (info.status == ValidationStatus::Valid || info.status == ValidationStatus::MissingExecutable) {
        m_currentInfo = info;
        AppConfig& cfg = GetGlobalConfig();
        cfg.installationPath = path;
        if (info.executableFound) {
            cfg.activeExecutable = info.executableName;
        }
        cfg.Save();
        return true;
    }
    return false;
}

} // namespace miami
