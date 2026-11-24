#include "runtime/audio_service.h"
#include <windows.h>
#include <mmsystem.h>
#include <vector>
#include <cmath>
#include <mutex>

#pragma comment(lib, "winmm.lib")

namespace miami {

class AudioService : public IAudioService {
public:
    AudioService() = default;
    ~AudioService() override { Shutdown(); }

    const char* GetServiceName() const override { return "AudioService"; }

    bool Initialize() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_operational = true;
        m_masterVolume = 1.0f;
        return true;
    }

    void Shutdown() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        StopAll();
        m_operational = false;
    }

    bool IsOperational() const override { return m_operational; }

    std::string GetStatusDetails() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        UINT numDevs = waveOutGetNumDevs();
        return "Audio Output Devices: " + std::to_string(numDevs) + ", Master Volume: " + std::to_string(static_cast<int>(m_masterVolume * 100)) + "%";
    }

    std::vector<AudioDeviceInfo> EnumerateDevices() override {
        std::vector<AudioDeviceInfo> devices;
        UINT numDevs = waveOutGetNumDevs();
        for (UINT i = 0; i < numDevs; ++i) {
            WAVEOUTCAPSA caps;
            if (waveOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                AudioDeviceInfo info;
                info.id = std::to_string(i);
                info.name = caps.szPname;
                info.channels = caps.wChannels;
                info.sampleRate = 48000;
                info.isDefault = (i == 0);
                devices.push_back(info);
            }
        }
        return devices;
    }

    bool PlayTone(float frequencyHz, float durationSeconds, float volume = 0.5f) override {
        if (!m_operational) return false;
        
        uint32_t sampleRate = 44100;
        size_t totalSamples = static_cast<size_t>(sampleRate * durationSeconds);
        std::vector<int16_t> pcm(totalSamples);

        float effVol = volume * m_masterVolume;
        float twoPiF = 6.28318530718f * frequencyHz;

        for (size_t i = 0; i < totalSamples; ++i) {
            float t = static_cast<float>(i) / sampleRate;
            float sample = sinf(twoPiF * t) * effVol;
            pcm[i] = static_cast<int16_t>(sample * 32767.0f);
        }

        return PlayPcmBuffer(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * sizeof(int16_t), 1, sampleRate);
    }

    bool PlayPcmBuffer(const uint8_t* pcmData, size_t dataSize, uint32_t channels, uint32_t sampleRate) override {
        if (!m_operational || !pcmData || dataSize == 0) return false;

        HWAVEOUT hWaveOut = NULL;
        WAVEFORMATEX wfx;
        ZeroMemory(&wfx, sizeof(wfx));
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = static_cast<WORD>(channels);
        wfx.nSamplesPerSec = sampleRate;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

        if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            return false;
        }

        WAVEHDR header;
        ZeroMemory(&header, sizeof(header));
        header.lpData = const_cast<LPSTR>(reinterpret_cast<const char*>(pcmData));
        header.dwBufferLength = static_cast<DWORD>(dataSize);

        if (waveOutPrepareHeader(hWaveOut, &header, sizeof(header)) != MMSYSERR_NOERROR) {
            waveOutClose(hWaveOut);
            return false;
        }

        waveOutWrite(hWaveOut, &header, sizeof(header));
        // Give small buffer moment or let finish
        Sleep(50);

        waveOutUnprepareHeader(hWaveOut, &header, sizeof(header));
        waveOutClose(hWaveOut);
        return true;
    }

    void StopAll() override {
        // Stop any active waveOut handles
    }

    void SetMasterVolume(float volume) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_masterVolume = (volume < 0.0f) ? 0.0f : ((volume > 1.0f) ? 1.0f : volume);
        DWORD dwordVol = static_cast<DWORD>(m_masterVolume * 65535.0f);
        DWORD stereoVol = (dwordVol & 0xFFFF) | ((dwordVol & 0xFFFF) << 16);
        waveOutSetVolume(NULL, stereoVol);
    }

    float GetMasterVolume() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_masterVolume;
    }

private:
    mutable std::mutex m_mutex;
    bool m_operational = false;
    float m_masterVolume = 1.0f;
};

std::unique_ptr<IAudioService> CreateAudioService() {
    return std::make_unique<AudioService>();
}

} // namespace miami
