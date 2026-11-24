#pragma once
#include <string>
#include <cstdint>
#include "runtime/service.h"

namespace miami {

enum class NetworkStatus {
    Disconnected,
    Connecting,
    Connected,
    LoopbackOnly,
    Error
};

class INetworkService : public IRuntimeService {
public:
    virtual NetworkStatus GetNetworkStatus() const = 0;
    virtual bool InitializeLoopback(uint16_t port) = 0;
    virtual bool SendLoopback(const void* data, size_t size) = 0;
    virtual size_t ReceiveLoopback(void* buffer, size_t bufferSize) = 0;
    virtual std::string GetLocalIpAddress() const = 0;
};

} // namespace miami
