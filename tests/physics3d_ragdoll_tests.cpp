// Ragdolls of kin/physics3d (kin/physics3d/ragdoll3d.hpp), without a GPU: the auto-built desc, the states and blends, bone
// lengths and limits under a fall, continuous collision, partial ragdolls, hand-over to and recovery from a character
// controller, the pose written into the animator, determinism with 0, 1 and 4 workers and saved states.
// docs/physics_3d.md ("Ragdolls").

#include "skin_fixtures.hpp"

#include <kin/core/jobs.hpp>
#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/world.hpp>
#include <kin/physics3d/world3d.hpp>
#include <kin/render3d/ik3d.hpp>
#include <kin/render3d/skeleton3d.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using namespace kin;

int failures = 0;
bool verbose = false;

#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        if (!(cond)) {                                                                               \
            ++failures;                                                                              \
            std::fprintf(stderr, "physics3d_ragdoll_tests:%d: CHECK failed: %s\n", __LINE__, #cond); \
        }                                                                                            \
    } while (0)

#define CHECK_NEAR(value, expected, eps)                                                                              \
    do {                                                                                                              \
        const double v_ = static_cast<double>(value), e_ = static_cast<double>(expected);                             \
        if (verbose) std::printf("  %s = %.4f (expected %.4f +- %.4f)\n", #value, v_, e_, static_cast<double>(eps));    \
        if (!(std::fabs(v_ - e_) <= static_cast<double>(eps))) {                                                      \
            ++failures;                                                                                               \
            std::fprintf(stderr, "physics3d_ragdoll_tests:%d: %s = %.5f, expected %.5f +- %.5f\n", __LINE__, #value, v_, \
                         e_, static_cast<double>(eps));                                                               \
        }                                                                                                             \
    } while (0)

constexpr f32 dt = 1.0f / 60.0f;
namespace B = skin_fixtures::biped;

World3DConfig serial() {
    World3DConfig c;
    c.single_threaded = true;
    return c;
}

bool near(Vec3f a, Vec3f b, f32 eps) { return length(a - b) <= eps; }

// A biped and a ragdoll of it at `at` (the model's origin is at the feet), over a floor whose top is y = 0.
struct Rig {
    Skeleton3D skeleton = skin_fixtures::make_biped();
    RagdollDesc desc;
    std::vector<Mat4f> model_world; // the rest pose, in the model's space
    World3D world;
    Body3D floor;
    RagdollHandle3D ragdoll;
    std::vector<Body3D> bodies;
    std::vector<Constraint3D> joints;
    Vec3f origin{};

    explicit Rig(Vec3f at = {0.0f, 0.0f, 0.0f}, World3DConfig cfg = serial(), bool with_floor = true,
                 const AutoRagdollOptions& options = {})
        : world(cfg) {
        origin = at;
        if (with_floor) {
            floor = world.create_body({.shape = Shape3D::box({20.0f, 0.5f, 20.0f}), .motion = Body3DMotion::Static,
                                       .position = {0.0f, -0.5f, 0.0f}, .friction = 0.6f, .user_id = 7});
        }
        desc = auto_ragdoll_desc(skeleton, skeleton.rest_pose(), options);
        compute_world_matrices(skeleton, skeleton.rest_pose(), model_world);
        make();
    }

    std::vector<Mat4f> bone_world(Vec3f at) const {
        std::vector<Mat4f> out;
        for (const RagdollBoneDesc3D& b : desc.bones) {
            out.push_back(Mat4f::translation(at) * model_world[static_cast<std::size_t>(b.joint)]);
        }
        return out;
    }
    void make() {
        ragdoll = world.create_ragdoll(desc, bone_world(origin));
        CHECK(ragdoll.valid());
        bodies = world.ragdoll_bodies(ragdoll);
    }
    i32 bone_of(i32 joint) const {
        for (std::size_t i = 0; i < desc.bones.size(); ++i) {
            if (desc.bones[i].joint == joint) {
                return static_cast<i32>(i);
            }
        }
        return -1;
    }
    Body3D body_of_joint(i32 joint) const { return bodies[static_cast<std::size_t>(bone_of(joint))]; }
    // The clip pose (here the rest pose) at `at`, for the kinematic bodies.
    void set_targets(Vec3f at) { world.set_ragdoll_targets(ragdoll, bone_world(at)); }
    void step(int n = 1) {
        for (int i = 0; i < n; ++i) {
            world.step(dt);
        }
    }
    // The lowest point of any bone's capsule (its axis's ends less the radius), and of any axis end alone.
    f32 lowest(bool axis_only = false) const {
        f32 low = 1e9f;
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            const RagdollBoneDesc3D& b = desc.bones[i];
            const Quatf q = world.rotation(bodies[i]);
            const Vec3f centre = world.position(bodies[i]) + q.rotate(b.offset);
            const Vec3f axis = (q * b.rotation).rotate({0.0f, 1.0f, 0.0f}) * b.half_height;
            const f32 r = axis_only ? 0.0f : b.radius;
            low = std::min({low, centre.y + axis.y - r, centre.y - axis.y - r});
        }
        return low;
    }
};

// ---- the desc from a skeleton -------------------------------------------------------------------------------------

void test_auto_desc() {
    const Skeleton3D sk = skin_fixtures::make_biped();
    const RagdollDesc d = auto_ragdoll_desc(sk, sk.rest_pose());
    // The pelvis and everything below it: pelvis, spine, chest, neck, head, hips, knees, ankles, shoulders, elbows, hands.
    CHECK(d.bones.size() == 17);
    CHECK(d.root_bone == 0 && d.bones[0].joint == B::pelvis && d.bones[0].parent == -1);
    for (std::size_t i = 0; i < d.bones.size(); ++i) {
        CHECK(d.bones[i].parent < static_cast<i32>(i));
        CHECK(i == 0 || d.bones[i].parent >= 0);
        CHECK(d.bones[i].radius >= 0.025f && d.bones[i].radius <= 0.12f);
        CHECK(d.bones[i].half_height > 0.0f);
    }
    // The thigh runs from the hip to the knee (0.5 m): a capsule of that length in all.
    const RagdollBoneDesc3D& thigh = d.bones[0].joint == B::pelvis ? d.bones[static_cast<std::size_t>(std::find_if(d.bones.begin(), d.bones.end(), [](const auto& b) { return b.joint == B::hip_l; }) - d.bones.begin())] : d.bones[0];
    CHECK_NEAR(2.0f * (thigh.half_height + thigh.radius), 0.5f, 0.01f);
    CHECK_NEAR(length(thigh.offset), 0.25f, 0.01f);
    // Knees and elbows are hinges, the rest swing-twist.
    int hinges = 0;
    for (const RagdollBoneDesc3D& b : d.bones) {
        const bool should = b.joint == B::knee_l || b.joint == B::knee_r || b.joint == B::elbow_l || b.joint == B::elbow_r;
        CHECK((b.joint_kind == RagdollJoint3D::Hinge) == should);
        hinges += b.joint_kind == RagdollJoint3D::Hinge ? 1 : 0;
    }
    CHECK(hinges == 4);
    // Mass shared by volume: 70 kg in all.
    f32 total = 0.0f;
    for (const RagdollBoneDesc3D& b : d.bones) {
        total += b.mass;
    }
    CHECK_NEAR(total, 70.0f, 0.5f);
    // A foot is a capsule along the way the character faces (+Z), not down into the floor.
    const RagdollBoneDesc3D& foot = d.bones[static_cast<std::size_t>(std::find_if(d.bones.begin(), d.bones.end(), [](const auto& b) { return b.joint == B::ankle_l; }) - d.bones.begin())];
    CHECK(foot.offset.z > 0.05f && std::fabs(foot.offset.y) < 0.01f);
    // Options: skip names, a bigger radius, no overrides.
    AutoRagdollOptions o;
    o.skip = {"hand"};
    o.radius_ratio = 0.3f;
    o.overrides.clear();
    const RagdollDesc d2 = auto_ragdoll_desc(sk, sk.rest_pose(), o);
    CHECK(d2.bones.size() == 15);
    CHECK(std::none_of(d2.bones.begin(), d2.bones.end(), [](const auto& b) { return b.joint_kind == RagdollJoint3D::Hinge; }));
    // A skeleton with a pose of the wrong size or nothing: an empty desc.
    CHECK(auto_ragdoll_desc(Skeleton3D{}, Pose3D{}).bones.empty());
}

void test_create_and_refusals() {
    Rig rig({0.0f, 0.0f, 0.0f});
    CHECK(rig.world.contains(rig.ragdoll) && rig.world.ragdoll_count() == 1);
    CHECK(rig.bodies.size() == 17);
    // 17 bodies and the floor; 16 joints.
    CHECK(rig.world.body_count() == 18);
    CHECK(rig.world.constraint_count() == 16);
    const RagdollState3D st = rig.world.ragdoll_state(rig.ragdoll);
    CHECK(st.mode == RagdollMode3D::Animated && st.alpha == 0.0f && st.dynamic_bodies == 0);
    for (const Body3D b : rig.bodies) {
        CHECK(rig.world.motion(b) == Body3DMotion::Kinematic);
    }
    // Refused: no bones, a wrong number of matrices, a parent that is not lower, a non-finite matrix.
    CHECK(!rig.world.create_ragdoll({}, {}).valid());
    CHECK(!rig.world.create_ragdoll(rig.desc, std::span<const Mat4f>{}).valid());
    RagdollDesc bad = rig.desc;
    bad.bones[3].parent = 5;
    CHECK(!rig.world.create_ragdoll(bad, rig.bone_world({})).valid());
    std::vector<Mat4f> nan_world = rig.bone_world({});
    nan_world[2].m[12] = std::nanf("");
    CHECK(!rig.world.create_ragdoll(rig.desc, nan_world).valid());
    CHECK(rig.world.ragdoll_count() == 1 && rig.world.body_count() == 18);
    // Destroyed: the bodies and joints go; the handle is stale.
    rig.world.destroy_ragdoll(rig.ragdoll);
    CHECK(!rig.world.contains(rig.ragdoll) && rig.world.body_count() == 1 && rig.world.constraint_count() == 0);
    RagdollHandle3D again = rig.world.create_ragdoll(rig.desc, rig.bone_world({}));
    CHECK(again.valid() && again.index == rig.ragdoll.index && again.generation != rig.ragdoll.generation);
}

// ---- animated: kinematic bodies on the clip -------------------------------------------------------------------------

void test_animated_follows_targets() {
    Rig rig({0.0f, 0.0f, 0.0f});
    // The clip moves the whole body 2 m along +X at 1 m/s: the bodies stay on it.
    for (int i = 1; i <= 120; ++i) {
        rig.set_targets({static_cast<f32>(i) * dt, 0.0f, 0.0f});
        rig.step();
    }
    const Vec3f want = Vec3f{2.0f, 1.0f, 0.0f};
    CHECK(near(rig.world.position(rig.body_of_joint(B::pelvis)), want, 0.03f));
    for (std::size_t i = 0; i < rig.bodies.size(); ++i) {
        const Vec3f at = rig.world.position(rig.bodies[i]);
        const Vec3f rest = Vec3f{rig.model_world[static_cast<std::size_t>(rig.desc.bones[i].joint)].m[12] + 2.0f,
                                 rig.model_world[static_cast<std::size_t>(rig.desc.bones[i].joint)].m[13],
                                 rig.model_world[static_cast<std::size_t>(rig.desc.bones[i].joint)].m[14]};
        CHECK(near(at, rest, 0.03f));
    }
}

void test_animated_bodies_are_hit() {
    // A ball thrown at the chest of an animated ragdoll is stopped by the kinematic bodies (they do not move).
    Rig rig({0.0f, 0.0f, 0.0f});
    const Body3D ball = rig.world.create_body({.shape = Shape3D::sphere(0.15f), .motion = Body3DMotion::Dynamic,
                                               .position = {0.0f, 1.3f, 3.0f}, .linear_velocity = {0.0f, 0.0f, -12.0f},
                                               .mass = 2.0f, .gravity_factor = 0.0f, .continuous = true});
    const Vec3f chest_before = rig.world.position(rig.body_of_joint(B::chest));
    rig.set_targets({0.0f, 0.0f, 0.0f});
    for (int i = 0; i < 90; ++i) {
        rig.set_targets({0.0f, 0.0f, 0.0f});
        rig.step();
    }
    CHECK(rig.world.position(ball).z > 0.0f);                                 // it did not go through
    CHECK(rig.world.linear_velocity(ball).z > -2.0f);                          // it bounced or stopped
    CHECK(near(rig.world.position(rig.body_of_joint(B::chest)), chest_before, 0.01f));
}

void test_parked_bodies_are_not_hit() {
    RagdollDesc d;
    {
        Rig probe;
        d = probe.desc;
    }
    Rig rig({0.0f, 0.0f, 0.0f});
    rig.world.destroy_ragdoll(rig.ragdoll);
    d.animated_bodies = false;
    rig.desc = d;
    rig.make();
    const Body3D ball = rig.world.create_body({.shape = Shape3D::sphere(0.15f), .motion = Body3DMotion::Dynamic,
                                               .position = {0.0f, 1.3f, 3.0f}, .linear_velocity = {0.0f, 0.0f, -12.0f},
                                               .mass = 2.0f, .gravity_factor = 0.0f});
    rig.step(2);
    for (int i = 0; i < 60; ++i) {
        rig.step();
    }
    CHECK(rig.world.position(ball).z < -1.0f); // through it: no collision while parked
    // Starting un-parks: the bodies are where the pose is and collide again.
    rig.world.start_ragdoll(rig.ragdoll, {});
    const RagdollState3D st = rig.world.ragdoll_state(rig.ragdoll);
    CHECK(st.mode == RagdollMode3D::Ragdoll && st.dynamic_bodies == 17);
}

// ---- falling ------------------------------------------------------------------------------------------------------------

// Drops a standing ragdoll `height` m and runs `steps`; returns the greatest joint separation seen (m).
f32 drop_and_measure(Rig& rig, int steps, f32* lowest_seen = nullptr) {
    f32 worst = 0.0f;
    f32 low = 1e9f;
    for (int i = 0; i < steps; ++i) {
        rig.step();
        for (const Constraint3D c : rig.world.constraints()) {
            const Constraint3DInfo info = rig.world.constraint_info(c);
            worst = std::max(worst, length(info.point_a - info.point_b));
        }
        low = std::min(low, rig.lowest());
        if (verbose && i % 6 == 0 && i < 120) {
            f32 now = 0.0f;
            for (const Constraint3D c : rig.world.constraints()) {
                const Constraint3DInfo info = rig.world.constraint_info(c);
                now = std::max(now, length(info.point_a - info.point_b));
            }
            std::printf("    step %d stretch %.4f lowest %.3f pelvis y %.3f\n", i, now, rig.lowest(), rig.world.position(rig.bodies[0]).y);
        }
    }
    if (lowest_seen != nullptr) {
        *lowest_seen = low;
    }
    return worst;
}

void test_fall_settles_and_holds_together() {
    Rig rig({0.0f, 2.0f, 0.0f});
    rig.set_targets({0.0f, 2.0f, 0.0f});
    rig.world.start_ragdoll(rig.ragdoll, {});
    f32 low = 0.0f;
    const f32 worst = drop_and_measure(rig, 360, &low);
    const RagdollState3D st = rig.world.ragdoll_state(rig.ragdoll);
    if (verbose) std::printf("  stretch %.4f, lowest %.3f, settled %d, awake %u, speed %.3f\n", worst, low, st.settled ? 1 : 0, st.awake_bodies, st.max_speed);
    CHECK(worst < 0.06f);          // no joint opened by more than 6 cm (the impact; 2 cm at rest)
    CHECK(low > -0.06f);           // nothing went through the floor (the capsules' skin a few cm into it at the impact)
    CHECK(st.settled);
    // Lying down on the floor: the pelvis near the floor, no body far from it.
    if (verbose) std::printf("  pelvis y %.3f\n", rig.world.position(rig.body_of_joint(B::pelvis)).y);
    CHECK(rig.world.position(rig.body_of_joint(B::pelvis)).y < 0.6f);
    const auto events = rig.world.drain_ragdoll_events();
    CHECK(events.size() == 2 && events[0].kind == RagdollEventKind3D::Started && events[1].kind == RagdollEventKind3D::Settled);
    // Asleep or nearly so, which is the cost control: after settling the bodies sleep by themselves.
    for (int i = 0; i < 120; ++i) {
        rig.step();
    }
    CHECK(rig.world.ragdoll_state(rig.ragdoll).awake_bodies == 0);
}

void test_freeze_on_settle() {
    Rig rig({0.0f, 1.0f, 0.0f});
    rig.world.destroy_ragdoll(rig.ragdoll);
    rig.desc.freeze_on_settle = true;
    rig.make();
    rig.set_targets({0.0f, 1.0f, 0.0f});
    rig.world.start_ragdoll(rig.ragdoll, {});
    for (int i = 0; i < 360; ++i) {
        rig.step();
        if (verbose && i % 30 == 0) {
            f32 w = 0.0f;
            for (const Body3D b : rig.bodies) w = std::max(w, length(rig.world.angular_velocity(b)));
            std::printf("    step %d speed %.3f ang %.3f awake %u\n", i, rig.world.ragdoll_state(rig.ragdoll).max_speed, w, rig.world.ragdoll_state(rig.ragdoll).awake_bodies);
        }
    }
    const RagdollState3D st = rig.world.ragdoll_state(rig.ragdoll);
    if (verbose) std::printf("  freeze: settled %d frozen %d dynamic %u awake %u mode %d\n", st.settled, st.frozen, st.dynamic_bodies, st.awake_bodies, static_cast<int>(st.mode));
    CHECK(st.settled && st.frozen && st.dynamic_bodies == 0 && st.awake_bodies == 0);
    for (const Body3D b : rig.bodies) {
        CHECK(rig.world.motion(b) == Body3DMotion::Kinematic && !rig.world.is_active(b));
    }
    // Still hittable (a ball bounces off a frozen body) and it stays where it lies.
    const Vec3f lying = rig.world.position(rig.body_of_joint(B::pelvis));
    // A ball thrown along the floor at the pelvis's capsule is stopped by it: a frozen body is still there to be hit.
    const std::size_t pi = static_cast<std::size_t>(rig.bone_of(B::pelvis));
    const Vec3f centre = lying + rig.world.rotation(rig.bodies[pi]).rotate(rig.desc.bones[pi].offset);
    f32 min_x = 1e9f, max_x = -1e9f;
    for (const Body3D b : rig.bodies) {
        min_x = std::min(min_x, rig.world.position(b).x);
        max_x = std::max(max_x, rig.world.position(b).x);
    }
    const Body3D ball = rig.world.create_body({.shape = Shape3D::sphere(0.06f), .motion = Body3DMotion::Dynamic,
                                               .position = {min_x - 1.5f, centre.y, centre.z},
                                               .linear_velocity = {6.0f, 0.0f, 0.0f}, .mass = 1.0f, .gravity_factor = 0.0f});
    rig.step(150);
    if (verbose) std::printf("  ball x %.3f, bodies x %.3f .. %.3f\n", rig.world.position(ball).x, min_x, max_x);
    CHECK(rig.world.position(ball).x < max_x + 0.1f);
    CHECK(near(rig.world.position(rig.body_of_joint(B::pelvis)), lying, 1e-5f));
    // Starting again thaws it.
    rig.world.start_ragdoll(rig.ragdoll, {});
    CHECK(!rig.world.ragdoll_state(rig.ragdoll).frozen && rig.world.ragdoll_state(rig.ragdoll).dynamic_bodies == 17);
}

void test_limits_respected() {
    Rig rig({0.0f, 2.0f, 0.0f});
    if (const char* v = std::getenv("RAGDOLL_VEL")) {
        rig.world.destroy_ragdoll(rig.ragdoll);
        rig.desc.velocity_steps = static_cast<u32>(std::atoi(v));
        rig.desc.position_steps = static_cast<u32>(std::atoi(std::getenv("RAGDOLL_POS")));
        rig.make();
    }
    rig.set_targets({0.0f, 2.0f, 0.0f});
    rig.world.start_ragdoll(rig.ragdoll, {.impulse_bone = rig.bone_of(B::chest), .impulse = {30.0f, 0.0f, 60.0f}});
    // Hinges: the knees bend 0 to 140 degrees about +X, the elbows -140 to 0 (left) and 0 to 140 (right) about Y.
    f32 knee_min = 1e9f, knee_max = -1e9f;
    for (int i = 0; i < 300; ++i) {
        rig.step();
        for (const Constraint3D c : rig.world.constraints()) {
            const Constraint3DInfo info = rig.world.constraint_info(c);
            if (info.kind == Constraint3DKind::Hinge && (info.b == rig.body_of_joint(B::knee_l) || info.b == rig.body_of_joint(B::knee_r))) {
                if (verbose && info.value < -1.0f) std::printf("    step %d knee %.1f\n", i, info.value);
                knee_min = std::min(knee_min, info.value);
                knee_max = std::max(knee_max, info.value);
            }
        }
    }
    if (verbose) std::printf("  knee angle %.1f .. %.1f\n", knee_min, knee_max);
    CHECK(knee_min > -3.0f && knee_max < 143.0f);
}

void test_no_tunnelling_at_speed() {
    // Thrown at 40 m/s into a thin floor (2 cm): continuous collision keeps every bone above it.
    World3D world(serial());
    const Skeleton3D sk = skin_fixtures::make_biped();
    const RagdollDesc d = auto_ragdoll_desc(sk, sk.rest_pose());
    std::vector<Mat4f> mw;
    compute_world_matrices(sk, sk.rest_pose(), mw);
    std::vector<Mat4f> bw;
    for (const auto& b : d.bones) {
        bw.push_back(Mat4f::translation({0.0f, 6.0f, 0.0f}) * mw[static_cast<std::size_t>(b.joint)]);
    }
    world.create_body({.shape = Shape3D::box({20.0f, 0.01f, 20.0f}), .motion = Body3DMotion::Static, .position = {0.0f, -0.01f, 0.0f}});
    // A wall as well.
    world.create_body({.shape = Shape3D::box({0.01f, 5.0f, 20.0f}), .motion = Body3DMotion::Static, .position = {3.0f, 3.0f, 0.0f}});
    const RagdollHandle3D r = world.create_ragdoll(d, bw);
    world.set_ragdoll_targets(r, bw);
    world.start_ragdoll(r, {.set_velocity = true, .velocity = {25.0f, -40.0f, 0.0f}});
    f32 lowest = 1e9f, furthest = -1e9f;
    const std::vector<Body3D> bodies = world.ragdoll_bodies(r);
    for (int i = 0; i < 180; ++i) {
        world.step(dt);
        for (const Body3D b : bodies) {
            lowest = std::min(lowest, world.position(b).y);
            furthest = std::max(furthest, world.position(b).x);
        }
    }
    CHECK(lowest > -0.1f);
    CHECK(furthest < 3.2f);
}

void test_connected_bones_do_not_collide() {
    // Without gravity a ragdoll at rest stays as it is: the capsules of neighbours overlap at the joints, and would push
    // each other apart if they collided. With self collision ON, so that the joints (not the group) are what keeps them from it.
    Rig rig({0.0f, 5.0f, 0.0f});
    rig.world.destroy_ragdoll(rig.ragdoll);
    rig.desc.self_collision = true;
    rig.make();
    rig.world.set_gravity({0.0f, 0.0f, 0.0f});
    rig.set_targets({0.0f, 5.0f, 0.0f});
    rig.world.start_ragdoll(rig.ragdoll, {});
    std::vector<Vec3f> before;
    for (const Body3D b : rig.bodies) {
        before.push_back(rig.world.position(b));
    }
    rig.step(120);
    f32 moved = 0.0f;
    for (std::size_t i = 0; i < rig.bodies.size(); ++i) {
        moved = std::max(moved, length(rig.world.position(rig.bodies[i]) - before[i]));
    }
    CHECK(moved < 0.01f);
    // And no contact is ever made between a bone and the one it hangs from (the joint is the pair's whole business), while
    // the world does report contacts of other pairs of the ragdoll's bones (the legs against the spine, say).
    int neighbours = 0;
    for (const ContactEvent3D& e : rig.world.drain_events()) {
        const auto a = std::find(rig.bodies.begin(), rig.bodies.end(), e.a);
        const auto b = std::find(rig.bodies.begin(), rig.bodies.end(), e.b);
        if (a == rig.bodies.end() || b == rig.bodies.end()) {
            continue;
        }
        const i32 ia = static_cast<i32>(a - rig.bodies.begin()), ib = static_cast<i32>(b - rig.bodies.begin());
        if (rig.desc.bones[static_cast<std::size_t>(ia)].parent == ib || rig.desc.bones[static_cast<std::size_t>(ib)].parent == ia) {
            ++neighbours;
        }
    }
    CHECK(neighbours == 0);
}

void test_disabled_character_is_left_alone() {
    // A character handed over is not updated: a held move command does not walk it, nothing is reported for it, its body
    // collides with nothing; enabled again it walks from where it stands.
    World3D world(serial());
    world.create_body({.shape = Shape3D::box({20.0f, 0.5f, 20.0f}), .motion = Body3DMotion::Static, .position = {0.0f, -0.5f, 0.0f}});
    CharacterDesc3D d;
    d.position = {0.0f, 0.0f, 0.0f};
    const Character3D c = world.create_character(d);
    CharacterInput3D in;
    in.forward = {0.0f, 0.0f, -1.0f};
    in.move = {0.0f, 1.0f};
    world.set_character_input(c, in);
    world.step(dt);
    world.set_character_enabled(c, false);
    CHECK(!world.character_enabled(c));
    const Vec3f at = world.character(c).position;
    for (int i = 0; i < 60; ++i) {
        world.step(dt);
    }
    CHECK(near(world.character(c).position, at, 1e-5f));
    // Its body is in no one's way: a ball dropped on it falls through to the floor.
    const Body3D ball = world.create_body({.shape = Shape3D::sphere(0.2f), .motion = Body3DMotion::Dynamic,
                                           .position = at + Vec3f{0.0f, 3.0f, 0.0f}, .mass = 2.0f});
    for (int i = 0; i < 120; ++i) {
        world.step(dt);
    }
    CHECK(world.position(ball).y < 0.3f);
    world.set_character_enabled(c, true);
    for (int i = 0; i < 60; ++i) {
        world.step(dt);
    }
    CHECK(world.character(c).position.z < at.z - 2.0f);
}

void test_self_collision_flag() {
    // Two thighs put through each other: with self collision on they push apart, with it off they stay overlapped.
    f32 gap[2] = {0.0f, 0.0f};
    for (int with = 0; with < 2; ++with) {
        Rig rig({0.0f, 5.0f, 0.0f});
        rig.world.destroy_ragdoll(rig.ragdoll);
        rig.desc.self_collision = with == 1;
        rig.make();
        rig.world.set_gravity({0.0f, 0.0f, 0.0f});
        rig.set_targets({0.0f, 5.0f, 0.0f});
        rig.world.start_ragdoll(rig.ragdoll, {});
        const Body3D l = rig.body_of_joint(B::hip_l), r = rig.body_of_joint(B::hip_r);
        rig.world.set_position(r, rig.world.position(l) + Vec3f{0.02f, 0.0f, 0.0f});
        rig.world.set_linear_velocity(r, {});
        rig.step(30);
        gap[with] = length(rig.world.position(r) - rig.world.position(l));
    }
    if (verbose) std::printf("  thigh gap: off %.3f on %.3f\n", gap[0], gap[1]);
    CHECK(gap[0] < 0.2f);     // off: still through each other (the joints hold them near)
    CHECK(gap[1] > gap[0] + 0.02f); // on: pushed apart
}

// ---- states and blends ----------------------------------------------------------------------------------------------

void test_blend_and_get_up() {
    Rig rig({0.0f, 1.5f, 0.0f});
    rig.set_targets({0.0f, 1.5f, 0.0f});
    std::vector<RagdollBonePose3D> pose;
    rig.world.ragdoll_pose(rig.ragdoll, pose);
    CHECK(pose.size() == 17 && std::all_of(pose.begin(), pose.end(), [](const auto& p) { return p.weight == 0.0f; }));
    // Blend in over 0.5 s: the share rises monotonically from 0 to 1 and the bodies are the pose's.
    rig.world.start_ragdoll(rig.ragdoll, {.blend_seconds = 0.5f});
    f32 last = 0.0f;
    bool monotone = true;
    for (int i = 0; i < 40; ++i) {
        rig.step();
        const f32 a = rig.world.ragdoll_state(rig.ragdoll).alpha;
        monotone = monotone && a >= last;
        last = a;
    }
    CHECK(monotone && last == 1.0f);
    rig.world.ragdoll_pose(rig.ragdoll, pose);
    for (std::size_t i = 0; i < pose.size(); ++i) {
        CHECK(pose[i].weight == 1.0f);
        CHECK(near(pose[i].position, rig.world.position(rig.bodies[i]), 1e-6f));
    }
    // Let it lie, then get up over 0.8 s: the share falls monotonically to 0 and Recovered comes once.
    rig.step(240);
    rig.world.drain_ragdoll_events();
    rig.world.ragdoll_get_up(rig.ragdoll, 0.8f);
    CHECK(rig.world.ragdoll_state(rig.ragdoll).mode == RagdollMode3D::GetUp);
    last = 1.0f;
    monotone = true;
    int recovered = 0;
    for (int i = 0; i < 60; ++i) {
        rig.set_targets({0.0f, 0.0f, 0.0f});
        rig.step();
        const f32 a = rig.world.ragdoll_state(rig.ragdoll).alpha;
        monotone = monotone && a <= last;
        last = a;
        for (const RagdollEvent3D& e : rig.world.drain_ragdoll_events()) {
            recovered += e.kind == RagdollEventKind3D::Recovered ? 1 : 0;
        }
    }
    CHECK(monotone && last == 0.0f && recovered == 1);
    const RagdollState3D st = rig.world.ragdoll_state(rig.ragdoll);
    CHECK(st.mode == RagdollMode3D::Animated && st.dynamic_bodies == 0);
    rig.world.ragdoll_pose(rig.ragdoll, pose);
    CHECK(std::all_of(pose.begin(), pose.end(), [](const auto& p) { return p.weight == 0.0f; }));
    // During the get-up the pose blended from where it lay: the bodies were already back on the clip.
    // A Blend of 0.5 held.
    rig.world.blend_ragdoll(rig.ragdoll, 0.5f, 0.2f);
    rig.step(30);
    CHECK_NEAR(rig.world.ragdoll_state(rig.ragdoll).alpha, 0.5f, 1e-6f);
    rig.world.ragdoll_pose(rig.ragdoll, pose);
    CHECK_NEAR(pose[3].weight, 0.5f, 1e-6f);
    // Per-bone weights multiply it.
    std::vector<f32> w(17, 1.0f);
    w[3] = 0.5f;
    w[4] = 0.0f;
    rig.world.set_ragdoll_bone_weights(rig.ragdoll, w);
    rig.world.ragdoll_pose(rig.ragdoll, pose);
    CHECK_NEAR(pose[3].weight, 0.25f, 1e-6f);
    CHECK(pose[4].weight == 0.0f);
    // Blend back to 0 over a time: Animated again, with no Recovered (that is the get-up's).
    rig.world.blend_ragdoll(rig.ragdoll, 0.0f, 0.2f);
    rig.step(30);
    CHECK(rig.world.ragdoll_state(rig.ragdoll).mode == RagdollMode3D::Animated);
    for (const RagdollEvent3D& e : rig.world.drain_ragdoll_events()) {
        CHECK(e.kind != RagdollEventKind3D::Recovered);
    }
}

void test_get_up_blends_from_the_lying_pose() {
    Rig rig({0.0f, 1.0f, 0.0f});
    rig.set_targets({0.0f, 1.0f, 0.0f});
    rig.world.start_ragdoll(rig.ragdoll, {});
    rig.step(240);
    std::vector<RagdollBonePose3D> lying;
    rig.world.ragdoll_pose(rig.ragdoll, lying);
    rig.set_targets({0.0f, 0.0f, 0.0f});
    rig.world.ragdoll_get_up(rig.ragdoll, 1.0f);
    std::vector<RagdollBonePose3D> now;
    rig.world.ragdoll_pose(rig.ragdoll, now);
    // The first frame of the get-up shows the pose it lay in (the snapshot), at nearly the whole share.
    for (std::size_t i = 0; i < now.size(); ++i) {
        CHECK(near(now[i].position, lying[i].position, 1e-5f));
    }
    rig.step(10);
    rig.world.ragdoll_pose(rig.ragdoll, now);
    for (std::size_t i = 0; i < now.size(); ++i) {
        CHECK(near(now[i].position, lying[i].position, 1e-5f)); // the snapshot does not drift
        CHECK(now[i].weight < 1.0f && now[i].weight > 0.0f);
    }
    // The bodies themselves are kinematic on the clip.
    CHECK(near(rig.world.position(rig.body_of_joint(B::pelvis)), Vec3f{0.0f, 1.0f, 0.0f}, 0.05f));
    CHECK(rig.world.motion(rig.body_of_joint(B::pelvis)) == Body3DMotion::Kinematic);
}

void test_get_up_turned_round() {
    // Started again in the middle of a get-up: the bodies are put where the pose shows them (between the lying pose and the
    // clip's), not where the clip is, so nothing pops; and it is a ragdoll again.
    Rig rig({0.0f, 1.0f, 0.0f});
    rig.set_targets({0.0f, 1.0f, 0.0f});
    rig.world.start_ragdoll(rig.ragdoll, {});
    rig.step(240);
    const Vec3f lying = rig.world.position(rig.body_of_joint(B::head));
    rig.set_targets({0.0f, 0.0f, 0.0f});
    rig.world.ragdoll_get_up(rig.ragdoll, 1.0f);
    rig.step(30);
    const f32 alpha = rig.world.ragdoll_state(rig.ragdoll).alpha;
    CHECK(alpha > 0.2f && alpha < 0.8f);
    const Mat4f head_clip = rig.model_world[static_cast<std::size_t>(B::head)];
    const Vec3f clip{head_clip.m[12], head_clip.m[13], head_clip.m[14]};
    const Vec3f shown = lerp(clip, lying, alpha);
    rig.world.start_ragdoll(rig.ragdoll, {});
    CHECK(near(rig.world.position(rig.body_of_joint(B::head)), shown, 0.03f));
    CHECK(rig.world.ragdoll_state(rig.ragdoll).mode == RagdollMode3D::Ragdoll);
    CHECK(rig.world.ragdoll_state(rig.ragdoll).dynamic_bodies == 17);
}

void test_partial_ragdoll() {
    Rig rig({0.0f, 1.0f, 0.0f}); // standing on nothing: the body is held by the clip, the arm falls
    rig.world.set_gravity({0.0f, -9.81f, 0.0f});
    rig.set_targets({0.0f, 1.0f, 0.0f});
    rig.world.set_ragdoll_subtree(rig.ragdoll, rig.bone_of(B::shoulder_l), 1.0f);
    rig.world.start_ragdoll(rig.ragdoll, {});
    const RagdollState3D st = rig.world.ragdoll_state(rig.ragdoll);
    CHECK(st.dynamic_bodies == 3); // shoulder, elbow, hand
    const std::vector<Vec3f> arm_before = {rig.world.position(rig.body_of_joint(B::hand_l))};
    const Vec3f chest_before = rig.world.position(rig.body_of_joint(B::chest));
    for (int i = 0; i < 90; ++i) {
        rig.set_targets({0.0f, 1.0f, 0.0f});
        rig.step();
    }
    // The hand fell (the arm hangs at the shoulder); the rest is on the clip, bit for bit where the clip puts it.
    CHECK(rig.world.position(rig.body_of_joint(B::hand_l)).y < arm_before[0].y - 0.5f);
    for (std::size_t i = 0; i < rig.bodies.size(); ++i) {
        const i32 j = rig.desc.bones[i].joint;
        const bool arm = j == B::shoulder_l || j == B::elbow_l || j == B::hand_l;
        if (!arm) {
            const Mat4f& m = rig.model_world[static_cast<std::size_t>(j)];
            CHECK(near(rig.world.position(rig.bodies[i]), Vec3f{m.m[12], m.m[13] + 1.0f, m.m[14]}, 0.02f));
            CHECK(rig.world.motion(rig.bodies[i]) == Body3DMotion::Kinematic);
        }
    }
    CHECK(near(rig.world.position(rig.body_of_joint(B::chest)), chest_before, 0.02f));
    std::vector<RagdollBonePose3D> pose;
    rig.world.ragdoll_pose(rig.ragdoll, pose);
    for (std::size_t i = 0; i < pose.size(); ++i) {
        const i32 j = rig.desc.bones[i].joint;
        const bool arm = j == B::shoulder_l || j == B::elbow_l || j == B::hand_l;
        CHECK((pose[i].weight > 0.0f) == arm);
    }
}

void test_projectile_starts_ragdoll() {
    // The game's rule: a body hit hard enough starts the ragdoll with the impulse at the bone. The ragdoll then moves.
    Rig rig({0.0f, 0.0f, 0.0f});
    rig.set_targets({0.0f, 0.0f, 0.0f});
    const Body3D ball = rig.world.create_body({.shape = Shape3D::sphere(0.15f), .motion = Body3DMotion::Dynamic,
                                               .position = {0.0f, 1.3f, 3.0f}, .linear_velocity = {0.0f, 0.0f, -15.0f},
                                               .mass = 5.0f, .gravity_factor = 0.0f, .continuous = true});
    bool started = false;
    const Vec3f chest_before = rig.world.position(rig.body_of_joint(B::chest));
    for (int i = 0; i < 120; ++i) {
        rig.set_targets({0.0f, 0.0f, 0.0f});
        rig.step();
        if (!started) {
            for (const ContactEvent3D& e : rig.world.drain_events()) {
                const bool hit = (e.a == ball || e.b == ball) &&
                                 (std::find(rig.bodies.begin(), rig.bodies.end(), e.a) != rig.bodies.end() ||
                                  std::find(rig.bodies.begin(), rig.bodies.end(), e.b) != rig.bodies.end());
                if (hit && e.kind == ContactKind3D::Begin) {
                    rig.world.start_ragdoll(rig.ragdoll, {.impulse_bone = rig.bone_of(B::chest), .impulse = {0.0f, 0.0f, -75.0f},
                                                          .impulse_point = e.point, .point_given = true});
                    started = true;
                }
            }
        }
    }
    CHECK(started);
    CHECK(length(rig.world.position(rig.body_of_joint(B::chest)) - chest_before) > 0.5f);
}

// ---- events ---------------------------------------------------------------------------------------------------------

void test_events_sorted() {
    // Two ragdolls started in the opposite order to their numbers: the batch is sorted by ragdoll.
    Rig rig({0.0f, 0.0f, 0.0f});
    const RagdollHandle3D second = rig.world.create_ragdoll(rig.desc, rig.bone_world({4.0f, 0.0f, 0.0f}));
    CHECK(second.valid() && second.index == 1);
    rig.world.start_ragdoll(second, {});
    rig.world.start_ragdoll(rig.ragdoll, {});
    rig.step(1);
    const auto events = rig.world.drain_ragdoll_events();
    CHECK(events.size() == 2 && events[0].ragdoll == rig.ragdoll && events[1].ragdoll == second);
    // Drained: nothing left.
    CHECK(rig.world.ragdoll_events().empty());
}

// ---- the place to get up at ----------------------------------------------------------------------------------------------

void test_get_up_place() {
    Rig rig({0.0f, 1.0f, 0.0f});
    rig.set_targets({0.0f, 1.0f, 0.0f});
    rig.world.start_ragdoll(rig.ragdoll, {.set_velocity = true, .velocity = {0.0f, 0.0f, 2.0f}});
    rig.step(300);
    const RagdollPlace3D place = rig.world.ragdoll_get_up_place(rig.ragdoll, {0.0f, 1.0f, 0.0f});
    CHECK(place.found);
    CHECK_NEAR(place.position.y, 0.0f, 0.01f);
    const Vec3f pelvis = rig.world.position(rig.body_of_joint(B::pelvis));
    CHECK_NEAR(place.position.x, pelvis.x, 1e-4f);
    CHECK_NEAR(place.position.z, pelvis.z, 1e-4f);
    CHECK_NEAR(place.height, pelvis.y, 1e-4f);
    // The rotation turns +Z to a horizontal direction.
    const Vec3f facing = place.rotation.rotate({0.0f, 0.0f, 1.0f});
    CHECK_NEAR(facing.y, 0.0f, 1e-3f);
    CHECK_NEAR(length(facing), 1.0f, 1e-3f);
    // Nothing under it: not found.
    Rig air({0.0f, 100.0f, 0.0f}, serial(), false);
    CHECK(!air.world.ragdoll_get_up_place(air.ragdoll, {0.0f, 1.0f, 0.0f}).found);
}

void test_lying_poses_face() {
    // A pelvis turned to lie on its back (forward up) and on its belly (forward down) reports it and faces by the head.
    for (int back = 0; back < 2; ++back) {
        Rig rig({0.0f, 0.2f, 0.0f});
        // The whole body turned about X by 90 degrees (belly down: +Z goes to -Y... forward points down) or -90 (up).
        const Quatf turn = Quatf::axis_angle({1.0f, 0.0f, 0.0f}, back == 1 ? -90.0f : 90.0f);
        std::vector<Mat4f> bw = rig.bone_world({0.0f, 0.0f, 0.0f});
        // Turn about the pelvis (0, 1, 0) and lower it to the floor.
        const Mat4f about = Mat4f::translation({0.0f, 0.2f, 0.0f}) * Mat4f::rotation(turn) * Mat4f::translation({0.0f, -1.0f, 0.0f});
        for (Mat4f& m : bw) {
            m = about * m;
        }
        rig.world.destroy_ragdoll(rig.ragdoll);
        rig.ragdoll = rig.world.create_ragdoll(rig.desc, bw);
        rig.bodies = rig.world.ragdoll_bodies(rig.ragdoll);
        rig.world.start_ragdoll(rig.ragdoll, {});
        const RagdollPlace3D place = rig.world.ragdoll_get_up_place(rig.ragdoll, {0.0f, 1.0f, 0.0f}, all_layers3d, 3.0f);
        CHECK(place.found && place.lying);
        CHECK(place.face_down == (back == 0));
        const Vec3f facing = place.rotation.rotate({0.0f, 0.0f, 1.0f});
        CHECK_NEAR(facing.y, 0.0f, 1e-3f);
    }
}

// ---- determinism ---------------------------------------------------------------------------------------------------------

// A scene: three ragdolls dropped at different places and speeds, boxes to land on, hit by a ball; the per-step hashes.
std::vector<u64> run_scene(World3DConfig cfg, int steps) {
    Rig rig({0.0f, 2.0f, 0.0f}, cfg);
    rig.world.create_body({.shape = Shape3D::box({0.6f, 0.2f, 0.6f}), .motion = Body3DMotion::Static,
                           .position = {0.7f, 0.2f, 0.0f}, .rotation = Quatf::axis_angle({0, 0, 1}, 25.0f)});
    const RagdollHandle3D b = rig.world.create_ragdoll(rig.desc, rig.bone_world({3.0f, 1.5f, 0.5f}));
    const RagdollHandle3D c = rig.world.create_ragdoll(rig.desc, rig.bone_world({-3.0f, 3.0f, -0.5f}));
    rig.world.create_body({.shape = Shape3D::sphere(0.2f), .motion = Body3DMotion::Dynamic, .position = {-6.0f, 3.0f, -0.5f},
                           .linear_velocity = {6.0f, 0.0f, 0.0f}, .mass = 6.0f, .gravity_factor = 0.5f});
    std::vector<u64> hashes;
    for (int i = 0; i < steps; ++i) {
        if (i == 0) {
            rig.world.set_ragdoll_targets(rig.ragdoll, rig.bone_world({0.0f, 2.0f, 0.0f}));
            rig.world.set_ragdoll_targets(b, rig.bone_world({3.0f, 1.5f, 0.5f}));
            rig.world.set_ragdoll_targets(c, rig.bone_world({-3.0f, 3.0f, -0.5f}));
            rig.world.start_ragdoll(rig.ragdoll, {.set_velocity = true, .velocity = {1.0f, 0.0f, 0.0f}});
            rig.world.start_ragdoll(b, {.blend_seconds = 0.2f, .impulse_bone = 4, .impulse = {40.0f, 0.0f, 0.0f}});
        }
        if (i == 40) {
            rig.world.start_ragdoll(c, {});
        }
        if (i == 150) {
            rig.world.ragdoll_get_up(rig.ragdoll, 0.5f);
        }
        rig.world.step(dt);
        hashes.push_back(rig.world.state_hash());
    }
    return hashes;
}

void test_determinism() {
    const std::vector<u64> a = run_scene(serial(), 240);
    const std::vector<u64> b = run_scene(serial(), 240);
    CHECK(a == b);
    CHECK(std::adjacent_find(a.begin(), a.end(), std::not_equal_to<>()) != a.end()); // it did move
    // Worker counts: none (the calling thread), 1 and 4.
    for (const int workers : {1, 4}) {
        JobSystem jobs({.workers = workers, .max_background = workers});
        World3DConfig cfg;
        cfg.jobs = &jobs;
        cfg.single_threaded = false;
        CHECK(run_scene(cfg, 240) == a);
    }
}

void test_save_restore_mid_fall() {
    // Save at step 60 (mid fall), run on, restore into the same world, run again: the same hashes.
    Rig rig({0.0f, 2.0f, 0.0f});
    rig.set_targets({0.0f, 2.0f, 0.0f});
    rig.world.start_ragdoll(rig.ragdoll, {.blend_seconds = 0.3f, .impulse_bone = 4, .impulse = {30.0f, 0.0f, 10.0f}});
    rig.step(30);
    const std::vector<u8> saved = rig.world.save_state();
    const u64 hash_at_save = rig.world.state_hash();
    std::vector<u64> first;
    for (int i = 0; i < 200; ++i) {
        if (i == 100) {
            rig.world.ragdoll_get_up(rig.ragdoll, 0.4f);
        }
        rig.set_targets({0.0f, 0.0f, 0.0f});
        rig.step();
        first.push_back(rig.world.state_hash());
    }
    CHECK(rig.world.restore_state(saved));
    CHECK(rig.world.state_hash() == hash_at_save);
    std::vector<u64> second;
    for (int i = 0; i < 200; ++i) {
        if (i == 100) {
            rig.world.ragdoll_get_up(rig.ragdoll, 0.4f);
        }
        rig.set_targets({0.0f, 0.0f, 0.0f});
        rig.step();
        second.push_back(rig.world.state_hash());
    }
    CHECK(first == second);
    // The blend and the mode are in the state: a restore that was taken mid get-up returns to it.
    const std::vector<u8> mid = rig.world.save_state();
    rig.world.start_ragdoll(rig.ragdoll, {});
    rig.step(20);
    CHECK(rig.world.restore_state(mid));
    CHECK(rig.world.ragdoll_state(rig.ragdoll).mode == RagdollMode3D::Animated);
    // The hash sees the mode and the blend.
    Rig a({0.0f, 2.0f, 0.0f}), b({0.0f, 2.0f, 0.0f});
    CHECK(a.world.state_hash() == b.world.state_hash());
    a.world.blend_ragdoll(a.ragdoll, 0.0f, 0.5f);
    b.world.blend_ragdoll(b.ragdoll, 0.3f, 0.5f);
    a.step();
    b.step();
    CHECK(a.world.state_hash() != b.world.state_hash());
    // A world with another set of ragdolls refuses the state.
    Rig other({0.0f, 2.0f, 0.0f});
    other.world.create_ragdoll(other.desc, other.bone_world({5.0f, 0.0f, 0.0f}));
    CHECK(!other.world.restore_state(a.world.save_state()));
}

void test_redirected_gravity() {
    // Gravity along +X: the ragdoll falls to a wall at x = 3 and lies against it.
    Rig rig({0.0f, 1.0f, 0.0f}, serial(), false);
    rig.world.set_gravity({9.81f, 0.0f, 0.0f});
    rig.world.create_body({.shape = Shape3D::box({0.5f, 10.0f, 10.0f}), .motion = Body3DMotion::Static, .position = {3.5f, 0.0f, 0.0f}});
    rig.set_targets({0.0f, 1.0f, 0.0f});
    rig.world.start_ragdoll(rig.ragdoll, {});
    rig.step(240);
    f32 furthest = -1e9f;
    for (const Body3D b : rig.bodies) {
        furthest = std::max(furthest, rig.world.position(b).x);
    }
    CHECK(furthest < 3.05f);
    CHECK(rig.world.position(rig.body_of_joint(B::pelvis)).x > 2.5f);
    CHECK(rig.world.ragdoll_state(rig.ragdoll).settled);
}

// ---- the ECS system: pose written into the animator, hand-over, recovery -----------------------------------------------------

struct EcsRig {
    kin::EcsWorld ecs;
    World3D physics;
    WorldPhysicsState3D bodies;
    WorldRenderState3D render;
    std::shared_ptr<Model3D> model;
    flecs::entity hero;
    flecs::entity floor;

    explicit EcsRig(Vec3f at = {0.0f, 0.0f, 0.0f}, World3DConfig cfg = serial(), bool with_character = false, bool ragdoll = true)
        : physics(cfg)
        , bodies(ecs, physics)
        , render(ecs) {
        model = std::make_shared<Model3D>();
        model->skeletons.push_back(skin_fixtures::make_biped());
        AnimClip3D idle;
        idle.name = "idle";
        idle.duration = 1.0f;
        idle.channels.resize(model->skeletons[0].joint_count());
        model->clips.push_back(idle);
        floor = ecs.raw().entity().set(Transform3D{.position = {0.0f, -0.5f, 0.0f}}).set(Collider3D{.kind = Collider3D::Kind::Box,
                                      .half_extents = {20.0f, 0.5f, 20.0f}});
        Animator3D animator;
        animator.clip = "idle";
        hero = ecs.raw().entity().set(Transform3D{.position = at}).set(SkinnedMeshRenderer3D{.model = model}).set(animator);
        if (with_character) {
            CharacterController3D controller;
            hero.set(controller);
        }
        if (ragdoll) {
            hero.set(Ragdoll3D{});
        }
    }
    void frame() {
        bodies.step(dt);
        render.animate(dt);
        render.propagate_transforms();
        render.apply_ik();
    }
    void run(int n) {
        for (int i = 0; i < n; ++i) {
            frame();
        }
    }
    const Animator3D& animator() const { return *hero.get<Animator3D>(); }
    // A joint's place in the world (the animator's matrices are in the model's space).
    Vec3f joint(int j) const {
        const Mat4f m = hero.get<Transform3D>()->matrix() * animator().ik_world[static_cast<std::size_t>(j)];
        return {m.m[12], m.m[13], m.m[14]};
    }
    Body3D body_of_joint(int j) const {
        const RagdollHandle3D r = bodies.ragdoll_of(hero);
        const std::vector<Body3D> bs = physics.ragdoll_bodies(r);
        const RagdollDesc* d = physics.ragdoll_desc(r);
        for (std::size_t i = 0; i < d->bones.size(); ++i) {
            if (d->bones[i].joint == j) {
                return bs[i];
            }
        }
        return {};
    }
};

void test_ecs_ragdoll_made_in_order() {
    EcsRig rig;
    // A second skinned entity with a ragdoll and a character on the first: made after the bodies and characters, in id order.
    const flecs::entity other = rig.ecs.raw().entity().set(Transform3D{.position = {3.0f, 0.0f, 0.0f}}).set(SkinnedMeshRenderer3D{.model = rig.model}).set(Ragdoll3D{});
    rig.bodies.sync_bodies();
    CHECK(rig.bodies.ragdoll_of(rig.hero).valid() && rig.bodies.ragdoll_of(other).valid());
    CHECK(rig.bodies.ragdoll_of(rig.hero).index == 0 && rig.bodies.ragdoll_of(other).index == 1);
    CHECK(rig.hero.get<PhysicsRagdoll3D>() != nullptr && rig.hero.get<PhysicsRagdoll3D>()->ragdoll == rig.bodies.ragdoll_of(rig.hero));
    // The floor's body is number 0, the ragdolls' bodies after it.
    CHECK(rig.physics.body_count() == 1 + 17 + 17);
    // A ragdoll's bodies carry the entity's id as their user id (events name it).
    CHECK(rig.physics.user_id(rig.physics.ragdoll_bodies(rig.bodies.ragdoll_of(rig.hero))[0]) == rig.hero.id());
    // Destroyed with the component.
    rig.hero.remove<Ragdoll3D>();
    rig.bodies.sync_bodies();
    CHECK(!rig.bodies.ragdoll_of(rig.hero).valid() && rig.physics.ragdoll_count() == 1);
    CHECK(rig.physics.body_count() == 1 + 17);
}

void test_pose_reaches_the_animator() {
    EcsRig rig({0.0f, 2.0f, 0.0f});
    rig.run(2);
    // Animated: the override is empty and the clip's pose is what is drawn.
    CHECK(rig.animator().pose_override.empty());
    CHECK(rig.bodies.start_ragdoll(rig.hero, {.set_velocity = true, .velocity = {1.0f, 0.0f, 0.0f}}));
    rig.run(50);
    CHECK(!rig.animator().pose_override.empty());
    const Animator3D& a = rig.animator();
    CHECK(a.ik_world.size() == 18 && a.ik_pose.joints.size() == 18);
    const Skeleton3D& sk = rig.model->skeletons[0];
    std::vector<Mat4f> rest;
    compute_world_matrices(sk, sk.rest_pose(), rest);
    // The joint places are the bodies' (within the joints' stretch) and the bones keep their lengths exactly.
    f32 worst_place = 0.0f, worst_length = 0.0f, worst_turn = 0.0f;
    for (int j = 0; j < static_cast<int>(sk.joint_count()); ++j) {
        const Body3D b = rig.body_of_joint(j);
        if (!b.valid()) {
            continue;
        }
        worst_place = std::max(worst_place, length(rig.joint(j) - rig.physics.position(b)));
        const Quatf q = rig.physics.rotation(b);
        const Mat4f world = rig.hero.get<Transform3D>()->matrix() * a.ik_world[static_cast<std::size_t>(j)];
        // The rotation of the matrix against the body's: the image of +X and +Y.
        const Vec3f x = q.rotate({1, 0, 0}), y = q.rotate({0, 1, 0});
        worst_turn = std::max({worst_turn, length(Vec3f{world.m[0], world.m[1], world.m[2]} - x), length(Vec3f{world.m[4], world.m[5], world.m[6]} - y)});
        const i32 parent = sk.parents[static_cast<std::size_t>(j)];
        if (parent >= 0 && rig.body_of_joint(parent).valid()) {
            const f32 bind = length(Vec3f{rest[static_cast<std::size_t>(j)].m[12], rest[static_cast<std::size_t>(j)].m[13], rest[static_cast<std::size_t>(j)].m[14]} -
                                    Vec3f{rest[static_cast<std::size_t>(parent)].m[12], rest[static_cast<std::size_t>(parent)].m[13], rest[static_cast<std::size_t>(parent)].m[14]});
            worst_length = std::max(worst_length, std::fabs(length(rig.joint(j) - rig.joint(parent)) - bind));
        }
    }
    if (verbose) std::printf("  pose: place %.4f, bone length %.6f, turn %.5f\n", worst_place, worst_length, worst_turn);
    CHECK(worst_place < 0.12f);   // the joints' own opening, summed down a chain (the pose is a rigid skeleton)
    CHECK(worst_length < 2e-5f);  // exact: the pose keeps the bind pose's lengths
    CHECK(worst_turn < 1e-3f);
    // The pelvis (the top of the ragdolled part) is exactly where its body is.
    CHECK(near(rig.joint(B::pelvis), rig.physics.position(rig.body_of_joint(B::pelvis)), 1e-4f));
    // The weights say all of it.
    for (const f32 w : a.pose_override.weights) {
        CHECK(w == 0.0f || w == 1.0f);
    }
}

void test_alpha_zero_is_the_clip_bit_for_bit_and_one_is_the_body() {
    EcsRig rig({0.0f, 1.0f, 0.0f});
    rig.run(3);
    const Skeleton3D& sk = rig.model->skeletons[0];
    Animator3D* a = rig.hero.get_mut<Animator3D>();
    // An override with every weight 0 gives the clips' pose to the bit: pose and world matrices.
    a->pose_override.weights.assign(sk.joint_count(), 0.0f);
    a->pose_override.position.assign(sk.joint_count(), Vec3f{5.0f, 5.0f, 5.0f});
    a->pose_override.rotation.assign(sk.joint_count(), Quatf::axis_angle({0, 1, 0}, 77.0f));
    rig.render.apply_ik();
    const Pose3D clip = a->player.pose();
    std::vector<Mat4f> clip_world;
    compute_world_matrices(sk, clip, clip_world);
    bool same = a->ik_pose.joints.size() == clip.joints.size() && a->ik_world.size() == clip_world.size();
    for (std::size_t j = 0; same && j < clip.joints.size(); ++j) {
        same = same && a->ik_pose.joints[j].translation == clip.joints[j].translation &&
               a->ik_pose.joints[j].rotation == clip.joints[j].rotation && a->ik_pose.joints[j].scale == clip.joints[j].scale &&
               a->ik_world[j] == clip_world[j];
    }
    CHECK(same);
    // All weights 1 and bodies given: the joints' world places are those.
    for (std::size_t j = 0; j < sk.joint_count(); ++j) {
        a->pose_override.weights[j] = 1.0f;
        a->pose_override.position[j] = Vec3f{0.1f * static_cast<f32>(j), 2.0f, 0.5f};
        a->pose_override.rotation[j] = Quatf::axis_angle({0, 1, 0}, 10.0f * static_cast<f32>(j));
    }
    rig.render.apply_ik();
    // The root joint has no body here in particular, but every joint with weight 1 and no weight above it takes the
    // translation: joint 0's (the root) is the top.
    CHECK(near(rig.joint(0), a->pose_override.position[0], 1e-4f));
    // The rotation of every joint is its body's: the matrices' x axis (their rotation part, no scale in the bind pose).
    for (std::size_t j = 0; j < sk.joint_count(); ++j) {
        const Mat4f& m = a->ik_world[j];
        const Vec3f x = a->pose_override.rotation[j].rotate({1, 0, 0});
        CHECK(near(Vec3f{m.m[0], m.m[1], m.m[2]}, x, 1e-4f));
    }
}

void test_partial_ragdoll_leaves_the_rest_exact() {
    EcsRig rig({0.0f, 1.0f, 0.0f});
    rig.run(3);
    CHECK(rig.bodies.set_ragdoll_subtree(rig.hero, "shoulder_L", 1.0f));
    rig.bodies.start_ragdoll(rig.hero, {});
    rig.run(40);
    const Animator3D& a = rig.animator();
    const Skeleton3D& sk = rig.model->skeletons[0];
    const Pose3D& clip = a.player.pose();
    const std::vector<u8> arm = sk.subtree(B::shoulder_l);
    int arm_moved = 0;
    for (std::size_t j = 0; j < sk.joint_count(); ++j) {
        const bool same = a.ik_pose.joints[j].translation == clip.joints[j].translation &&
                          a.ik_pose.joints[j].rotation == clip.joints[j].rotation && a.ik_pose.joints[j].scale == clip.joints[j].scale;
        if (arm[j] == 0) {
            CHECK(same); // exactly the clip's
        } else if (!same) {
            ++arm_moved;
        }
    }
    CHECK(arm_moved >= 2);
    // The arm hangs: the hand is well below the shoulder's height.
    CHECK(rig.joint(B::hand_l).y < rig.joint(B::shoulder_l).y - 0.3f);
    // The rest stands: pelvis and head where the clip puts them.
    CHECK(near(rig.joint(B::head), Vec3f{0.0f, 1.0f + 1.8f, 0.0f}, 0.4f)); // above the pelvis
    CHECK(near(rig.joint(B::pelvis), Vec3f{0.0f, 2.0f, 0.0f}, 1e-4f));
}

void test_ik_after_the_pose_is_written() {
    // The left arm is a ragdoll, the right hand reaches a target by IK: the IK is solved first and the pose written over
    // the ragdolled joints only, so that the IK arm is where the IK put it.
    EcsRig rig({0.0f, 1.0f, 0.0f});
    IKConstraint3D hand;
    hand.name = "hand";
    hand.kind = IKKind3D::TwoBone;
    hand.joints = {"shoulder_R", "elbow_R", "hand_R"};
    hand.pole = IKConstraint3D::Pole::ModelDirection;
    hand.pole_vector = {0.0f, 0.0f, -1.0f};
    rig.hero.get_mut<Animator3D>()->add_ik(hand);
    rig.run(3);
    const Vec3f target{-0.5f, 2.4f, 0.4f};
    rig.bodies.set_ragdoll_subtree(rig.hero, "shoulder_L", 1.0f);
    rig.bodies.start_ragdoll(rig.hero, {});
    for (int i = 0; i < 40; ++i) {
        rig.hero.get_mut<Animator3D>()->ik("hand").set_target(target).set_weight(1.0f);
        rig.frame();
    }
    CHECK(near(rig.joint(B::hand_r), target, 0.02f));
    CHECK(rig.joint(B::hand_l).y < rig.joint(B::shoulder_l).y - 0.3f);
}

void test_hand_over_and_recovery() {
    EcsRig rig({0.0f, 0.0f, 0.0f}, serial(), true);
    CharacterController3D* controller = rig.hero.get_mut<CharacterController3D>();
    controller->input.move = {0.0f, 1.0f};
    controller->input.forward = {0.0f, 0.0f, -1.0f};
    controller->facing = Quatf{};
    rig.run(90);
    const Character3D c = rig.bodies.character_of(rig.hero);
    CHECK(c.valid() && rig.physics.character_enabled(c));
    const Vec3f at_hand_over = rig.hero.get<Transform3D>()->position;
    const f32 speed = rig.physics.character(c).ground_speed;
    CHECK(speed > 3.0f);
    // To a ragdoll: the capsule gives way, the bodies carry the speed, a push at the chest.
    CHECK(rig.bodies.hand_over_to_ragdoll(rig.hero, {.impulse_bone = 2, .impulse = {0.0f, 0.0f, 0.0f}}));
    CHECK(!rig.physics.character_enabled(c));
    controller->input = {};
    rig.run(1);
    const std::vector<Body3D> bs = rig.physics.ragdoll_bodies(rig.bodies.ragdoll_of(rig.hero));
    f32 mean_speed = 0.0f;
    for (const Body3D b : bs) {
        mean_speed += length(rig.physics.linear_velocity(b)) / static_cast<f32>(bs.size());
    }
    if (verbose) std::printf("  hand over: character %.2f m/s, bodies %.2f\n", speed, mean_speed);
    CHECK_NEAR(mean_speed, speed, 1.0f);
    // It slides and falls and lies; the entity follows the pelvis's ground point.
    rig.run(240);
    const RagdollState3D st = rig.physics.ragdoll_state(rig.bodies.ragdoll_of(rig.hero));
    CHECK(st.mode == RagdollMode3D::Ragdoll);
    const Vec3f pelvis = rig.physics.position(rig.body_of_joint(B::pelvis));
    const Vec3f entity = rig.hero.get<Transform3D>()->position;
    CHECK_NEAR(entity.x, pelvis.x, 0.05f);
    CHECK_NEAR(entity.z, pelvis.z, 0.05f);
    CHECK(entity.z < at_hand_over.z - 1.0f); // it travelled
    // The events: started, then settled (the entity's id is the user id).
    int started = 0, settled = 0;
    for (const RagdollEvent3D& e : rig.bodies.ragdoll_events()) {
        CHECK(e.user_id == rig.hero.id());
        started += e.kind == RagdollEventKind3D::Started ? 1 : 0;
        settled += e.kind == RagdollEventKind3D::Settled ? 1 : 0;
    }
    (void)started;
    (void)settled;
    // The character's capsule does not push (it collides with nothing): a ball rolled at where it was passes.
    // Recovery with room: it is enabled at the pelvis's ground point and the ragdoll blends back.
    const RagdollRecoverResult3D r = rig.bodies.recover_from_ragdoll(rig.hero, {.get_up_seconds = 0.5f});
    CHECK(r.recovered && !r.blocked && r.place.found);
    CHECK(rig.physics.character_enabled(c));
    CHECK_NEAR(rig.physics.character(c).position.x, pelvis.x, 0.05f);
    CHECK_NEAR(rig.physics.character(c).position.z, pelvis.z, 0.05f);
    CHECK(rig.physics.ragdoll_state(rig.bodies.ragdoll_of(rig.hero)).mode == RagdollMode3D::GetUp);
    int recovered = 0;
    for (int i = 0; i < 60; ++i) {
        rig.frame();
        for (const RagdollEvent3D& e : rig.bodies.ragdoll_events()) {
            recovered += e.kind == RagdollEventKind3D::Recovered ? 1 : 0;
        }
    }
    CHECK(recovered == 1);
    CHECK(rig.physics.ragdoll_state(rig.bodies.ragdoll_of(rig.hero)).mode == RagdollMode3D::Animated);
    // And the character walks again from there.
    controller->input.move = {0.0f, 1.0f};
    const f32 z0 = rig.hero.get<Transform3D>()->position.z;
    rig.run(60);
    CHECK(rig.hero.get<Transform3D>()->position.z < z0 - 2.0f);
}

void test_recovery_blocked_without_room() {
    EcsRig rig({0.0f, 0.0f, 0.0f}, serial(), true);
    rig.run(10);
    rig.bodies.hand_over_to_ragdoll(rig.hero, {});
    rig.run(240);
    const Character3D c = rig.bodies.character_of(rig.hero);
    const Vec3f pelvis = rig.physics.position(rig.body_of_joint(B::pelvis));
    // A low ceiling over where it lies (after it has settled): standing would not fit.
    rig.ecs.raw().entity().set(Transform3D{.position = {pelvis.x, 1.1f, pelvis.z}}).set(Collider3D{.kind = Collider3D::Kind::Box,
                               .half_extents = {2.0f, 0.1f, 2.0f}});
    rig.bodies.sync_bodies();
    const RagdollRecoverResult3D r = rig.bodies.recover_from_ragdoll(rig.hero, {});
    CHECK(!r.recovered && r.blocked && r.place.found);
    CHECK(!rig.physics.character_enabled(c));
    CHECK(rig.physics.ragdoll_state(rig.bodies.ragdoll_of(rig.hero)).mode == RagdollMode3D::Ragdoll);
    const auto events = rig.bodies.ragdoll_events();
    CHECK(std::any_of(events.begin(), events.end(), [](const RagdollEvent3D& e) { return e.kind == RagdollEventKind3D::Blocked; }));
    // No ground at all: refused too (a ragdoll far above the world).
    EcsRig air({0.0f, 100.0f, 0.0f}, serial(), true);
    air.bodies.sync_bodies();
    air.physics.set_gravity({0.0f, 0.0f, 0.0f});
    air.bodies.hand_over_to_ragdoll(air.hero, {});
    air.run(10);
    const RagdollRecoverResult3D none = air.bodies.recover_from_ragdoll(air.hero, {.max_ground_distance = 2.0f});
    CHECK(none.blocked && !none.recovered);
    // The ceiling goes: now it fits.
    for (const flecs::entity e : {rig.hero}) {
        (void)e;
    }
}

std::vector<u64> ecs_scene_hashes(World3DConfig cfg, int steps) {
    EcsRig rig({0.0f, 1.5f, 0.0f}, cfg, true);
    // A second character and a rope in the scene as well.
    rig.ecs.raw().entity().set(Transform3D{.position = {4.0f, 1.0f, 0.0f}}).set(SkinnedMeshRenderer3D{.model = rig.model}).set(Ragdoll3D{});
    std::vector<u64> hashes;
    for (int i = 0; i < steps; ++i) {
        if (i == 5) {
            rig.hero.get_mut<CharacterController3D>()->input.move = {0.0f, 1.0f};
        }
        if (i == 60) {
            rig.bodies.hand_over_to_ragdoll(rig.hero, {.impulse_bone = 3, .impulse = {40.0f, 0.0f, -20.0f}});
        }
        if (i == 120) {
            rig.bodies.start_ragdoll(rig.ecs.raw().lookup("none"), {}); // an invalid entity: ignored
        }
        if (i == 200) {
            rig.bodies.recover_from_ragdoll(rig.hero, {});
        }
        rig.frame();
        hashes.push_back(rig.physics.state_hash());
    }
    return hashes;
}

void test_ecs_determinism() {
    const std::vector<u64> a = ecs_scene_hashes(serial(), 300);
    CHECK(a == ecs_scene_hashes(serial(), 300));
    CHECK(std::adjacent_find(a.begin(), a.end(), std::not_equal_to<>()) != a.end());
    JobSystem jobs({.workers = 4, .max_background = 4});
    World3DConfig cfg;
    cfg.jobs = &jobs;
    cfg.single_threaded = false;
    CHECK(ecs_scene_hashes(cfg, 300) == a);
}

void test_interpolation_of_the_pose() {
    EcsRig rig({0.0f, 2.0f, 0.0f});
    rig.run(2);
    rig.bodies.start_ragdoll(rig.hero, {});
    rig.run(10);
    // The pose of the step before the last one, as it was written then.
    const std::vector<Vec3f> before_last = rig.animator().pose_override.position;
    rig.run(1);
    const std::vector<Vec3f> latest = rig.animator().pose_override.position;
    rig.bodies.interpolate(0.0f);
    const std::vector<Vec3f> at_zero = rig.animator().pose_override.position;
    rig.bodies.interpolate(0.5f);
    const std::vector<Vec3f> mid = rig.animator().pose_override.position;
    int moved = 0;
    for (std::size_t j = 0; j < latest.size(); ++j) {
        // 0 is the pose of the step before, 1 the last one's, 0.5 between.
        CHECK(near(at_zero[j], before_last[j], 1e-5f));
        if (length(latest[j] - at_zero[j]) > 1e-4f) {
            ++moved;
            CHECK(near(mid[j], lerp(at_zero[j], latest[j], 0.5f), 1e-5f));
        }
    }
    CHECK(moved > 5);
    rig.bodies.interpolate(1.0f);
    CHECK(rig.animator().pose_override.position == latest);
}

// A person on a rope through the ECS: the hands' IK constraints are on the two anchors on the rope (the grips are at different
// places of it), and the character hangs from them.
void test_hands_on_a_rope_by_ik() {
    EcsRig rig({0.0f, 0.0f, 0.0f}, serial(), false, false);
    kin::Rope3D rope;
    rope.start = kin::RopeAttach3D::at({0.0f, 7.0f, 0.0f});
    rope.end = kin::RopeAttach3D::free();
    rope.length = 3.6f;
    rig.ecs.raw().entity().set(Transform3D{}).set(rope);
    kin::CharacterController3D controller;
    controller.desc.traversal.enabled = true;
    controller.desc.traversal.resource.max = 1000.0f;
    controller.input.forward = {0.0f, 0.0f, -1.0f};
    Animator3D animator;
    animator.clip = "idle";
    for (const char* side : {"L", "R"}) {
        IKConstraint3D hand;
        hand.name = std::string("hand_") + side;
        hand.kind = IKKind3D::TwoBone;
        hand.joints = {std::string("shoulder_") + side, std::string("elbow_") + side, std::string("hand_") + side};
        hand.weight = 0.0f;
        hand.pole = IKConstraint3D::Pole::ModelDirection;
        hand.pole_vector = {side[0] == 'L' ? 0.5f : -0.5f, -0.7f, -0.5f};
        hand.two_bone.max_stretch = 1.1f;
        animator.add_ik(hand);
    }
    CharacterAnimationLink3D link;
    link.hands = {"hand_L", "hand_R"};
    link.hand_clearance = 0.0f;
    rig.hero.set(controller).set(link);
    rig.hero.set(static_cast<const Animator3D&>(animator));
    rig.hero.get_mut<Transform3D>()->position = {0.0f, 1.85f, 0.7f};
    rig.bodies.sync_bodies();
    CharacterController3D* c = rig.hero.get_mut<CharacterController3D>();
    c->traversal.grab = {true, true};
    c->traversal.reach = {0.0f, 0.5f, -1.0f};
    rig.run(150);
    const TraversalState3D t = rig.bodies.traversal_of(rig.hero);
    CHECK(t.mode == TraversalMode3D::Hanging && t.held == 2);
    CHECK(t.grips[0].kind == GripKind3D::Rope && t.grips[1].kind == GripKind3D::Rope);
    // The hand joints (in the world) are on the anchors.
    const f32 left = length(rig.joint(B::hand_l) - t.grips[0].point);
    const f32 right = length(rig.joint(B::hand_r) - t.grips[1].point);
    if (verbose) std::printf("  hands on the rope: left %.3f m, right %.3f m from their anchors\n", left, right);
    CHECK(left < 0.05f && right < 0.05f);
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-v") verbose = true;
    }
    test_auto_desc();
    test_create_and_refusals();
    test_animated_follows_targets();
    test_animated_bodies_are_hit();
    test_parked_bodies_are_not_hit();
    test_fall_settles_and_holds_together();
    test_freeze_on_settle();
    test_limits_respected();
    test_no_tunnelling_at_speed();
    test_connected_bones_do_not_collide();
    test_disabled_character_is_left_alone();
    test_self_collision_flag();
    test_blend_and_get_up();
    test_get_up_blends_from_the_lying_pose();
    test_get_up_turned_round();
    test_partial_ragdoll();
    test_projectile_starts_ragdoll();
    test_events_sorted();
    test_get_up_place();
    test_lying_poses_face();
    test_determinism();
    test_save_restore_mid_fall();
    test_redirected_gravity();
    test_ecs_ragdoll_made_in_order();
    test_pose_reaches_the_animator();
    test_alpha_zero_is_the_clip_bit_for_bit_and_one_is_the_body();
    test_partial_ragdoll_leaves_the_rest_exact();
    test_ik_after_the_pose_is_written();
    test_hand_over_and_recovery();
    test_recovery_blocked_without_room();
    test_ecs_determinism();
    test_interpolation_of_the_pose();
    test_hands_on_a_rope_by_ik();
    if (failures != 0) {
        std::fprintf(stderr, "physics3d_ragdoll_tests: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("physics3d_ragdoll_tests: all passed\n");
    return 0;
}
