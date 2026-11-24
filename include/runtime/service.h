#pragma once
#include <string>
#include <cstdint>
#include <functional>

namespace miami {

enum class RuntimeState {
    Uninitialized,
    Initializing,
    Ready,
    StartingApplication,
    Running,
    Paused,
    Stopping,
    Shutdown,
    Failed
};

const char* RuntimeStateToString(RuntimeState state);

// Service lifecycle interface
class IRuntimeService {
public:
    virtual ~IRuntimeService() = default;
    virtual const char* GetServiceName() const = 0;
    virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;
    virtual bool IsOperational() const = 0;
    virtual std::string GetStatusDetails() const = 0;
};

} // namespace miami
