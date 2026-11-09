#include <algorithm>
#include <cmath>
#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/scene3d.hpp>

namespace kin {

void register_scene3d_settings(EcsWorld& world);
void register_character3d_components(EcsWorld& world);
void register_particle3d_components(EcsWorld& world);
void register_surface_details3d_components(EcsWorld& world);

void register_render3d_components(EcsWorld& world) {
    register_scene3d_settings(world);
    register_particle3d_components(world);
    register_surface_details3d_components(world);
    auto& registry = world.components();
    if (!registry.find("Transform3D"))
        registry.native<Transform3D>("Transform3D")
            .field("position", &Transform3D::position)
            .units("m")
            .description("Where it is, from its parent (or the world).")
            .field("rotation", &Transform3D::rotation)
            .description("How it is turned from its parent: a quaternion (x, y, z, w).")
            .field("scale", &Transform3D::scale)
            .description("Its size on each of its axes; 1 is as made.");
    if (!registry.find("ModelRenderer3D"))
        registry.native<ModelRenderer3D>("ModelRenderer3D")
            .field("asset", &ModelRenderer3D::asset)
            .asset_type("Model")
            .description("The model drawn: a .glb or .gltf without a skin (one with a skin is SkinnedMeshRenderer's).")
            .field("tint", &ModelRenderer3D::tint)
            .description("Multiplies its materials' colours.")
            .field("visible", &ModelRenderer3D::visible)
            .field("id", &ModelRenderer3D::id)
            .description("What picking answers for it; 0: the entity's own.");
    if (!registry.find("ModelMaterials3D"))
        registry.native<ModelMaterials3D>("ModelMaterials3D")
            .field("asset", &ModelMaterials3D::asset)
            .asset_type("Materials3D")
            .description("A .kinmaterials file: changes to its model's materials.")
            .field("variant", &ModelMaterials3D::variant)
            .description("Which of the file's variants; empty: its base.");
    if (!registry.find("SkinnedMeshRenderer"))
        registry.native<SkinnedMeshRenderer3D>("SkinnedMeshRenderer")
            .field("asset", &SkinnedMeshRenderer3D::asset)
            .asset_type("Model")
            .description("The model drawn: a .glb or .gltf with a skin (Animator3D moves its joints).")
            .field("tint", &SkinnedMeshRenderer3D::tint)
            .description("Multiplies its materials' colours.")
            .field("visible", &SkinnedMeshRenderer3D::visible)
            .field("id", &SkinnedMeshRenderer3D::id)
            .description("What picking answers for it; 0: the entity's own.");
    if (!registry.find("Animator3D"))
        registry.native<Animator3D>("Animator3D")
            .field("clip", &Animator3D::clip)
            .description("The model's clip it plays, by name.")
            .field("speed", &Animator3D::speed)
            .description("1 plays at the clip's own speed.")
            .field("loop", &Animator3D::loop)
            .field("playing", &Animator3D::playing)
            .field("fade_seconds", &Animator3D::fade_seconds)
            .units("s")
            .description("How long a new clip fades in over the last.")
            .field("apply_root_motion", &Animator3D::apply_root_motion)
            .field("remove_root_motion", &Animator3D::remove_root_motion);
    if (!registry.find("Mirror3D"))
        registry.native<Mirror3D>("Mirror3D")
            .field("enabled", &Mirror3D::enabled)
            .field("size", &Mirror3D::size)
            .units("m")
            .description("How wide and high it is, in the entity's X and Y (its front faces +Z).")
            .field("color", &Mirror3D::color)
            .description("What it reflects with: white silver; a dark colour with metallic 0 for water or glass.")
            .field("metallic", &Mirror3D::metallic)
            .range(0.0, 1.0)
            .description("1: a metal mirror, which reflects all of it; 0: water, glass, a floor (more at a glance).")
            .field("roughness", &Mirror3D::roughness)
            .range(0.045, 1.0)
            .description("0.045 sharp; the picture blurs as it grows and gives way to the sky's past 0.4.")
            .field("normal", &Mirror3D::normal_asset)
            .asset_type("Image")
            .description("A normal map (water's ripples); empty: flat.")
            .field("normal_scale", &Mirror3D::normal_scale)
            .field("uv_scale", &Mirror3D::uv_scale)
            .description("How many times the normal map repeats across it.")
            .field("flow", &Mirror3D::flow)
            .description("How far the normal map moves a second (its repeats).")
            .field("distortion", &Mirror3D::distortion)
            .units("m")
            .description("How far its ripples move where the reflection is read: 0.25 for water.")
            .field("strength", &Mirror3D::strength)
            .range(0.0, 1.0)
            .description("How much of its reflection the picture is (the rest the sky's).")
            .field("resolution", &Mirror3D::resolution)
            .range(0.05, 1.0)
            .description(
                "Its picture's sharpness, a share of the view's (1: as sharp; it covers only where it shows).");
    if (!registry.find("Portal3D"))
        registry.native<Portal3D>("Portal3D")
            .field("enabled", &Portal3D::enabled)
            .field("size", &Portal3D::size)
            .units("m")
            .description("Its opening, wide and high, in the entity's X and Y (its front faces +Z).")
            .field("exit", &Portal3D::exit)
            .description("The entity it opens onto: in at this one's front, out of the exit's front.")
            .field("depth", &Portal3D::depth)
            .range(1.0, 8.0)
            .description("How many levels of it seen through itself are drawn.")
            .field("resolution", &Portal3D::resolution)
            .range(0.05, 1.0)
            .description(
                "Its pictures' sharpness, a share of the view's (1: as sharp; each covers only where it shows).")
            .field("stand_in", &Portal3D::stand_in)
            .description("What it shows where it has no picture (its deepest level, in mirrors).");
    if (!registry.find("AnimatorAsset3D"))
        registry.native<AnimatorAsset3D>("AnimatorAsset3D")
            .field("asset", &AnimatorAsset3D::asset)
            .asset_type("Animator3D")
            .description("A kin.animator3d/1 file (.kinanimator): its states, blends, layers and IK.");
    const auto light_fields = [&]<class T>(const char* name) {
        if (registry.find(name))
            return;
        auto b = registry.native<T>(name);
        b.template property<Vec3f>(
             "color", [](const T& v) { return Vec3f{v.color.r, v.color.g, v.color.b}; },
             [](T& v, const Vec3f& c) {
                 if (c.x < 0 || c.y < 0 || c.z < 0)
                     return false;
                 v.color = {c.x, c.y, c.z};
                 return true;
             })
            .description("Linear; above 1 is brighter.")
            .field("radius", &T::radius)
            .units("m")
            .description("How far it reaches: nothing is lit beyond.")
            .field("size", &T::size)
            .units("m")
            .description("The light's own size: a larger one has softer shadows.")
            .field("enabled", &T::enabled);
        b.template property<bool>(
            "shadow_enabled", [](const T& v) { return v.shadow_enabled; },
            [](T& v, const bool& value) {
                v.shadow_enabled = value;
                return true;
            });
        b.template property<u32>(
            "shadow_resolution", [](const T& v) { return v.shadow_resolution; },
            [](T& v, const u32& value) {
                if (value && (value < 64 || value > 8192))
                    return false;
                v.shadow_resolution = value;
                return true;
            });
        const auto scalar = [&](const char* field, f32 T::*member, bool positive = false) {
            b.template property<f32>(
                field, [member](const T& v) { return v.*member; },
                [member, positive](T& v, const f32& value) {
                    if (!std::isfinite(value) || value < 0 || (positive && value == 0))
                        return false;
                    v.*member = value;
                    return true;
                });
        };
        scalar("shadow_bias", &T::shadow_bias);
        scalar("shadow_normal_offset", &T::shadow_normal_offset);
        scalar("shadow_near_plane", &T::shadow_near_plane, true);
        scalar("shadow_distance", &T::shadow_distance);
        scalar("shadow_softness", &T::shadow_softness);
        scalar("volumetric", &T::volumetric);
        if constexpr (std::is_same_v<T, SpotLight3D>) {
            b.field("inner_angle", &T::inner_angle)
                .units("degrees")
                .description("Inside it, full light; it fades out to the outer angle.")
                .field("outer_angle", &T::outer_angle)
                .units("degrees")
                .description("Beyond it, no light.");
        }
    };
    light_fields.template operator()<PointLight3D>("PointLight3D");
    light_fields.template operator()<SpotLight3D>("SpotLight3D");
}

void register_physics3d_components(EcsWorld& world) {
    register_character3d_components(world);
    auto& registry = world.components();
    if (!registry.find("Collider3D"))
        registry.native<Collider3D>("Collider3D")
            .property<std::string>(
                "kind",
                [](const Collider3D& collider) {
                    switch (collider.kind) {
                    case Collider3D::Kind::Box:
                        return std::string("box");
                    case Collider3D::Kind::Sphere:
                        return std::string("sphere");
                    case Collider3D::Kind::Capsule:
                        return std::string("capsule");
                    case Collider3D::Kind::Cylinder:
                        return std::string("cylinder");
                    default:
                        return std::string("runtime_shape");
                    }
                },
                [](Collider3D& collider, const std::string& kind) {
                    if (kind == "box")
                        collider.kind = Collider3D::Kind::Box;
                    else if (kind == "sphere")
                        collider.kind = Collider3D::Kind::Sphere;
                    else if (kind == "capsule")
                        collider.kind = Collider3D::Kind::Capsule;
                    else if (kind == "cylinder")
                        collider.kind = Collider3D::Kind::Cylinder;
                    else
                        return false;
                    return true;
                })
            .choices({"box", "sphere", "capsule", "cylinder"})
            .field("asset", &Collider3D::asset)
            .asset_type("Collider3D")
            .description(
                "A kin.collider3d/1 recipe (.kincollider): a hull, a mesh or a compound, in place of the kind's shape.")
            .field("half_extents", &Collider3D::half_extents)
            .units("m")
            .field("radius", &Collider3D::radius)
            .units("m")
            .field("half_height", &Collider3D::half_height)
            .units("m")
            .field("offset", &Collider3D::offset)
            .field("rotation", &Collider3D::rotation)
            .field("scale", &Collider3D::scale);
    if (!registry.find("RigidBody3D"))
        registry.native<RigidBody3D>("RigidBody3D")
            .property<std::string>(
                "motion",
                [](const RigidBody3D& body) {
                    return std::string(body.motion == Body3DMotion::Dynamic     ? "dynamic"
                                       : body.motion == Body3DMotion::Kinematic ? "kinematic"
                                                                                : "static");
                },
                [](RigidBody3D& body, const std::string& motion) {
                    if (motion == "dynamic")
                        body.motion = Body3DMotion::Dynamic;
                    else if (motion == "kinematic")
                        body.motion = Body3DMotion::Kinematic;
                    else if (motion == "static")
                        body.motion = Body3DMotion::Static;
                    else
                        return false;
                    return true;
                })
            .choices({"static", "dynamic", "kinematic"})
            .field("mass", &RigidBody3D::mass)
            .units("kg")
            .field("density", &RigidBody3D::density)
            .field("friction", &RigidBody3D::friction)
            .field("restitution", &RigidBody3D::restitution)
            .field("linear_damping", &RigidBody3D::linear_damping)
            .field("angular_damping", &RigidBody3D::angular_damping)
            .field("gravity_factor", &RigidBody3D::gravity_factor)
            .field("sensor", &RigidBody3D::sensor)
            .field("allow_sleeping", &RigidBody3D::allow_sleeping)
            .field("continuous", &RigidBody3D::continuous)
            .field("linear_velocity", &RigidBody3D::linear_velocity)
            .field("angular_velocity", &RigidBody3D::angular_velocity)
            .property<u32>(
                "layer", [](const RigidBody3D& body) { return u32(body.layer); },
                [](RigidBody3D& body, const u32& layer) {
                    if (layer >= 32 && layer != layers3d::Auto)
                        return false;
                    body.layer = u8(layer);
                    return true;
                });
}

static std::vector<std::string> resolve_render3d_assets_impl(EcsWorld& world, Assets3D& assets,
                                                             ParticleLimits3D particles,
                                                             std::span<const EcsId> selected) {
    std::vector<std::string> diagnostics;
    // Gather before structural copy-on-write changes, so inherited prefab data
    // is never mutated while iterating a query or shared with another instance.
    const auto bind = [&]<typename Renderer>(bool skinned) {
        std::vector<EcsEntity> entities;
        if (selected.empty())
            world.each([&](flecs::entity entity, const Renderer&) { entities.emplace_back(entity); });
        else
            for (auto id : selected)
                if (auto e = world.entity_by_id(id); e && e.has<Renderer>())
                    entities.push_back(e);
        std::sort(entities.begin(), entities.end(), [](auto a, auto b) { return a.id() < b.id(); });
        for (auto entity : entities) {
            const auto* original = entity.get<Renderer>();
            if (original->asset.id.empty())
                continue;
            const std::string path = original->asset.id;
            std::string error;
            auto model = assets.load_model(path).value_or({}).shared();
            error = diagnostics3d_text(assets.diagnostics());
            if (model) {
                const bool has_skin = std::any_of(model->parts.begin(), model->parts.end(), [](const auto& part) {
                    return part.skin >= 0 || part.mesh.skinned();
                });
                if (has_skin != skinned || (skinned && model->skeletons.empty())) {
                    error = skinned ? "SkinnedMeshRenderer requires a skinned model with a skeleton"
                                    : "ModelRenderer3D requires a static model";
                    model.reset();
                }
            }
            if (!model) {
                diagnostics.push_back("entity '" + entity.name() + "' (" + std::to_string(entity.id()) + "), " +
                                      (skinned ? "SkinnedMeshRenderer" : "ModelRenderer3D") + " asset '" + path +
                                      "': " + error);
                continue;
            }
            auto* renderer = entity.get_mut<Renderer>();
            const bool changed_model = renderer->model != model;
            if constexpr (std::is_same_v<Renderer, SkinnedMeshRenderer3D>) {
                if (renderer->model != model) {
                    if (auto* animator = entity.get_mut<Animator3D>()) {
                        animator->player = AnimationPlayer3D{};
                        animator->bound = nullptr;
                        animator->applied.clear();
                        animator->applied_layers.clear();
                        animator->ik_pose = {};
                        animator->ik_world.clear();
                        animator->ik_dt = 0;
                        animator->pose_override = {};
                        for (auto& constraint : animator->ik_constraints) {
                            constraint.current = 0;
                            constraint.target_valid = false;
                            constraint.has_last_target = false;
                            constraint.look_state = {};
                            constraint.foot_state = {};
                            constraint.bend_state = {};
                            constraint.ground_cache.clear();
                            constraint.resolved.clear();
                            constraint.resolved_toes.clear();
                            constraint.resolved_for = nullptr;
                        }
                    }
                }
            }
            entity.get_mut<Renderer>()->model = std::move(model);
            if (changed_model)
                entity.modified<Renderer>();
        }
    };
    bind.operator()<ModelRenderer3D>(false);
    bind.operator()<SkinnedMeshRenderer3D>(true);
    // Mirrors' normal maps (linear data). A failed load keeps the previous map.
    {
        std::vector<EcsEntity> mirrors;
        if (selected.empty())
            world.each([&](flecs::entity entity, const Mirror3D&) { mirrors.emplace_back(entity); });
        else
            for (auto id : selected)
                if (auto e = world.entity_by_id(id); e && e.has<Mirror3D>())
                    mirrors.push_back(e);
        std::sort(mirrors.begin(), mirrors.end(), [](auto a, auto b) { return a.id() < b.id(); });
        for (auto entity : mirrors) {
            const std::string path = entity.get<Mirror3D>()->normal_asset.id;
            if (path.empty() && entity.get<Mirror3D>()->bound_normal.empty())
                continue; // a C++ map, or none
            Texture normal;
            if (!path.empty()) {
                normal = assets.load_texture(path, false, false).value_or({});
                if (!normal) {
                    diagnostics.push_back("entity '" + entity.name() + "' (" + std::to_string(entity.id()) +
                                          "), Mirror3D normal '" + path +
                                          "': " + diagnostics3d_text(assets.diagnostics()));
                    continue;
                }
            }
            auto* mirror = entity.get_mut<Mirror3D>();
            mirror->normal = std::move(normal);
            mirror->bound_normal = path;
            entity.modified<Mirror3D>();
        }
    }
    auto material_errors = resolve_materials3d_assets(world, assets, selected).diagnostics;
    diagnostics.insert(diagnostics.end(), material_errors.begin(), material_errors.end());
    auto animation_errors = resolve_animator3d_assets(world, assets.root(), selected).diagnostics;
    diagnostics.insert(diagnostics.end(), animation_errors.begin(), animation_errors.end());
    auto effects = resolve_particles3d_assets(world, assets, particles, selected).diagnostics;
    diagnostics.insert(diagnostics.end(), effects.begin(), effects.end());
    auto decals = resolve_decals3d_assets(world, assets, selected).diagnostics;
    diagnostics.insert(diagnostics.end(), decals.begin(), decals.end());
    return diagnostics;
}
Status3D resolve_render3d_assets(EcsWorld& world, Assets3D& assets, ParticleLimits3D particles,
                                 std::span<const EcsId> selected) {
    auto diagnostics = resolve_render3d_assets_impl(world, assets, particles, selected);
    const bool accepted = diagnostics.empty();
    return status3d(accepted, std::move(diagnostics));
}

static std::vector<std::string> resolve_materials3d_assets_impl(EcsWorld& world, Assets3D& assets,
                                                                std::span<const EcsId> selected) {
    std::vector<std::string> diagnostics;
    std::vector<EcsEntity> entities;
    if (selected.empty())
        world.each([&](flecs::entity entity, const ModelMaterials3D&) { entities.emplace_back(entity); });
    else
        for (auto id : selected)
            if (auto e = world.entity_by_id(id); e && e.has<ModelMaterials3D>())
                entities.push_back(e);
    std::ranges::sort(entities, {}, &EcsEntity::id);
    for (auto entity : entities) {
        const auto reference = *entity.get<ModelMaterials3D>(); // COW may move the table
        if (reference.asset.id.empty()) {
            if (reference.binding) {
                entity.get_mut<ModelMaterials3D>()->binding.reset();
                entity.modified<ModelMaterials3D>();
            }
            continue;
        }
        const auto* statics = entity.get<ModelRenderer3D>();
        const auto* skinned = entity.get<SkinnedMeshRenderer3D>();
        std::string error;
        std::shared_ptr<const MaterialBinding3D> binding;
        if (bool(statics) == bool(skinned))
            error = "requires exactly one ModelRenderer3D or SkinnedMeshRenderer";
        else {
            binding =
                assets.load_materials(reference.asset.id, statics ? statics->model : skinned->model, reference.variant)
                    .value_or({})
                    .shared();
            error = diagnostics3d_text(assets.diagnostics());
        }
        if (!binding)
            diagnostics.push_back("entity '" + entity.name() + "', ModelMaterials3D asset '" + reference.asset.id +
                                  "': " + error);
        else if (reference.binding != binding) {
            entity.get_mut<ModelMaterials3D>()->binding = std::move(binding);
            entity.modified<ModelMaterials3D>();
        }
    }
    return diagnostics;
}
Status3D resolve_materials3d_assets(EcsWorld& world, Assets3D& assets, std::span<const EcsId> selected) {
    auto diagnostics = resolve_materials3d_assets_impl(world, assets, selected);
    const bool accepted = diagnostics.empty();
    return status3d(accepted, std::move(diagnostics));
}

} // namespace kin
