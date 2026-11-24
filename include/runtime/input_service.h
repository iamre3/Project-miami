#pragma once
#include <cstdint>
#include <string>
#include "runtime/service.h"

namespace miami {

struct GamepadState {
    bool connected = false;
    uint32_t buttons = 0; // Bitmask of buttons
    float leftStickX = 0.0f;
    float leftStickY = 0.0f;
    float rightStickX = 0.0f;
    float rightStickY = 0.0f;
    float leftTrigger = 0.0f;
    float rightTrigger = 0.0f;
};

class IInputService : public IRuntimeService {
public:
    virtual void Poll() = 0;
    virtual bool IsKeyDown(uint32_t virtualKey) const = 0;
    virtual void GetMousePosition(int& outX, int& outY) const = 0;
    virtual bool IsMouseButtonDown(int buttonIndex) const = 0; // 0=left, 1=right, 2=middle
    virtual GamepadState GetGamepadState(uint32_t userIndex = 0) const = 0;
    virtual void SetVibration(uint32_t userIndex, float leftMotor, float rightMotor) = 0;
};

} // namespace miami
