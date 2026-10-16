#include <kin/ecs/scene3d.hpp>
#include <kin/ecs/physics3d.hpp>

#include <algorithm>

namespace kin {

void register_scene3d_settings(EcsWorld& world);
void register_character3d_components(EcsWorld& world);
void register_particle3d_components(EcsWorld& world);

void register_render3d_components(EcsWorld& world) {
    register_scene3d_settings(world);
    register_particle3d_components(world);
    auto& registry = world.components();
    if (!registry.find("Transform3D")) registry.native<Transform3D>("Transform3D")
        .field("position", &Transform3D::position).units("m")
        .field("rotation", &Transform3D::rotation)
        .field("scale", &Transform3D::scale);
    if (!registry.find("ModelRenderer3D")) registry.native<ModelRenderer3D>("ModelRenderer3D")
        .field("asset", &ModelRenderer3D::asset).field("tint", &ModelRenderer3D::tint)
        .field("visible", &ModelRenderer3D::visible).field("id", &ModelRenderer3D::id);
    if (!registry.find("ModelMaterials3D")) registry.native<ModelMaterials3D>("ModelMaterials3D")
        .field("asset", &ModelMaterials3D::asset).field("variant", &ModelMaterials3D::variant);
    if (!registry.find("SkinnedMeshRenderer")) registry.native<SkinnedMeshRenderer>("SkinnedMeshRenderer")
        .field("asset", &SkinnedMeshRenderer::asset).field("tint", &SkinnedMeshRenderer::tint)
        .field("visible", &SkinnedMeshRenderer::visible).field("id", &SkinnedMeshRenderer::id);
    if (!registry.find("Animator3D")) registry.native<Animator3D>("Animator3D")
        .field("clip", &Animator3D::clip).field("speed", &Animator3D::speed)
        .field("loop", &Animator3D::loop).field("playing", &Animator3D::playing)
        .field("fade_seconds", &Animator3D::fade_seconds).units("s")
        .field("apply_root_motion", &Animator3D::apply_root_motion)
        .field("remove_root_motion", &Animator3D::remove_root_motion);
    if (!registry.find("AnimatorAsset3D")) registry.native<AnimatorAsset3D>("AnimatorAsset3D")
        .field("asset", &AnimatorAsset3D::asset);
    if (!registry.find("PointLight3D")) registry.native<PointLight3D>("PointLight3D")
        .field("color", &PointLight3D::color).field("radius", &PointLight3D::radius).units("m")
        .field("size", &PointLight3D::size).units("m").field("enabled", &PointLight3D::enabled)
        .property<bool>("shadows", [](const PointLight3D& light) { return light.shadow.enabled; },
            [](PointLight3D& light, const bool& enabled) { light.shadow.enabled = enabled; return true; });
    if (!registry.find("SpotLight3D")) registry.native<SpotLight3D>("SpotLight3D")
        .field("color", &SpotLight3D::color).field("radius", &SpotLight3D::radius).units("m")
        .field("size", &SpotLight3D::size).units("m").field("enabled", &SpotLight3D::enabled)
        .field("inner_angle", &SpotLight3D::inner_angle).units("degrees")
        .field("outer_angle", &SpotLight3D::outer_angle).units("degrees")
        .property<bool>("shadows", [](const SpotLight3D& light) { return light.shadow.enabled; },
            [](SpotLight3D& light, const bool& enabled) { light.shadow.enabled = enabled; return true; });
}

void register_physics3d_components(EcsWorld& world) {
    register_character3d_components(world);
    auto& registry = world.components();
    if (!registry.find("Collider3D")) registry.native<Collider3D>("Collider3D")
        .property<std::string>("kind", [](const Collider3D& collider) {
            switch (collider.kind) {
            case Collider3D::Kind::Box: return std::string("box");
            case Collider3D::Kind::Sphere: return std::string("sphere");
            case Collider3D::Kind::Capsule: return std::string("capsule");
            case Collider3D::Kind::Cylinder: return std::string("cylinder");
            default: return std::string("runtime_shape");
            }
        }, [](Collider3D& collider, const std::string& kind) {
            if (kind == "box") collider.kind = Collider3D::Kind::Box;
            else if (kind == "sphere") collider.kind = Collider3D::Kind::Sphere;
            else if (kind == "capsule") collider.kind = Collider3D::Kind::Capsule;
            else if (kind == "cylinder") collider.kind = Collider3D::Kind::Cylinder;
            else return false;
            return true;
        })
        .field("asset", &Collider3D::asset)
        .field("half_extents", &Collider3D::half_extents).units("m")
        .field("radius", &Collider3D::radius).units("m")
        .field("half_height", &Collider3D::half_height).units("m")
        .field("offset", &Collider3D::offset).field("rotation", &Collider3D::rotation).field("scale", &Collider3D::scale);
    if (!registry.find("RigidBody3D")) registry.native<RigidBody3D>("RigidBody3D")
        .property<std::string>("motion", [](const RigidBody3D& body) {
            return std::string(body.motion == Body3DMotion::Dynamic ? "dynamic" : body.motion == Body3DMotion::Kinematic ? "kinematic" : "static");
        }, [](RigidBody3D& body, const std::string& motion) {
            if (motion == "dynamic") body.motion = Body3DMotion::Dynamic;
            else if (motion == "kinematic") body.motion = Body3DMotion::Kinematic;
            else if (motion == "static") body.motion = Body3DMotion::Static;
            else return false;
            return true;
        })
        .field("mass", &RigidBody3D::mass).units("kg").field("density", &RigidBody3D::density)
        .field("friction", &RigidBody3D::friction).field("restitution", &RigidBody3D::restitution)
        .field("linear_damping", &RigidBody3D::linear_damping).field("angular_damping", &RigidBody3D::angular_damping)
        .field("gravity_factor", &RigidBody3D::gravity_factor).field("sensor", &RigidBody3D::sensor)
        .field("allow_sleeping", &RigidBody3D::allow_sleeping).field("continuous", &RigidBody3D::continuous)
        .field("linear_velocity", &RigidBody3D::linear_velocity).field("angular_velocity", &RigidBody3D::angular_velocity)
        .property<u32>("layer",[](const RigidBody3D& body){return u32(body.layer);},[](RigidBody3D& body,const u32& layer){
            if(layer>=32 && layer!=layers3d::Auto)return false;
            body.layer=u8(layer);return true;
        });
}

std::vector<std::string> resolve_render3d_assets(EcsWorld& world, Assets3D& assets,ParticleLimits3D particles,std::span<const EcsId> selected) {
    std::vector<std::string> diagnostics;
    // Gather before structural copy-on-write changes, so inherited prefab data
    // is never mutated while iterating a query or shared with another instance.
    const auto bind = [&]<typename Renderer>(bool skinned) {
        std::vector<EcsEntity> entities;
        if(selected.empty())world.raw().each([&](flecs::entity entity, const Renderer&) { entities.emplace_back(entity); });
        else for(auto id:selected)if(auto e=world.entity_by_id(id);e&&e.has<Renderer>())entities.push_back(e);
        std::sort(entities.begin(), entities.end(), [](auto a, auto b) { return a.id() < b.id(); });
        for (auto entity : entities) {
            const auto* original = entity.get<Renderer>();
            if (original->asset.id.empty()) continue;
            const std::string path = original->asset.id;
            std::string error;
            auto model = assets.load_model(path, &error).shared();
            if (model) {
                const bool has_skin = std::any_of(model->parts.begin(), model->parts.end(), [](const auto& part) { return part.skin >= 0 || part.mesh.skinned(); });
                if (has_skin != skinned || (skinned && model->skeletons.empty())) {
                    error = skinned ? "SkinnedMeshRenderer requires a skinned model with a skeleton" : "ModelRenderer3D requires a static model";
                    model.reset();
                }
            }
            if (!model) {
                diagnostics.push_back("entity '" + entity.name() + "' (" + std::to_string(entity.id()) + "), " +
                    (skinned ? "SkinnedMeshRenderer" : "ModelRenderer3D") + " asset '" + path + "': " + error);
                continue;
            }
            auto* renderer = entity.get_mut<Renderer>();
            if constexpr (std::is_same_v<Renderer, SkinnedMeshRenderer>) {
                if (renderer->model != model) {
                    if (auto* animator = entity.get_mut<Animator3D>()) {
                        animator->player = AnimationPlayer3D{};
                        animator->bound = nullptr; animator->applied.clear(); animator->applied_layers.clear();
                        animator->ik_pose = {}; animator->ik_world.clear(); animator->ik_dt = 0;
                        animator->pose_override = {};
                        for (auto& constraint : animator->ik_constraints) {
                            constraint.current = 0; constraint.target_valid = false; constraint.has_last_target = false;
                            constraint.look_state = {}; constraint.foot_state = {}; constraint.bend_state = {};
                            constraint.ground_cache.clear(); constraint.resolved.clear(); constraint.resolved_toes.clear();
                            constraint.resolved_for = nullptr;
                        }
                    }
                }
            }
            entity.get_mut<Renderer>()->model = std::move(model);
        }
    };
    bind.operator()<ModelRenderer3D>(false);
    bind.operator()<SkinnedMeshRenderer>(true);
    auto material_errors = resolve_materials3d_assets(world, assets,selected);
    diagnostics.insert(diagnostics.end(), material_errors.begin(), material_errors.end());
    auto animation_errors = resolve_animator3d_assets(world, assets.root(),selected);
    diagnostics.insert(diagnostics.end(), animation_errors.begin(), animation_errors.end());
    auto effects=resolve_particles3d_assets(world,assets,particles,selected);
    diagnostics.insert(diagnostics.end(),effects.begin(),effects.end());
    return diagnostics;
}

std::vector<std::string> resolve_materials3d_assets(EcsWorld& world, Assets3D& assets,std::span<const EcsId> selected) {
    std::vector<std::string> diagnostics;
    std::vector<EcsEntity> entities;
    if(selected.empty())world.raw().each([&](flecs::entity entity, const ModelMaterials3D&) { entities.emplace_back(entity); });
    else for(auto id:selected)if(auto e=world.entity_by_id(id);e&&e.has<ModelMaterials3D>())entities.push_back(e);
    std::ranges::sort(entities, {}, &EcsEntity::id);
    for (auto entity : entities) {
        const auto reference = *entity.get<ModelMaterials3D>(); // COW may move the table
        if (reference.asset.id.empty()) {
            if (reference.binding) entity.get_mut<ModelMaterials3D>()->binding.reset();
            continue;
        }
        const auto* statics = entity.get<ModelRenderer3D>();
        const auto* skinned = entity.get<SkinnedMeshRenderer>();
        std::string error;
        std::shared_ptr<const MaterialBinding3D> binding;
        if (bool(statics) == bool(skinned)) error = "requires exactly one ModelRenderer3D or SkinnedMeshRenderer";
        else binding = assets.load_materials(reference.asset.id, statics ? statics->model : skinned->model, reference.variant, &error).shared();
        if (!binding) diagnostics.push_back("entity '" + entity.name() + "', ModelMaterials3D asset '" + reference.asset.id + "': " + error);
        else entity.get_mut<ModelMaterials3D>()->binding = std::move(binding);
    }
    return diagnostics;
}

} // namespace kin
