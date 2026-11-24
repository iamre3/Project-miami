#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "runtime/service.h"

namespace miami {

struct StorageFileInfo {
    std::string path;
    uint64_t size = 0;
    bool isDirectory = false;
    bool isReadOnly = false;
};

class IStorageService : public IRuntimeService {
public:
    virtual bool Mount(const std::string& virtualPrefix, const std::string& physicalRoot) = 0;
    virtual bool Unmount(const std::string& virtualPrefix) = 0;
    virtual std::string ResolveVirtualPath(const std::string& vpath) const = 0;

    virtual bool FileExists(const std::string& path) const = 0;
    virtual bool DirectoryExists(const std::string& path) const = 0;
    virtual bool ReadFile(const std::string& path, std::vector<uint8_t>& outData) = 0;
    virtual bool WriteFile(const std::string& path, const void* data, size_t size) = 0;
    virtual bool GetFileInfo(const std::string& path, StorageFileInfo& outInfo) = 0;
};

} // namespace miami
