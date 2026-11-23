#include "miami/config.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <mutex>
#include <shlobj.h>

namespace miami {

static std::mutex g_logMutex;
static std::vector<std::string> g_recentLogs;
static AppConfig g_appConfig;

std::string GetAppDataFolder() {
    char szPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, szPath))) {
        std::string dir = std::string(szPath) + "\\ProjectMiami";
        CreateDirectoryA(dir.c_str(), NULL);
        return dir;
    }
    return ".";
}

std::string GetLogFilePath() {
    return GetAppDataFolder() + "\\miami_launcher.log";
}

std::string GetConfigFilePath() {
    return GetAppDataFolder() + "\\miami_config.ini";
}

std::string GetDefaultInstallationPath() {
    DWORD dwAttrib = GetFileAttributesA("e:\\Gta6");
    if (dwAttrib != INVALID_FILE_ATTRIBUTES && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
        return "e:\\Gta6";
    }
    return "C:\\Games\\Grand Theft Auto VI";
}

void LogMessage(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    
    const char* lvlStr = "INFO";
    switch (level) {
        case LogLevel::Debug:   lvlStr = "DEBUG"; break;
        case LogLevel::Info:    lvlStr = "INFO";  break;
        case LogLevel::Warning: lvlStr = "WARN";  break;
        case LogLevel::Error:   lvlStr = "ERROR"; break;
        case LogLevel::Fatal:   lvlStr = "FATAL"; break;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    char timeBuf[64];
    snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::ostringstream ss;
    ss << "[" << timeBuf << "] [" << lvlStr << "] " << message;
    std::string formatted = ss.str();

    g_recentLogs.push_back(formatted);
    if (g_recentLogs.size() > 200) {
        g_recentLogs.erase(g_recentLogs.begin());
    }

    std::ofstream out(GetLogFilePath(), std::ios::app);
    if (out.is_open()) {
        out << formatted << "\n";
    }
    
    OutputDebugStringA((formatted + "\n").c_str());
}

void LogDebug(const std::string& message) { LogMessage(LogLevel::Debug, message); }
void LogInfo(const std::string& message)  { LogMessage(LogLevel::Info, message); }
void LogWarn(const std::string& message)  { LogMessage(LogLevel::Warning, message); }
void LogError(const std::string& message) { LogMessage(LogLevel::Error, message); }

std::vector<std::string> GetRecentLogLines(size_t maxLines) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_recentLogs.size() <= maxLines) {
        return g_recentLogs;
    }
    return std::vector<std::string>(g_recentLogs.end() - maxLines, g_recentLogs.end());
}

void AppConfig::ResetDefaults() {
    configVersion = 2;
    setupCompleted = false;
    installationPath = GetDefaultInstallationPath();
    activeExecutable = "game_scarlett_master.exe";

    display.resolutionWidth = 1920;
    display.resolutionHeight = 1080;
    display.windowMode = 0;
    display.uiScale = 1.0f;
    display.refreshRate = 60;
    display.brightness = 50;

    performance.targetFps = 60;
    performance.vsync = true;
    performance.framePacing = true;
    performance.highPriorityProcess = false;

    audio.masterVolume = 80;
    audio.musicVolume = 70;
    audio.sfxVolume = 85;
    audio.mute = false;
    audio.audioDevice = "Default System Device";

    input.enableController = true;
    input.vibration = true;
    input.controllerLayout = 0;
    input.keyboardMouseEnabled = true;

    ui.theme = "Vice Midnight";
    ui.accentColor = "Miami Cyan";
    ui.animatedBackground = true;
    ui.reducedMotion = false;
    ui.playAmbientAtmosphere = true;

    diagnostics.logVerbosity = 1;
    diagnostics.recordTelemetry = true;
}

static std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

bool AppConfig::Load(const std::string& path) {
    std::string iniPath = path.empty() ? GetConfigFilePath() : path;
    std::ifstream file(iniPath);
    if (!file.is_open()) {
        LogWarn("Launcher config not found at " + iniPath + ". Generating defaults.");
        ResetDefaults();
        return false;
    }

    std::string line;
    std::string currentSection;

    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line.front() == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.size() - 2);
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));

        if (currentSection == "General") {
            if (key == "Version") configVersion = std::stoi(val);
            else if (key == "SetupCompleted") setupCompleted = (val == "1" || val == "true");
            else if (key == "InstallationPath") installationPath = val;
            else if (key == "ActiveExecutable") activeExecutable = val;
        } else if (currentSection == "Display") {
            if (key == "ResolutionWidth") display.resolutionWidth = std::stoi(val);
            else if (key == "ResolutionHeight") display.resolutionHeight = std::stoi(val);
            else if (key == "WindowMode") display.windowMode = std::stoi(val);
            else if (key == "UiScale") display.uiScale = std::stof(val);
            else if (key == "RefreshRate") display.refreshRate = std::stoi(val);
            else if (key == "Brightness") display.brightness = std::stoi(val);
        } else if (currentSection == "Performance") {
            if (key == "TargetFps") performance.targetFps = std::stoi(val);
            else if (key == "VSync") performance.vsync = (val == "1" || val == "true");
            else if (key == "FramePacing") performance.framePacing = (val == "1" || val == "true");
            else if (key == "HighPriorityProcess") performance.highPriorityProcess = (val == "1" || val == "true");
        } else if (currentSection == "Audio") {
            if (key == "MasterVolume") audio.masterVolume = std::stoi(val);
            else if (key == "MusicVolume") audio.musicVolume = std::stoi(val);
            else if (key == "SfxVolume") audio.sfxVolume = std::stoi(val);
            else if (key == "Mute") audio.mute = (val == "1" || val == "true");
            else if (key == "AudioDevice") audio.audioDevice = val;
        } else if (currentSection == "Input") {
            if (key == "EnableController") input.enableController = (val == "1" || val == "true");
            else if (key == "Vibration") input.vibration = (val == "1" || val == "true");
            else if (key == "ControllerLayout") input.controllerLayout = std::stoi(val);
            else if (key == "KeyboardMouseEnabled") input.keyboardMouseEnabled = (val == "1" || val == "true");
        } else if (currentSection == "Interface") {
            if (key == "Theme") ui.theme = val;
            else if (key == "AccentColor") ui.accentColor = val;
            else if (key == "AnimatedBackground") ui.animatedBackground = (val == "1" || val == "true");
            else if (key == "ReducedMotion") ui.reducedMotion = (val == "1" || val == "true");
            else if (key == "PlayAmbientAtmosphere") ui.playAmbientAtmosphere = (val == "1" || val == "true");
        } else if (currentSection == "Diagnostics") {
            if (key == "LogVerbosity") diagnostics.logVerbosity = std::stoi(val);
            else if (key == "RecordTelemetry") diagnostics.recordTelemetry = (val == "1" || val == "true");
        }
    }

    LogInfo("Loaded PC launcher configuration from " + iniPath);
    return true;
}

bool AppConfig::Save(const std::string& path) {
    std::string iniPath = path.empty() ? GetConfigFilePath() : path;
    std::ofstream file(iniPath);
    if (!file.is_open()) {
        LogError("Failed to write launcher configuration to: " + iniPath);
        return false;
    }

    file << "# PROJECT MIAMI — PC Launcher Configuration\n";
    file << "[General]\n";
    file << "Version=" << configVersion << "\n";
    file << "SetupCompleted=" << (setupCompleted ? "1" : "0") << "\n";
    file << "InstallationPath=" << installationPath << "\n";
    file << "ActiveExecutable=" << activeExecutable << "\n\n";

    file << "[Display]\n";
    file << "ResolutionWidth=" << display.resolutionWidth << "\n";
    file << "ResolutionHeight=" << display.resolutionHeight << "\n";
    file << "WindowMode=" << display.windowMode << "\n";
    file << "UiScale=" << display.uiScale << "\n";
    file << "RefreshRate=" << display.refreshRate << "\n";
    file << "Brightness=" << display.brightness << "\n\n";

    file << "[Performance]\n";
    file << "TargetFps=" << performance.targetFps << "\n";
    file << "VSync=" << (performance.vsync ? "1" : "0") << "\n";
    file << "FramePacing=" << (performance.framePacing ? "1" : "0") << "\n";
    file << "HighPriorityProcess=" << (performance.highPriorityProcess ? "1" : "0") << "\n\n";

    file << "[Audio]\n";
    file << "MasterVolume=" << audio.masterVolume << "\n";
    file << "MusicVolume=" << audio.musicVolume << "\n";
    file << "SfxVolume=" << audio.sfxVolume << "\n";
    file << "Mute=" << (audio.mute ? "1" : "0") << "\n";
    file << "AudioDevice=" << audio.audioDevice << "\n\n";

    file << "[Input]\n";
    file << "EnableController=" << (input.enableController ? "1" : "0") << "\n";
    file << "Vibration=" << (input.vibration ? "1" : "0") << "\n";
    file << "ControllerLayout=" << input.controllerLayout << "\n";
    file << "KeyboardMouseEnabled=" << (input.keyboardMouseEnabled ? "1" : "0") << "\n\n";

    file << "[Interface]\n";
    file << "Theme=" << ui.theme << "\n";
    file << "AccentColor=" << ui.accentColor << "\n";
    file << "AnimatedBackground=" << (ui.animatedBackground ? "1" : "0") << "\n";
    file << "ReducedMotion=" << (ui.reducedMotion ? "1" : "0") << "\n";
    file << "PlayAmbientAtmosphere=" << (ui.playAmbientAtmosphere ? "1" : "0") << "\n\n";

    file << "[Diagnostics]\n";
    file << "LogVerbosity=" << diagnostics.logVerbosity << "\n";
    file << "RecordTelemetry=" << (diagnostics.recordTelemetry ? "1" : "0") << "\n";

    LogInfo("Saved PC launcher configuration to " + iniPath);
    return true;
}

AppConfig& GetGlobalConfig() {
    return g_appConfig;
}

} // namespace miami
