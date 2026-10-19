#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace kin {

using Diagnostics3D = std::vector<std::string>;

// One contextual runtime failure, allocated only on failure. Scene submission
// can latch it until clear; loading/renderer operations replace it each call.
inline bool fail3d(Diagnostics3D& errors, std::string_view operation, std::string_view reason) {
    errors.clear();
    errors.emplace_back(std::string(operation) + ": " + std::string(reason));
    return false;
}

inline std::string diagnostics3d_text(const Diagnostics3D& errors) {
    std::string text;
    for (const auto& error : errors) {
        if (!text.empty()) text += '\n';
        text += error;
    }
    return text;
}

} // namespace kin
