#include <kin/ecs/render3d.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace kin {

namespace {

// The clip a state plays when it has no blend space: the machine's animation name for it, else its own name.
std::string clip_of_state(const Animator3D& a, const std::string& state) {
    if (a.machine) {
        const auto found = a.machine->states.find(state);
        if (found != a.machine->states.end() && !found->second.empty()) {
            return found->second;
        }
    }
    return state;
}

f32 fade_for(const Animator3D& a, const std::string& from, const std::string& to) {
    for (const Animator3D::Fade& fade : a.fades) {
        if ((fade.from.empty() || fade.from == from) && (fade.to.empty() || fade.to == to)) {
            return fade.seconds;
        }
    }
    return a.fade_seconds;
}

// What the base layer should be playing: the machine's state (stepped by one transition), or the clip named.
// The key identifies it (a change of key crossfades); `blend` is the state's blend space, if any.
struct Wanted {
    std::string key;
    const Animator3D::BlendSpace* blend = nullptr;
    std::string clip;
};

} // namespace

WorldRenderState3D::WorldRenderState3D(flecs::world& world)
    : _transform_observer(world.observer<Transform3D>("add_world_transform_3d")
                              .event(flecs::OnAdd)
                              .yield_existing()
                              .each([](flecs::entity entity, Transform3D&) { entity.ensure<WorldTransform3D>(); })),
      _transforms(world.query_builder<WorldTransform3D, const Transform3D, const WorldTransform3D>()
                      .term_at(2).parent().cascade().optional()
                      .cached()
                      .build()),
      _meshes(world.query_builder<const WorldTransform3D, const MeshRenderer>().cached().build()),
      _animators(world.query_builder<const SkinnedMeshRenderer, Animator3D, Transform3D*>().build()),
      _skinned(world.query_builder<const WorldTransform3D, const SkinnedMeshRenderer, Animator3D*>().build()),
      _time_of_day(world.query_builder<TimeOfDay3D>().build()),
      _point_lights(world.query_builder<const WorldTransform3D, const PointLight3D>().build()),
      _spot_lights(world.query_builder<const WorldTransform3D, const SpotLight3D>().build()) {}

void WorldRenderState3D::advance_time_of_day(f32 dt) {
    _time_of_day.each([dt](TimeOfDay3D& time) { kin::advance(time, dt); });
}

bool WorldRenderState3D::apply_time_of_day(Scene3D& scene) const {
    bool applied = false;
    _time_of_day.each([&](TimeOfDay3D& time) {
        if (!applied) {
            kin::apply(time, scene.lights, scene.sky, scene.fog);
            applied = true;
        }
    });
    return applied;
}

void WorldRenderState3D::animate(f32 dt) {
    ++_epoch;
    _animators.each([&](flecs::iter& it, size_t, const SkinnedMeshRenderer& renderer, Animator3D& a, Transform3D* own) {
        const Model* model = renderer.model.get();
        if (!model || model->skeletons.empty()) {
            return;
        }
        const Skeleton& skeleton = model->skeletons[0];
        if (a.bound != model || a.player.skeleton() != &skeleton) {
            // Bound to this model's skeleton (its first), whatever played before: the pose and the layers start afresh.
            a.player.set_skeleton(&skeleton);
            a.player.clear_layers();
            a.bound = model;
            a.applied.clear();
            a.applied_layers.clear();
        }
        // The state machine: one transition a step (kin/anim's evaluate, consuming matched triggers), never to the state it is in.
        Wanted want;
        f32 fade = a.fade_seconds;
        if (a.machine) {
            if (a.state.empty()) {
                a.state = a.machine->initial;
            }
            if (const AnimTransition* t = evaluate(*a.machine, a.state, a.params)) {
                if (t->to != a.state && !t->to.empty()) {
                    fade = fade_for(a, a.state, t->to);
                    a.state = t->to;
                }
            }
            const auto space = a.blend_spaces.find(a.state);
            want.key = "state:" + a.state;
            if (space != a.blend_spaces.end() && !space->second.entries.empty()) {
                want.blend = &space->second;
            } else {
                want.clip = clip_of_state(a, a.state);
            }
        } else {
            want.key = "clip:" + a.clip;
            want.clip = a.clip;
        }
        const bool first = a.applied.empty();
        if (want.key != a.applied) {
            a.applied = want.key;
            const f32 seconds = first ? 0.0f : fade;
            if (want.blend) {
                std::vector<BlendPoint3D> points;
                for (const Animator3D::BlendEntry& e : want.blend->entries) {
                    points.push_back({model->find_clip(e.clip), e.position});
                }
                a.player.crossfade_to_blend(points, a.params.get_float(want.blend->parameter), seconds, a.loop);
            } else {
                a.player.crossfade_to(model->find_clip(want.clip), seconds, a.loop);
            }
        }
        if (a.machine && want.blend) {
            a.player.set_blend_parameter(a.params.get_float(want.blend->parameter));
        }
        a.player.set_speed(a.speed);
        a.player.set_root_motion(a.apply_root_motion || a.remove_root_motion, a.root_motion_axes);
        // Layers over the base, as data: the clip by name and the mask by its joint, looked up when they change.
        a.applied_layers.resize(a.layers.size());
        for (std::size_t i = 0; i < a.layers.size(); ++i) {
            const Animator3D::LayerConfig& config = a.layers[i];
            AnimationPlayer3D::Layer& layer = a.player.layer(static_cast<u32>(i));
            const std::string key = config.clip + "|" + config.mask_joint + (config.include_root ? "+" : "-");
            if (a.applied_layers[i] != key) {
                a.applied_layers[i] = key;
                layer.clip = config.clip.empty() ? nullptr : model->find_clip(config.clip);
                layer.time = 0.0f;
                const i32 joint = config.mask_joint.empty() ? -1 : skeleton.find(config.mask_joint);
                layer.mask = joint >= 0 ? PoseMask::subtree(skeleton, joint, 1.0f, config.include_root) : PoseMask{};
            }
            layer.weight = config.weight;
            layer.speed = config.speed;
            layer.loop = config.loop;
            layer.additive = config.additive;
        }
        for (std::size_t i = a.layers.size(); i < a.player.layer_count(); ++i) {
            a.player.layer(static_cast<u32>(i)).clip = nullptr;
        }
        if (a.playing) {
            a.player.update(dt);
            if (a.apply_root_motion) {
                const RootMotion& m = a.player.root_motion();
                if (own) {
                    // The motion is in the root's parent space (the model's): turned and scaled as the entity is.
                    own->position += own->rotation.rotate(Vec3f{m.translation.x * own->scale.x, m.translation.y * own->scale.y, m.translation.z * own->scale.z});
                    if (a.root_motion_axes.rotation) {
                        own->rotation = (own->rotation * m.rotation).normalized();
                    }
                }
            }
        }
        if (!a.ik_constraints.empty()) {
            // The weights ease toward their goals (a target that is gone has the goal 0); the solving itself waits for
            // collect(), when the entity's WorldTransform3D of this frame is known.
            a.ik_dt += dt;
            const flecs::world ecs = it.world();
            for (IKConstraint& k : a.ik_constraints) {
                k.target_valid = false;
                if (k.target.kind == IKTarget::Kind::World) {
                    k.target_valid = true;
                } else if (k.target.kind == IKTarget::Kind::Entity) {
                    k.target_valid = ecs.is_alive(k.target.entity) && ecs.entity(k.target.entity).get<WorldTransform3D>() != nullptr;
                }
                // A target that is gone (or was never given) lets go: the goal is 0, and the fade runs from where it was.
                const bool needs_target = k.kind != IKKind::FootPlacer;
                const f32 want = needs_target && !k.target_valid ? 0.0f : std::clamp(k.weight, 0.0f, 1.0f);
                if (k.fade_seconds > 0.0f) {
                    const f32 step = dt / k.fade_seconds;
                    k.current += std::clamp(want - k.current, -step, step);
                } else {
                    k.current = want;
                }
            }
        }
    });
}

void WorldRenderState3D::solve_ik(Animator3D& a, const Skeleton& sk, const Pose& base, const Mat4f& entity_matrix, const flecs::world& ecs,
                                  u64 epoch) {
    a.ik_pose = base;
    compute_world_matrices(sk, a.ik_pose, a.ik_world);
    const Mat4f to_model = entity_matrix.inverse();
    const f32 dt = a.ik_dt;
    for (IKConstraint& k : a.ik_constraints) {
        const f32 c = std::clamp(k.current, 0.0f, 1.0f);
        const f32 w = c * c * (3.0f - 2.0f * c);
        if (k.resolved_for != &sk) {
            k.resolved.clear();
            for (const std::string& name : k.joints) {
                k.resolved.push_back(sk.find(name));
            }
            k.resolved_toes.clear();
            for (const std::string& name : k.toe_joints) {
                k.resolved_toes.push_back(name.empty() ? -1 : sk.find(name));
            }
            k.resolved_for = &sk;
        }
        const auto resolved = [&](std::size_t count) {
            if (k.resolved.size() < count) {
                return false;
            }
            return std::none_of(k.resolved.begin(), k.resolved.begin() + static_cast<std::ptrdiff_t>(count), [](i32 j) { return j < 0; });
        };
        if (w <= 0.0f) {
            k.look_state.valid = false;
            k.foot_state.valid = false;
            k.bend_state.valid = false;
            continue;
        }
        // The target in the model's space: the point of the world, or the entity's grip point; a vanished entity is
        // left from the point it was last at (the weight is fading out).
        Vec3f target_world{};
        bool has_target = false;
        if (k.kind != IKKind::FootPlacer) {
            if (k.target.kind == IKTarget::Kind::World) {
                target_world = k.target.position;
                has_target = true;
            } else if (k.target.kind == IKTarget::Kind::Entity && ecs.is_alive(k.target.entity)) {
                if (const WorldTransform3D* t = ecs.entity(k.target.entity).get<WorldTransform3D>()) {
                    target_world = t->matrix.apply(k.target.position);
                    has_target = true;
                }
            }
            if (has_target) {
                k.last_target = target_world;
                k.has_last_target = true;
            } else if (k.has_last_target) {
                target_world = k.last_target;
                has_target = true;
            }
            if (!has_target) {
                continue;
            }
        }
        const Vec3f target = to_model.apply(target_world);
        const auto pole_mode = [&](TwoBoneOptions& o) {
            switch (k.pole) {
            case IKConstraint::Pole::WorldPosition:
                o.pole_mode = PoleMode::Position;
                o.pole = to_model.apply(k.pole_vector);
                break;
            case IKConstraint::Pole::ModelDirection:
                o.pole_mode = PoleMode::Direction;
                o.pole = k.pole_vector;
                break;
            case IKConstraint::Pole::None:
                o.pole_mode = PoleMode::None;
                break;
            }
        };
        switch (k.kind) {
        case IKKind::TwoBone: {
            if (!resolved(3)) {
                break;
            }
            TwoBoneOptions o = k.two_bone;
            o.weight = w;
            pole_mode(o);
            if (k.match_rotation) {
                o.match_rotation = true;
                o.end_rotation = (rotation_of(to_model) * k.target_rotation).normalized();
                o.rotation_weight = k.rotation_weight;
            }
            solve_two_bone(sk, a.ik_pose, a.ik_world, {k.resolved[0], k.resolved[1], k.resolved[2]}, target, o, &k.bend_state, dt);
            break;
        }
        case IKKind::Fabrik: {
            if (k.resolved.size() < 2 || !resolved(k.resolved.size())) {
                break;
            }
            FabrikOptions o = k.fabrik;
            o.weight = k.fabrik.weight * w;
            switch (k.pole) {
            case IKConstraint::Pole::WorldPosition:
                o.pole_mode = PoleMode::Position;
                o.pole = to_model.apply(k.pole_vector);
                break;
            case IKConstraint::Pole::ModelDirection:
                o.pole_mode = PoleMode::Direction;
                o.pole = k.pole_vector;
                break;
            case IKConstraint::Pole::None:
                o.pole_mode = PoleMode::None;
                break;
            }
            solve_chain_fabrik(sk, a.ik_pose, a.ik_world, k.resolved, target, o);
            break;
        }
        case IKKind::LookAt: {
            if (!resolved(1) || !resolved(k.resolved.size())) {
                break;
            }
            std::array<LookAtJoint, 8> chain{};
            const std::size_t n = std::min<std::size_t>(k.resolved.size(), chain.size());
            for (std::size_t i = 0; i < n; ++i) {
                chain[i] = i < k.look_links.size() ? k.look_links[i] : LookAtJoint{};
                chain[i].joint = k.resolved[i];
            }
            LookAtOptions o = k.look_at;
            o.weight = k.look_at.weight * w;
            LookAtState state = k.look_state;
            if (state.valid) {
                state.direction = normalize(to_model.apply_vector(state.direction));
            }
            solve_look_at(sk, a.ik_pose, a.ik_world, std::span<const LookAtJoint>{chain.data(), n}, target, o, &state, dt);
            k.look_state.direction = normalize(entity_matrix.apply_vector(state.direction));
            k.look_state.valid = state.valid;
            break;
        }
        case IKKind::FootPlacer: {
            const std::size_t legs = k.resolved.size() >= 4 ? (k.resolved.size() - 1) / 3 : 0;
            if (legs == 0 || !k.ground || !resolved(1 + legs * 3)) {
                break;
            }
            std::array<FootLeg, 4> feet{};
            const std::size_t n = std::min<std::size_t>(legs, feet.size());
            for (std::size_t i = 0; i < n; ++i) {
                feet[i].leg = {k.resolved[1 + i * 3], k.resolved[2 + i * 3], k.resolved[3 + i * 3]};
                feet[i].foot_height = i < k.foot_heights.size() ? k.foot_heights[i] : 0.1f;
                feet[i].toe = i < k.resolved_toes.size() ? k.resolved_toes[i] : -1;
                feet[i].toe_height = i < k.toe_heights.size() ? k.toe_heights[i] : 0.03f;
            }
            const std::span<const FootLeg> leg_span{feet.data(), n};
            // The game's query is in the world: the segment goes out, the hit comes back into the model's space. Its answers are
            // kept for the frame (the epoch: animate() and propagate_transforms() advance it) by the order they were asked in, for
            // the same segment: a second solve in the frame (collect() after apply_ik(), two views) does not ask again.
            std::size_t asked = 0;
            const GroundQuery model_ground = [&](Vec3f from, Vec3f to) -> std::optional<GroundHit> {
                const Vec3f world_from = entity_matrix.apply(from), world_to = entity_matrix.apply(to);
                if (k.ground_cache.size() <= asked) {
                    k.ground_cache.resize(asked + 1);
                }
                IKConstraint::GroundCache& cached = k.ground_cache[asked++];
                if (cached.epoch != epoch || !(cached.from == world_from) || !(cached.to == world_to)) {
                    cached.hit = k.ground(world_from, world_to);
                    cached.epoch = epoch;
                    cached.from = world_from;
                    cached.to = world_to;
                }
                const std::optional<GroundHit>& hit = cached.hit;
                if (!hit) {
                    return std::nullopt;
                }
                return GroundHit{to_model.apply(hit->position), normalize(entity_matrix.transposed().apply_vector(hit->normal))};
            };
            FootPlan plan = plan_foot_placement(sk, a.ik_pose, a.ik_world, leg_span, model_ground, k.foot, &k.foot_state, dt, &entity_matrix);
            plan.pelvis_offset *= w;
            TwoBoneOptions o = k.two_bone;
            o.weight = w;
            pole_mode(o);
            apply_foot_placement(sk, a.ik_pose, a.ik_world, k.resolved[0], leg_span, plan, o, k.foot.up);
            break;
        }
        }
    }
    a.ik_dt = 0.0f;
}

void WorldRenderState3D::solve_pose(Animator3D& a, const Skeleton& sk, const Pose& base, const Mat4f& entity_matrix,
                                    const flecs::world& ecs, u64 epoch) {
    if (!a.ik_constraints.empty()) {
        solve_ik(a, sk, base, entity_matrix, ecs, epoch);
    } else {
        a.ik_pose = base;
        compute_world_matrices(sk, a.ik_pose, a.ik_world);
    }
    const PoseOverride3D& o = a.pose_override;
    const u32 n = sk.joint_count();
    if (o.weights.size() != n || o.position.size() != n || o.rotation.size() != n) {
        return;
    }
    const Mat4f to_model = entity_matrix.inverse();
    // Parent first. A joint with a share takes the body's rotation (more or less of it), and the body's place when its parent
    // has less share than it does: the top of the ragdolled part. The rest of the part keeps the pose's bone lengths.
    for (u32 j = 0; j < n; ++j) {
        const i32 parent = sk.parents[j];
        const Mat4f parent_world = parent >= 0 ? a.ik_world[static_cast<std::size_t>(parent)] : sk.root_transform;
        const f32 w = std::clamp(o.weights[j], 0.0f, 1.0f);
        JointPose& jp = a.ik_pose.joints[j];
        if (w > 0.0f) {
            const Mat4f body = to_model * Mat4f::trs(o.position[j], o.rotation[j]);
            const Mat4f local = parent_world.inverse() * body;
            const Quatf target = rotation_of(local);
            const f32 parent_w = parent >= 0 ? std::clamp(o.weights[static_cast<std::size_t>(parent)], 0.0f, 1.0f) : 0.0f;
            if (parent_w < w) {
                jp.translation = lerp(jp.translation, Vec3f{local.m[12], local.m[13], local.m[14]}, w - parent_w);
            }
            jp.rotation = w >= 1.0f ? target : slerp(jp.rotation, target, w);
        }
        a.ik_world[j] = parent_world * jp.matrix();
    }
}

void WorldRenderState3D::apply_ik() {
    _skinned.each([&](flecs::iter& it, size_t, const WorldTransform3D& world, const SkinnedMeshRenderer& renderer, Animator3D* animator) {
        const Model* model = renderer.model.get();
        if (!animator || (animator->ik_constraints.empty() && animator->pose_override.empty()) || !model || model->skeletons.empty()) {
            return;
        }
        const Skeleton& skeleton = model->skeletons[0];
        if (animator->bound == model && animator->player.skeleton() == &skeleton) {
            solve_pose(*animator, skeleton, animator->player.pose(), world.matrix, it.world(), _epoch);
        }
    });
}

void WorldRenderState3D::propagate_transforms() {
    ++_epoch;
    // Tables come parents first (cascade), and every entity in a table shares
    // its parent: one tight loop a table.
    _transforms.run([](flecs::iter& it) {
        while (it.next()) {
            const auto self = it.field<WorldTransform3D>(0);
            const auto local = it.field<const Transform3D>(1);
            if (!it.is_set(2)) {
                for (const auto i : it) {
                    self[i].matrix = local[i].matrix();
                }
                continue;
            }
            const Mat4f parent = it.field<const WorldTransform3D>(2)[0].matrix;
            for (const auto i : it) {
                self[i].matrix = parent * local[i].matrix();
            }
        }
    });
}

void WorldRenderState3D::collect(Scene3D& scene) {
    for (Bucket& bucket : _buckets) {
        bucket.instances.clear();
    }
    _meshes.run([&](flecs::iter& it) {
        while (it.next()) {
            const auto world = it.field<const WorldTransform3D>(0);
            const auto renderers = it.field<const MeshRenderer>(1);
            Bucket* last = nullptr; // entities of a table mostly share a mesh
            for (const auto i : it) {
                const MeshRenderer& r = renderers[i];
                if (!r.visible || !r.mesh.valid()) {
                    continue;
                }
                if (!last || !(last->mesh == r.mesh) || !(last->material == r.material)) {
                    last = nullptr;
                    for (Bucket& bucket : _buckets) {
                        if (bucket.mesh == r.mesh && bucket.material == r.material) {
                            last = &bucket;
                            break;
                        }
                    }
                    if (!last) {
                        last = &_buckets.emplace_back(Bucket{r.mesh, r.material, {}});
                    }
                }
                last->instances.push_back({world[i].matrix, r.tint});
            }
        }
    });
    for (const Bucket& bucket : _buckets) {
        scene.draw_instanced(bucket.mesh, bucket.instances, bucket.material);
    }
    // Lights (P3b): the entity's place, and for a spot its -Z axis; the entity's id names the light across frames.
    _point_lights.each([&](flecs::entity entity, const WorldTransform3D& world, const PointLight3D& light) {
        if (light.enabled) {
            scene.add_light({.position = world.position(), .color = light.color, .radius = light.radius, .size = light.size, .shadow = light.shadow,
                             .id = static_cast<u32>(entity.id() & 0xFFFFFFFFu)});
        }
    });
    _spot_lights.each([&](flecs::entity entity, const WorldTransform3D& world, const SpotLight3D& light) {
        if (light.enabled) {
            const Vec3f axis = world.matrix.apply_vector({0.0f, 0.0f, -1.0f});
            scene.add_spot_light({.position = world.position(), .direction = length_squared(axis) > 0.0f ? normalize(axis) : Vec3f{0.0f, -1.0f, 0.0f},
                                  .color = light.color, .radius = light.radius, .size = light.size, .inner_angle = light.inner_angle,
                                  .outer_angle = light.outer_angle, .shadow = light.shadow, .id = static_cast<u32>(entity.id() & 0xFFFFFFFFu)});
        }
    });
    // Characters: each model in its animator's pose (the rest pose with none, or before its first animate()).
    _skinned.each([&](flecs::iter& it, size_t row, const WorldTransform3D& world, const SkinnedMeshRenderer& renderer, Animator3D* animator) {
        const Model* model = renderer.model.get();
        if (!renderer.visible || !model || model->skeletons.empty()) {
            return;
        }
        const Skeleton& skeleton = model->skeletons[0];
        _pose_scratch.resize(skeleton.joint_count());
        if (animator && animator->bound == model && animator->player.skeleton() == &skeleton) {
            if (animator->ik_constraints.empty() && animator->pose_override.empty()) {
                animator->player.joint_matrices(_pose_scratch);
                if (!animator->ik_world.empty()) {
                    animator->ik_world.clear();
                    animator->ik_pose.joints.clear();
                }
            } else {
                // The clips' pose with the constraints solved on it (and the physics override over that), in this frame's place
                // of the entity.
                solve_pose(*animator, skeleton, animator->player.pose(), world.matrix, it.world(), _epoch);
                world_to_joint_matrices(skeleton, animator->ik_world, _pose_scratch);
            }
        } else {
            compute_joint_matrices(skeleton, skeleton.rest_pose(), _pose_scratch);
        }
        // The entity's id when none is given: pick names the entity, and the velocity matches the character by it.
        const u32 id = renderer.id != 0 ? renderer.id : static_cast<u32>(it.entity(row).id() & 0xffffffffu);
        scene.draw_skinned(*model, _pose_scratch, world.matrix, renderer.tint, id);
    });
    // Meshes no longer drawn are let go.
    std::erase_if(_buckets, [](const Bucket& bucket) { return bucket.instances.empty(); });
}

} // namespace kin
