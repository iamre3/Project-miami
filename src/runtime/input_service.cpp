#include "runtime/input_service.h"
#include <windows.h>
#include <xinput.h>
#include <mutex>

#pragma comment(lib, "xinput.lib")

namespace miami {

class InputService : public IInputService {
public:
    InputService() = default;
    ~InputService() override { Shutdown(); }

    const char* GetServiceName() const override { return "InputService"; }

    bool Initialize() override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_operational = true;
        }
        Poll();
        return true;
    }

    void Shutdown() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Reset any vibration
        for (DWORD i = 0; i < 4; ++i) {
            XINPUT_VIBRATION vib = { 0, 0 };
            XInputSetState(i, &vib);
        }
        m_operational = false;
    }

    bool IsOperational() const override { return m_operational; }

    std::string GetStatusDetails() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string res = "Gamepads: ";
        for (int i = 0; i < 4; ++i) {
            if (m_gamepads[i].connected) res += "[" + std::to_string(i) + ": Connected] ";
        }
        if (res == "Gamepads: ") res += "None connected (Keyboard/Mouse Active)";
        return res;
    }

    void Poll() override {
        if (!m_operational) return;
        std::lock_guard<std::mutex> lock(m_mutex);

        // Poll XInput
        for (DWORD i = 0; i < 4; ++i) {
            XINPUT_STATE state;
            ZeroMemory(&state, sizeof(state));
            DWORD dwResult = XInputGetState(i, &state);
            if (dwResult == ERROR_SUCCESS) {
                m_gamepads[i].connected = true;
                m_gamepads[i].buttons = state.Gamepad.wButtons;
                m_gamepads[i].leftStickX = NormalizeAxis(state.Gamepad.sThumbLX);
                m_gamepads[i].leftStickY = NormalizeAxis(state.Gamepad.sThumbLY);
                m_gamepads[i].rightStickX = NormalizeAxis(state.Gamepad.sThumbRX);
                m_gamepads[i].rightStickY = NormalizeAxis(state.Gamepad.sThumbRY);
                m_gamepads[i].leftTrigger = static_cast<float>(state.Gamepad.bLeftTrigger) / 255.0f;
                m_gamepads[i].rightTrigger = static_cast<float>(state.Gamepad.bRightTrigger) / 255.0f;
            } else {
                m_gamepads[i].connected = false;
            }
        }
    }

    bool IsKeyDown(uint32_t virtualKey) const override {
        return (GetAsyncKeyState(static_cast<int>(virtualKey)) & 0x8000) != 0;
    }

    void GetMousePosition(int& outX, int& outY) const override {
        POINT pt;
        GetCursorPos(&pt);
        outX = pt.x;
        outY = pt.y;
    }

    bool IsMouseButtonDown(int buttonIndex) const override {
        int vk = VK_LBUTTON;
        if (buttonIndex == 1) vk = VK_RBUTTON;
        else if (buttonIndex == 2) vk = VK_MBUTTON;
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    GamepadState GetGamepadState(uint32_t userIndex = 0) const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (userIndex < 4) return m_gamepads[userIndex];
        return GamepadState{};
    }

    void SetVibration(uint32_t userIndex, float leftMotor, float rightMotor) override {
        if (!m_operational || userIndex >= 4) return;
        XINPUT_VIBRATION vib;
        vib.wLeftMotorSpeed = static_cast<WORD>(leftMotor * 65535.0f);
        vib.wRightMotorSpeed = static_cast<WORD>(rightMotor * 65535.0f);
        XInputSetState(userIndex, &vib);
    }

private:
    mutable std::mutex m_mutex;
    bool m_operational = false;
    GamepadState m_gamepads[4];

    static float NormalizeAxis(SHORT val) {
        if (val > 0) return static_cast<float>(val) / 32767.0f;
        return static_cast<float>(val) / 32768.0f;
    }
};

std::unique_ptr<IInputService> CreateInputService() {
    return std::make_unique<InputService>();
}

} // namespace miami
