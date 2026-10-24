#pragma once

// Summit's presentation (docs/3d_plan.md, P4a minimum and P4b's look): the level's rock with its textures, the ground
// and ranges, the person (a skinned climber driven from the game's state), the rope, camps with tents, fires and
// lanterns, the sky, fog, weather and light by altitude and time, and the dust and breath puffs. It reads a `Game` and
// writes a Scene3D; it never writes the game.
//
//     summit::SummitView view(r3d, game.level());
//     view.set_look("afternoon");
//     ...each fixed step:  view.on_step(game);
//     ...each frame:       view.fill(scene, game, camera_view, frame_seconds);  r3d.render(target, scene);

#include "camera.hpp"
#include "climber.hpp"
#include "environment.hpp"
#include "external_assets.hpp"
#include "game.hpp"

#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/render3d.hpp>
#include <kin/ecs/world.hpp>
#include <kin/render3d/particles3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <memory>
#include <string>
#include <vector>

namespace summit {
using namespace kin;

class SummitView {
public:
    SummitView(Renderer3D& r3d, const SummitLevel& level, bool external_assets = true, bool core_quality = false);

    // False for a name that is not a look (the look is unchanged).
    bool set_look(const std::string& name);
    const Look& look() const { return *_look; }
    // Starts the clock at this time of day instead of the look's (negative: the look's).
    void set_start_hours(f32 hours) { _start_hours = hours; }
    // The effects that are on, and the game hours a minute the clock runs at (0: frozen).
    void set_effects(const EffectSettings& effects, f32 time_hours_per_minute);
    const EffectSettings& effects() const { return _effects; }
    // Native quality follows the look's HDR/exposure/bloom; core uses SDR and one sample.
    ViewOptions3D view_options(u32 samples) const;
    bool core_quality() const { return _core_quality; }

    // Once a fixed step after Game::step(): what happened (a landing's dust, a camp reached) for the frames to draw.
    void on_step(const Game& game, i32 player = 0);

    // Clears `scene` and fills it for `player`: camera, lights, sky, fog, weather, the mountain, the person, the rope.
    // `dt` is the frame's time (the person's clips, the fires and the puffs advance by it: presentation only).
    // Where the person is drawn instead of where the last step left it (CameraRig::person_position / person_facing at
    // the frame's alpha, the same instant as the camera); unset, the step's. Held until changed.
    void set_person_pose(Vec3f feet, f32 facing_yaw) {
        _pose_feet = feet;
        _pose_facing = facing_yaw;
        _pose_set = true;
    }
    void fill(Scene3D& scene, const Game& game, const CameraView& camera, f32 dt, i32 player = 0);

    // What the last fill() did: the environment it used, the lights and particles it added.
    const EnvironmentState& environment() const { return _env; }
    u32 lights_added() const { return _lights_added; }
    u32 particles_alive() const;
    // Draws the colliders within `radius` m of the camera as wireframes (a debug aid, off by default): the level's
    // boxes grey, the decor's hulls orange (buttresses) and cyan (stones).
    void set_show_colliders(bool on, f32 radius = 70.0f) {
        _show_colliders = on;
        _collider_radius = radius;
    }
    // A triangle count of what is drawn each frame (rock, decor, terrain), for the report.
    u64 static_triangles() const { return _static_triangles; }
    u32 scenery_instances() const { return _external_assets.scenery_instances(); }
    u64 scenery_triangles() const { return _external_assets.scenery_triangles(); }
    u64 instance_record_bytes() const { return _external_assets.instance_record_bytes(); }
    bool bundled_assets_ready() const {
        return _imported_person && _imported_materials && _external_assets.loaded_models() == PropCount;
    }

private:
    void drive_person(const Game& game, i32 player, f32 dt);
    Vec3f _pose_feet{};
    f32 _pose_facing = 0.0f;
    bool _pose_set = false;
    void make_textures(bool external_assets);
    void draw_expedition(Scene3D& scene, const Game& game);
    std::vector<Piece> _equipment;

    Renderer3D* _r3d;
    bool _core_quality = false;
    sky_model::AmbientLight3D _core_ambient;
    Assets3D _assets;
    bool _imported_person = false, _imported_materials = false;
    ExternalAssets _external_assets;
    const Look* _look;
    EffectSettings _effects{};
    f32 _time_speed = 0.25f;
    f32 _start_hours = -1.0f;
    Bounds3D _bounds;
    f32 _level_height = 150.0f;
    struct Part {
        Mesh3D mesh;
        Material3D material;
    };
    std::vector<Part> _rock, _decor, _terrain;
    bool _show_colliders = false;
    f32 _collider_radius = 70.0f;
    Mesh3D _collider_mesh;
    u64 _static_triangles = 0;
    Mesh3D _cylinder, _cone, _box, _sphere, _cone4;
    Texture _rock_albedo, _rock_normal, _grass_albedo, _grass_normal;
    Texture _rock_orm, _grass_orm;
    std::shared_ptr<const Model3D> _person_model;
    std::unique_ptr<::kin::EcsWorld> _ecs;
    std::unique_ptr<WorldRenderState3D> _state;
    flecs::entity _person;
    bool _person_ok = false;
    std::vector<Camp> _camps;
    Vec3f _summit{};
    // Per camp: flames and smoke; shared: dust of landings and breath.
    struct Fire {
        ParticleSystem3D flames{1}, smoke{2}, sparks{3};
    };
    std::vector<Fire> _fires;
    ParticleSystem3D _dust{11}, _breath{12};
    f32 _breath_timer = 0.0f;
    f32 _squash = 0.0f;               // the landing's crouch, 1 to 0
    f32 _hang_sag = 0.0f;             // how far the body hangs below where the capsule is, to straighten the arms (m)
    f64 _clock = 0.0;                 // presentation time (the fires' flicker)
    std::vector<Vec3f> _pending_dust; // places of landings waiting for a frame
    std::vector<f32> _pending_dust_size;
    EnvironmentState _env{};
    u32 _lights_added = 0;
    // Scratch for the pose override.
    std::vector<RagdollBonePose3D> _bones;
};

} // namespace summit
