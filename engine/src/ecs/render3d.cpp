#include "../render3d/history3d.hpp"
#include "../scene/scene_identity3d.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <kin/anim/events.hpp>
#include <kin/ecs/render3d.hpp>
#include <kin/render3d/material_asset3d.hpp>
#include <string_view>
#include <unordered_map>

namespace kin {

namespace {

// Whether skeletons[0] is a skin: a model's node skeleton comes after its skins, so it is the first only when there are
// none.
bool has_skin(const Model3D& model) {
    return !model.skeletons.empty() && model.node_skeleton != 0;
}

// What an animator poses: the first skin of its entity's SkinnedMeshRenderer3D, else the node skeleton of its
// ModelRenderer3D (the plain nodes the model's animations move). Null model: neither.
struct Animated {
    const std::shared_ptr<const Model3D>* model = nullptr;
    u32 skeleton = 0;
};
Animated animated(const SkinnedMeshRenderer3D* skin, const ModelRenderer3D* rigid) {
    if (skin && skin->model && has_skin(*skin->model))
        return {&skin->model, 0};
    if (rigid && rigid->model && rigid->model->node_skeleton >= 0 &&
        static_cast<std::size_t>(rigid->model->node_skeleton) < rigid->model->skeletons.size()) {
        return {&rigid->model, static_cast<u32>(rigid->model->node_skeleton)};
    }
    return {};
}

void bind_animator(Animator3D& a, const std::shared_ptr<const Model3D>& model, u32 index) {
    const auto& skeleton = model->skeletons[index];
    if (a.bound_to(model) && a.player.skeleton() == &skeleton)
        return;
    a.events.clear();
    a.player.set_skeleton(&skeleton);
    a.player.play(nullptr);
    a.player.clear_layers();
    a.bound = model.get();
    a.bound_model = model;
    a.applied.clear();
    a.applied_layers.clear();
    // The constraints' joints were looked up in the old skeleton, whose address a new one can take.
    for (IKConstraint3D& k : a.ik_constraints)
        k.resolved_for = nullptr;
}

// Whether `key` is `parts` end to end: what the animators ask every frame, without building the string (the key is made
// only when it changes).
bool key_is(const std::string& key, std::initializer_list<std::string_view> parts) {
    std::size_t at = 0;
    for (const std::string_view part : parts) {
        if (at + part.size() > key.size() || key.compare(at, part.size(), part) != 0) {
            return false;
        }
        at += part.size();
    }
    return at == key.size();
}

// The clip a state plays when it has no blend space: the machine's animation name for it, else its own name.
const std::string& clip_of_state(const Animator3D& a, const std::string& state) {
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

// What the base layer should be playing: the machine's state (stepped by one transition), or the clip named. `kind` and
// `name` make the key that identifies it (a change of key crossfades); `blend` is the state's blend space, if any.
struct Wanted {
    std::string_view kind;
    const std::string* name = nullptr;
    const Animator3D::BlendSpace* blend = nullptr;
    std::string_view clip;
};

} // namespace

// Stable callback storage; observers never capture the movable Flecs table data.
struct WorldRenderState3D::Tracking {
    // Observer entity deletion may be deferred. Their callbacks retain this
    // small liveness token, never a dangling pointer to destroyed state.
    struct Callbacks {
        Tracking* state = nullptr;
    };
    std::shared_ptr<Callbacks> callbacks = std::make_shared<Callbacks>(Callbacks{this});
    struct Node {
        flecs::entity_t id = 0, parent = 0, source = 0;
        u32 parent_index = ~u32{0};
        Transform3D local{};
        Mat4f world{};
        u32 first_child = ~u32{0}, next_sibling = ~u32{0};
        bool dirty = false, collected = false;
    };
    struct Binding {
        u32 bucket = 0, slot = 0, part = ~u32{0}, next = ~u32{0};
    };
    flecs::world_t* world = nullptr;
    SceneMutationMode3D mode = SceneMutationMode3D::Conservative;
    SceneWorkStats3D stats{};
    bool topology = true, collection = false;
    u64 particle_revision = 1;
    std::vector<Node> nodes;
    std::unordered_map<flecs::entity_t, u32> indices;
    std::unordered_map<flecs::entity_t, std::vector<u32>> sources;
    std::vector<flecs::entity_t> pending, changed;
    std::vector<u32> dirty;
    std::unordered_map<flecs::entity_t, Binding> bindings;
    std::vector<Binding> extra_bindings;
    std::vector<flecs::observer> observers;
    ~Tracking() {
        callbacks->state = nullptr;
        for (auto& observer : observers)
            observer.destruct();
    }
};

WorldRenderState3D::~WorldRenderState3D() = default;
void WorldRenderState3D::mutation_mode(SceneMutationMode3D mode) {
    if (_tracking->mode != mode) {
        _tracking->mode = mode;
        invalidate();
        if (mode == SceneMutationMode3D::Conservative) {
            auto& t = *_tracking;
            decltype(t.nodes){}.swap(t.nodes);
            decltype(t.indices){}.swap(t.indices);
            decltype(t.sources){}.swap(t.sources);
            decltype(t.bindings){}.swap(t.bindings);
            decltype(t.extra_bindings){}.swap(t.extra_bindings);
            decltype(t.pending){}.swap(t.pending);
            decltype(t.changed){}.swap(t.changed);
            decltype(t.dirty){}.swap(t.dirty);
        }
    }
}
SceneMutationMode3D WorldRenderState3D::mutation_mode() const {
    return _tracking->mode;
}
void WorldRenderState3D::invalidate() {
    _tracking->topology = true;
    _tracking->collection = false;
    ++_tracking->particle_revision;
}
SceneWorkStats3D WorldRenderState3D::work_stats() const {
    auto stats = _tracking->stats;
    // Capacity estimate, including hash nodes/buckets; allocator overhead is not observable here.
    const auto& t = *_tracking;
    stats.tracking_bytes =
        t.nodes.capacity() * sizeof(Tracking::Node) +
        (t.pending.capacity() + t.changed.capacity()) * sizeof(flecs::entity_t) + t.dirty.capacity() * sizeof(u32) +
        t.indices.size() * sizeof(decltype(t.indices)::value_type) + t.indices.bucket_count() * sizeof(void*) +
        t.sources.size() * sizeof(decltype(t.sources)::value_type) + t.sources.bucket_count() * sizeof(void*) +
        t.bindings.size() * sizeof(decltype(t.bindings)::value_type) + t.bindings.bucket_count() * sizeof(void*) +
        t.extra_bindings.capacity() * sizeof(Tracking::Binding);
    for (const auto& [id, uses] : t.sources)
        stats.tracking_bytes += uses.capacity() * sizeof(u32);
    return stats;
}
u64 WorldRenderState3D::particle_configuration_revision() const {
    return _tracking->particle_revision;
}
void WorldRenderState3D::note_particle_validation(bool reused) {
    if (reused)
        ++_tracking->stats.particle_validation_reuses;
    else
        ++_tracking->stats.particle_validation_runs;
}
void WorldRenderState3D::reset_work_stats() {
    _tracking->stats = {};
}

u64 WorldRenderState3D::history_id(flecs::entity entity) const {
    const auto* lease = entity.get<detail::SceneLease3D>();
    const u64 scope = detail::history3d_mix(_history_identity);
    const u64 object = detail::history3d_mix(entity.id() ^ scope);
    const u64 id = lease ? detail::history3d_mix(object ^ lease->instance ^ 0xd6e8feb86659fd93ULL) : object;
    return id ? id : 1;
}

WorldRenderState3D::WorldRenderState3D(flecs::world& world)
    : _tracking(std::make_unique<Tracking>()), _history_identity([] {
          static std::atomic<u64> next{1};
          return next.fetch_add(1, std::memory_order_relaxed);
      }()),
      _attachments(world.query_builder<JointAttachment3D>().term_at(0).self().build()),
      _transform_observer(world.observer<Transform3D>("add_world_transform_3d")
                              .query_flags(EcsQueryMatchDisabled | EcsQueryMatchPrefab)
                              .event(flecs::OnAdd)
                              .yield_existing()
                              .each([](flecs::entity entity, Transform3D&) { entity.ensure<WorldTransform3D>(); })),
      _transforms(world.query_builder<WorldTransform3D, const Transform3D, const WorldTransform3D>()
                      .term_at(2)
                      .parent()
                      .cascade()
                      .optional()
                      .cached()
                      .build()),
      _meshes(world.query_builder<const WorldTransform3D, const MeshRenderer3D, const RenderTransform3D*>()
                  .cached()
                  .build()),
      _models(
          world.query_builder<const WorldTransform3D, const ModelRenderer3D, const RenderTransform3D*, Animator3D*>()
              .cached()
              .build()),
      _animators(world.query_builder<const SkinnedMeshRenderer3D*, const ModelRenderer3D*, Animator3D, Transform3D*>()
                     .build()),
      _skinned(world
                   .query_builder<const WorldTransform3D, const SkinnedMeshRenderer3D, Animator3D*,
                                  const RenderTransform3D*>()
                   .build()),
      _time_of_day(world.query_builder<TimeOfDay3D>().build()),
      _point_lights(
          world.query_builder<const WorldTransform3D, const PointLight3D, const RenderTransform3D*>().build()),
      _spot_lights(world.query_builder<const WorldTransform3D, const SpotLight3D, const RenderTransform3D*>().build()),
      _emitters(world.query_builder<const WorldTransform3D, const ParticleEmitter3D>().build()),
      _weather(world.query_builder<Weather3D>().build()) {
    world.component<detail::SceneLease3D>();
    // Optional lookups during collection must not lazily register a component
    // while a Flecs table is being iterated, including in non-authored worlds.
    world.component<ModelMaterials3D>();
    auto* tracking = _tracking.get();
    tracking->world = world.c_ptr();
    const auto transform_id = world.id<Transform3D>(), world_id = world.id<WorldTransform3D>();
    const auto emitter_id = world.id<ParticleEmitter3D>();
    const auto structural = [callbacks = tracking->callbacks, transform_id, world_id, emitter_id](flecs::iter& it) {
        while (it.next()) {
            auto* tracking = callbacks->state;
            if (!tracking)
                continue;
            const auto id = it.event_id();
            const bool hierarchy = id.is_pair() && (id.first() == flecs::ChildOf || id.first() == flecs::IsA);
            const bool configuration = id == emitter_id || id == transform_id || id == flecs::Disabled || hierarchy;
            if (configuration)
                ++tracking->particle_revision;
            for (auto row : it) {
                if (hierarchy || id == transform_id || id == world_id || id == flecs::Disabled ||
                    it.entity(row).has<Transform3D>()) {
                    tracking->topology = true;
                    tracking->collection = false;
                    break;
                }
            }
        }
    };
    tracking->observers.push_back(world.observer()
                                      .with(flecs::Wildcard)
                                      .query_flags(EcsQueryMatchDisabled | EcsQueryMatchPrefab)
                                      .event(flecs::OnAdd)
                                      .event(flecs::OnRemove)
                                      .run(structural));
    tracking->observers.push_back(world.observer()
                                      .with(flecs::Wildcard, flecs::Wildcard)
                                      .query_flags(EcsQueryMatchDisabled | EcsQueryMatchPrefab)
                                      .event(flecs::OnAdd)
                                      .event(flecs::OnRemove)
                                      .run(structural));
    tracking->observers.push_back(
        world.observer()
            .with(flecs::Wildcard)
            .query_flags(EcsQueryMatchDisabled | EcsQueryMatchPrefab)
            .event(flecs::OnSet)
            .run([callbacks = tracking->callbacks, transform = transform_id, world_id, emitter_id](flecs::iter& it) {
                while (it.next()) {
                    auto* tracking = callbacks->state;
                    if (!tracking)
                        continue;
                    if (it.event_id() == transform) {
                        if (tracking->mode == SceneMutationMode3D::Tracked && !tracking->topology)
                            for (auto row : it)
                                tracking->pending.push_back(it.entity(row).id());
                    } else {
                        if (it.event_id() == emitter_id)
                            ++tracking->particle_revision;
                        tracking->collection = false;
                        // An explicitly edited derived parent may affect children.
                        if (it.event_id() == world_id)
                            tracking->topology = true;
                    }
                }
            }));
}

void WorldRenderState3D::simulate_particles(f64 dt, const Wind3D* wind) {
    // Copy inherited state before simulation, outside the query: a structural
    // change can move Flecs tables and invalidate their component pointers.
    std::vector<EcsEntity> entities;
    _emitters.each([&](flecs::entity entity, const WorldTransform3D&, const ParticleEmitter3D&) {
        if (!entity.owns<ParticleEmitter3D>())
            entities.emplace_back(entity);
    });
    for (auto entity : entities)
        entity.get_mut<ParticleEmitter3D>();
    _emitters.each([&](flecs::entity entity, const WorldTransform3D& world, const ParticleEmitter3D&) {
        auto* emitter = entity.get_mut<ParticleEmitter3D>(); // all owned now, no structural change
        emitter->system.transform = world.matrix;
        const bool native_emitting = emitter->system.emitting;
        emitter->system.emitting = native_emitting && emitter->enabled && emitter->emitting;
        emitter->system.update(dt, wind, ParticleMotion3D::Capture);
        emitter->system.emitting = native_emitting;
    });
}

void WorldRenderState3D::advance_weather(f64 dt) {
    _weather.each([dt](Weather3D& weather) { weather.time += dt; });
}

bool WorldRenderState3D::apply_weather(Scene3D& scene, const Fog3D& base_fog) const {
    bool applied = false;
    _weather.each([&](Weather3D& weather) {
        if (!applied) {
            scene.weather = weather;
            weather.apply(scene.lights, scene.fog, base_fog);
            applied = true;
        }
    });
    return applied;
}

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

ActionToken3D play_action3d(EcsEntity actor, std::string_view name, ActionStart3D start) {
    if (!actor)
        return {};
    const auto* animator = actor.get<Animator3D>();
    const Animated target = animated(actor.get<SkinnedMeshRenderer3D>(), actor.get<ModelRenderer3D>());
    if (!animator || !animator->actions || !target.model)
        return {};
    const std::shared_ptr<const Model3D>& model = *target.model;
    const auto definitions = animator->actions;
    const auto found =
        std::find_if(definitions->begin(), definitions->end(), [&](const auto& d) { return d.name == name; });
    if (found == definitions->end())
        return {};
    const auto* clip = model->find_clip(found->clip, target.skeleton);
    auto options = found->playback;
    options.mask_joint = found->mask_joint.empty() ? -1 : model->skeletons[target.skeleton].find(found->mask_joint);
    if (!clip || (!found->mask_joint.empty() && options.mask_joint < 0))
        return {};
    auto* own = actor.get_mut<Animator3D>();
    bind_animator(*own, model, target.skeleton);
    return own->player.play_action(clip, options, start);
}
bool cancel_action3d(EcsEntity actor, ActionToken3D token, f32 fade_seconds) {
    if (!actor || !actor.has<Animator3D>())
        return false;
    return actor.get_mut<Animator3D>()->player.cancel_action(token, fade_seconds);
}
ActionStatus3D action_status3d(EcsEntity actor, ActionToken3D token) {
    if (!actor || !actor.has<Animator3D>())
        return token ? ActionStatus3D::Superseded : ActionStatus3D::None;
    return actor.get<Animator3D>()->player.action_status(token);
}

u32 dispatch_animation_events3d(EcsWorld& world, const AnimationEventDispatch& dispatch) {
    u32 unhandled = 0;
    world.raw().each([&](flecs::entity entity, Animator3D& animator) {
        // The batch retains its actual source table, even after a native table
        // replacement or a handler replacing the animator.
        const auto batch = animator.events;
        animator.events.clear();
        for (u32 i = 0; i < batch.count; ++i) {
            if (batch.actions[i]) {
                const auto* current = entity.get<Animator3D>();
                if (!current || !current->player.action_event_valid(batch.actions[i]))
                    continue;
            }
            if (!dispatch.dispatch(EcsEntity{entity}, *batch.entries[i]))
                ++unhandled;
        }
    });
    return unhandled;
}

void WorldRenderState3D::animate(f32 dt) {
    ++_epoch;
    const auto advance = [&](flecs::iter& it, size_t row, const SkinnedMeshRenderer3D* renderer,
                             const ModelRenderer3D* rigid, Animator3D& a, Transform3D* own) {
        a.events.clear();
        const Animated target = animated(renderer, rigid);
        const Model3D* model = target.model ? target.model->get() : nullptr;
        if (!model) {
            if (a.bound || a.player.skeleton()) {
                a.player.set_skeleton(nullptr);
                a.player.play(nullptr);
                a.player.clear_layers();
                a.bound = nullptr;
                a.bound_model.reset();
                a.applied.clear();
                a.applied_layers.clear();
            }
            return;
        }
        const u32 index = target.skeleton; // 0, the first skin, for a SkinnedMeshRenderer3D
        const Skeleton3D& skeleton = model->skeletons[index];
        bind_animator(a, *target.model, index);
        // The state machine: one transition a step (kin/anim's evaluate, consuming matched triggers), never to the
        // state it is in.
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
            want.kind = "state:";
            want.name = &a.state;
            if (space != a.blend_spaces.end() && !space->second.entries.empty()) {
                want.blend = &space->second;
            } else {
                want.clip = clip_of_state(a, a.state);
            }
        } else {
            want.kind = "clip:";
            want.name = &a.clip;
            want.clip = a.clip;
        }
        const bool first = a.applied.empty();
        if (!key_is(a.applied, {want.kind, *want.name})) {
            a.applied = std::string(want.kind) + *want.name;
            const f32 seconds = first ? 0.0f : fade;
            if (want.blend) {
                std::vector<BlendPoint3D> points;
                for (const Animator3D::BlendEntry& e : want.blend->entries) {
                    points.push_back({model->find_clip(e.clip, index), e.position});
                }
                a.player.crossfade_to_blend(points, a.params.get_float(want.blend->parameter), seconds, a.loop);
            } else {
                a.player.crossfade_to(model->find_clip(want.clip, index), seconds, a.loop);
            }
        }
        if (a.machine && want.blend) {
            a.player.set_blend_parameter(a.params.get_float(want.blend->parameter));
        }
        a.player.set_speed(a.speed);
        a.player.set_loop(a.loop);
        a.player.set_root_motion(a.apply_root_motion || a.remove_root_motion, a.root_motion_axes);
        // Layers over the base, as data: the clip by name and the mask by its joint, looked up when they change.
        a.applied_layers.resize(a.layers.size());
        for (std::size_t i = 0; i < a.layers.size(); ++i) {
            const Animator3D::LayerConfig& config = a.layers[i];
            AnimationPlayer3D::Layer& layer = a.player.layer(static_cast<u32>(i));
            if (!key_is(a.applied_layers[i], {config.clip, "|", config.mask_joint, config.include_root ? "+" : "-"})) {
                a.applied_layers[i] = config.clip + "|" + config.mask_joint + (config.include_root ? "+" : "-");
                layer.clip = config.clip.empty() ? nullptr : model->find_clip(config.clip, index);
                layer.time = 0.0f;
                const i32 joint = config.mask_joint.empty() ? -1 : skeleton.find(config.mask_joint);
                layer.mask =
                    joint >= 0 ? PoseMask3D::subtree(skeleton, joint, 1.0f, config.include_root) : PoseMask3D{};
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
            a.player.update(dt,
                            a.event_tracks ? std::span<const ClipEventTrack3D>{*a.event_tracks}
                                           : std::span<const ClipEventTrack3D>{},
                            a.events);
            if (a.events.count)
                a.events._owner = a.event_tracks;
            if (a.apply_root_motion) {
                const RootMotion3D& m = a.player.root_motion();
                if (own) {
                    const Transform3D before = *own;
                    // The motion is in the root's parent space (the model's): turned and scaled as the entity is.
                    own->position +=
                        own->rotation.rotate(Vec3f{m.translation.x * own->scale.x, m.translation.y * own->scale.y,
                                                   m.translation.z * own->scale.z});
                    if (a.root_motion_axes.rotation) {
                        own->rotation = (own->rotation * m.rotation).normalized();
                    }
                    if (own->position != before.position || own->rotation != before.rotation)
                        it.entity(row).modified<Transform3D>();
                }
            }
        }
        if (!a.ik_constraints.empty()) {
            // The weights ease toward their goals (a target that is gone has the goal 0); the solving itself waits for
            // collect(), when the entity's WorldTransform3D of this frame is known.
            a.ik_dt += dt;
            const flecs::world ecs = it.world();
            for (IKConstraint3D& k : a.ik_constraints) {
                k.target_valid = false;
                if (k.target.kind == IKTarget3D::Kind::World) {
                    k.target_valid = true;
                } else if (k.target.kind == IKTarget3D::Kind::SceneEntity) {
                    const auto target = _ecs ? k.target.scene_entity.resolve(*_ecs) : EcsEntity{};
                    k.target_valid = target && target.has<WorldTransform3D>();
                } else if (k.target.kind == IKTarget3D::Kind::Entity) {
                    k.target_valid = (k.target.entity != 0 && ecs.is_alive(k.target.entity)) &&
                                     ecs.entity(k.target.entity).get<WorldTransform3D>() != nullptr;
                }
                // A target that is gone (or was never given) lets go: the goal is 0, and the fade runs from where it
                // was.
                const bool needs_target = k.kind != IKKind3D::FootPlacer;
                const f32 want = needs_target && !k.target_valid ? 0.0f : std::clamp(k.weight, 0.0f, 1.0f);
                if (k.fade_seconds > 0.0f) {
                    const f32 step = dt / k.fade_seconds;
                    k.current += std::clamp(want - k.current, -step, step);
                } else {
                    k.current = want;
                }
            }
        }
    };
    // Root-motion modified() notifications must wait until the query unlocks
    // its tables. Publish them before the caller's transform barrier.
    _ecs->raw().defer([&] { _animators.each(advance); });
}

void WorldRenderState3D::solve_ik(Animator3D& a, const Skeleton3D& sk, const Pose3D& base, const Mat4f& entity_matrix,
                                  const flecs::world& ecs, u64 epoch) {
    a.ik_pose = base;
    compute_world_matrices(sk, a.ik_pose, a.ik_world);
    const Mat4f to_model = entity_matrix.inverse();
    const f32 dt = a.ik_dt;
    for (IKConstraint3D& k : a.ik_constraints) {
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
            return std::none_of(k.resolved.begin(), k.resolved.begin() + static_cast<std::ptrdiff_t>(count),
                                [](i32 j) { return j < 0; });
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
        const auto presented =
            _ik_presentation && *_ik_presentation ? (*_ik_presentation)(k) : std::optional<IKTargetPose3D>{};
        if (k.kind != IKKind3D::FootPlacer) {
            if (k.target.kind == IKTarget3D::Kind::World) {
                target_world = presented ? presented->position : k.target.position;
                has_target = true;
            } else if (k.target.kind == IKTarget3D::Kind::SceneEntity) {
                const auto target = _ecs ? k.target.scene_entity.resolve(*_ecs) : EcsEntity{};
                if (const auto* t = target ? target.get<WorldTransform3D>() : nullptr) {
                    const auto* explicit_pose = target.get<RenderTransform3D>();
                    const auto pose = explicit_pose && explicit_pose->enabled ? explicit_pose->matrix
                                      : _presentation && *_presentation
                                          ? (*_presentation)(target.raw()).value_or(t->matrix)
                                          : t->matrix;
                    target_world = pose.apply(k.target.position);
                    has_target = true;
                }
            } else if (k.target.kind == IKTarget3D::Kind::Entity &&
                       (k.target.entity != 0 && ecs.is_alive(k.target.entity))) {
                if (const WorldTransform3D* t = ecs.entity(k.target.entity).get<WorldTransform3D>()) {
                    const auto entity = ecs.entity(k.target.entity);
                    const auto* explicit_pose = entity.get<RenderTransform3D>();
                    const auto pose = explicit_pose && explicit_pose->enabled ? explicit_pose->matrix
                                      : _presentation && *_presentation ? (*_presentation)(entity).value_or(t->matrix)
                                                                        : t->matrix;
                    target_world = pose.apply(k.target.position);
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
        const auto pole_mode = [&](TwoBoneOptions3D& o) {
            switch (k.pole) {
            case IKConstraint3D::Pole::WorldPosition:
                o.pole_mode = PoleMode3D::Position;
                o.pole = to_model.apply(k.pole_vector);
                break;
            case IKConstraint3D::Pole::ModelDirection:
                o.pole_mode = PoleMode3D::Direction;
                o.pole = k.pole_vector;
                break;
            case IKConstraint3D::Pole::None:
                o.pole_mode = PoleMode3D::None;
                break;
            }
        };
        switch (k.kind) {
        case IKKind3D::TwoBone: {
            if (!resolved(3)) {
                break;
            }
            TwoBoneOptions3D o = k.two_bone;
            o.weight = w;
            pole_mode(o);
            if (k.match_rotation) {
                o.match_rotation = true;
                o.end_rotation =
                    (rotation_of(to_model) * (presented ? presented->rotation : k.target_rotation)).normalized();
                o.rotation_weight = k.rotation_weight;
            }
            solve_two_bone(sk, a.ik_pose, a.ik_world, {k.resolved[0], k.resolved[1], k.resolved[2]}, target, o,
                           &k.bend_state, dt);
            break;
        }
        case IKKind3D::Fabrik: {
            if (k.resolved.size() < 2 || !resolved(k.resolved.size())) {
                break;
            }
            FabrikOptions3D o = k.fabrik;
            o.weight = k.fabrik.weight * w;
            switch (k.pole) {
            case IKConstraint3D::Pole::WorldPosition:
                o.pole_mode = PoleMode3D::Position;
                o.pole = to_model.apply(k.pole_vector);
                break;
            case IKConstraint3D::Pole::ModelDirection:
                o.pole_mode = PoleMode3D::Direction;
                o.pole = k.pole_vector;
                break;
            case IKConstraint3D::Pole::None:
                o.pole_mode = PoleMode3D::None;
                break;
            }
            solve_chain_fabrik(sk, a.ik_pose, a.ik_world, k.resolved, target, o);
            break;
        }
        case IKKind3D::LookAt: {
            if (!resolved(1) || !resolved(k.resolved.size())) {
                break;
            }
            std::array<LookAtJoint3D, 8> chain{};
            const std::size_t n = std::min<std::size_t>(k.resolved.size(), chain.size());
            for (std::size_t i = 0; i < n; ++i) {
                chain[i] = i < k.look_links.size() ? k.look_links[i] : LookAtJoint3D{};
                chain[i].joint = k.resolved[i];
            }
            LookAtOptions3D o = k.look_at;
            o.weight = k.look_at.weight * w;
            LookAtState3D state = k.look_state;
            if (state.valid) {
                state.direction = normalize(to_model.apply_vector(state.direction));
            }
            solve_look_at(sk, a.ik_pose, a.ik_world, std::span<const LookAtJoint3D>{chain.data(), n}, target, o, &state,
                          dt);
            k.look_state.direction = normalize(entity_matrix.apply_vector(state.direction));
            k.look_state.valid = state.valid;
            break;
        }
        case IKKind3D::FootPlacer: {
            const std::size_t legs = k.resolved.size() >= 4 ? (k.resolved.size() - 1) / 3 : 0;
            if (legs == 0 || !k.ground || !resolved(1 + legs * 3)) {
                break;
            }
            std::array<FootLeg3D, 4> feet{};
            const std::size_t n = std::min<std::size_t>(legs, feet.size());
            for (std::size_t i = 0; i < n; ++i) {
                feet[i].leg = {k.resolved[1 + i * 3], k.resolved[2 + i * 3], k.resolved[3 + i * 3]};
                feet[i].foot_height = i < k.foot_heights.size() ? k.foot_heights[i] : 0.1f;
                feet[i].toe = i < k.resolved_toes.size() ? k.resolved_toes[i] : -1;
                feet[i].toe_height = i < k.toe_heights.size() ? k.toe_heights[i] : 0.03f;
            }
            const std::span<const FootLeg3D> leg_span{feet.data(), n};
            // The game's query is in the world: the segment goes out, the hit comes back into the model's space. Its
            // answers are kept for the frame (the epoch: animate() and propagate_transforms() advance it) by the order
            // they were asked in, for the same segment: a second solve in the frame (collect() after apply_ik(), two
            // views) does not ask again.
            std::size_t asked = 0;
            const GroundQuery3D model_ground = [&](Vec3f from, Vec3f to) -> std::optional<GroundHit3D> {
                const Vec3f world_from = entity_matrix.apply(from), world_to = entity_matrix.apply(to);
                if (k.ground_cache.size() <= asked) {
                    k.ground_cache.resize(asked + 1);
                }
                IKConstraint3D::GroundCache& cached = k.ground_cache[asked++];
                if (cached.epoch != epoch || !(cached.from == world_from) || !(cached.to == world_to)) {
                    cached.hit = k.ground(world_from, world_to);
                    cached.epoch = epoch;
                    cached.from = world_from;
                    cached.to = world_to;
                }
                const std::optional<GroundHit3D>& hit = cached.hit;
                if (!hit) {
                    return std::nullopt;
                }
                return GroundHit3D{to_model.apply(hit->position),
                                   normalize(entity_matrix.transposed().apply_vector(hit->normal))};
            };
            FootPlan3D plan = plan_foot_placement(sk, a.ik_pose, a.ik_world, leg_span, model_ground, k.foot,
                                                  &k.foot_state, dt, &entity_matrix);
            plan.pelvis_offset *= w;
            TwoBoneOptions3D o = k.two_bone;
            o.weight = w;
            pole_mode(o);
            apply_foot_placement(sk, a.ik_pose, a.ik_world, k.resolved[0], leg_span, plan, o, k.foot.up);
            break;
        }
        }
    }
    a.ik_dt = 0.0f;
}

void WorldRenderState3D::solve_pose(Animator3D& a, const Skeleton3D& sk, const Pose3D& base, const Mat4f& entity_matrix,
                                    const flecs::world& ecs, u64 epoch, const PoseOverride3D* presented) {
    if (!a.ik_constraints.empty()) {
        solve_ik(a, sk, base, entity_matrix, ecs, epoch);
    } else {
        a.ik_pose = base;
        compute_world_matrices(sk, a.ik_pose, a.ik_world);
    }
    const PoseOverride3D& o = presented ? *presented : a.pose_override;
    const u32 n = sk.joint_count();
    if (o.weights.size() != n || o.position.size() != n || o.rotation.size() != n) {
        return;
    }
    const Mat4f to_model = entity_matrix.inverse();
    // Parent first. A joint with a share takes the body's rotation (more or less of it), and the body's place when its
    // parent has less share than it does: the top of the ragdolled part. The rest of the part keeps the pose's bone
    // lengths.
    for (u32 j = 0; j < n; ++j) {
        const i32 parent = sk.parents[j];
        const Mat4f parent_world = parent >= 0 ? a.ik_world[static_cast<std::size_t>(parent)] : sk.root_transform;
        const f32 w = std::clamp(o.weights[j], 0.0f, 1.0f);
        JointPose3D& jp = a.ik_pose.joints[j];
        if (w > 0.0f) {
            const Mat4f body = to_model * Mat4f::trs(o.position[j], o.rotation[j]);
            const Mat4f local = parent_world.inverse() * body;
            const Quatf target = rotation_of(local);
            const f32 parent_w =
                parent >= 0 ? std::clamp(o.weights[static_cast<std::size_t>(parent)], 0.0f, 1.0f) : 0.0f;
            if (parent_w < w) {
                jp.translation = lerp(jp.translation, Vec3f{local.m[12], local.m[13], local.m[14]}, w - parent_w);
            }
            jp.rotation = w >= 1.0f ? target : slerp(jp.rotation, target, w);
        }
        a.ik_world[j] = parent_world * jp.matrix();
    }
}

void WorldRenderState3D::apply_ik() {
    _skinned.each([&](flecs::iter& it, size_t, const WorldTransform3D& world, const SkinnedMeshRenderer3D& renderer,
                      Animator3D* animator, const RenderTransform3D*) {
        const Model3D* model = renderer.model.get();
        if (!animator || (animator->ik_constraints.empty() && animator->pose_override.empty()) || !model ||
            !has_skin(*model)) {
            return;
        }
        const Skeleton3D& skeleton = model->skeletons[0];
        if (animator->bound_to(renderer.model) && animator->player.skeleton() == &skeleton) {
            solve_pose(*animator, skeleton, animator->player.pose(), world.matrix, it.world(), _epoch);
        }
    });
    // Models whose animated nodes an animator plays, with constraints on those nodes (a turret's look-at).
    _models.each([&](flecs::entity entity, const WorldTransform3D& world, const ModelRenderer3D& renderer,
                     const RenderTransform3D*, Animator3D* animator) {
        const Model3D* model = renderer.model.get();
        if (!animator || (animator->ik_constraints.empty() && animator->pose_override.empty()) || !model ||
            model->node_skeleton < 0 || static_cast<std::size_t>(model->node_skeleton) >= model->skeletons.size()) {
            return;
        }
        const Skeleton3D& skeleton = model->skeletons[static_cast<std::size_t>(model->node_skeleton)];
        if (animator->bound_to(renderer.model) && animator->player.skeleton() == &skeleton) {
            solve_pose(*animator, skeleton, animator->player.pose(), world.matrix, entity.world(), _epoch);
        }
    });
}

void WorldRenderState3D::propagate_transforms() {
    ++_epoch;
    auto& t = *_tracking;
    ++t.stats.transform_flushes;
    if (t.mode == SceneMutationMode3D::Conservative) {
        // Preserve the original tight table loops. A value cache increased the
        // cost of this compatibility scan; sparse tracking is an explicit choice.
        _transforms.run([&](flecs::iter& it) {
            while (it.next()) {
                const auto self = it.field<WorldTransform3D>(0);
                const auto local = it.field<const Transform3D>(1);
                const bool own_local = it.is_self(1);
                t.stats.transform_visits += it.count();
                t.stats.transforms_recomputed += it.count();
                if (!it.is_set(2)) {
                    if (own_local)
                        for (auto row : it)
                            self[row].matrix = local[row].matrix();
                    else {
                        const Mat4f matrix = local[0].matrix();
                        for (auto row : it)
                            self[row].matrix = matrix;
                    }
                    continue;
                }
                const Mat4f parent = it.field<const WorldTransform3D>(2)[0].matrix;
                if (own_local)
                    for (auto row : it)
                        self[row].matrix = parent * local[row].matrix();
                else {
                    const Mat4f matrix = parent * local[0].matrix();
                    for (auto row : it)
                        self[row].matrix = matrix;
                }
            }
        });
        return;
    }

    const bool rebuild = t.topology;
    if (rebuild) {
        t.nodes.clear();
        t.indices.clear();
        t.sources.clear();
        t.changed.clear();
        t.bindings.clear();
        t.extra_bindings.clear();
        t.collection = false;
        ++t.stats.topology_rebuilds;
        _transforms.run([&](flecs::iter& it) {
            while (it.next()) {
                const auto local = it.field<const Transform3D>(1);
                for (auto row : it) {
                    const u32 index = static_cast<u32>(t.nodes.size());
                    const auto id = it.entity(row).id();
                    const auto parent = it.is_set(2) ? it.src(2).id() : 0;
                    const auto source = it.is_self(1) ? id : it.src(1).id();
                    t.indices.emplace(id, index);
                    if (source != id)
                        t.sources[source].push_back(index);
                    t.nodes.push_back(
                        {.id = id, .parent = parent, .source = source, .local = local[it.is_self(1) ? row : 0]});
                    if (auto p = t.indices.find(parent); p != t.indices.end()) {
                        t.nodes.back().next_sibling = t.nodes[p->second].first_child;
                        t.nodes[p->second].first_child = index;
                        t.nodes.back().parent_index = p->second;
                    }
                }
            }
        });
        t.topology = false;
    }
    const auto update = [&](u32 index, const Transform3D* local, WorldTransform3D* world, const Mat4f& parent) {
        auto& node = t.nodes[index];
        if (!world || !local) {
            t.topology = true;
            t.collection = false;
            return;
        }
        ++t.stats.transform_visits;
        const bool same_local = local->position == node.local.position && local->rotation == node.local.rotation &&
                                local->scale == node.local.scale;
        const bool parent_changed = node.parent_index != ~u32{0} && t.nodes[node.parent_index].dirty;
        // External derived parents are uncommon and deliberately conservative.
        const bool external_parent = node.parent && node.parent_index == ~u32{0};
        if (rebuild || !same_local || parent_changed || external_parent || world->matrix != node.world) {
            const Mat4f result = parent * local->matrix();
            node.dirty = result != node.world || rebuild;
            world->matrix = result;
            node.world = result;
            node.local = *local;
            ++t.stats.transforms_recomputed;
            if (node.dirty && !node.collected) {
                t.changed.push_back(node.id);
                node.collected = true;
            }
        } else {
            node.dirty = false;
            ++t.stats.transforms_reused;
        }
    };
    if (rebuild) {
        u32 index = 0;
        _transforms.run([&](flecs::iter& it) {
            while (it.next()) {
                auto worlds = it.field<WorldTransform3D>(0);
                const auto locals = it.field<const Transform3D>(1);
                const Mat4f parent = it.is_set(2) ? it.field<const WorldTransform3D>(2)[0].matrix : Mat4f{};
                for (auto row : it)
                    update(index++, &locals[it.is_self(1) ? row : 0], &worlds[row], parent);
            }
        });
    } else {
        t.dirty.clear();
        const auto mark = [&](u32 index) {
            if (!t.nodes[index].dirty) {
                t.nodes[index].dirty = true;
                t.dirty.push_back(index);
            }
        };
        for (auto id : t.pending) {
            if (auto node = t.indices.find(id); node != t.indices.end())
                mark(node->second);
            if (auto uses = t.sources.find(id); uses != t.sources.end())
                for (auto index : uses->second)
                    mark(index);
        }
        for (std::size_t i = 0; i < t.dirty.size(); ++i)
            for (u32 child = t.nodes[t.dirty[i]].first_child; child != ~u32{0}; child = t.nodes[child].next_sibling)
                mark(child);
        std::sort(t.dirty.begin(), t.dirty.end());
        for (auto index : t.dirty) {
            t.nodes[index].dirty = false;
            flecs::entity entity(t.world, t.nodes[index].id);
            const auto* parent =
                t.nodes[index].parent ? flecs::entity(t.world, t.nodes[index].parent).get<WorldTransform3D>() : nullptr;
            update(index, entity.get<Transform3D>(), entity.get_mut<WorldTransform3D>(),
                   parent ? parent->matrix : Mat4f{});
        }
    }
    // Retain only the changed IDs for collection, not transient dependency marks.
    if (rebuild) {
        for (auto& node : t.nodes)
            node.dirty = false;
    } else {
        for (auto index : t.dirty)
            t.nodes[index].dirty = false;
    }
    t.pending.clear();
}

void WorldRenderState3D::clear_collection_cache() {
    _tracking->collection = false;
    _tracking->bindings.clear();
    _tracking->extra_bindings.clear();
    _buckets.clear();
    _bucket_lookup.clear();
    _bucket_lookup_dirty = false;
}

// A collection's retention policy is separate from gathering mesh/model instances.
struct WorldRenderState3D::RetainedBudget {
    static constexpr std::size_t record_budget = 16 * 1024 * 1024;
    static constexpr std::size_t min_records = 256, max_batches = 128, group_size = 256;
    std::size_t bytes = 0, batches = 0;
};

bool WorldRenderState3D::Bucket::eligible(const RetainedBudget& budget) const {
    const std::size_t bytes = instances.size() * sizeof(MeshInstance3D);
    const std::size_t minimum = batch || spare ? RetainedBudget::min_records / 2 : RetainedBudget::min_records;
    return instances.size() >= minimum && !mesh.skinned() &&
           (material.blend == Blend3D::Opaque || material.blend == Blend3D::Mask) && !material.wireframe &&
           !material.shader.backend() && budget.batches < RetainedBudget::max_batches &&
           bytes <= RetainedBudget::record_budget - budget.bytes;
}

void WorldRenderState3D::Bucket::rebuild() {
    std::vector<InstanceGroup3D> groups;
    // Consecutive source ranges preserve draw order. Bounds follow edits;
    // no spatial sorting, duplicate world or caller-maintained boxes.
    if (instances.size() >= 2 * RetainedBudget::group_size) {
        groups.reserve((instances.size() + RetainedBudget::group_size - 1) / RetainedBudget::group_size);
        for (std::size_t first = 0; first < instances.size(); first += RetainedBudget::group_size)
            groups.push_back({static_cast<u32>(first),
                              static_cast<u32>(std::min(RetainedBudget::group_size, instances.size() - first))});
    }
    batch = InstanceBatch3D::create(mesh, instances, std::move(groups)).value_or({});
}

void WorldRenderState3D::Bucket::retain(RetainedBudget& budget) {
    const std::size_t bytes = instances.size() * sizeof(MeshInstance3D);
    const bool spare_usable =
        spare && spare.instances().size() == instances.size() && spare.lifetime().use_count() == 1;
    if (spare_usable && (!batch || batch.instances().size() != instances.size() || batch.lifetime().use_count() > 1))
        std::swap(batch, spare);
    if (!batch || batch.instances().size() != instances.size()) {
        std::swap(batch, spare);
        rebuild();
    } else {
        const auto previous = batch.instances();
        const bool shared = batch.lifetime().use_count() > 1;
        const auto changed = [&](std::size_t i) {
            const auto& a = previous[i];
            const auto& b = instances[i];
            return a.tint != b.tint || a.id != b.id || a.history_id != b.history_id ||
                   std::memcmp(a.transform.m, b.transform.m, sizeof(a.transform.m)) != 0;
        };
        for (std::size_t i = 0; i < previous.size();) {
            if (!changed(i)) {
                ++i;
                continue;
            }
            // A separately collected scene must keep its snapshot. The usual
            // clear/collect/render loop owns no old handle and edits in place.
            if (shared) {
                std::swap(batch, spare);
                rebuild();
                break;
            }
            const std::size_t first = i++;
            while (i < previous.size() && changed(i))
                ++i;
            if (!batch.update(static_cast<u32>(first),
                              std::span<const MeshInstance3D>(instances).subspan(first, i - first))) {
                rebuild();
                break;
            }
        }
    }
    budget.bytes += bytes;
    ++budget.batches;
    const auto spare_bytes = spare.instances().size_bytes();
    if (spare && budget.batches < RetainedBudget::max_batches &&
        spare_bytes <= RetainedBudget::record_budget - budget.bytes) {
        budget.bytes += spare_bytes;
        ++budget.batches;
    } else {
        spare = {};
    }
}

const PoseOverride3D& WorldRenderState3D::pose_override_for(flecs::entity entity, const Animator3D& animator) const {
    if (_pose_presentation && *_pose_presentation)
        if (const auto* pose = (*_pose_presentation)(entity))
            return *pose;
    return animator.pose_override;
}

void WorldRenderState3D::collect(Scene3D& scene, const Presentation3D& presentation, const PosePresentation3D& poses,
                                 const IKPresentation3D& ik, f32 alpha) {
    struct Guard {
        const Presentation3D*& value;
        ~Guard() { value = nullptr; }
    } guard{_presentation};
    _presentation = &presentation;
    struct PoseGuard {
        const PosePresentation3D*& value;
        ~PoseGuard() { value = nullptr; }
    } pose_guard{_pose_presentation};
    _pose_presentation = &poses;
    struct IKGuard {
        const IKPresentation3D*& value;
        ~IKGuard() { value = nullptr; }
    } ik_guard{_ik_presentation};
    _ik_presentation = &ik;
    auto& tracking = *_tracking;
    const bool tracked = tracking.mode == SceneMutationMode3D::Tracked;
    const bool simple = tracked && !presentation && _animators.count() == 0 && _skinned.count() == 0 &&
                        _attachments.count() == 0 && _point_lights.count() == 0 && _spot_lights.count() == 0 &&
                        _emitters.count() == 0;
    if (simple && tracking.collection && !tracking.topology) {
        std::unordered_map<u32, std::vector<u32>> edits;
        for (auto id : tracking.changed) {
            auto binding = tracking.bindings.find(id);
            if (binding == tracking.bindings.end())
                continue;
            flecs::entity entity(tracking.world, id);
            const auto* world = entity.get<WorldTransform3D>();
            const auto placement = draw_placement(entity, *world, entity.get<RenderTransform3D>());
            if (!placement) {
                tracking.collection = false;
                break;
            }
            const auto* record = &binding->second;
            const auto* model =
                record->part == ~u32{0} && record->next == ~u32{0} ? nullptr : entity.get<ModelRenderer3D>();
            while (record) {
                if (record->part != ~u32{0} &&
                    (!model || !model->model || record->part >= model->model->parts.size())) {
                    tracking.collection = false;
                    break;
                }
                auto& bucket = _buckets[record->bucket];
                bucket.instances[record->slot].transform =
                    record->part == ~u32{0} ? *placement : *placement * model->model->parts[record->part].transform;
                edits[record->bucket].push_back(record->slot);
                ++tracking.stats.collection_updates;
                record = record->next == ~u32{0} ? nullptr : &tracking.extra_bindings[record->next];
            }
            if (!tracking.collection)
                break;
        }
        if (tracking.collection) {
            for (auto& [index, slots] : edits) {
                auto& bucket = _buckets[index];
                if (!bucket.batch)
                    continue;
                if (bucket.batch.lifetime().use_count() > 1) {
                    std::swap(bucket.batch, bucket.spare);
                    bucket.rebuild();
                    continue;
                }
                std::sort(slots.begin(), slots.end());
                for (std::size_t i = 0; i < slots.size();) {
                    const u32 first = slots[i++];
                    u32 end = first + 1;
                    while (i < slots.size() && slots[i] == end) {
                        ++end;
                        ++i;
                    }
                    if (!bucket.batch.update(
                            first, std::span<const MeshInstance3D>(bucket.instances).subspan(first, end - first)))
                        bucket.rebuild();
                }
            }
            for (auto& bucket : _buckets) {
                if (bucket.batch)
                    (void)scene.draw_instanced(bucket.batch, bucket.material);
                else
                    (void)scene.draw_instanced(bucket.mesh, bucket.instances, bucket.material);
            }
            for (auto id : tracking.changed)
                tracking.nodes[tracking.indices.at(id)].collected = false;
            tracking.changed.clear();
            ++tracking.stats.collection_reuses;
            return;
        }
    }
    ++tracking.stats.collection_fallbacks;
    tracking.collection = false;
    tracking.bindings.clear();
    tracking.extra_bindings.clear();
    prepare_joint_attachments();
    auto& by_mesh = _bucket_lookup;
    if (_bucket_lookup_dirty) {
        by_mesh.clear();
        for (std::size_t i = 0; i < _buckets.size(); ++i)
            by_mesh[_buckets[i].mesh.backend().get()].push_back(i);
        _bucket_lookup_dirty = false;
    }
    const auto bucket_for = [&](const Mesh3D& mesh, const Material3D& material) -> Bucket& {
        auto& candidates = by_mesh[mesh.backend().get()];
        for (const auto index : candidates)
            if (_buckets[index].material == material)
                return _buckets[index];
        candidates.push_back(_buckets.size());
        return _buckets.emplace_back(Bucket{mesh, material, {}, {}});
    };
    const auto bind = [&](flecs::entity_t id, Bucket& bucket, u32 part = ~u32{0}) {
        if (!simple)
            return;
        Tracking::Binding record{static_cast<u32>(&bucket - _buckets.data()), static_cast<u32>(bucket.instances.size()),
                                 part};
        auto [entry, inserted] = tracking.bindings.try_emplace(id, record);
        if (!inserted) {
            record.next = entry->second.next;
            entry->second.next = static_cast<u32>(tracking.extra_bindings.size());
            tracking.extra_bindings.push_back(record);
        }
    };
    for (Bucket& bucket : _buckets) {
        bucket.instances.clear();
    }
    _meshes.run([&](flecs::iter& it) {
        while (it.next()) {
            const auto world = it.field<const WorldTransform3D>(0);
            const auto renderers = it.field<const MeshRenderer3D>(1);
            const auto presentation = it.field<const RenderTransform3D>(2);
            const bool own_world = it.is_self(0), own_renderer = it.is_self(1), own_presentation = it.is_self(2);
            Bucket* last = nullptr; // entities of a table mostly share a mesh
            for (const auto i : it) {
                ++tracking.stats.collection_visits;
                const MeshRenderer3D& r = renderers[own_renderer ? i : 0];
                if (!r.visible || !r.mesh.valid()) {
                    continue;
                }
                const auto placement = draw_placement(it.entity(i), world[own_world ? i : 0],
                                                      it.is_set(2) ? &presentation[own_presentation ? i : 0] : nullptr);
                if (!placement)
                    continue;
                if (!last || !(last->mesh == r.mesh) || !(last->material == r.material)) {
                    last = &bucket_for(r.mesh, r.material);
                }
                // Picking retains the low entity index; history includes generation, scope and lease.
                bind(it.entity(i).id(), *last);
                last->instances.push_back(
                    {*placement, r.tint, r.id ? r.id : u32(it.entity(i).id() & 0xFFFFFFFFu), history_id(it.entity(i))});
            }
        }
    });
    _models.each([&](flecs::entity entity, const WorldTransform3D& world, const ModelRenderer3D& renderer,
                     const RenderTransform3D* presentation, Animator3D* animator) {
        ++tracking.stats.collection_visits;
        if (!renderer.visible || !renderer.model)
            return;
        const auto placement = draw_placement(entity, world, presentation);
        if (!placement)
            return;
        const u32 id = renderer.id ? renderer.id : u32(entity.id() & 0xFFFFFFFFu);
        const auto* overrides = entity.get<ModelMaterials3D>();
        const auto* binding = overrides && overrides->binding && overrides->binding->model.lock() == renderer.model &&
                                      overrides->binding->materials.size() == renderer.model->parts.size()
                                  ? overrides->binding.get()
                                  : nullptr;
        // The animator's pose of the model's animated nodes (their world matrices), once animate() has bound it to
        // them. None: every part at rest, exactly as without an animator.
        std::span<const Mat4f> nodes;
        const Model3D& model = *renderer.model;
        if (animator && model.node_skeleton >= 0 &&
            static_cast<std::size_t>(model.node_skeleton) < model.skeletons.size()) {
            const Skeleton3D& skeleton = model.skeletons[static_cast<std::size_t>(model.node_skeleton)];
            if (animator->bound_to(renderer.model) && animator->player.skeleton() == &skeleton) {
                if (animator->ik_constraints.empty() && animator->pose_override.empty()) {
                    compute_world_matrices(skeleton, animator->player.pose(), _pose_scratch);
                    if (!animator->ik_world.empty()) {
                        animator->ik_world.clear();
                        animator->ik_pose.joints.clear();
                    }
                    nodes = _pose_scratch;
                } else {
                    if (!attachment_pose_prepared(entity.id()))
                        solve_pose(*animator, skeleton, animator->player.pose(), *placement, entity.world(), _epoch,
                                   &pose_override_for(entity, *animator));
                    nodes = animator->ik_world;
                }
            }
        }
        for (size_t index = 0; index < model.parts.size(); ++index) {
            const auto& part = model.parts[index];
            const auto& material = binding ? binding->materials[index] : part.material;
            if (!part.mesh || part.mesh.skinned() || part.skin >= 0)
                continue;
            auto& bucket = bucket_for(part.mesh, material);
            bind(entity.id(), bucket, static_cast<u32>(index));
            bucket.instances.push_back(
                {*placement * (nodes.empty() ? part.transform : model.part_transform(index, nodes)), renderer.tint, id,
                 detail::part_history3d(history_id(entity), index)});
        }
    });
    const bool empty_buckets =
        std::any_of(_buckets.begin(), _buckets.end(), [](const Bucket& b) { return b.instances.empty(); });
    tracking.collection = simple && !empty_buckets;
    for (auto id : tracking.changed)
        if (auto found = tracking.indices.find(id); found != tracking.indices.end())
            tracking.nodes[found->second].collected = false;
    tracking.changed.clear();
    // A bucket is a reuse aid for live draws, never a resident asset cache.
    if (std::erase_if(_buckets, [](const Bucket& bucket) { return bucket.instances.empty(); }))
        _bucket_lookup_dirty = true;
    RetainedBudget budget;
    for (Bucket& bucket : _buckets) {
        if (!bucket.eligible(budget)) {
            bucket.batch = {};
            bucket.spare = {};
            (void)scene.draw_instanced(bucket.mesh, bucket.instances, bucket.material);
            continue;
        }
        bucket.retain(budget);
        (void)scene.draw_instanced(bucket.batch, bucket.material);
    }
    // Lights (P3b): the entity's place, and for a spot its -Z axis; the entity's id names the light across frames.
    _point_lights.each([&](flecs::entity entity, const WorldTransform3D& world, const PointLight3D& light,
                           const RenderTransform3D* presentation) {
        const auto matrix = draw_placement(entity, world, presentation);
        if (!matrix)
            return;
        const WorldTransform3D placement{*matrix};
        if (light.enabled) {
            scene.add_point_light({.position = placement.position(),
                                   .color = light.color,
                                   .radius = light.radius,
                                   .size = light.size,
                                   .shadow_enabled = light.shadow_enabled,
                                   .shadow_resolution = light.shadow_resolution,
                                   .shadow_bias = light.shadow_bias,
                                   .shadow_normal_offset = light.shadow_normal_offset,
                                   .shadow_near_plane = light.shadow_near_plane,
                                   .shadow_distance = light.shadow_distance,
                                   .shadow_softness = light.shadow_softness,
                                   .id = static_cast<u32>(entity.id() & 0xFFFFFFFFu)});
        }
    });
    _spot_lights.each([&](flecs::entity entity, const WorldTransform3D& world, const SpotLight3D& light,
                          const RenderTransform3D* presentation) {
        const auto matrix = draw_placement(entity, world, presentation);
        if (!matrix)
            return;
        const WorldTransform3D placement{*matrix};
        if (light.enabled) {
            const Vec3f axis = placement.matrix.apply_vector({0.0f, 0.0f, -1.0f});
            scene.add_spot_light({.position = placement.position(),
                                  .direction = length_squared(axis) > 0.0f ? normalize(axis) : Vec3f{0.0f, -1.0f, 0.0f},
                                  .color = light.color,
                                  .radius = light.radius,
                                  .size = light.size,
                                  .inner_angle = light.inner_angle,
                                  .outer_angle = light.outer_angle,
                                  .shadow_enabled = light.shadow_enabled,
                                  .shadow_resolution = light.shadow_resolution,
                                  .shadow_bias = light.shadow_bias,
                                  .shadow_normal_offset = light.shadow_normal_offset,
                                  .shadow_near_plane = light.shadow_near_plane,
                                  .shadow_distance = light.shadow_distance,
                                  .shadow_softness = light.shadow_softness,
                                  .id = static_cast<u32>(entity.id() & 0xFFFFFFFFu)});
        }
    });
    // Particles (P3c): each enabled emitter's live particles, as a batch with its desc's look.
    _emitters.each([&](flecs::entity entity, const WorldTransform3D& world, const ParticleEmitter3D& emitter) {
        if (!draw_placement(entity, world, nullptr))
            return;
        if (emitter.enabled && emitter.system.count() > 0) {
            if (alpha >= 1) {
                (void)scene.draw_particles(emitter.system, history_id(entity));
                return;
            }
            _particle_sprites.clear();
            emitter.system.sprites(_particle_sprites, alpha);
            (void)scene.draw_particles(emitter.system.desc.look, _particle_sprites, history_id(entity));
        }
    });
    // Characters: each model in its animator's pose (the rest pose with none, or before its first animate()).
    _skinned.each([&](flecs::iter& it, size_t row, const WorldTransform3D& world, const SkinnedMeshRenderer3D& renderer,
                      Animator3D* animator, const RenderTransform3D* presentation) {
        const Model3D* model = renderer.model.get();
        if (!renderer.visible || !model || !has_skin(*model)) {
            return;
        }
        const auto placement = draw_placement(it.entity(row), world, presentation);
        if (!placement)
            return;
        const Skeleton3D& skeleton = model->skeletons[0];
        _pose_scratch.resize(skeleton.joint_count());
        const bool playing = animator && animator->bound_to(renderer.model) && animator->player.skeleton() == &skeleton;
        const auto* override = animator ? &pose_override_for(it.entity(row), *animator) : nullptr;
        if (playing || (override && !override->empty())) {
            if (!playing)
                _rest_pose.joints = skeleton.rest;
            const Pose3D& base = playing ? animator->player.pose() : _rest_pose;
            if (animator->ik_constraints.empty() && override->empty()) {
                animator->player.joint_matrices(_pose_scratch);
                if (!animator->ik_world.empty()) {
                    animator->ik_world.clear();
                    animator->ik_pose.joints.clear();
                }
            } else {
                // The clips' pose with the constraints solved on it (and the physics override over that), in this
                // frame's place of the entity.
                if (!attachment_pose_prepared(it.entity(row).id()))
                    solve_pose(*animator, skeleton, base, *placement, it.world(), _epoch,
                               &pose_override_for(it.entity(row), *animator));
                world_to_joint_matrices(skeleton, animator->ik_world, _pose_scratch);
            }
        } else {
            _rest_pose.joints = skeleton.rest;
            compute_joint_matrices(skeleton, _rest_pose, _pose_scratch);
        }
        // Picking and temporal history have independent identities.
        const u32 id = renderer.id != 0 ? renderer.id : static_cast<u32>(it.entity(row).id() & 0xffffffffu);
        const auto* overrides = it.entity(row).get<ModelMaterials3D>();
        const auto* binding = overrides && overrides->binding && overrides->binding->model.lock() == renderer.model &&
                                      overrides->binding->materials.size() == model->parts.size()
                                  ? overrides->binding.get()
                                  : nullptr;
        (void)scene.draw_skinned(*model, _pose_scratch, *placement, renderer.tint, id,
                                 binding ? std::span<const Material3D>{binding->materials}
                                         : std::span<const Material3D>{},
                                 history_id(it.entity(row)));
    });
    // Meshes no longer drawn are let go.
    if (std::erase_if(_buckets, [](const Bucket& bucket) { return bucket.instances.empty(); }))
        _bucket_lookup_dirty = true;
}

} // namespace kin
