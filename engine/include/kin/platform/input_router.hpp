#pragma once

#include <kin/platform/input.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

struct RoutedAction {
    bool held = false;
    bool pressed = false;
    bool released = false;
};

// Main-thread input layers, ordered bottom to top. Raw Input remains available
// to UI/text editors; gameplay reads the resolved layer instead of raw actions.
class InputRouter {
public:
    struct Layer {
        std::string name;
        InputMap map;
        bool enabled = true;
        bool consume = true; // matching physical controls stop at this layer
        bool modal = false;  // all lower layers are suspended
        // The map as pushed (push() sets it): what saved overrides are relative to.
        InputMap defaults;
    };

    void push(Layer layer);
    bool remove(std::string_view name);
    const Layer* layer(std::string_view name) const;
    // False for an unknown layer. Change a layer's map (rebinding, loaded
    // overrides) through set_map(), so revision() tells copies of it.
    bool set_enabled(std::string_view name, bool enabled);
    bool set_map(std::string_view name, InputMap map);
    // Moves on every push(), remove() and set_map(): copy a layer's map into
    // Input (for its actions, and what automation presses) only when it moved.
    u64 revision() const { return _revision; }
    void update(const Input& input); // once per simulation step, after input is pumped
    RoutedAction action(std::string_view layer, std::string_view action) const;
    f32 axis(std::string_view layer, std::string_view axis) const;

private:
    struct NameHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
    };
    template <class T> using ByName = std::unordered_map<std::string, T, NameHash, std::equal_to<>>;

    struct Routed {
        Layer layer;
        ByName<RoutedAction> actions; // last update()'s results, by action
        ByName<f32> axes;
    };

    Routed* find(std::string_view name);
    const Routed* find(std::string_view name) const;

    std::vector<Routed> _layers;
    std::vector<u64> _claimed; // update()'s scratch: controls taken by a higher layer
    std::vector<u64> _used;    // and by the layer being resolved
    u64 _revision = 0;
};

enum class RebindState { Idle, Listening, Captured, Cancelled, TimedOut };
enum class RebindConflict { Reject, Replace, Allow };

// Existing held controls are ignored until released; Escape cancels. A captured
// candidate is reviewable before apply(), so a conflict never mutates the map.
class InputRebinder {
public:
    void begin(const Input& input, std::string action, f32 timeout = 10.0f);
    // Once per simulation step, as gameplay reads input: it takes the step's
    // presses (binding_pressed), which survive frames that run no step.
    RebindState update(const Input& input, f32 dt);
    void cancel() { _state = RebindState::Cancelled; }
    RebindState state() const { return _state; }
    std::optional<InputBinding> candidate() const { return _candidate; }
    std::vector<std::string> conflicts(const InputMap& map) const;
    // The candidate replaces the action's bindings on its own device (a key
    // replaces its keys); its bindings on other devices (a controller button) stay.
    bool apply(InputMap& map, RebindConflict policy, std::string& error);

private:
    RebindState _state = RebindState::Idle;
    std::string _action;
    f32 _remaining = 0;
    std::optional<InputBinding> _candidate;
    std::vector<InputBinding> _blocked;
};

// Only differences from defaults are saved, including explicitly unbound
// actions and removed axes. Applying malformed overrides leaves output alone.
std::string serialize_input_overrides(const InputMap& defaults, const InputMap& effective);
bool apply_input_overrides(std::string_view text, const InputMap& defaults, InputMap& output, std::string& error);
// Written whole or not at all (write_user_file).
bool save_input_overrides(const InputMap& defaults, const InputMap& effective, const std::filesystem::path& path);
bool load_input_overrides(const InputMap& defaults, InputMap& output, const std::filesystem::path& path,
                          std::string& error);
// Where the running game keeps the input profile `profile` (letters, digits,
// '_' and '-', at most 80): game_data_dir(user_data_game_id()) / "input" /
// "<profile>.json", beside its saves. Empty, with `error`, for an invalid name
// or while the game has set no id (set_user_data_game_id).
std::filesystem::path input_profile_path(std::string_view profile, std::string& error);

} // namespace kin
