// Game controllers: hot-plugging, telling real gamepads from keyboards that pretend to be one,
// and turning buttons, triggers and the left stick into button presses.
#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <vector>

// The first 15 match SDL_GamepadButton; the triggers are read from their axes.
enum class PadButton : int {
    South, East, West, North, Back, Guide, Start, LeftStick, RightStick, LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight, LeftTrigger, RightTrigger, Count,
    None = -1
};
enum class PadStyle { Xbox, PlayStation, Steam };

const char *padButtonId(PadButton b); // stable name for settings files, e.g. "south"
PadButton padButtonFromId(const std::string &id);

class Gamepads {
public:
    ~Gamepads();
    // Off: every controller is closed and its events are ignored.
    void setEnabled(bool on);
    bool enabled() const { return enabled_; }
    // Feed every SDL event (also keyboard and mouse ones: they matter for the fake-controller
    // check and for which glyphs to show).
    void event(const SDL_Event &e, double now);
    // Button presses ready to act on (after the keyboard-coincidence check), held D-pad repeats.
    std::vector<PadButton> poll(double now);
    // True while the last real input came from a controller (show controller glyphs).
    bool active() const { return active_; }
    PadStyle style() const { return style_; }
    // For the menu: the controller in use, or why none is.
    std::string describe() const;
    // Testing: a virtual controller ("xbox", "playstation", "steam") and pressing its buttons.
    bool attachVirtual(const std::string &kind);
    void pressVirtual(PadButton b, bool down);

private:
    struct Device {
        SDL_JoystickID id = 0;
        SDL_Gamepad *pad = nullptr; // null: ignored (see why)
        std::string name, why;
        PadStyle style = PadStyle::Xbox;
        int coincidences = 0;       // inputs that arrived together with key presses
        bool held[int(PadButton::Count)] = {};
    };
    struct Pending {
        PadButton button;
        SDL_JoystickID device;
        double at;
    };
    void add(SDL_JoystickID id);
    void remove(SDL_JoystickID id);
    Device *find(SDL_JoystickID id);
    void press(Device &d, PadButton b, bool down, double now);

    bool enabled_ = true;
    std::vector<Device> devices_;
    std::vector<Pending> pending_;  // presses waiting out the keyboard-coincidence window
    std::vector<PadButton> ready_;
    double lastKey_ = -10;
    bool active_ = false;
    PadStyle style_ = PadStyle::Xbox;
    // Held D-pad / left-stick direction repeat.
    PadButton repeatButton_ = PadButton::None;
    double repeatAt_ = 0;
    SDL_JoystickID virtual_ = 0;
    SDL_Joystick *virtualJoy_ = nullptr;
};
