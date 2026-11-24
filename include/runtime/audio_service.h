#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "runtime/service.h"

namespace miami {

struct AudioDeviceInfo {
    std::string id;
    std::string name;
    uint32_t channels = 2;
    uint32_t sampleRate = 48000;
    bool isDefault = false;
};

class IAudioService : public IRuntimeService {
public:
    virtual std::vector<AudioDeviceInfo> EnumerateDevices() = 0;
    virtual bool PlayTone(float frequencyHz, float durationSeconds, float volume = 0.5f) = 0;
    virtual bool PlayPcmBuffer(const uint8_t* pcmData, size_t dataSize, uint32_t channels, uint32_t sampleRate) = 0;
    virtual void StopAll() = 0;
    virtual void SetMasterVolume(float volume) = 0;
    virtual float GetMasterVolume() const = 0;
};

} // namespace miami
