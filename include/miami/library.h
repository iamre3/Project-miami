#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace miami {

struct GameEntry {
    std::string id;
    std::string title;
    std::string installPath;
    std::string executableName;
    std::string bannerAsset;
    std::string logoAsset;
    uint64_t sizeBytes = 0;
    bool isInstalled = false;
    std::string versionString = "1.0.0";
    std::string lastPlayed = "Never";
};

class GameLibrary {
public:
    GameLibrary();
    
    void LoadDefaults();
    bool Load(const std::string& iniPath = "");
    bool Save(const std::string& iniPath = "") const;

    const std::vector<GameEntry>& GetGames() const { return m_games; }
    GameEntry* GetActiveGame();
    const GameEntry* GetActiveGame() const;
    
    bool SelectGame(const std::string& id);
    bool AddGame(const GameEntry& entry);
    bool RemoveGame(const std::string& id);
    bool UpdateActiveInstallPath(const std::string& path, const std::string& exeName, uint64_t sizeBytes);
    bool RepairGamePath(const std::string& id, const std::string& newPath);

private:
    std::string GetDefaultLibraryPath() const;

    std::vector<GameEntry> m_games;
    std::string m_activeGameId;
};

} // namespace miami
