#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace miami {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
    Fatal
};

void LogMessage(LogLevel level, const std::string& message);
void LogDebug(const std::string& message);
void LogInfo(const std::string& message);
void LogWarn(const std::string& message);
void LogError(const std::string& message);

std::string GetLogFilePath();
std::string GetConfigFilePath();
std::string GetAppDataFolder();
std::string GetDefaultInstallationPath();
std::vector<std::string> GetRecentLogLines(size_t maxLines = 50);

struct DisplaySettings {
    int resolutionWidth = 1920;
    int resolutionHeight = 1080;
    int windowMode = 0; // 0=Windowed, 1=Borderless Fullscreen, 2=Exclusive Fullscreen
    float uiScale = 1.0f;
    int refreshRate = 60;
    int brightness = 50;
};

struct PerformanceSettings {
    int targetFps = 60; // 30, 60, 120, 0=unlimited (target preference)
    bool vsync = true;
    bool framePacing = true;
    bool highPriorityProcess = false;
};

struct AudioSettings {
    int masterVolume = 80;
    int musicVolume = 70;
    int sfxVolume = 85;
    bool mute = false;
    std::string audioDevice = "Default System Device";
};

struct InputSettings {
    bool enableController = true;
    bool vibration = true;
    int controllerLayout = 0; // 0=Default, 1=Alternative
    bool keyboardMouseEnabled = true;
};

struct InterfaceSettings {
    std::string theme = "Vice Midnight";
    std::string accentColor = "Miami Cyan"; // Cyan or Pink
    bool animatedBackground = true;
    bool reducedMotion = false;
    bool playAmbientAtmosphere = true;
};

struct DiagnosticsSettings {
    int logVerbosity = 1; // 0=Quiet, 1=Normal, 2=Verbose
    bool recordTelemetry = true;
};

struct AppConfig {
    int configVersion = 2;
    bool setupCompleted = false;
    std::string installationPath = "e:\\Gta6";
    std::string activeExecutable = "game_scarlett_master.exe";
    
    DisplaySettings display;
    PerformanceSettings performance;
    AudioSettings audio;
    InputSettings input;
    InterfaceSettings ui;
    DiagnosticsSettings diagnostics;

    bool Load(const std::string& path = "");
    bool Save(const std::string& path = "");
    void ResetDefaults();
};

AppConfig& GetGlobalConfig();

} // namespace miami
