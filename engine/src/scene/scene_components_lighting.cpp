#include "scene_components_internal.hpp"
#include "scene_components_blend.hpp"

#include <kin/assets/asset_manager.hpp>
#include <kin/ecs/lighting.hpp>
#include <kin/ecs/shader_effect.hpp>

#include <optional>
#include <string>

namespace kin::scene_detail {

void register_scene_lighting_components(EcsWorld& world) {
    if (!world.components().find("LightComponent")) {
        // A light where its entity is: it brightens the world when the scene
        // has a SceneLighting (kin/ecs/lighting.hpp).
        world.components().native<LightComponent>("LightComponent")
            .field("color", &LightComponent::color)
            .field("radius", &LightComponent::radius)
            .range(0.0, 100000.0)
            .description("Nothing further is lit; scaled with its entity.")
            .field("intensity", &LightComponent::intensity)
            .range(0.0, 4.0)
            .description("1 adds its colour in full at the centre.")
            .field("shape", &LightComponent::shape_asset)
            .asset_type(std::string{asset_type_name(AssetType::Image)})
            .description("An image whose alpha shapes it (a cone, a window); none: round.")
            .field("rotation", &LightComponent::rotation)
            .units("degrees")
            .field("enabled", &LightComponent::enabled)
            .field("shadows", &LightComponent::shadows)
            .description("LightOccluders cast its shadows.")
            .field("shadow_strength", &LightComponent::shadow_strength)
            .range(0.0, 1.0)
            .description("How much light its shadows take: 1 all of it, 0 none.")
            .field("shadow_softness", &LightComponent::shadow_softness)
            .units("points")
            .range(0.0, 1000.0)
            .description("The light's size: its shadows' edges soft; 0: sharp.")
            .field("height", &LightComponent::height)
            .field("mask", &LightComponent::mask);
    }
    if (!world.components().find("LightOccluder")) {
        // What stops lights that cast shadows (kin/ecs/lighting.hpp).
        world.components().native<LightOccluder>("LightOccluder")
            .property<std::string>("shape", [](const LightOccluder& o) { return std::string(occluder_shape_name(o.shape)); },
                                   [](LightOccluder& o, const std::string& name) {
                                       const std::optional<OccluderShape> shape = occluder_shape_named(name);
                                       if (!shape) return false;
                                       o.shape = *shape;
                                       return true;
                                   })
            .choices({"box", "circle", "polygon", "tiles"})
            .description("tiles: its TilemapComponent's solid cells.")
            .field("size", &LightOccluder::size)
            .description("A box's, centred on the entity.")
            .field("radius", &LightOccluder::radius)
            .description("A circle's.")
            .field("points", &LightOccluder::points)
            .description("A polygon's corners, in the entity's units.")
            .field("offset", &LightOccluder::offset)
            .field("layer", &LightOccluder::layer)
            .description("tiles: the map layer whose solid cells stop light; empty: every layer's.")
            .field("lit", &LightOccluder::lit)
            .description("Its own area lit (only what is behind it dark); off: dark too.")
            .field("enabled", &LightOccluder::enabled);
    }
    if (!world.components().find("SceneLighting")) {
        // The scene darkened to an ambient light and lit by its lights (the
        // first one found is the scene's).
        world.components().native<SceneLighting>("SceneLighting")
            .field("enabled", &SceneLighting::enabled)
            .field("ambient", &SceneLighting::ambient)
            .description("What is not lit looks like this much light: black is dark, white unlit.")
            .field("resolution", &SceneLighting::resolution)
            .range(0.05, 1.0)
            .description("Light-map pixels per screen point (lights are smooth: half is plenty).")
            .field("forward_sprites", &SceneLighting::forward_sprites);
    }
    if (!world.components().find("ColorGrade")) {
        // The game's colour output while it is in the scene: exposure and
        // tonemapping (a linear game's), a look, dithering.
        world.components().native<ColorGrade>("ColorGrade")
            .field("enabled", &ColorGrade::enabled)
            .field("exposure", &ColorGrade::exposure)
            .range(0.0, 16.0)
            .description("Scales the light (a linear game's).")
            .property<std::string>("tonemap", [](const ColorGrade& g) { return std::string(tonemap_name(g.tonemap)); },
                                   [](ColorGrade& g, const std::string& name) {
                                       const std::optional<Tonemap> tonemap = tonemap_named(name.empty() ? "none" : name);
                                       if (!tonemap) return false;
                                       g.tonemap = *tonemap;
                                       return true;
                                   })
            .choices({"none", "reinhard", "aces"})
            .description("How light above white comes into range (a linear game's).")
            .field("look", &ColorGrade::lut_asset)
            .asset_type(std::string{asset_type_name(AssetType::ColorLut)} + "," + std::string{asset_type_name(AssetType::Image)})
            .description("A .cube file or a LUT image the colours are graded through.")
            .field("strength", &ColorGrade::strength)
            .range(0.0, 1.0)
            .description("How much of the look.")
            .field("dither", &ColorGrade::dither)
            .description("Noise of half a step, so dark gradients do not band.");
    }
    if (!world.components().find("ShaderEffect")) {
        // A fragment shader of the content drawn over a box at the entity
        // (kin/ecs/shader_effect.hpp).
        auto effect = world.components().native<ShaderEffect>("ShaderEffect");
        effect.field("asset", &ShaderEffect::asset)
            .asset_type(std::string{asset_type_name(AssetType::Shader)})
            .description("A .frag.glsl file of the content.")
            .field("texture", &ShaderEffect::texture_asset)
            .asset_type(std::string{asset_type_name(AssetType::Image)})
            .description("An image it samples (sampler 0).")
            .field("size", &ShaderEffect::size)
            .description("The box it covers, centred on the entity.")
            .field("offset", &ShaderEffect::offset)
            .field("params", &ShaderEffect::params)
            .description("Its uniforms' values by name: \"speed=1.5 tint=1,0.8,0.6,1\".")
            .field("layer", &ShaderEffect::layer)
            .field("order", &ShaderEffect::order)
            .field("y_sort", &ShaderEffect::y_sort)
            .field("visible", &ShaderEffect::visible);
        describe_blend(effect);
    }
}

} // namespace kin::scene_detail
