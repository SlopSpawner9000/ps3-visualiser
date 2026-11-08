#include "gamepad.h"

#include <algorithm>
#include <cctype>

namespace {

const char *const kIds[int(PadButton::Count)] = {
    "south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
    "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "lefttrigger", "righttrigger"};

// A press this close to a key press (either order) is taken to come from the keyboard itself:
// keyboards with analog keys or a "gamepad mode" send both at once.
constexpr double kCoincidence = 0.04;
constexpr int kCoincidencesToIgnore = 3;
// Held D-pad / stick direction: first repeat after 0.4 s, then every 0.08 s.
constexpr double kRepeatDelay = 0.4, kRepeatEvery = 0.08;

bool isDirection(PadButton b)
{
    return b == PadButton::DpadUp || b == PadButton::DpadDown || b == PadButton::DpadLeft || b == PadButton::DpadRight;
}

// Keyboards known to show up as game controllers (vendor IDs): Ducky, Wooting.
bool keyboardVendor(Uint16 vid) { return vid == 0x3233 || vid == 0x31e3; }

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

const char *padButtonId(PadButton b)
{
    return b >= PadButton::South && b < PadButton::Count ? kIds[int(b)] : "none";
}

PadButton padButtonFromId(const std::string &id)
{
    for (int i = 0; i < int(PadButton::Count); i++)
        if (id == kIds[i])
            return PadButton(i);
    return PadButton::None;
}

Gamepads::~Gamepads()
{
    for (auto &d : devices_)
        if (d.pad)
            SDL_CloseGamepad(d.pad);
    if (virtualJoy_)
        SDL_CloseJoystick(virtualJoy_);
    if (virtual_)
        SDL_DetachVirtualJoystick(virtual_);
}

void Gamepads::setEnabled(bool on)
{
    if (on == enabled_)
        return;
    enabled_ = on;
    if (!on) {
        for (auto &d : devices_)
            if (d.pad)
                SDL_CloseGamepad(d.pad);
        devices_.clear();
        pending_.clear();
        active_ = false;
        repeatButton_ = PadButton::None;
        return;
    }
    int n = 0;
    if (SDL_JoystickID *ids = SDL_GetJoysticks(&n)) {
        for (int i = 0; i < n; i++)
            add(ids[i]);
        SDL_free(ids);
    }
}

Gamepads::Device *Gamepads::find(SDL_JoystickID id)
{
    for (auto &d : devices_)
        if (d.id == id)
            return &d;
    return nullptr;
}

void Gamepads::add(SDL_JoystickID id)
{
    if (!enabled_ || find(id))
        return;
    Device d;
    d.id = id;
    const char *name = SDL_GetJoystickNameForID(id);
    d.name = name ? name : "controller";
    const Uint16 vid = SDL_GetJoystickVendorForID(id);
    if (!SDL_IsGamepad(id))
        d.why = "not a gamepad";
    else if (keyboardVendor(vid) || lower(d.name).find("keyboard") != std::string::npos)
        d.why = "looks like a keyboard";
    else if (!(d.pad = SDL_OpenGamepad(id)))
        d.why = SDL_GetError();
    const SDL_GamepadType type = SDL_GetGamepadTypeForID(id);
    d.style = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5 ? PadStyle::PlayStation
              : vid == 0x28de                                                                       ? PadStyle::Steam
                                                                                                      : PadStyle::Xbox;
    SDL_Log("controller \"%s\" (%04x:%04x): %s", d.name.c_str(), vid, SDL_GetJoystickProductForID(id),
            d.pad ? "in use" : ("ignored, " + d.why).c_str());
    devices_.push_back(std::move(d));
}

void Gamepads::remove(SDL_JoystickID id)
{
    for (auto it = devices_.begin(); it != devices_.end(); ++it)
        if (it->id == id) {
            if (it->pad)
                SDL_CloseGamepad(it->pad);
            SDL_Log("controller \"%s\" disconnected", it->name.c_str());
            devices_.erase(it);
            break;
        }
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [id](const Pending &p) { return p.device == id; }),
                   pending_.end());
    repeatButton_ = PadButton::None;
    if (std::none_of(devices_.begin(), devices_.end(), [](const Device &d) { return d.pad; }))
        active_ = false;
}

void Gamepads::press(Device &d, PadButton b, bool down, double now)
{
    bool &held = d.held[int(b)];
    if (down == held)
        return;
    held = down;
    if (!down) {
        if (repeatButton_ == b)
            repeatButton_ = PadButton::None;
        return;
    }
    if (now - lastKey_ < kCoincidence) { // arrived right after a key press: the keyboard's own
        if (++d.coincidences >= kCoincidencesToIgnore && d.pad) {
            SDL_Log("controller \"%s\" ignored: it moves together with the keyboard", d.name.c_str());
            SDL_CloseGamepad(d.pad);
            d.pad = nullptr;
            d.why = "moves together with the keyboard";
        }
        return;
    }
    pending_.push_back({b, d.id, now});
    if (isDirection(b)) {
        repeatButton_ = b;
        repeatAt_ = now + kRepeatDelay;
    }
}

void Gamepads::event(const SDL_Event &e, double now)
{
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        if (e.key.repeat)
            break;
        lastKey_ = now;
        active_ = false;
        // Presses that came just before this key press: the keyboard's own as well.
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (now - it->at < kCoincidence) {
                if (Device *d = find(it->device); d && ++d->coincidences >= kCoincidencesToIgnore && d->pad) {
                    SDL_Log("controller \"%s\" ignored: it moves together with the keyboard", d->name.c_str());
                    SDL_CloseGamepad(d->pad);
                    d->pad = nullptr;
                    d->why = "moves together with the keyboard";
                }
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        active_ = false;
        break;
    case SDL_EVENT_JOYSTICK_ADDED:
        add(e.jdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        add(e.gdevice.which);
        break;
    case SDL_EVENT_JOYSTICK_REMOVED:
        remove(e.jdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        remove(e.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (Device *d = find(e.gbutton.which); d && d->pad && e.gbutton.button < int(PadButton::LeftTrigger))
            press(*d, PadButton(e.gbutton.button), e.gbutton.down, now);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        Device *d = find(e.gaxis.which);
        if (!d || !d->pad)
            break;
        const float v = float(e.gaxis.value) / 32767.0f;
        // Triggers and the left stick act as buttons (with hysteresis against wobble).
        auto threshold = [&](PadButton b, float amount) {
            const bool held = d->held[int(b)];
            if (!held && amount > 0.6f)
                press(*d, b, true, now);
            else if (held && amount < 0.35f)
                press(*d, b, false, now);
        };
        switch (e.gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: threshold(PadButton::LeftTrigger, v); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: threshold(PadButton::RightTrigger, v); break;
        case SDL_GAMEPAD_AXIS_LEFTX:
            threshold(PadButton::DpadLeft, -v);
            threshold(PadButton::DpadRight, v);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            threshold(PadButton::DpadUp, -v);
            threshold(PadButton::DpadDown, v);
            break;
        default: break;
        }
        break;
    }
    default:
        break;
    }
}

std::vector<PadButton> Gamepads::poll(double now)
{
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now - it->at >= kCoincidence) { // no key press came along: a real controller press
            ready_.push_back(it->button);
            active_ = true;
            if (const Device *d = find(it->device))
                style_ = d->style;
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    if (repeatButton_ != PadButton::None && now >= repeatAt_ && active_) {
        ready_.push_back(repeatButton_);
        repeatAt_ = std::max(repeatAt_ + kRepeatEvery, now - kRepeatEvery);
    }
    std::vector<PadButton> out;
    out.swap(ready_);
    return out;
}

std::string Gamepads::describe() const
{
    if (!enabled_)
        return "off";
    for (const auto &d : devices_)
        if (d.pad)
            return d.name;
    for (const auto &d : devices_)
        if (!d.why.empty() && d.why != "not a gamepad")
            return "none (ignored " + d.name + ": " + d.why + ")";
    return "none connected";
}

bool Gamepads::attachVirtual(const std::string &kind)
{
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = int(PadButton::LeftTrigger);
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.button_mask = (1u << int(PadButton::LeftTrigger)) - 1;
    if (kind == "playstation") {
        desc.vendor_id = 0x054c, desc.product_id = 0x0ce6, desc.name = "Virtual DualSense";
    } else if (kind == "steam") {
        desc.vendor_id = 0x28de, desc.product_id = 0x1205, desc.name = "Virtual Steam Deck";
    } else {
        desc.vendor_id = 0x045e, desc.product_id = 0x0b12, desc.name = "Virtual Xbox Controller";
    }
    virtual_ = SDL_AttachVirtualJoystick(&desc);
    if (!virtual_)
        return false;
    virtualJoy_ = SDL_OpenJoystick(virtual_);
    add(virtual_);
    return virtualJoy_ != nullptr;
}

void Gamepads::pressVirtual(PadButton b, bool down)
{
    if (!virtualJoy_)
        return;
    if (b == PadButton::LeftTrigger || b == PadButton::RightTrigger)
        SDL_SetJoystickVirtualAxis(virtualJoy_,
                                   b == PadButton::LeftTrigger ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                                   down ? 32767 : 0);
    else
        SDL_SetJoystickVirtualButton(virtualJoy_, int(b), down);
}
