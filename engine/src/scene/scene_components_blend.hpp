#pragma once

#include <kin/ecs/component.hpp>
#include <kin/renderer/material.hpp>

#include <optional>
#include <string>

namespace kin::scene_detail {

// A renderer's `blend` field described: how it combines with what is under it.
template <typename T>
EcsComponentRegistry::NativeComponentBuilder<T>& describe_blend(EcsComponentRegistry::NativeComponentBuilder<T>& builder) {
    return builder
        .template property<std::string>("blend", [](const T& r) { return std::string(blend_mode_name(r.blend)); },
                                        [](T& r, const std::string& name) {
                                            const std::optional<BlendMode> mode = blend_mode_named(name.empty() ? "alpha" : name);
                                            if (!mode) return false;
                                            r.blend = *mode;
                                            return true;
                                        })
        .choices({"alpha", "additive", "multiply", "max", "min"})
        .description("How it combines with what is under it: additive for glows and light, multiply for shade.");
}

} // namespace kin::scene_detail
