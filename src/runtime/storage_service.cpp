#include "runtime/storage_service.h"
#include <windows.h>
#include <fstream>
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace miami {

class StorageService : public IStorageService {
public:
    StorageService() = default;
    ~StorageService() override { Shutdown(); }

    const char* GetServiceName() const override { return "StorageService"; }

    bool Initialize() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_operational = true;
        return true;
    }

    void Shutdown() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mounts.clear();
        m_operational = false;
    }

    bool IsOperational() const override { return m_operational; }

    std::string GetStatusDetails() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string details = "Mounted Points: " + std::to_string(m_mounts.size());
        for (const auto& pair : m_mounts) {
            details += " [" + pair.first + " -> " + pair.second + "]";
        }
        return details;
    }

    bool Mount(const std::string& virtualPrefix, const std::string& physicalRoot) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string vnorm = NormalizePath(virtualPrefix);
        std::string pnorm = NormalizePath(physicalRoot);
        m_mounts[vnorm] = pnorm;
        return true;
    }

    bool Unmount(const std::string& virtualPrefix) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string vnorm = NormalizePath(virtualPrefix);
        return m_mounts.erase(vnorm) > 0;
    }

    std::string ResolveVirtualPath(const std::string& vpath) const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string norm = NormalizePath(vpath);
        
        // Find longest matching prefix
        std::string bestPrefix;
        std::string bestTarget;
        for (const auto& pair : m_mounts) {
            if (norm.rfind(pair.first, 0) == 0 && pair.first.length() > bestPrefix.length()) {
                bestPrefix = pair.first;
                bestTarget = pair.second;
            }
        }

        if (!bestPrefix.empty()) {
            std::string sub = norm.substr(bestPrefix.length());
            if (!sub.empty() && (sub[0] == '/' || sub[0] == '\\')) {
                sub = sub.substr(1);
            }
            return bestTarget + "\\" + sub;
        }

        return vpath;
    }

    bool FileExists(const std::string& path) const override {
        std::string resolved = ResolveVirtualPath(path);
        DWORD attr = GetFileAttributesA(resolved.c_str());
        return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
    }

    bool DirectoryExists(const std::string& path) const override {
        std::string resolved = ResolveVirtualPath(path);
        DWORD attr = GetFileAttributesA(resolved.c_str());
        return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
    }

    bool ReadFile(const std::string& path, std::vector<uint8_t>& outData) override {
        std::string resolved = ResolveVirtualPath(path);
        std::ifstream file(resolved, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        outData.resize(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(outData.data()), size)) {
            outData.clear();
            return false;
        }
        return true;
    }

    bool WriteFile(const std::string& path, const void* data, size_t size) override {
        std::string resolved = ResolveVirtualPath(path);
        std::ofstream file(resolved, std::ios::binary);
        if (!file.is_open()) return false;

        file.write(reinterpret_cast<const char*>(data), size);
        return file.good();
    }

    bool GetFileInfo(const std::string& path, StorageFileInfo& outInfo) override {
        std::string resolved = ResolveVirtualPath(path);
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExA(resolved.c_str(), GetFileExInfoStandard, &fad)) {
            return false;
        }

        outInfo.path = resolved;
        outInfo.isDirectory = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        outInfo.isReadOnly = (fad.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
        outInfo.size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        return true;
    }

private:
    mutable std::mutex m_mutex;
    bool m_operational = false;
    std::unordered_map<std::string, std::string> m_mounts;

    static std::string NormalizePath(const std::string& path) {
        std::string res = path;
        std::replace(res.begin(), res.end(), '/', '\\');
        while (!res.empty() && res.back() == '\\') {
            res.pop_back();
        }
        return res;
    }
};

std::unique_ptr<IStorageService> CreateStorageService() {
    return std::make_unique<StorageService>();
}

} // namespace miami
