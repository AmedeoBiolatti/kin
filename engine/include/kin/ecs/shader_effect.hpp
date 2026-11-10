#pragma once

// A fragment shader of the game's content drawn over a box at its entity: a
// water surface, a portal, a force field, a heat haze. Scene files give it as
// a .frag.glsl file (Assets2D compiles it with glslc where the game finds
// one, and again when it is saved; else it loads the .frag.spv beside it,
// which Kin Studio writes), an image it samples (sampler 0, `sTexture`), and
// its uniforms' values by name:
//
//     "ShaderEffect": {"asset": "shaders/water.frag.glsl", "size": {"x": 128, "y": 64},
//                      "params": "tint=0.2,0.5,0.9,1 speed=1.5"}
//
// A shader takes them in its uniform block (set 3, binding 0), by member
// name; members named `time` (seconds, update_shader_effects() moves it on)
// and `size` (the box's, in world units) are set for it. Its vertices give it
// `In.UV` (0..1 across the box) and `In.Color` (white), as for
// Renderer2D::draw_shader_surface:
//
//     layout(location = 0) in struct { vec4 Color; vec2 UV; } In;
//     layout(location = 0) out vec4 fColor;
//     layout(set = 2, binding = 0) uniform sampler2D sTexture;
//     layout(set = 3, binding = 0) uniform Params { float time; vec4 tint; };
//
// The box is centred on the entity (`offset` from it), turned and scaled
// with it, and drawn among the world's renderers at its layer and order.

#include <kin/ecs/component.hpp>
#include <kin/ecs/world.hpp>
#include <kin/renderer/material.hpp>
#include <kin/renderer/render_layer.hpp>
#include <kin/renderer/shader.hpp>
#include <kin/renderer/texture.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace kin {

class Renderer2D;

struct ShaderEffect {
    ComponentAssetRef asset{};         // a .frag.glsl (or .spv) file of the content
    ComponentAssetRef texture_asset{}; // an image it samples (sampler 0); none: white
    Vec2f size{64.0f, 64.0f};
    Vec2f offset{};
    // Its uniforms' values: "name=1.5 tint=1,0.8,0.6,1", by member name.
    std::string params;
    i32 layer = layer_value(RenderLayer::World);
    i32 order = 0;
    bool y_sort = false;
    f32 sort_y_offset = 0.0f;
    bool visible = true;
    BlendMode blend = BlendMode::Alpha;
    f32 time = 0.0f; // seconds it has run (update_shader_effects)

    // Bound by Assets2D (bind_shader_effects): the shader, the image, and the
    // uniforms made of `params` for its layout.
    ShaderHandle shader{};
    Texture texture{};
    ShaderParams uniforms{};
    std::string made_from;            // the params `uniforms` were made of
    u64 made_for = 0;                 // and the shader's version (Assets2D::shader_version)
};

// A ShaderEffect's `params` read: each name's values ("tint=1,0.8,0.6,1"
// gives tint 4 floats). Nothing for a word that is not name=numbers.
struct ShaderParamValue {
    std::string name;
    std::vector<f32> values;
};
std::vector<ShaderParamValue> parse_shader_params(std::string_view text);
std::string format_shader_params(const std::vector<ShaderParamValue>& values);

// `params` set on `uniforms` (sized for `shader`'s block by `renderer`).
ShaderParams make_shader_params(const Renderer2D& renderer, ShaderHandle shader, std::string_view params);

// Their `time` moved on by `dt`.
void update_shader_effects(EcsWorld& world, f32 dt);

} // namespace kin
