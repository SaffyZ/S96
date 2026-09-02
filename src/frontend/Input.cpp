#include "frontend/Input.hpp"
#include "common/Log.hpp"

namespace ps96 {

Input::Input() = default;

void Input::initialize() {
    if (m_gamepad) return;
    const int count = SDL_NumJoysticks();
    for (int i = 0; i < count; ++i) {
        if (SDL_IsGameController(i)) {
            m_gamepad = SDL_GameControllerOpen(i);
            if (m_gamepad) {
                Log::info("Gamepad: %s", SDL_GameControllerName(m_gamepad));
                break;
            }
        }
    }
    set_defaults();
}

void Input::set_defaults() {
    // Defaults applied in poll
}

std::string Input::gamepad_name() const {
    if (!m_gamepad) return "None";
    const char* n = SDL_GameControllerName(m_gamepad);
    return n ? n : "Unknown";
}

void Input::set_keyboard_button(PadState& state, SDL_Scancode key, bool down) {
    // PS96 keyboard layout:
    //   Arrow keys = D-pad
    //   I/J/K/L   = Cross/Circle/Square/Triangle
    //   A/D       = L1/L2
    //   Q/E       = R1/R2
    //   Enter     = Start, Right Shift = Select
    // W/S are intentionally not mapped to the controller.
    switch (key) {
        case SDL_SCANCODE_UP:      state.up = down; break;
        case SDL_SCANCODE_DOWN:    state.down = down; break;
        case SDL_SCANCODE_LEFT:    state.left = down; break;
        case SDL_SCANCODE_RIGHT:   state.right = down; break;
        case SDL_SCANCODE_I:       state.cross = down; break;
        case SDL_SCANCODE_J:       state.circle = down; break;
        case SDL_SCANCODE_K:       state.square = down; break;
        case SDL_SCANCODE_L:       state.triangle = down; break;
        case SDL_SCANCODE_A:       state.l1 = down; break;
        case SDL_SCANCODE_D:       state.l2 = down; break;
        case SDL_SCANCODE_Q:       state.r1 = down; break;
        case SDL_SCANCODE_E:       state.r2 = down; break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: state.start = down; break;
        case SDL_SCANCODE_RSHIFT:  state.select = down; break;
        default: break;
    }
}

void Input::set_gamepad_button(PadState& state, SDL_GameControllerButton button, bool down) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A: state.cross = down; break;
        case SDL_CONTROLLER_BUTTON_B: state.circle = down; break;
        case SDL_CONTROLLER_BUTTON_X: state.square = down; break;
        case SDL_CONTROLLER_BUTTON_Y: state.triangle = down; break;
        case SDL_CONTROLLER_BUTTON_START: state.start = down; break;
        case SDL_CONTROLLER_BUTTON_BACK: state.select = down; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: state.l1 = down; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: state.r1 = down; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: state.up = down; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: state.down = down; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: state.left = down; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: state.right = down; break;
        default: break;
    }
}

void Input::handle_event(const SDL_Event& e) {
    if (e.type == SDL_CONTROLLERDEVICEADDED) {
        if (!m_gamepad && SDL_IsGameController(e.cdevice.which)) {
            m_gamepad = SDL_GameControllerOpen(e.cdevice.which);
            if (m_gamepad) Log::info("Gamepad: %s", SDL_GameControllerName(m_gamepad));
        }
        return;
    }
    if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
        if (m_gamepad) {
            SDL_GameControllerClose(m_gamepad);
            m_gamepad = nullptr;
            m_gamepad_state = {};
            m_event_latched = {};
            Log::info("Gamepad disconnected");
        }
        return;
    }
    if (e.type == SDL_KEYDOWN && e.key.repeat == 0) {
        set_keyboard_button(m_event_latched, e.key.keysym.scancode, true);
        return;
    }
    if (e.type == SDL_CONTROLLERBUTTONDOWN) {
        set_gamepad_button(m_event_latched, static_cast<SDL_GameControllerButton>(e.cbutton.button), true);
    }
}

void Input::poll_keyboard() {
    const u8* kb = SDL_GetKeyboardState(nullptr);
    m_keyboard_state = {};
    m_keyboard_state.up      = kb[SDL_SCANCODE_UP];
    m_keyboard_state.down    = kb[SDL_SCANCODE_DOWN];
    m_keyboard_state.left    = kb[SDL_SCANCODE_LEFT];
    m_keyboard_state.right   = kb[SDL_SCANCODE_RIGHT];
    m_keyboard_state.cross   = kb[SDL_SCANCODE_I];
    m_keyboard_state.circle  = kb[SDL_SCANCODE_J];
    m_keyboard_state.square  = kb[SDL_SCANCODE_K];
    m_keyboard_state.triangle= kb[SDL_SCANCODE_L];
    m_keyboard_state.start   = kb[SDL_SCANCODE_RETURN] || kb[SDL_SCANCODE_KP_ENTER];
    m_keyboard_state.select  = kb[SDL_SCANCODE_RSHIFT];
    m_keyboard_state.l1      = kb[SDL_SCANCODE_A];
    m_keyboard_state.l2      = kb[SDL_SCANCODE_D];
    m_keyboard_state.r1      = kb[SDL_SCANCODE_Q];
    m_keyboard_state.r2      = kb[SDL_SCANCODE_E];
}

void Input::poll_gamepad() {
    m_gamepad_state = {};
    if (!m_gamepad) return;
    // Xbox 360 mapping → PS1
    m_gamepad_state.cross    = SDL_GameControllerGetButton(m_gamepad, SDL_CONTROLLER_BUTTON_A);
    m_gamepad_state.circle   = SDL_GameControllerGetButton(m_gamepad, SDL_CONTROLLER_BUTTON_B);
    m_gamepad_state.square   = SDL_GameControllerGetButton(m_gamepad, SDL_CONTROLLER_BUTTON_X);
    m_gamepad_state.triangle = SDL_GameControllerGetButton(m_gamepad, SDL_CONTROLLER_BUTTON_Y);
    m_gamepad_state.start    = SDL_GameControllerGetButton(m_gamepad, SDL_CONTROLLER_BUTTON_START);
    m_gamepad_state.select   = SDL_GameControllerGetButton(m_gamepad, SDL_CONTROLLER_BUTTON_BACK);
    m_gamepad_state.l1       = SDL_GameControllerGetButton(m_gamepad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    m_gamepad_state.r1       = SDL_GameControllerGetButton(m_gamepad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    m_gamepad_state.up       = SDL_GameControllerGetButton(m_gamepad, SDL_CONTROLLER_BUTTON_DPAD_UP);
    m_gamepad_state.down     = SDL_GameControllerGetButton(m_gamepad, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    m_gamepad_state.left     = SDL_GameControllerGetButton(m_gamepad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    m_gamepad_state.right    = SDL_GameControllerGetButton(m_gamepad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

    int lt = SDL_GameControllerGetAxis(m_gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    int rt = SDL_GameControllerGetAxis(m_gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    m_gamepad_state.l2 = lt > 8000;
    m_gamepad_state.r2 = rt > 8000;

    m_gamepad_state.stick_lx = SDL_GameControllerGetAxis(m_gamepad, SDL_CONTROLLER_AXIS_LEFTX);
    m_gamepad_state.stick_ly = SDL_GameControllerGetAxis(m_gamepad, SDL_CONTROLLER_AXIS_LEFTY);
    m_gamepad_state.stick_rx = SDL_GameControllerGetAxis(m_gamepad, SDL_CONTROLLER_AXIS_RIGHTX);
    m_gamepad_state.stick_ry = SDL_GameControllerGetAxis(m_gamepad, SDL_CONTROLLER_AXIS_RIGHTY);
    m_gamepad_state.analog = true;
}

std::string Input::device_name() const {
    if (m_gamepad) {
        const char* n = SDL_GameControllerName(m_gamepad);
        if (n && n[0]) return std::string("Controller: ") + n;
        return "Controller";
    }
    return "Keyboard";
}

void Input::update(Controllers& controllers) {
    SDL_PumpEvents();
    poll_keyboard();
    poll_gamepad();

    PadState merged = m_keyboard_state;
    if (m_gamepad) {
        const auto& g = m_gamepad_state;
        merged.up |= g.up; merged.down |= g.down; merged.left |= g.left; merged.right |= g.right;
        merged.cross |= g.cross; merged.circle |= g.circle; merged.square |= g.square; merged.triangle |= g.triangle;
        merged.start |= g.start; merged.select |= g.select;
        merged.l1 |= g.l1; merged.l2 |= g.l2; merged.r1 |= g.r1; merged.r2 |= g.r2;
        merged.stick_lx = g.stick_lx; merged.stick_ly = g.stick_ly;
        merged.stick_rx = g.stick_rx; merged.stick_ry = g.stick_ry;
        merged.analog = g.analog;
    }

    // SDL state APIs are excellent for held buttons, but a quick key/gamepad
    // tap can begin and end between two host-frame samples. Frontend events
    // latch such a transition for one emulated polling interval, ensuring the
    // PS1 sees the same kind of discrete button press it would see from a real
    // controller scan without injecting a permanent key or game-specific input.
    merged.up |= m_event_latched.up; merged.down |= m_event_latched.down;
    merged.left |= m_event_latched.left; merged.right |= m_event_latched.right;
    merged.cross |= m_event_latched.cross; merged.circle |= m_event_latched.circle;
    merged.square |= m_event_latched.square; merged.triangle |= m_event_latched.triangle;
    merged.start |= m_event_latched.start; merged.select |= m_event_latched.select;
    merged.l1 |= m_event_latched.l1; merged.l2 |= m_event_latched.l2;
    merged.r1 |= m_event_latched.r1; merged.r2 |= m_event_latched.r2;

    static PadState last{};
    const bool changed =
        merged.start != last.start || merged.select != last.select ||
        merged.up != last.up || merged.down != last.down || merged.left != last.left || merged.right != last.right ||
        merged.cross != last.cross || merged.circle != last.circle || merged.square != last.square || merged.triangle != last.triangle ||
        merged.l1 != last.l1 || merged.l2 != last.l2 || merged.r1 != last.r1 || merged.r2 != last.r2;
    if (changed) {
        Log::info("HOST PAD p1: start=%d select=%d dpad=%d%d%d%d cross=%d circle=%d square=%d triangle=%d",
            merged.start, merged.select, merged.up, merged.down, merged.left, merged.right,
            merged.cross, merged.circle, merged.square, merged.triangle);
        last = merged;
    }
    controllers.set_pad_state(0, merged);
    m_event_latched = {};
}

} // namespace ps96
