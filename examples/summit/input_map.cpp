#include "input_map.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace summit {

namespace {

bool binding_down(const RawInput& raw, const Binding& b, f32 trigger_threshold) {
    switch (b.device) {
    case Binding::Device::Key: return raw.keys.test(static_cast<std::size_t>(b.code));
    case Binding::Device::Mouse: return raw.mouse.test(static_cast<std::size_t>(b.code));
    case Binding::Device::PadButton:
        return raw.pad.connected && b.code >= 0 && b.code < static_cast<i32>(GamepadButton::Count) &&
               raw.pad.button(static_cast<GamepadButton>(b.code));
    case Binding::Device::PadTrigger:
        return raw.pad.connected && (b.code == 0 ? raw.pad.left_trigger : raw.pad.right_trigger) > trigger_threshold;
    }
    return false;
}

bool action_down(const RawInput& raw, const InputBindings& bindings, Action a) {
    for (const Binding& b : bindings.bindings(a)) {
        if (binding_down(raw, b, bindings.trigger_threshold)) return true;
    }
    return false;
}

const char* const action_names[action_count] = {"forward", "back", "left", "right", "jump", "sprint",
                                                "grab_left", "grab_right", "release", "pull_up", "interact"};

const char* const pad_names[] = {"A", "B", "X", "Y", "Back", "Start", "LeftStick", "RightStick", "LeftShoulder", "RightShoulder",
                                 "DPadUp", "DPadDown", "DPadLeft", "DPadRight"};

std::string binding_text(const Binding& b) {
    switch (b.device) {
    case Binding::Device::Key: return "key:" + std::string(key_name(static_cast<Key>(b.code)));
    case Binding::Device::Mouse: return "mouse:" + std::string(mouse_button_name(static_cast<MouseButton>(b.code)));
    case Binding::Device::PadButton: return std::string("pad:") + pad_names[std::clamp(b.code, 0, 13)];
    case Binding::Device::PadTrigger: return b.code == 0 ? "trigger:left" : "trigger:right";
    }
    return "";
}

bool parse_binding(const std::string& word, Binding& out) {
    const auto colon = word.find(':');
    if (colon == std::string::npos) return false;
    const std::string kind = word.substr(0, colon), name = word.substr(colon + 1);
    if (kind == "key") {
        for (i32 k = 1; k <= static_cast<i32>(LastKey); ++k) {
            if (key_name(static_cast<Key>(k)) == name) {
                out = Binding::key(static_cast<Key>(k));
                return true;
            }
        }
    } else if (kind == "mouse") {
        for (i32 b = 0; b <= 4; ++b) {
            if (mouse_button_name(static_cast<MouseButton>(b)) == name) {
                out = Binding::mouse(static_cast<MouseButton>(b));
                return true;
            }
        }
    } else if (kind == "pad") {
        for (i32 b = 0; b < 14; ++b) {
            if (name == pad_names[b]) {
                out = Binding::pad(static_cast<GamepadButton>(b));
                return true;
            }
        }
    } else if (kind == "trigger" && (name == "left" || name == "right")) {
        out = Binding::trigger(name == "right");
        return true;
    }
    return false;
}

} // namespace

const char* action_name(Action a) { return action_names[static_cast<std::size_t>(a)]; }

RawInput read_raw_input(const Input& input, const GamepadState* pad) {
    RawInput raw;
    for (i32 k = 1; k <= static_cast<i32>(LastKey); ++k) {
        if (input.held(static_cast<Key>(k))) raw.keys.set(static_cast<std::size_t>(k));
    }
    for (i32 b = 0; b <= 4; ++b) {
        if (input.mouse_held(static_cast<MouseButton>(b))) raw.mouse.set(static_cast<std::size_t>(b));
    }
    raw.mouse_delta = input.mouse_motion();
    if (pad != nullptr) raw.pad = *pad;
    return raw;
}

InputBindings InputBindings::defaults() {
    InputBindings b;
    b.set(Action::Forward, {Binding::key(Key::W), Binding::key(Key::Up)});
    b.set(Action::Back, {Binding::key(Key::S), Binding::key(Key::Down)});
    b.set(Action::Left, {Binding::key(Key::A), Binding::key(Key::Left)});
    b.set(Action::Right, {Binding::key(Key::D), Binding::key(Key::Right)});
    b.set(Action::Jump, {Binding::key(Key::Space), Binding::pad(GamepadButton::A)});
    b.set(Action::Sprint, {Binding::key(Key::LeftShift), Binding::pad(GamepadButton::LeftStick)});
    b.set(Action::GrabLeft, {Binding::mouse(MouseButton::Left), Binding::key(Key::Q), Binding::trigger(false)});
    b.set(Action::GrabRight, {Binding::mouse(MouseButton::Right), Binding::key(Key::E), Binding::trigger(true)});
    b.set(Action::Release, {Binding::key(Key::X), Binding::pad(GamepadButton::B)});
    b.set(Action::PullUp, {Binding::key(Key::F), Binding::pad(GamepadButton::X)});
    b.set(Action::Interact, {Binding::key(Key::R), Binding::pad(GamepadButton::Y)});
    return b;
}

std::string InputBindings::to_text() const {
    std::ostringstream out;
    for (std::size_t a = 0; a < action_count; ++a) {
        out << action_names[a] << " =";
        for (const Binding& b : table[a]) out << ' ' << binding_text(b);
        out << '\n';
    }
    char line[160];
    std::snprintf(line, sizeof line, "dead_zone = %.4f\ntrigger_threshold = %.4f\nmouse_sensitivity = %.4f\nstick_look_speed = %.2f\ninvert_y = %d\n",
                  double(dead_zone), double(trigger_threshold), double(mouse_sensitivity), double(stick_look_speed), invert_y ? 1 : 0);
    out << line;
    return out.str();
}

bool InputBindings::from_text(const std::string& text) {
    InputBindings next = *this;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) return false;
        std::string name = line.substr(0, eq);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        std::istringstream words(line.substr(eq + 1));
        std::vector<std::string> list;
        for (std::string w; words >> w;) list.push_back(w);
        bool found = false;
        for (std::size_t a = 0; a < action_count && !found; ++a) {
            if (name != action_names[a]) continue;
            found = true;
            std::vector<Binding> bindings;
            for (const std::string& w : list) {
                Binding b;
                if (!parse_binding(w, b)) return false;
                bindings.push_back(b);
            }
            next.table[a] = std::move(bindings);
        }
        if (found) continue;
        if (list.size() != 1) return false;
        const f32 v = static_cast<f32>(std::atof(list[0].c_str()));
        if (name == "dead_zone") next.dead_zone = v;
        else if (name == "trigger_threshold") next.trigger_threshold = v;
        else if (name == "mouse_sensitivity") next.mouse_sensitivity = v;
        else if (name == "stick_look_speed") next.stick_look_speed = v;
        else if (name == "invert_y") next.invert_y = v != 0.0f;
        else return false;
    }
    *this = next;
    return true;
}

Vec2f apply_dead_zone(Vec2f stick, f32 dead_zone) {
    const f32 m = std::sqrt(stick.x * stick.x + stick.y * stick.y);
    if (!(m > dead_zone) || m <= 0.0f) return {};
    const f32 scaled = std::min(1.0f, (m - dead_zone) / (1.0f - dead_zone));
    return {stick.x / m * scaled, stick.y / m * scaled};
}

Vec2f look_of(const RawInput& raw, const InputBindings& b, f32 dt) {
    const f32 flip = b.invert_y ? -1.0f : 1.0f;
    Vec2f look{raw.mouse_delta.x * b.mouse_sensitivity, -raw.mouse_delta.y * b.mouse_sensitivity * flip};
    if (raw.pad.connected) {
        const Vec2f r = apply_dead_zone({raw.pad.right_x, raw.pad.right_y}, b.dead_zone);
        look.x += r.x * b.stick_look_speed * dt;
        look.y += r.y * b.stick_look_speed * dt * flip;
    }
    return look;
}

PlayerCommand map_input(const RawInput& raw, const InputBindings& b, f32 dt) {
    PlayerCommand c;
    Vec2f move{};
    if (action_down(raw, b, Action::Forward)) move.y += 1.0f;
    if (action_down(raw, b, Action::Back)) move.y -= 1.0f;
    if (action_down(raw, b, Action::Right)) move.x += 1.0f;
    if (action_down(raw, b, Action::Left)) move.x -= 1.0f;
    if (raw.pad.connected) {
        const Vec2f s = apply_dead_zone({raw.pad.left_x, raw.pad.left_y}, b.dead_zone);
        move.x += s.x;
        move.y += s.y;
    }
    // Keys at 45 degrees are not faster than at 90.
    const f32 m = std::sqrt(move.x * move.x + move.y * move.y);
    if (m > 1.0f) move = {move.x / m, move.y / m};
    c.move = move;

    c.look = look_of(raw, b, dt);

    c.jump = action_down(raw, b, Action::Jump);
    c.sprint = action_down(raw, b, Action::Sprint);
    c.grab_left = action_down(raw, b, Action::GrabLeft);
    c.grab_right = action_down(raw, b, Action::GrabRight);
    c.release = action_down(raw, b, Action::Release);
    c.pull_up = action_down(raw, b, Action::PullUp);
    c.interact = action_down(raw, b, Action::Interact);
    return c;
}

} // namespace summit
