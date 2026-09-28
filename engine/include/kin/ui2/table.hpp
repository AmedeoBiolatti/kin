#pragma once

#include <kin/core/types.hpp>

#include <string>

namespace kin::ui2 {

enum class UiTableColumnSizing {
    Fixed,
    Stretch,
};

struct UiTableColumn {
    std::string label;
    f32 width = 80.0f;
    UiTableColumnSizing sizing = UiTableColumnSizing::Fixed;
};

} // namespace kin::ui2
