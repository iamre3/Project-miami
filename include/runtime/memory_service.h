#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include "runtime/service.h"

namespace miami {

struct MemoryStats {
    size_t hostAllocatedBytes = 0;
    size_t hostPeakBytes = 0;
    size_t guestAllocatedBytes = 0;
    size_t guestPeakBytes = 0;
    size_t graphicsPoolBytes = 0;
    size_t allocationCount = 0;
    size_t deallocationCount = 0;
};

class IMemoryService : public IRuntimeService {
public:
    virtual void* AllocateHost(size_t size, size_t alignment = 16) = 0;
    virtual void FreeHost(void* ptr) = 0;
    
    // Console guest address space allocation (64-bit alignment and MEM_GRAPHICS pool tracking)
    virtual void* AllocateGuest(size_t size, uint32_t memFlags = 0) = 0;
    virtual void FreeGuest(void* ptr) = 0;
    
    virtual MemoryStats GetStats() const = 0;
};

} // namespace miami
