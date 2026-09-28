#pragma once

#include <kin/core/types.hpp>

#include <string>

namespace kin::ui2 {

struct UiTreeItem {
    std::string id;
    std::string label;
    i32 depth = 0;
    bool has_children = false;
    bool expanded = false;
    bool enabled = true;
};

} // namespace kin::ui2
