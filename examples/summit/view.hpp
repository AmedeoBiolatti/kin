#pragma once

// Summit's presentation (docs/3d_plan.md, P4a minimum; P4b builds the HUD, sound and the rest on this): the level's meshes, the
// person (a skinned model driven from the game's state), the rope, camps and summit markers, and a sky, fog and weather preset at
// a fixed look. It reads a `Game` and writes a Scene3D; it never writes the game.
//
//     summit::SummitView view(r3d, game.level());
//     view.set_look("afternoon");
//     ...each frame:  view.fill(scene, game, camera_view, alpha, frame_seconds);  r3d.render(target, scene);

#include "camera.hpp"
#include "game.hpp"

#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/render3d.hpp>
#include <kin/ecs/world.hpp>
#include <kin/render3d/renderer3d.hpp>

#include <memory>
#include <string>
#include <vector>

namespace summit {
using namespace kin;

// A look: the time of day and the air (a fixed picture of the mountain, not a mechanic).
struct Look {
    const char* name;
    f32 hours;
    f32 turbidity;
    f32 fog_density;
    f32 fog_height_density;
    f32 rain;
    f32 snow;
    f32 mist;
    f32 exposure;
    f32 bloom;
    bool stars;
};

const std::vector<Look>& looks();
const Look* find_look(const std::string& name);

class SummitView {
public:
    SummitView(Renderer3D& r3d, const SummitLevel& level);

    // False for a name that is not a look (the look is unchanged).
    bool set_look(const std::string& name);
    const Look& look() const { return *_look; }
    // The view's options the look wants (HDR, exposure, bloom); the size and samples are the caller's.
    View3DOptions view_options(u32 samples) const;

    // Clears `scene` and fills it for `player`: camera, lights, sky, fog, weather, the mountain, the person, the rope.
    // `dt` is the frame's time (the person's clips and IK advance by it: presentation only).
    void fill(Scene3D& scene, const Game& game, const CameraView& camera, f32 dt, i32 player = 0);

private:
    void drive_person(const Game& game, i32 player, f32 dt);

    Renderer3D* _r3d;
    const Look* _look;
    Bounds3 _bounds;
    struct Part {
        Mesh mesh;
        Material3D material;
    };
    std::vector<Part> _rock;
    Mesh _cylinder, _cone, _box;
    std::shared_ptr<const Model> _person_model;
    std::unique_ptr<::kin::EcsWorld> _ecs;
    std::unique_ptr<WorldRenderState3D> _state;
    flecs::entity _person;
    bool _person_ok = false;
    std::vector<Vec3f> _camp_points;
    Vec3f _summit{};
    // Scratch for the pose override.
    std::vector<RagdollBonePose3D> _bones;
};

} // namespace summit
