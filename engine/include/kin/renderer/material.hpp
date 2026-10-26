#pragma once

#include <kin/renderer/color.hpp>
#include <kin/renderer/texture.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace kin {

enum class BlendMode {
    Alpha,
    Additive,
    Multiply,
    Replace,
    Max, // per channel, the larger of src and dst (alpha too): coverage, shadows
    Min, // per channel, the smaller
};

struct Material2D {
    std::string id;
    Texture albedo;
    Texture normal;
    Texture surface;
    Texture emissive;
    Color tint = colors::white;
    BlendMode blend = BlendMode::Alpha;
    bool receives_light = true;
    bool casts_shadow = false;
};

// "alpha", "additive", "multiply", "replace", "max", "min".
std::string_view blend_mode_name(BlendMode mode);
std::optional<BlendMode> blend_mode_named(std::string_view name);
// A material that only draws in `mode` (shared, for queued commands: a
// renderer's `blend` field); null for Alpha, as drawing without one does.
const Material2D* blend_material(BlendMode mode);

struct MaterialRef {
    const Material2D* material = nullptr;
    std::string id;

    bool valid() const { return material != nullptr || !id.empty(); }
    explicit operator bool() const { return valid(); }
};

// blend_material(mode), as a reference for a queue's draws.
inline MaterialRef blend_ref(BlendMode mode) {
    MaterialRef ref;
    ref.material = blend_material(mode);
    return ref;
}

} // namespace kin
