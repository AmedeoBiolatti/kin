#include <kin/renderer/material.hpp>

#include <array>

namespace kin {

std::string_view blend_mode_name(BlendMode mode) {
    switch (mode) {
    case BlendMode::Alpha: return "alpha";
    case BlendMode::Additive: return "additive";
    case BlendMode::Multiply: return "multiply";
    case BlendMode::Replace: return "replace";
    case BlendMode::Max: return "max";
    case BlendMode::Min: return "min";
    }
    return "alpha";
}

std::optional<BlendMode> blend_mode_named(std::string_view name) {
    for (const BlendMode mode : {BlendMode::Alpha, BlendMode::Additive, BlendMode::Multiply, BlendMode::Replace, BlendMode::Max,
                                 BlendMode::Min}) {
        if (blend_mode_name(mode) == name) {
            return mode;
        }
    }
    return std::nullopt;
}

const Material2D* blend_material(BlendMode mode) {
    static const std::array<Material2D, 6> materials = [] {
        std::array<Material2D, 6> made{};
        for (std::size_t i = 0; i < made.size(); ++i) {
            made[i].blend = static_cast<BlendMode>(i);
            made[i].id = std::string("blend:") + std::string(blend_mode_name(made[i].blend));
        }
        return made;
    }();
    if (mode == BlendMode::Alpha) {
        return nullptr;
    }
    return &materials[static_cast<std::size_t>(mode)];
}

} // namespace kin
