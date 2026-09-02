#pragma once

#include "common/Types.hpp"
#include "controllers/Controllers.hpp"
#include <SDL.h>
#include <array>
#include <string>

namespace ps96 {

class Input {
public:
    Input();
    void initialize();
    void update(Controllers& controllers);
    std::string device_name() const;
    void handle_event(const SDL_Event& e);

    bool has_gamepad() const { return m_gamepad != nullptr; }
    std::string gamepad_name() const;

    // Mapping display
    struct Mapping {
        SDL_GameControllerButton btn;
        const char* name;
    };

    void set_defaults();

private:
    SDL_GameController* m_gamepad = nullptr;
    PadState m_keyboard_state{};
    PadState m_gamepad_state{};
    PadState m_event_latched{};

    static void set_keyboard_button(PadState& state, SDL_Scancode key, bool down);
    static void set_gamepad_button(PadState& state, SDL_GameControllerButton button, bool down);

    void poll_gamepad();
    void poll_keyboard();
};

} // namespace ps96
