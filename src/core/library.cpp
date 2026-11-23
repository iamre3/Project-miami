#include "miami/library.h"
#include <windows.h>

namespace miami {

GameLibrary::GameLibrary() {
    LoadDefaults();
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

    // Check if installation exists
    DWORD dwAttrib = GetFileAttributesA(gta6.installPath.c_str());
    if (dwAttrib != INVALID_FILE_ATTRIBUTES && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
        gta6.isInstalled = true;
    }

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
    g->isInstalled = true;
    return true;
}

} // namespace miami
