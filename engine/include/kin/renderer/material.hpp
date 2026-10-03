#pragma once

#include <kin/renderer/color.hpp>
#include <kin/renderer/texture.hpp>

#include <string>

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

struct MaterialRef {
    const Material2D* material = nullptr;
    std::string id;

    bool valid() const { return material != nullptr || !id.empty(); }
    explicit operator bool() const { return valid(); }
};

} // namespace kin
