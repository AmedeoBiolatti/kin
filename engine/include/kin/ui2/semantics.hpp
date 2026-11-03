#pragma once

#include <kin/ui2/state.hpp>
#include <optional>
#include <string>

namespace kin { class JsonWriter; }
namespace kin::ui2 {

struct SemanticElement {
    Id id{};
    Id parent{};
    Id modal{}; // owning modal, if any
    std::string key; // optional author-facing id, e.g. a .kinui element's id
    std::string role = "region";
    std::string label;
    std::string value;
    Rectf bounds{}; // logical drawing coordinates, after mirrors/clipping
    bool enabled = true;
    bool visible = true;
    bool blocked = false; // outside the active modal or focus trap
    bool focusable = false;
    bool focused = false;
    bool checked = false;
    std::optional<f64> numeric_value, numeric_min, numeric_max, numeric_step;
};

enum class SemanticAction { Click, Focus, SetText, Increment, Decrement, SetNumber };

struct ControllerNavigation {
    bool enabled = true;
    f32 dead_zone = 0.5f;
    f32 repeat_delay = 0.4f;
    f32 repeat_interval = 0.1f;
};

} // namespace kin::ui2
