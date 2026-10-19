#include "skin_fixtures.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <kin/core/instrumentation.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/scene/scene_session3d.hpp>

namespace {
using namespace kin;
void check(bool value, const char* why) {
    if (!value)
        throw std::runtime_error(why);
}
bool close(const Mat4f& a, const Mat4f& b) {
    for (int i = 0; i < 16; ++i)
        if (std::abs(a.m[i] - b.m[i]) > .001f)
            return false;
    return true;
}
struct Scratch {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("kin-attachment-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
Mat4f drawn(const Scene3D& scene, kin::EcsId id) {
    for (const auto& instance : scene.instances())
        if (instance.id == u32(id))
            return instance.transform;
    throw std::runtime_error("expected instance missing");
}
bool has_draw(const Scene3D& scene, kin::EcsId id) {
    for (const auto& instance : scene.instances())
        if (instance.id == u32(id))
            return true;
    return false;
}
Mat4f rendered_joint(const Scene3D& scene, const Skeleton3D& skeleton, kin::EcsId actor, i32 joint) {
    for (const auto& draw : scene.draws())
        if (draw.skinned)
            for (u32 i = 0; i < draw.count; ++i) {
                const auto& instance = scene.instances()[draw.first + i];
                if (instance.id != u32(actor))
                    continue;
                const auto first = scene.skin_offsets()[draw.skin_first + i];
                return instance.transform * scene.joint_matrices()[first + joint] *
                       skeleton.inverse_bind[std::size_t(joint)].inverse();
            }
    throw std::runtime_error("skinned joint missing");
}
void attachments(Renderer3D& renderer) {
    Scratch scratch;
    std::ofstream(scratch.path / "level.kinscene") << R"({"entities":[]})";
    std::ofstream(scratch.path / "actor.kinprefab")
        << R"({"id":"actor","root":"actor","entities":[{"id":"actor","components":{"Transform3D":{},"Collider3D":{"half_extents":[0.1,0.1,0.1]}}}]})";
    std::ofstream(scratch.path / "item.kinprefab") << R"({"id":"item","root":"item","entities":[
      {"id":"item","components":{"Transform3D":{"position":[0.2,0.1,0],"scale":[0.5,0.5,0.5]},"Collider3D":{"half_extents":[0.1,0.1,0.1]}}},
      {"id":"light","parent":"item","components":{"Transform3D":{"position":[0,0.3,0]},"PointLight3D":{},"SpotLight3D":{}}}]})";
    auto tube = skin_fixtures::make_tube(4, 3, 2, .1f);
    auto model = std::make_shared<Model3D>();
    model->skeletons.push_back(tube.skeleton);
    model->parts.push_back({.mesh = renderer.create_mesh(tube.mesh), .skin = 0});
    AnimClip3D clip;
    clip.name = "bend";
    clip.duration = 1;
    clip.channels.resize(3);
    clip.channels[1].rotation.times = {0, 1};
    clip.channels[1].rotation.values = {{}, Quatf::axis_angle({0, 0, 1}, 60)};
    model->clips.push_back(std::move(clip));
    auto box = std::make_shared<Model3D>();
    box->parts.push_back({.mesh = renderer.create_mesh(meshes::box({.1f, .1f, .1f}))});
    Assets3D assets{renderer, scratch.path};
    SceneSession3DOptions options;
    options.viewport = {160, 120};
    options.warm_up = false;
    options.physics = World3DConfig{.max_bodies = 128,
                                    .max_body_pairs = 256,
                                    .max_contact_constraints = 256,
                                    .temp_memory_bytes = 1024 * 1024,
                                    .single_threaded = true};
    SceneSession3D session{renderer, assets, options};
    check(session.load("level.kinscene"), "attachment level failed");
    auto character =
        session.spawn("actor.kinprefab", {.position = Vec3f{2, 1, 0}, .configure = [&](SceneSpawnContext3D& created) {
                                              created.root.set(SkinnedMeshRenderer3D{.model = model});
                                              Animator3D animator;
                                              animator.clip = "bend";
                                              created.root.set(std::move(animator));
                                          }});
    check(bool(character), "actor spawn failed");
    const auto offset = Mat4f::translation({.1f, 0, 0}) * Mat4f::rotation(Quatf::axis_angle({0, 1, 0}, 25));
    auto item = session.spawn(
        "item.kinprefab", {.configure = [&](SceneSpawnContext3D& created) {
            created.root.set(ModelRenderer3D{.model = box}).set(JointAttachment3D{character.root, "top", offset});
            created.entity("light").set(ModelRenderer3D{.model = box});
        }});
    check(bool(item), "item spawn failed");
    auto actor = session.resolve(character.root), root = session.resolve(item.root);
    flecs::entity child;
    root.raw().children([&](flecs::entity value) { child = value; });
    const auto child_id = child.id();
    check(session.step(.25f) && session.collect(), "animated attachment collect failed");
    auto binding = root.get<JointAttachment3D>();
    check(binding->status() == JointAttachmentStatus3D::Ready, "attachment not ready");
    auto expected = rendered_joint(*session.scene(), model->skeletons[0], actor.id(), 2) * offset *
                    root.get<Transform3D>()->matrix();
    check(close(drawn(*session.scene(), root.id()), expected) && close(*binding->placement(), expected),
          "attachment missed animated joint or offset/root transform");
    const auto expected_child = expected * child.get<Transform3D>()->matrix();
    check(close(drawn(*session.scene(), child_id), expected_child), "prefab child did not follow root");
    check(session.scene()->lights.points.size() == 1 &&
              length(session.scene()->lights.points[0].position - expected_child.apply({})) < .001f,
          "attached point light misplaced");
    check(session.scene()->lights.spots.size() == 1 &&
              length(session.scene()->lights.spots[0].position - expected_child.apply({})) < .001f,
          "attached spot light misplaced");
    const auto hash = session.physics()->state_hash();
    const auto body = session.bodies()->body_of(root.raw());
    const auto native = session.physics()->position(body);
    const auto local = root.get<Transform3D>()->matrix(), world = root.get<WorldTransform3D>()->matrix;
    actor.set(RenderTransform3D{Mat4f::translation({10, 1, 0}), true});
    check(session.collect(), "interpolated-root collect failed");
    expected = rendered_joint(*session.scene(), model->skeletons[0], actor.id(), 2) * offset * local;
    check(close(drawn(*session.scene(), root.id()), expected), "attachment did not use character render placement");
    check(hash == session.physics()->state_hash(), "render placement changed physics hash");
    auto* animator = actor.get_mut<Animator3D>();
    animator->add_ik({.name = "reach", .kind = IKKind3D::TwoBone, .joints = {"base", "middle", "top"}, .weight = 1})
        .set_target({3.3f, 1.7f, 0});
    check(session.step(.1f) && session.collect(), "IK attachment collect failed");
    expected = rendered_joint(*session.scene(), model->skeletons[0], actor.id(), 2) * offset * local;
    check(close(drawn(*session.scene(), root.id()), expected), "attachment and final IK drawing disagree");
    const auto stable = session.physics()->state_hash();
    Scene3D repeated;
    session.rendering()->collect(repeated);
    repeated.clear();
    session.rendering()->collect(repeated);
    {
        AllocationScope guard{true};
        for (int i = 0; i < 1000; ++i) {
            repeated.clear();
            session.rendering()->collect(repeated);
        }
    }
    check(stable == session.physics()->state_hash() && root.get<Transform3D>()->matrix() == local &&
              root.get<WorldTransform3D>()->matrix == world && session.physics()->position(body) == native,
          "render attachments changed simulation or body");
    auto* attachment = root.get_mut<JointAttachment3D>();
    const auto old_target = attachment->target();
    check(!attachment->configure({}, "top") && !attachment->configure(character.root, std::string(96, 'x')) &&
              attachment->target() == old_target,
          "invalid configuration replaced binding");
    attachment->enabled = false;
    check(session.collect() && close(drawn(*session.scene(), root.id()), world),
          "disabled attachment did not use ordinary placement");
    attachment->enabled = true;
    auto invalid_root = session.world()
                            ->entity("no-transform")
                            .set(WorldTransform3D{})
                            .set(ModelRenderer3D{.model = box})
                            .set(JointAttachment3D{character.root, "top"});
    check(session.collect() &&
              invalid_root.get<JointAttachment3D>()->status() == JointAttachmentStatus3D::InvalidTransform &&
              !has_draw(*session.scene(), invalid_root.id()),
          "missing root transform did not fail closed");
    invalid_root.destroy();
    auto foreign = character.root;
    ++foreign.session;
    check(attachment->configure(foreign, "top") && session.collect() &&
              attachment->status() == JointAttachmentStatus3D::MissingTarget,
          "foreign target was accepted");
    check(attachment->configure(character.root, "missing") && session.collect() &&
              !has_draw(*session.scene(), root.id()) && !has_draw(*session.scene(), child_id) &&
              session.scene()->lights.points.empty() && attachment->status() == JointAttachmentStatus3D::MissingJoint,
          "missing joint left stale geometry or light");
    check(attachment->configure(character.root, "top", offset), "joint rebind failed");
    // Replacement uses a different joint index and must not retain the old model.
    actor.get_mut<Animator3D>()->ik_constraints.clear();
    auto replaced = std::make_shared<Model3D>(*model);
    std::swap(replaced->skeletons[0].names[1], replaced->skeletons[0].names[2]);
    std::weak_ptr<const Model3D> old_model = model;
    actor.get_mut<SkinnedMeshRenderer3D>()->model = replaced;
    model.reset();
    check(session.step(.1f) && session.collect() && old_model.expired(),
          "model replacement retained socket model or failed");
    expected = rendered_joint(*session.scene(), replaced->skeletons[0], actor.id(), 1) * offset * local;
    check(close(drawn(*session.scene(), root.id()), expected), "replacement did not resolve joint again");
    actor.get_mut<SkinnedMeshRenderer3D>()->visible = false;
    check(session.collect() && !has_draw(*session.scene(), root.id()), "hidden actor left floating attachment");
    actor.get_mut<SkinnedMeshRenderer3D>()->visible = true;
    actor.disable();
    check(session.collect() && attachment->status() == JointAttachmentStatus3D::HiddenTarget &&
              !has_draw(*session.scene(), root.id()),
          "disabled target left an attachment");
    actor.enable();
    // A binding graph is deliberately single-level: no recursive socket cycles.
    actor.set(JointAttachment3D{item.root, "top"});
    check(session.collect() && attachment->status() == JointAttachmentStatus3D::UnsupportedTarget &&
              !has_draw(*session.scene(), root.id()),
          "attachment cycle was not rejected");
    actor.remove<JointAttachment3D>();
    check(session.despawn(character) && session.collect() &&
              attachment->status() == JointAttachmentStatus3D::MissingTarget && !has_draw(*session.scene(), root.id()),
          "despawned actor left an attachment draw");
    auto prepared = session.preload("actor.kinprefab");
    auto pool = session.create_pool(prepared, {.capacity = 1});
    auto lease = session.spawn(pool);
    check(bool(lease), "target pool failed");
    auto pooled = session.resolve(lease.root);
    pooled.set(SkinnedMeshRenderer3D{.model = replaced});
    check(attachment->configure(lease.root, "top") && session.collect() && attachment->placement(),
          "rest-pose target failed");
    check(session.despawn(lease), "target pool release failed");
    auto renewed = session.spawn(pool);
    session.resolve(renewed.root).set(SkinnedMeshRenderer3D{.model = replaced});
    check(renewed.root.entity == lease.root.entity && session.collect() &&
              attachment->status() == JointAttachmentStatus3D::MissingTarget,
          "old target followed a renewed pool lease");
    check(attachment->configure(renewed.root, "top") && session.collect() && attachment->placement(),
          "renewed target explicit binding failed");
    // Returned attachment slots must discard their runtime binding/cache.
    auto item_prepared = session.preload("item.kinprefab");
    auto item_pool = session.create_pool(item_prepared, {.capacity = 1});
    auto slot = session.spawn(item_pool);
    session.resolve(slot.root).set(JointAttachment3D{renewed.root, "top"});
    check(session.despawn(slot), "attachment pool release failed");
    auto next = session.spawn(item_pool);
    check(!session.resolve(next.root).has<JointAttachment3D>(), "pooled attachment inherited old binding");
    std::vector<ScenePrefab3D> extras;
    for (std::size_t i = 0; i < max_joint_attachments3d; ++i) {
        auto extra = session.spawn("item.kinprefab", {.configure = [&](SceneSpawnContext3D& created) {
                                       created.root.set(JointAttachment3D{renewed.root, "top"});
                                   }});
        check(bool(extra), "attachment budget fixture spawn failed");
        extras.push_back(extra);
    }
    check(session.collect(), "attachment budget collect failed");
    std::size_t ready = 0, limited = 0;
    session.world()->raw().each([&](const JointAttachment3D& value) {
        ready += value.status() == JointAttachmentStatus3D::Ready;
        limited += value.status() == JointAttachmentStatus3D::LimitExceeded;
    });
    check(ready == max_joint_attachments3d && limited == 1, "attachment budget did not fail closed at fixed capacity");
    const auto stale = *attachment;
    check(session.reload(), "attachment scene reload failed");
    auto fresh = session.spawn("item.kinprefab", {.configure = [&](SceneSpawnContext3D& created) {
                                   created.root.set(ModelRenderer3D{.model = box}).set(stale);
                               }});
    check(session.collect() && !has_draw(*session.scene(), session.resolve(fresh.root).id()) &&
              session.resolve(fresh.root).get<JointAttachment3D>()->status() == JointAttachmentStatus3D::MissingTarget,
          "reload copied attachment followed stale target");
    std::cout << "joint attachments: animation/IK/render pose, prefab children/lights, allocation guard, "
                 "model/lifecycle/pool/budget passed; state bytes="
              << sizeof(JointAttachment3D) << '\n';
}
} // namespace
int main() {
    bool ready = false;
    try {
        App app;
        auto& window =
            app.create_window({.title = "joint-attachment-tests", .width = 160, .height = 120, .hidden = true});
        auto backend = make_render_backend(window, false, true);
        if (!backend || backend->name() != "SDL_GPU")
            throw std::runtime_error("GPU backend required");
        ready = true;
        Renderer2D renderer{std::move(backend)};
        Renderer3D r3d{renderer};
        attachments(r3d);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        const auto* required = std::getenv("KIN_REQUIRE_GPU_TESTS");
        return ready || (required && std::string_view(required) != "0") ? 1 : 77;
    }
}
