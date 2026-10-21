#include <algorithm>
#include <cmath>
#include <kin/ecs/physics3d.hpp>
#include <kin/physics3d/assets3d.hpp>
#include <stdexcept>

namespace kin {
void register_character3d_components(EcsWorld& world) {
    auto& registry = world.components();
    if (!registry.find("CharacterController3D")) {
        auto builder = registry.native<CharacterController3D>("CharacterController3D");
#define NUMBER(name)                                                                                                   \
    builder.property<f32>(                                                                                             \
        #name, [](const CharacterController3D& c) { return c.desc.name; },                                             \
        [](CharacterController3D& c, const f32& v) {                                                                   \
            if (!std::isfinite(v) || v < 0 || v > 1e6f)                                                                \
                return false;                                                                                          \
            c.desc.name = v;                                                                                           \
            return true;                                                                                               \
        })
        NUMBER(radius);
        NUMBER(height);
        NUMBER(crouch_height);
        NUMBER(mass);
        NUMBER(push_strength);
        NUMBER(max_slope_degrees);
        NUMBER(max_step_height);
        NUMBER(step_down_distance);
        NUMBER(walk_speed);
        NUMBER(sprint_speed);
        NUMBER(crouch_speed);
        NUMBER(acceleration);
        NUMBER(deceleration);
        NUMBER(air_acceleration);
        NUMBER(ground_friction);
        NUMBER(wall_friction);
        NUMBER(gravity_scale);
        NUMBER(jump_speed);
        NUMBER(coyote_seconds);
        NUMBER(jump_buffer_seconds);
        NUMBER(up_blend_rate);
#undef NUMBER
        builder
            .property<Vec3f>(
                "up", [](const CharacterController3D& c) { return c.desc.up; },
                [](CharacterController3D& c, const Vec3f& v) {
                    c.desc.up = v;
                    return true;
                })
            .property<u32>(
                "layer", [](const CharacterController3D& c) { return u32(c.desc.layer); },
                [](CharacterController3D& c, const u32& v) {
                    if (v == 0 || v >= 32)
                        return false;
                    c.desc.layer = u8(v);
                    return true;
                })
            .property<u32>(
                "collides_with", [](const CharacterController3D& c) { return c.desc.collides_with; },
                [](CharacterController3D& c, const u32& v) {
                    c.desc.collides_with = v;
                    return true;
                })
            .field("facing", &CharacterController3D::facing)
            .field("apply_facing", &CharacterController3D::apply_facing);
    }
    if (!registry.find("CharacterAnimationLink3D"))
        registry.native<CharacterAnimationLink3D>("CharacterAnimationLink3D")
            .field("speed", &CharacterAnimationLink3D::speed)
            .field("airborne", &CharacterAnimationLink3D::airborne)
            .field("crouching", &CharacterAnimationLink3D::crouching)
            .field("vertical", &CharacterAnimationLink3D::vertical)
            .field("hanging", &CharacterAnimationLink3D::hanging)
            .field("feet", &CharacterAnimationLink3D::feet)
            .field("ground_mask", &CharacterAnimationLink3D::ground_mask);
}
namespace {
Vec3f world_scale(EcsEntity entity) {
    Mat4f matrix;
    size_t depth = 0;
    for (auto e = entity; e; e = e.parent()) {
        if (++depth > 128)
            throw std::runtime_error("physics transform hierarchy too deep");
        const auto* transform = e.get<Transform3D>();
        if (!transform)
            break;
        matrix = transform->matrix() * matrix;
    }
    Vec3f a{matrix.m[0], matrix.m[1], matrix.m[2]}, b{matrix.m[4], matrix.m[5], matrix.m[6]},
        c{matrix.m[8], matrix.m[9], matrix.m[10]};
    Vec3f scale{length(a), length(b), length(c)};
    const auto finite = [](f32 v) { return std::isfinite(v) && v > 0; };
    if (!finite(scale.x) || !finite(scale.y) || !finite(scale.z) || dot(a, cross(b, c)) <= 0 ||
        std::abs(dot(a, b)) > 1e-4f * scale.x * scale.y || std::abs(dot(a, c)) > 1e-4f * scale.x * scale.z ||
        std::abs(dot(b, c)) > 1e-4f * scale.y * scale.z)
        throw std::runtime_error("authored physics requires positive, non-reflected transforms without shear");
    return scale;
}
} // namespace
std::vector<std::string> resolve_physics3d_assets_impl(EcsWorld& world, PhysicsAssets3D& assets,
                                                  std::span<const EcsId> selected) {
    std::vector<std::string> errors;
    std::vector<EcsEntity> colliders, characters;
    if (selected.empty()) {
        world.raw().each([&](flecs::entity e, const Collider3D&) { colliders.emplace_back(e); });
        world.raw().each([&](flecs::entity e, const CharacterController3D&) { characters.emplace_back(e); });
    } else
        for (auto id : selected)
            if (auto e = world.entity_by_id(id); e) {
                if (e.has<Collider3D>())
                    colliders.push_back(e);
                if (e.has<CharacterController3D>())
                    characters.push_back(e);
            }
    std::ranges::sort(colliders, {}, &EcsEntity::id);
    std::ranges::sort(characters, {}, &EcsEntity::id);
    for (auto entity : colliders) {
        const auto collider = *entity.get<Collider3D>();
        if (collider.asset.id.empty()) {
            if (collider.asset_binding)
                entity.get_mut<Collider3D>()->asset_binding.reset();
            continue;
        }
        try {
            if (!entity.has<Transform3D>() || entity.has<CharacterController3D>())
                throw std::runtime_error(
                    "collider asset needs a Transform3D and cannot share a capsule controller entity");
            const auto scale = world_scale(entity) * collider.scale;
            std::string error;
            auto cooked = assets.load_collider(collider.asset.id, scale).value_or({}).shared();
            error = kin::diagnostics3d_text(assets.diagnostics());
            if (!cooked)
                throw std::runtime_error(error);
            const auto* body = entity.get<RigidBody3D>();
            if (cooked->contains_surface && body && body->motion == Body3DMotion::Dynamic)
                throw std::runtime_error("triangle meshes (including compounds containing them) cannot be dynamic");
            entity.get_mut<Collider3D>()->asset_binding = std::move(cooked);
        } catch (const std::exception& error) {
            errors.push_back("entity '" + entity.name() + "', Collider3D asset '" + collider.asset.id +
                             "': " + error.what());
        }
    }
    for (auto entity : characters)
        try {
            const auto& d = entity.get<CharacterController3D>()->desc;
            const auto scale = world_scale(entity);
            if (!entity.has<Transform3D>() || length(scale - Vec3f{1, 1, 1}) > 1e-4f)
                throw std::runtime_error(
                    "authored controller requires a Transform3D with unit world scale; dimensions are metres");
            const f32 values[] = {d.radius,          d.height,
                                  d.crouch_height,   d.mass,
                                  d.push_strength,   d.max_slope_degrees,
                                  d.max_step_height, d.step_down_distance,
                                  d.walk_speed,      d.sprint_speed,
                                  d.crouch_speed,    d.acceleration,
                                  d.deceleration,    d.air_acceleration,
                                  d.ground_friction, d.wall_friction,
                                  d.gravity_scale,   d.jump_speed,
                                  d.coyote_seconds,  d.jump_buffer_seconds,
                                  d.up_blend_rate};
            for (const auto v : values)
                if (!std::isfinite(v) || v < 0 || v > 1e6f)
                    throw std::runtime_error("controller settings must be finite and nonnegative");
            if (d.radius <= 0 || d.mass <= 0 || d.height < 2 * d.radius || d.crouch_height < 2 * d.radius ||
                d.crouch_height > d.height || d.max_slope_degrees >= 90 || d.layer == 0 || d.layer >= 32)
                throw std::runtime_error("invalid capsule dimensions, mass, slope or layer");
            if (!std::isfinite(d.up.x) || !std::isfinite(d.up.y) || !std::isfinite(d.up.z))
                throw std::runtime_error("controller up vector must be finite");
        } catch (const std::exception& error) {
            errors.push_back("entity '" + entity.name() + "', CharacterController3D: " + error.what());
        }
    return errors;
}
Status3D resolve_physics3d_assets(EcsWorld& world, PhysicsAssets3D& assets,
                                                  std::span<const EcsId> selected) {
    auto errors = resolve_physics3d_assets_impl(world, assets, selected);
    const bool accepted = errors.empty();
    return status3d(accepted, std::move(errors));
}
} // namespace kin
