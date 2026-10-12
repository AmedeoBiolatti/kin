#include "view.hpp"

#include "../sponza_walk/character.hpp"
#include "../sponza_walk/ik_rig.hpp"

#include <kin/render3d/timeofday3d.hpp>

#include <algorithm>
#include <cmath>

namespace summit {

namespace {

// A rotation taking +Y to the unit vector `d`.
Quatf from_up_to(Vec3f d) {
    const f32 c = std::clamp(d.y, -1.0f, 1.0f);
    if (c > 0.99999f) return {};
    if (c < -0.99999f) return Quatf::axis_angle({1.0f, 0.0f, 0.0f}, 180.0f);
    const Vec3f axis = cross({0.0f, 1.0f, 0.0f}, d);
    return Quatf::axis_angle(axis, std::acos(c) * 180.0f / pi);
}

} // namespace

const std::vector<Look>& looks() {
    //                  name         hours turb   fog      hfog     rain  snow  mist  exp   bloom stars
    static const std::vector<Look> all = {
        {"afternoon", 16.2f, 2.6f, 0.00045f, 0.0030f, 0.0f, 0.0f, 0.0f, 1.0f, 0.25f, false},
        {"morning", 7.3f, 3.0f, 0.00080f, 0.0100f, 0.0f, 0.0f, 0.15f, 1.0f, 0.30f, false},
        {"noon", 12.4f, 2.3f, 0.00030f, 0.0015f, 0.0f, 0.0f, 0.0f, 0.95f, 0.20f, false},
        {"dusk", 18.9f, 3.2f, 0.00090f, 0.0040f, 0.0f, 0.0f, 0.0f, 1.2f, 0.35f, false},
        {"night", 1.4f, 2.4f, 0.00060f, 0.0020f, 0.0f, 0.0f, 0.0f, 2.2f, 0.30f, true},
        {"snow", 14.0f, 3.5f, 0.00300f, 0.0100f, 0.0f, 0.85f, 0.2f, 1.0f, 0.20f, false},
        {"storm", 16.6f, 4.5f, 0.00450f, 0.0100f, 1.0f, 0.0f, 0.45f, 1.0f, 0.25f, false},
    };
    return all;
}

const Look* find_look(const std::string& name) {
    for (const Look& l : looks()) {
        if (name == l.name) return &l;
    }
    return nullptr;
}

SummitView::SummitView(Renderer3D& r3d, const SummitLevel& level) : _r3d(&r3d), _look(&looks().front()), _bounds(level.bounds) {
    for (const RenderMeshData& m : build_render_meshes(level)) {
        _rock.push_back({r3d.create_mesh(m.mesh), m.material});
    }
    _cylinder = r3d.create_mesh(meshes::cylinder(1.0f, 1.0f, 8));
    _cone = r3d.create_mesh(meshes::cone(1.0f, 1.0f, 10));
    _box = r3d.create_mesh(meshes::box({1.0f, 1.0f, 1.0f}));
    for (const Camp& c : level.camps) _camp_points.push_back(c.marker);
    _summit = level.summit;
    if (r3d.capabilities().skinning) {
        // The fixture's clips are a T pose at rest: an "Idle" with the arms down for standing still.
        {
            auto model = std::make_shared<Model>(*sponza_walk::make_person(r3d));
            const Skeleton& sk = model->skeletons[0];
            AnimClip3D idle;
            idle.name = "Idle";
            idle.duration = 4.0f;
            idle.channels.resize(sk.joint_count());
            const auto hold = [&](const char* joint, Vec3f axis, f32 degrees) {
                const i32 j = sk.find(joint);
                if (j < 0) return;
                Track3D<Quatf>& t = idle.channels[std::size_t(j)].rotation;
                t.times = {0.0f, 2.0f, 4.0f};
                t.values = {Quatf::axis_angle(axis, degrees), Quatf::axis_angle(axis, degrees * 0.94f), Quatf::axis_angle(axis, degrees)};
            };
            hold("shoulder_L", {0, 0, 1}, -72.0f);
            hold("shoulder_R", {0, 0, 1}, 72.0f);
            hold("elbow_L", {0, 1, 0}, -18.0f);
            hold("elbow_R", {0, 1, 0}, 18.0f);
            model->clips.push_back(std::move(idle));
            _person_model = std::move(model);
        }
        _ecs = std::make_unique<::kin::EcsWorld>();
        _state = std::make_unique<WorldRenderState3D>(*_ecs);
        Animator3D animator = locomotion_animator({.idle = "Idle", .walk = "Walk", .run = "Run", .fall = "Fall"});
        IKConstraint feet = sponza_walk::person_constraints(nullptr)[0];
        feet.fade_seconds = 0.12f;
        animator.add_ik(feet);
        // The tail hangs down (the rig's rest tail points straight back).
        IKConstraint tail = sponza_walk::person_constraints(nullptr)[3];
        tail.weight = 1.0f;
        animator.add_ik(tail);
        for (const char* side : {"L", "R"}) {
            IKConstraint hand;
            hand.name = std::string("hand_") + side;
            hand.kind = IKKind::TwoBone;
            hand.joints = {std::string("shoulder_") + side, std::string("elbow_") + side, std::string("hand_") + side};
            hand.weight = 0.0f;
            hand.fade_seconds = 0.12f;
            hand.pole = IKConstraint::Pole::ModelDirection;
            hand.pole_vector = {side[0] == 'L' ? 0.5f : -0.5f, -0.7f, -0.5f};
            hand.two_bone.max_stretch = 1.05f;
            animator.add_ik(hand);
        }
        _person = _ecs->raw().entity().set(Transform3D{}).set(SkinnedMeshRenderer{.model = _person_model}).set(static_cast<const Animator3D&>(animator));
        _person_ok = true;
    }
}

bool SummitView::set_look(const std::string& name) {
    const Look* l = find_look(name);
    if (l == nullptr) return false;
    _look = l;
    return true;
}

View3DOptions SummitView::view_options(u32 samples) const {
    return {.samples = samples, .background = Color::rgb(120, 150, 190), .hdr = true, .exposure = _look->exposure, .bloom = _look->bloom,
            .bloom_threshold = 1.0f};
}

void SummitView::drive_person(const Game& game, i32 player, f32 dt) {
    if (!_person_ok || !_person.is_alive()) return;
    const Player& p = game.player(player);
    const CharacterState3D cs = game.character_state(player);
    Transform3D* t = _person.get_mut<Transform3D>();
    t->position = cs.position;
    t->rotation = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, p.facing_yaw);
    Animator3D* a = _person.get_mut<Animator3D>();
    a->ik("tail").set_target(t->position + t->rotation.rotate({0.0f, 0.2f, -0.35f})).set_weight(1.0f);
    CharacterAnimationLink3D link;
    link.hands = {"hand_L", "hand_R"};
    link.hand_clearance = 0.05f;
    link.ground_mask = layer_bit(layers3d::Static);
    drive_character_animator(*a, cs, game.world(), link);
    drive_traversal_animator(*a, game.traversal(player), link, {0.0f, 1.0f, 0.0f});
    // The ragdoll's pose over the clips while it is the body (and while it blends back).
    game.ragdoll_pose(player, _bones);
    PoseOverride3D& o = a->pose_override;
    bool any = false;
    for (const RagdollBonePose3D& b : _bones) any = any || b.weight > 0.0f;
    if (!any) {
        o.clear();
    } else {
        const u32 joints = game.skeleton_joints();
        o.weights.assign(joints, 0.0f);
        o.position.assign(joints, Vec3f{});
        o.rotation.assign(joints, Quatf{});
        const std::span<const i32> map = game.ragdoll_joints();
        for (std::size_t k = 0; k < map.size() && k < _bones.size(); ++k) {
            const std::size_t j = std::size_t(map[k]);
            o.weights[j] = _bones[k].weight;
            o.position[j] = _bones[k].position;
            o.rotation[j] = _bones[k].rotation;
        }
    }
    _state->animate(dt);
}

void SummitView::fill(Scene3D& scene, const Game& game, const CameraView& camera, f32 dt, i32 player) {
    scene.clear();
    scene.camera = camera.camera();
    Lights3D& l = scene.lights;
    l = Lights3D{};
    const Look& k = *_look;
    scene.sky = {.enabled = true, .turbidity = k.turbidity, .sun_intensity = 3.0f, .intensity = 1.0f, .sun_size = 1.0f, .sun_disc = 30.0f,
                 .stars = k.stars, .moon = false};
    scene.fog = {.enabled = true, .density = k.fog_density, .height_density = k.fog_height_density, .height_falloff = 0.02f,
                 .base_height = game.level().start.y, .colour_from_sky = true, .sun_scatter = 0.25f, .sun_anisotropy = 0.6f};
    l.sky_lights_scene = true;
    kin::apply({.hours = k.hours, .day_of_year = 172.0f, .latitude = 46.0f}, l, scene.sky, scene.fog);
    l.shadow_area = _bounds;
    l.shadow_resolution = 2048;
    l.shadow_cascades = 3;
    l.shadow_distance = 160.0f;
    Weather3D& w = scene.weather;
    w = Weather3D{};
    w.rain.intensity = k.rain;
    w.snow.intensity = k.snow;
    w.mist.intensity = k.mist;
    const Wind& wind = game.wind();
    const f32 a = wind.direction * pi / 180.0f;
    w.wind.direction = {std::cos(a), 0.0f, std::sin(a)};
    w.wind.speed = wind.speed;
    w.time = game.time();
    if (w.active()) {
        const Fog3D base = scene.fog;
        w.apply(l, scene.fog, base);
    }

    for (const Part& p : _rock) scene.draw(p.mesh, Mat4f::identity(), p.material);

    // Camps: a tent and a flag; the summit: a pole and a flag.
    const Material3D tent{.color = Color::rgb(214, 96, 60), .double_sided = true, .roughness = 0.8f};
    const Material3D pole{.color = Color::rgb(230, 230, 225), .roughness = 0.6f};
    const Material3D flag{.color = Color::rgb(235, 190, 40), .double_sided = true, .roughness = 0.7f};
    for (std::size_t i = 0; i < _camp_points.size(); ++i) {
        const Vec3f c = _camp_points[i];
        scene.draw(_cone, Mat4f::trs(c + Vec3f{0.0f, 0.65f, 0.0f}, {}, {1.3f, 1.3f, 1.3f}), tent);
        scene.draw(_cylinder, Mat4f::trs(c + Vec3f{-1.4f, 1.2f, 0.0f}, {}, {0.04f, 2.4f, 0.04f}), pole);
        scene.draw(_box, Mat4f::trs(c + Vec3f{-1.4f, 2.1f, 0.35f}, {}, {0.02f, 0.4f, 0.7f}), flag);
    }
    scene.draw(_cylinder, Mat4f::trs(_summit + Vec3f{0.0f, 2.0f, 0.0f}, {}, {0.06f, 4.0f, 0.06f}), pole);
    scene.draw(_box, Mat4f::trs(_summit + Vec3f{0.0f, 3.4f, 0.55f}, {}, {0.03f, 0.7f, 1.1f}),
               Material3D{.color = Color::rgb(220, 40, 40), .double_sided = true, .roughness = 0.7f});

    // The rope(s): a cylinder to each segment.
    const Material3D rope{.color = Color::rgb(170, 130, 80), .roughness = 0.9f};
    for (const RopeHandle3D r : game.ropes()) {
        const f32 radius = game.world().rope_radius(r);
        for (const RopeSegment3D& s : game.world().rope_segments(r)) {
            const Vec3f d = s.b - s.a;
            const f32 len = length(d);
            if (len < 1e-4f) continue;
            scene.draw(_cylinder, Mat4f::trs((s.a + s.b) * 0.5f, from_up_to(d * (1.0f / len)), {radius * 1.3f, len + radius * 2.0f, radius * 1.3f}), rope);
        }
    }

    // The person.
    drive_person(game, player, dt);
    if (_person_ok) {
        _state->propagate_transforms();
        _state->collect(scene);
    } else {
        // Without skinning: a capsule's worth of box where it stands.
        const Vec3f f = game.feet(player);
        scene.draw(_box, Mat4f::trs(f + Vec3f{0.0f, 0.95f, 0.0f}, {}, {0.5f, 1.9f, 0.4f}), Material3D{.color = Color::rgb(215, 150, 110)});
    }
}

} // namespace summit
