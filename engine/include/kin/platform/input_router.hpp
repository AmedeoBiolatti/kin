#pragma once

#include <kin/platform/input.hpp>
#include <map>
#include <unordered_map>
#include <unordered_set>

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
    };
    void push(Layer layer);
    bool remove(std::string_view name);
    Layer* layer(std::string_view name);
    void update(const Input& input); // once per simulation step, after input is pumped
    RoutedAction action(std::string_view layer, std::string_view action) const;
    f32 axis(std::string_view layer, std::string_view axis) const;
    const std::vector<Layer>& layers() const { return _layers; }

private:
    std::vector<Layer> _layers;
    std::map<std::pair<std::string, std::string>, RoutedAction> _actions;
    std::map<std::pair<std::string, std::string>, f32> _axes;
};

enum class RebindState { Idle, Listening, Captured, Cancelled, TimedOut };
enum class RebindConflict { Reject, Replace, Allow };

// Existing held controls are ignored until released; Escape cancels. A captured
// candidate is reviewable before apply(), so a conflict never mutates the map.
class InputRebinder {
public:
    void begin(const Input& input, std::string action, f32 timeout = 10.0f);
    RebindState update(const Input& input, f32 dt);
    void cancel() { _state = RebindState::Cancelled; }
    RebindState state() const { return _state; }
    std::optional<InputBinding> candidate() const { return _candidate; }
    std::vector<std::string> conflicts(const InputMap& map) const;
    bool apply(InputMap& map, RebindConflict policy, std::string& error);

private:
    RebindState _state = RebindState::Idle;
    std::string _action;
    f32 _remaining = 0;
    std::optional<InputBinding> _candidate;
    std::vector<InputBinding> _blocked;
    GamepadState _previous_pad;
};

// Only differences from defaults are saved, including explicitly unbound
// actions and removed axes. Applying malformed overrides leaves output alone.
std::string serialize_input_overrides(const InputMap& defaults, const InputMap& effective);
bool apply_input_overrides(std::string_view text, const InputMap& defaults, InputMap& output, std::string& error);
bool save_input_overrides(const InputMap& defaults, const InputMap& effective, const std::filesystem::path& path);
bool load_input_overrides(const InputMap& defaults, InputMap& output, const std::filesystem::path& path,
                          std::string& error);

} // namespace kin
