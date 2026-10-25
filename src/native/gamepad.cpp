// XInput 1.3 for the native host on SDL game controllers (any pad SDL knows: Xbox, PlayStation, Android
// Bluetooth pads). Pads are given XInput slots 0..3 in the order they are first seen.
#include <SDL2/SDL.h>

#include <cstring>
#include <mutex>

#include "guest.h"

namespace native {

namespace {

constexpr uint32_t kNotConnected = 1167;  // ERROR_DEVICE_NOT_CONNECTED

std::mutex g_mutex;
SDL_GameController* g_pads[4] = {};
uint32_t g_packet = 0;

// Fills free slots with controllers that are not assigned yet and drops detached ones.
SDL_GameController* Pad(uint32_t index) {
    if (index >= 4) return nullptr;
    for (auto*& p : g_pads) {
        if (p && !SDL_GameControllerGetAttached(p)) {
            SDL_GameControllerClose(p);
            p = nullptr;
        }
    }
    if (!g_pads[index]) {
        for (int j = 0; j < SDL_NumJoysticks(); ++j) {
            if (!SDL_IsGameController(j)) continue;
            SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
            bool used = false;
            for (auto* p : g_pads) {
                if (p && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p)) == id) used = true;
            }
            if (used) continue;
            g_pads[index] = SDL_GameControllerOpen(j);
            if (g_pads[index]) Log("xinput: pad %u = %s", index, SDL_GameControllerName(g_pads[index]));
            break;
        }
    }
    return g_pads[index];
}

int16_t Axis(SDL_GameController* p, SDL_GameControllerAxis a, bool invert) {
    int v = SDL_GameControllerGetAxis(p, a);
    if (invert) v = -v - 1;  // SDL: down is positive; XInput: up is positive
    return int16_t(v);
}

void x_XInputGetState(Ctx* c) {
    // XINPUT_STATE: dwPacketNumber, XINPUT_GAMEPAD {wButtons, bLeftTrigger, bRightTrigger, sThumbLX, sThumbLY,
    // sThumbRX, sThumbRY}
    std::lock_guard<std::mutex> lock(g_mutex);
    SDL_GameController* p = Pad(Arg(c, 0));
    if (!p) {
        Return(c, kNotConnected, 2);
        return;
    }
    static const struct {
        SDL_GameControllerButton b;
        uint16_t x;
    } kButtons[] = {
        {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x0001}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x0002},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x0004}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x0008},
        {SDL_CONTROLLER_BUTTON_START, 0x0010}, {SDL_CONTROLLER_BUTTON_BACK, 0x0020},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, 0x0040}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0x0080},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x0100}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x0200},
        {SDL_CONTROLLER_BUTTON_A, 0x1000}, {SDL_CONTROLLER_BUTTON_B, 0x2000},
        {SDL_CONTROLLER_BUTTON_X, 0x4000}, {SDL_CONTROLLER_BUTTON_Y, 0x8000},
    };
    uint16_t buttons = 0;
    for (const auto& b : kButtons) {
        if (SDL_GameControllerGetButton(p, b.b)) buttons |= b.x;
    }
    uint32_t s = Arg(c, 1);
    W32(s, ++g_packet);
    W16(s + 4, buttons);
    W8(s + 6, uint8_t(SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7));
    W8(s + 7, uint8_t(SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7));
    W16(s + 8, uint16_t(Axis(p, SDL_CONTROLLER_AXIS_LEFTX, false)));
    W16(s + 10, uint16_t(Axis(p, SDL_CONTROLLER_AXIS_LEFTY, true)));
    W16(s + 12, uint16_t(Axis(p, SDL_CONTROLLER_AXIS_RIGHTX, false)));
    W16(s + 14, uint16_t(Axis(p, SDL_CONTROLLER_AXIS_RIGHTY, true)));
    Return(c, 0, 2);
}

void x_XInputSetState(Ctx* c) {
    // XINPUT_VIBRATION: wLeftMotorSpeed, wRightMotorSpeed
    std::lock_guard<std::mutex> lock(g_mutex);
    SDL_GameController* p = Pad(Arg(c, 0));
    if (!p) {
        Return(c, kNotConnected, 2);
        return;
    }
    SDL_GameControllerRumble(p, R16(Arg(c, 1)), R16(Arg(c, 1) + 2), 1000);
    Return(c, 0, 2);
}

void x_XInputGetCapabilities(Ctx* c) {
    // XINPUT_CAPABILITIES: Type, SubType, Flags, XINPUT_GAMEPAD Gamepad, XINPUT_VIBRATION Vibration
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!Pad(Arg(c, 0))) {
        Return(c, kNotConnected, 3);
        return;
    }
    uint32_t caps = Arg(c, 2);
    memset(Ptr<void>(caps), 0, 20);
    W8(caps, 1);      // XINPUT_DEVTYPE_GAMEPAD
    W8(caps + 1, 1);  // XINPUT_DEVSUBTYPE_GAMEPAD
    W16(caps + 4, 0xf3ff);
    W8(caps + 6, 0xff);
    W8(caps + 7, 0xff);
    for (uint32_t i = 0; i < 4; ++i) W16(caps + 8 + 2 * i, 0xffc0);
    W16(caps + 16, 0xffff);
    W16(caps + 18, 0xffff);
    Return(c, 0, 3);
}

// XInput 1.3 is imported by ordinal: #2 XInputGetState, #3 XInputSetState, #4 XInputGetCapabilities.
const ImportDef kXInput[] = {
    {"xinput1_3.dll", "#2", x_XInputGetState},         {"xinput1_3.dll", "#3", x_XInputSetState},
    {"xinput1_3.dll", "#4", x_XInputGetCapabilities},  {"xinput1_3.dll", "XInputGetState", x_XInputGetState},
    {"xinput1_3.dll", "XInputSetState", x_XInputSetState},
    {"xinput1_3.dll", "XInputGetCapabilities", x_XInputGetCapabilities},
};
ImportRegistrar g_xinput(kXInput);

}  // namespace

}  // namespace native
