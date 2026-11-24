#include "runtime/memory_service.h"
#include <windows.h>
#include <mutex>
#include <unordered_map>
#include <iostream>

namespace miami {

class MemoryService : public IMemoryService {
public:
    MemoryService() = default;
    ~MemoryService() override { Shutdown(); }

    const char* GetServiceName() const override { return "MemoryService"; }

    bool Initialize() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_operational = true;
        m_stats = MemoryStats{};
        return true;
    }

    void Shutdown() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Free any remaining allocations
        for (auto& pair : m_hostAllocations) {
            _aligned_free(pair.first);
        }
        m_hostAllocations.clear();

        for (auto& pair : m_guestAllocations) {
            VirtualFree(pair.first, 0, MEM_RELEASE);
        }
        m_guestAllocations.clear();
        m_operational = false;
    }

    bool IsOperational() const override { return m_operational; }

    std::string GetStatusDetails() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return "Host Allocations: " + std::to_string(m_stats.hostAllocatedBytes / 1024) + " KB, " +
               "Guest Allocations: " + std::to_string(m_stats.guestAllocatedBytes / 1024) + " KB (Graphics: " +
               std::to_string(m_stats.graphicsPoolBytes / 1024) + " KB)";
    }

    void* AllocateHost(size_t size, size_t alignment = 16) override {
        if (!m_operational || size == 0) return nullptr;
        void* ptr = _aligned_malloc(size, alignment);
        if (!ptr) return nullptr;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_hostAllocations[ptr] = size;
        m_stats.hostAllocatedBytes += size;
        m_stats.allocationCount++;
        if (m_stats.hostAllocatedBytes > m_stats.hostPeakBytes) {
            m_stats.hostPeakBytes = m_stats.hostAllocatedBytes;
        }
        return ptr;
    }

    void FreeHost(void* ptr) override {
        if (!ptr) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_hostAllocations.find(ptr);
        if (it != m_hostAllocations.end()) {
            m_stats.hostAllocatedBytes -= it->second;
            m_stats.deallocationCount++;
            m_hostAllocations.erase(it);
        }
        _aligned_free(ptr);
    }

    void* AllocateGuest(size_t size, uint32_t memFlags = 0) override {
        if (!m_operational || size == 0) return nullptr;
        // Allocate page-aligned virtual memory
        void* ptr = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!ptr) return nullptr;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_guestAllocations[ptr] = size;
        m_stats.guestAllocatedBytes += size;
        m_stats.allocationCount++;
        if (memFlags & 0x1) { // MEM_GRAPHICS flag
            m_stats.graphicsPoolBytes += size;
        }
        if (m_stats.guestAllocatedBytes > m_stats.guestPeakBytes) {
            m_stats.guestPeakBytes = m_stats.guestAllocatedBytes;
        }
        return ptr;
    }

    void FreeGuest(void* ptr) override {
        if (!ptr) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_guestAllocations.find(ptr);
        if (it != m_guestAllocations.end()) {
            m_stats.guestAllocatedBytes -= it->second;
            m_stats.deallocationCount++;
            m_guestAllocations.erase(it);
        }
        VirtualFree(ptr, 0, MEM_RELEASE);
    }

    MemoryStats GetStats() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stats;
    }

private:
    mutable std::mutex m_mutex;
    bool m_operational = false;
    MemoryStats m_stats;
    std::unordered_map<void*, size_t> m_hostAllocations;
    std::unordered_map<void*, size_t> m_guestAllocations;
};

std::unique_ptr<IMemoryService> CreateMemoryService() {
    return std::make_unique<MemoryService>();
}

} // namespace miami
