#include <algorithm>
#include <cmath>
#include <kin/ecs/scene3d.hpp>
#include <stdexcept>

namespace kin {
namespace {
bool scalar(f32 v) {
    return std::isfinite(v) && std::abs(v) <= 1e6f;
}
bool vector(Vec3f v) {
    return scalar(v.x) && scalar(v.y) && scalar(v.z);
}
template <class T> bool valid_field(const T& v) {
    if constexpr (std::is_same_v<T, f32>)
        return scalar(v);
    else if constexpr (std::is_same_v<T, Vec3f>)
        return vector(v);
    else
        return true;
}
} // namespace
void register_particle3d_components(EcsWorld& world) {
    auto& registry = world.components();
    if (registry.find("ParticleEmitter3D"))
        return;
    auto b = registry.native<ParticleEmitter3D>("ParticleEmitter3D");
    b.field("enabled", &ParticleEmitter3D::enabled)
        .field("emitting", &ParticleEmitter3D::emitting)
        .field("texture", &ParticleEmitter3D::texture_asset)
        .field("texture_nearest", &ParticleEmitter3D::texture_nearest)
        .property<u64>(
            "seed", [](const auto& e) { return e.system.seed(); },
            [](auto& e, const u64& seed) {
                e.system.set_seed(seed);
                return true;
            });
#define SETTING(name, member)                                                                                          \
    b.property<decltype(ParticleEmitter3D{}.member)>(                                                                  \
        name, [](const auto& e) { return e.member; },                                                                  \
        [](auto& e, const auto& v) {                                                                                   \
            if (!valid_field(v))                                                                                       \
                return false;                                                                                          \
            e.member = v;                                                                                              \
            return true;                                                                                               \
        })
    SETTING("rate_scale", system.rate_scale);
#define DESC(member) SETTING(#member, system.desc.member)
    DESC(extent);
    DESC(surface_only);
    DESC(cone_angle);
    DESC(spread);
    DESC(rate);
    DESC(max_particles);
    DESC(velocity_min);
    DESC(velocity_max);
    DESC(gravity);
    DESC(drag);
    DESC(wind_influence);
    DESC(size_start);
    DESC(size_mid);
    DESC(size_end);
    const auto rgb = [&](const char* name, LinearRgb ParticleDesc3D::*member) {
        b.property<Vec3f>(
            name,
            [member](const auto& e) {
                const auto c = e.system.desc.*member;
                return Vec3f{c.r, c.g, c.b};
            },
            [member](auto& e, const Vec3f& c) {
                if (!vector(c) || c.x < 0 || c.y < 0 || c.z < 0)
                    return false;
                e.system.desc.*member = {c.x, c.y, c.z};
                return true;
            });
    };
    rgb("colour_start", &ParticleDesc3D::colour_start);
    rgb("colour_end", &ParticleDesc3D::colour_end);
    DESC(alpha_start);
    DESC(alpha_end);
    DESC(fade_in);
    DESC(colour_variation);
    DESC(frames_per_life);
    DESC(frame_random);
    DESC(ground);
    DESC(ground_height);
    DESC(bounce);
#undef DESC
#define LOOK(member) SETTING(#member, system.desc.look.member)
    LOOK(axis);
    LOOK(stretch);
    LOOK(stretch_min);
    LOOK(atlas_columns);
    LOOK(atlas_rows);
    LOOK(atlas_frames);
    LOOK(blend_frames);
    LOOK(softness);
    LOOK(ring);
    LOOK(soft);
    LOOK(soft_distance);
    LOOK(lit);
    LOOK(receives_shadow);
    LOOK(casts_shadow);
    LOOK(shadow_cutoff);
    LOOK(fogged);
    LOOK(sort);
#undef LOOK
#undef SETTING
#define RANGE(member)                                                                                                  \
    b.property<Vec2f>(                                                                                                 \
        #member, [](const auto& e) { return Vec2f{e.system.desc.member.min, e.system.desc.member.max}; },              \
        [](auto& e, const Vec2f& v) {                                                                                  \
            if (!scalar(v.x) || !scalar(v.y) || v.x > v.y)                                                             \
                return false;                                                                                          \
            e.system.desc.member = {v.x, v.y};                                                                         \
            return true;                                                                                               \
        })
    RANGE(lifetime);
    RANGE(speed);
    RANGE(size);
    RANGE(rotation);
    RANGE(spin);
#undef RANGE
    b.property<std::string>(
         "shape",
         [](const auto& e) {
             switch (e.system.desc.shape) {
             case ParticleShape3D::Point:
                 return std::string("point");
             case ParticleShape3D::Box:
                 return std::string("box");
             case ParticleShape3D::Sphere:
                 return std::string("sphere");
             case ParticleShape3D::Cone:
                 return std::string("cone");
             case ParticleShape3D::Disc:
                 return std::string("disc");
             }
             return std::string("invalid");
         },
         [](auto& e, const std::string& v) {
             if (v == "point")
                 e.system.desc.shape = ParticleShape3D::Point;
             else if (v == "box")
                 e.system.desc.shape = ParticleShape3D::Box;
             else if (v == "sphere")
                 e.system.desc.shape = ParticleShape3D::Sphere;
             else if (v == "cone")
                 e.system.desc.shape = ParticleShape3D::Cone;
             else if (v == "disc")
                 e.system.desc.shape = ParticleShape3D::Disc;
             else
                 return false;
             return true;
         })
        .property<std::string>(
            "blend",
            [](const auto& e) {
                switch (e.system.desc.look.blend) {
                case ParticleBlend3D::Alpha:
                    return std::string("alpha");
                case ParticleBlend3D::Additive:
                    return std::string("additive");
                case ParticleBlend3D::Premultiplied:
                    return std::string("premultiplied");
                }
                return std::string("invalid");
            },
            [](auto& e, const std::string& v) {
                if (v == "alpha")
                    e.system.desc.look.blend = ParticleBlend3D::Alpha;
                else if (v == "additive")
                    e.system.desc.look.blend = ParticleBlend3D::Additive;
                else if (v == "premultiplied")
                    e.system.desc.look.blend = ParticleBlend3D::Premultiplied;
                else
                    return false;
                return true;
            })
        .property<std::string>(
            "facing",
            [](const auto& e) {
                switch (e.system.desc.look.facing) {
                case ParticleFacing3D::Camera:
                    return std::string("camera");
                case ParticleFacing3D::Stretched:
                    return std::string("stretched");
                case ParticleFacing3D::Axis:
                    return std::string("axis");
                case ParticleFacing3D::Flat:
                    return std::string("flat");
                }
                return std::string("invalid");
            },
            [](auto& e, const std::string& v) {
                if (v == "camera")
                    e.system.desc.look.facing = ParticleFacing3D::Camera;
                else if (v == "stretched")
                    e.system.desc.look.facing = ParticleFacing3D::Stretched;
                else if (v == "axis")
                    e.system.desc.look.facing = ParticleFacing3D::Axis;
                else if (v == "flat")
                    e.system.desc.look.facing = ParticleFacing3D::Flat;
                else
                    return false;
                return true;
            });
}

namespace {
void require(bool valid, const char* why) {
    if (!valid)
        throw std::runtime_error(why);
}
bool positive(Vec3f v) {
    return vector(v) && v.x >= 0 && v.y >= 0 && v.z >= 0;
}
bool positive(LinearRgb c) {
    return scalar(c.r) && scalar(c.g) && scalar(c.b) && c.r >= 0 && c.g >= 0 && c.b >= 0;
}
bool unit(f32 v) {
    return scalar(v) && v >= 0 && v <= 1;
}
void validate(const ParticleEmitter3D& e) {
    const auto& d = e.system.desc;
    const auto& l = d.look;
    require(d.shape <= ParticleShape3D::Disc && l.blend <= ParticleBlend3D::Premultiplied &&
                l.facing <= ParticleFacing3D::Flat,
            "unknown particle enum");
    const auto range = [](ParticleRange3D r) { return scalar(r.min) && scalar(r.max) && r.min <= r.max; };
    require(range(d.lifetime) && d.lifetime.min >= .001f && d.lifetime.max <= 3600,
            "lifetime must be an ordered range in [0.001,3600] seconds");
    require(range(d.speed) && range(d.size) && d.size.min >= 0 && range(d.rotation) && range(d.spin),
            "invalid speed/size/rotation/spin range");
    require(positive(d.extent) && vector(d.velocity_min) && vector(d.velocity_max) && vector(d.gravity) &&
                positive(d.colour_start) && positive(d.colour_end),
            "invalid particle vectors");
    require(d.velocity_min.x <= d.velocity_max.x && d.velocity_min.y <= d.velocity_max.y &&
                d.velocity_min.z <= d.velocity_max.z,
            "velocity ranges are reversed");
    require(scalar(d.cone_angle) && d.cone_angle >= 0 && d.cone_angle <= 180 && scalar(d.spread) && d.spread >= 0 &&
                d.spread <= 180,
            "particle spread/cone must be in [0,180] degrees");
    const f32 nonnegative[] = {
        d.rate,    d.drag,        d.wind_influence,   d.size_start, d.size_mid, d.size_end, d.frames_per_life,
        l.stretch, l.stretch_min, e.system.rate_scale};
    for (auto v : nonnegative)
        require(scalar(v) && v >= 0, "particle rates/forces/size scales/stretch must be finite and nonnegative");
    // Keep the native per-step birth count representable before its u32 cast.
    require(f64(d.rate) * e.system.rate_scale * ParticleSystem3D::step <= 1e6,
            "particle birth rate exceeds step budget");
    require(unit(d.alpha_start) && unit(d.alpha_end) && unit(d.fade_in) && unit(d.colour_variation) && unit(d.bounce) &&
                unit(l.softness) && unit(l.ring) && unit(l.lit) && unit(l.shadow_cutoff),
            "particle alpha/fade/bounce/softness/ring/lit/shadow cutoff must be in [0,1]");
    require(vector(l.axis) && ((l.facing != ParticleFacing3D::Axis && l.facing != ParticleFacing3D::Flat) ||
                               length_squared(l.axis) > 1e-12f),
            "axis/flat particles require a nonzero finite axis");
    require(scalar(d.ground_height) && scalar(l.soft_distance) && l.soft_distance >= 0 &&
                (!l.soft || l.soft_distance > 0),
            "invalid ground/soft-particle distance");
    require(l.atlas_columns && l.atlas_columns <= 256 && l.atlas_rows && l.atlas_rows <= 256 &&
                l.atlas_frames <= u64(l.atlas_columns) * l.atlas_rows,
            "invalid particle atlas dimensions/frame count");
}
} // namespace

std::vector<std::string> validate_particles3d(EcsWorld& world, ParticleLimits3D limits,
                                              std::span<const EcsId> selected) {
    std::vector<std::string> errors;
    u64 capacity = 0;
    size_t count = 0;
    const auto inspect = [&](flecs::entity entity, const ParticleEmitter3D& e) {
        ++count;
        capacity += e.system.desc.max_particles;
        try {
            require(entity.has<Transform3D>(), "particle emitter requires Transform3D");
            validate(e);
            require(e.system.desc.max_particles > 0 && e.system.desc.max_particles <= limits.per_emitter,
                    "particle emitter capacity exceeds per-emitter budget (or is zero)");
            require(e.system.count() <= e.system.desc.max_particles,
                    "live particle count exceeds edited capacity; clear/restart the emitter");
            require(e.texture_asset.id.size() <= 4096, "particle texture path exceeds byte limit");
        } catch (const std::exception& error) {
            errors.push_back("entity '" + EcsEntity{entity}.name() + "', ParticleEmitter3D: " + error.what());
        }
    };
    // Pool/reference entities retain their particle storage while disabled.
    // Include them in configured capacity budgets; prefab templates stay excluded.
    if (selected.empty())
        world.raw().query_builder<const ParticleEmitter3D>().query_flags(EcsQueryMatchDisabled).build().each(inspect);
    else
        for (auto id : selected)
            if (auto e = world.entity_by_id(id); e && e.has<ParticleEmitter3D>())
                inspect(e.raw(), *e.get<ParticleEmitter3D>());
    if (count > limits.emitters)
        errors.push_back("particle scene exceeds emitter budget");
    if (capacity > limits.total_particles)
        errors.push_back("particle scene exceeds total configured particle capacity");
    return errors;
}

std::vector<std::string> resolve_particles3d_assets(EcsWorld& world, Assets3D& assets, ParticleLimits3D limits,
                                                    std::span<const EcsId> selected) {
    auto errors = validate_particles3d(world, limits, selected);
    if (!errors.empty())
        return errors;
    std::vector<EcsEntity> entities;
    if (selected.empty())
        world.raw().each([&](flecs::entity entity, const ParticleEmitter3D&) { entities.emplace_back(entity); });
    else
        for (auto id : selected)
            if (auto e = world.entity_by_id(id); e && e.has<ParticleEmitter3D>())
                entities.push_back(e);
    std::ranges::sort(entities, {}, &EcsEntity::id);
    for (auto entity : entities) {
        // Do not copy the particle vector just to inspect a reference.
        const auto path = entity.get<ParticleEmitter3D>()->texture_asset.id;
        Texture texture;
        if (!path.empty()) {
            std::string error;
            try {
                texture = assets.load_texture(path, entity.get<ParticleEmitter3D>()->texture_nearest, true);
                error = kin::diagnostics3d_text(assets.diagnostics());
            } catch (const std::exception& failure) {
                error = failure.what();
            }
            if (!texture) {
                errors.push_back("entity '" + entity.name() + "', ParticleEmitter3D texture '" + path + "': " + error);
                continue;
            }
        }
        auto* e = entity.get_mut<ParticleEmitter3D>(); // own state before first simulation
        if (!path.empty() || !e->bound_texture.empty())
            e->system.desc.look.texture = std::move(texture);
        e->bound_texture = path;
    }
    return errors;
}
} // namespace kin
