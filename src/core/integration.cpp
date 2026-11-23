#include "miami/integration.h"
#include <windows.h>

namespace miami {

LeonidaRuntimeAdapter::LeonidaRuntimeAdapter() {
}

IntegrationInfo LeonidaRuntimeAdapter::GetInfo() const {
    IntegrationInfo info;
    info.name = "Leonida PC Runtime";
    info.version = "0.2.0-preview";
    info.isAvailable = true;
    info.status = IntegrationStatus::Ready;
    info.statusMessage = "Static recompilation and native presentation adapter ready.";
    return info;
}

bool LeonidaRuntimeAdapter::ValidateTarget(const std::string& path, std::string& outError) {
    DWORD dwAttrib = GetFileAttributesA(path.c_str());
    if (dwAttrib == INVALID_FILE_ATTRIBUTES || (dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
        outError = "Target binary does not exist at path: " + path;
        return false;
    }
    return true;
}

} // namespace miami
