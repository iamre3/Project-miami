#include "miami/library.h"
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>

namespace miami {

GameLibrary::GameLibrary() {
    if (!Load()) {
        LoadDefaults();
        Save();
    }
}

std::string GameLibrary::GetDefaultLibraryPath() const {
    char appData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appData))) {
        std::string dir = std::string(appData) + "\\ProjectMiami";
        CreateDirectoryA(dir.c_str(), NULL);
        return dir + "\\library.ini";
    }
    return "e:\\ProjectMiami\\out\\library.ini";
}

bool GameLibrary::Load(const std::string& iniPath) {
    std::string path = iniPath.empty() ? GetDefaultLibraryPath() : iniPath;
    std::ifstream file(path);
    if (!file.is_open()) return false;

    m_games.clear();
    std::string line;
    GameEntry cur;
    bool inGame = false;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line.front() == '[' && line.back() == ']') {
            std::string section = line.substr(1, line.length() - 2);
            if (section.find("Game_") == 0) {
                if (inGame && !cur.id.empty()) {
                    m_games.push_back(cur);
                }
                cur = GameEntry();
                cur.id = section.substr(5);
                inGame = true;
            }
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "ActiveGame") {
            m_activeGameId = val;
        } else if (inGame) {
            if (key == "title") cur.title = val;
            else if (key == "installPath") cur.installPath = val;
            else if (key == "executableName") cur.executableName = val;
            else if (key == "version") cur.versionString = val;
            else if (key == "lastPlayed") cur.lastPlayed = val;
        }
    }

    if (inGame && !cur.id.empty()) {
        m_games.push_back(cur);
    }

    // Refresh installation states
    for (auto& g : m_games) {
        std::string fullExe = g.installPath + "\\" + g.executableName;
        g.isInstalled = (GetFileAttributesA(fullExe.c_str()) != INVALID_FILE_ATTRIBUTES);
    }

    if (m_games.empty()) {
        LoadDefaults();
    }
    if (m_activeGameId.empty() && !m_games.empty()) {
        m_activeGameId = m_games[0].id;
    }
    return true;
}

bool GameLibrary::Save(const std::string& iniPath) const {
    std::string path = iniPath.empty() ? GetDefaultLibraryPath() : iniPath;
    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << "[Library]\n";
    file << "ActiveGame=" << m_activeGameId << "\n";
    file << "Count=" << m_games.size() << "\n\n";

    for (const auto& g : m_games) {
        file << "[Game_" << g.id << "]\n";
        file << "title=" << g.title << "\n";
        file << "installPath=" << g.installPath << "\n";
        file << "executableName=" << g.executableName << "\n";
        file << "version=" << g.versionString << "\n";
        file << "lastPlayed=" << g.lastPlayed << "\n\n";
    }
    return true;
}

void GameLibrary::LoadDefaults() {
    m_games.clear();
    
    GameEntry gta6;
    gta6.id = "gta6_main";
    gta6.title = "Grand Theft Auto VI";
    gta6.installPath = "e:\\Gta6";
    gta6.executableName = "game_scarlett_master.exe";
    gta6.bannerAsset = "e:\\ProjectMiami\\assets\\images\\banner.png";
    gta6.logoAsset = "e:\\ProjectMiami\\assets\\images\\logo.png";
    gta6.versionString = "1.0.2.1 PC";
    gta6.lastPlayed = "Ready to play";

    std::string fullExe = gta6.installPath + "\\" + gta6.executableName;
    gta6.isInstalled = (GetFileAttributesA(fullExe.c_str()) != INVALID_FILE_ATTRIBUTES);

    m_games.push_back(gta6);
    m_activeGameId = gta6.id;
}

GameEntry* GameLibrary::GetActiveGame() {
    for (auto& g : m_games) {
        if (g.id == m_activeGameId) return &g;
    }
    return m_games.empty() ? nullptr : &m_games[0];
}

const GameEntry* GameLibrary::GetActiveGame() const {
    for (const auto& g : m_games) {
        if (g.id == m_activeGameId) return &g;
    }
    return m_games.empty() ? nullptr : &m_games[0];
}

bool GameLibrary::SelectGame(const std::string& id) {
    for (const auto& g : m_games) {
        if (g.id == id) {
            m_activeGameId = id;
            Save();
            return true;
        }
    }
    return false;
}

bool GameLibrary::AddGame(const GameEntry& entry) {
    if (entry.id.empty()) return false;
    for (auto& g : m_games) {
        if (g.id == entry.id) {
            g = entry;
            Save();
            return true;
        }
    }
    m_games.push_back(entry);
    Save();
    return true;
}

bool GameLibrary::RemoveGame(const std::string& id) {
    for (auto it = m_games.begin(); it != m_games.end(); ++it) {
        if (it->id == id) {
            m_games.erase(it);
            if (m_activeGameId == id) {
                m_activeGameId = m_games.empty() ? "" : m_games[0].id;
            }
            Save();
            return true;
        }
    }
    return false;
}

bool GameLibrary::UpdateActiveInstallPath(const std::string& path, const std::string& exeName, uint64_t sizeBytes) {
    GameEntry* g = GetActiveGame();
    if (!g) return false;
    g->installPath = path;
    g->executableName = exeName;
    g->sizeBytes = sizeBytes;
    std::string fullExe = path + "\\" + exeName;
    g->isInstalled = (GetFileAttributesA(fullExe.c_str()) != INVALID_FILE_ATTRIBUTES);
    Save();
    return true;
}

bool GameLibrary::RepairGamePath(const std::string& id, const std::string& newPath) {
    for (auto& g : m_games) {
        if (g.id == id) {
            g.installPath = newPath;
            std::string fullExe = newPath + "\\" + g.executableName;
            g.isInstalled = (GetFileAttributesA(fullExe.c_str()) != INVALID_FILE_ATTRIBUTES);
            Save();
            return true;
        }
    }
    return false;
}

} // namespace miami
