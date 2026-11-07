#include "scene_components_internal.hpp"
#include "scene_components_blend.hpp"

#include <kin/assets/asset_manager.hpp>
#include <kin/ecs/particles.hpp>
#include <kin/ecs/tilemap.hpp>

#include <optional>
#include <string>

namespace kin::scene_detail {

void register_scene_particles_components(EcsWorld& world) {
    if (!world.components().find("ParticleSystemComponent")) {
        // Particles: an effect of a particle catalog of the game's content,
        // burst where the entity is (update_particles; Assets2D binds the catalog).
        auto particles = world.components().native<ParticleSystemComponent>("ParticleSystemComponent");
        particles.field("asset", &ParticleSystemComponent::asset)
            .asset_type(std::string{asset_type_name(AssetType::Particles)})
            .field("effect", &ParticleSystemComponent::effect)
            .entry_of("asset")
            .description("An effect of the catalog; empty: the game emits.")
            .field("rate", &ParticleSystemComponent::rate)
            .units("bursts/s")
            .range(0.0, 1000.0)
            .description("How often it bursts; an effect that streams streams at its own rate instead.")
            .field("emitting", &ParticleSystemComponent::emitting)
            .description("It bursts at its rate, or its effect streams.")
            .field("burst_at_start", &ParticleSystemComponent::burst_at_start)
            .description("One burst as it starts.")
            .field("offset", &ParticleSystemComponent::offset)
            .description("Where it bursts, from the entity.")
            .field("sprites", &ParticleSystemComponent::sprites_asset)
            .asset_type(std::string{asset_type_name(AssetType::SpriteCatalog)})
            .description("The sprite catalog its effects' sprites are of.")
            .property<u32>("active", [](const ParticleSystemComponent& p) { return static_cast<u32>(p.system.active_count()); },
                           [](ParticleSystemComponent&, const u32&) { return true; }) // as it is: nothing to set
            .readonly()
            .description("Its particles alive now.");
        describe_blend(particles);
    }
    if (!world.components().find("ParticleFieldComponent")) {
        // A field of particles (snow, stars) of a particle catalog, its area
        // from where the entity is (Assets2D makes it).
        auto field = world.components().native<ParticleFieldComponent>("ParticleFieldComponent");
        field.field("asset", &ParticleFieldComponent::asset)
            .asset_type(std::string{asset_type_name(AssetType::Particles)})
            .field("field", &ParticleFieldComponent::name)
            .entry_of("asset")
            .description("A field of the catalog.")
            .field("sprites", &ParticleFieldComponent::sprites_asset)
            .asset_type(std::string{asset_type_name(AssetType::SpriteCatalog)})
            .description("The sprite catalog its sprites are of.");
        describe_blend(field);
    }
    if (!world.components().find("TilemapComponent")) {
        // A tilemap file (square cells or hexagons) drawn from where the entity
        // is (Assets2D reads it, its tileset and the tileset's image).
        world.components().native<TilemapComponent>("TilemapComponent")
            .field("asset", &TilemapComponent::asset)
            .asset_type(std::string{asset_type_name(AssetType::TileMap)})
            .field("layer", &TilemapComponent::layer)
            .field("order", &TilemapComponent::order)
            .description("Its layers' orders come after this.")
            .field("visible", &TilemapComponent::visible)
            .property<u32>("cells",
                           [](const TilemapComponent& t) { return t.tilemap ? static_cast<u32>(t.tilemap->map.cols * t.tilemap->map.rows) : 0u; },
                           [](TilemapComponent&, const u32&) { return true; }) // as it is: nothing to set
            .readonly()
            .description("Its map's cells, once read (0: not read).");
    }
}

} // namespace kin::scene_detail
