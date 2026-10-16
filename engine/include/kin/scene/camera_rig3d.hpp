#pragma once

#include <kin/scene/scene_session3d.hpp>
#include <kin/physics3d/spring_arm3d.hpp>

namespace kin {
struct CameraRig3DSettings {
    Vec3f target_offset{0,1.2f,0}; // world-space offset from the target's origin
    SpringArm3DSettings arm;
    Layer3DMask mask=all_layers3d;
    bool sensors=false;
    f32 fov=60,z_near=.1f,z_far=1000;
    f32 pitch_min=-75,pitch_max=35;
    f32 initial_yaw=0,initial_pitch=-15;
    f32 teleport_distance=12;
};
struct CameraRig3DFrame {
    Camera3D camera;
    SceneEntity3D target;
    Mat4f target_transform; // presentation only, at the same alpha as the camera
    SpringArm3DFrame arm;
};
// A checked target and two fixed-step snapshots. No world/device ownership.
// orbit() in the input step, observe() after fresh world transforms, frame()
// once per render with the host's alpha and real frame seconds. It never writes
// Transform3D, WorldTransform3D or native physics. Missing targets return no frame.
class CameraRig3D {
public:
    explicit CameraRig3D(CameraRig3DSettings settings={});
    bool configure(CameraRig3DSettings settings); // failure retains current settings/history
    const CameraRig3DSettings& settings()const{return _settings;}
    void follow(SceneEntity3D target);
    SceneEntity3D target()const{return _target;}
    bool orbit(Vec2f degrees); // right/up input; finite values only
    bool set_angles(f32 yaw,f32 pitch);
    f32 yaw()const{return _yaw;}
    f32 pitch()const{return _pitch;}
    bool observe(EcsWorld& world);
    // Forget interpolation and arm history; preserve target and orbit angles.
    // The next observation snaps. Automatic distance/scale-change snaps too.
    void reset();
    std::optional<CameraRig3DFrame> frame(const SceneQueries3D& queries,f32 alpha=1,f32 frame_seconds=0);
    // Optional gameplay view: call once after observe per fixed step. Its arm
    // history is independent of frame(), so render frequency cannot affect LOS.
    std::optional<CameraRig3DFrame> fixed_frame(const SceneQueries3D& queries,f32 fixed_seconds);
private:
    std::optional<CameraRig3DFrame> sample(const SceneQueries3D&,f32,f32,SpringArm3D&);
    struct Pose {Vec3f position{},scale{1,1,1};Quatf rotation{};f32 yaw=0,pitch=0;};
    CameraRig3DSettings _settings;
    SpringArm3D _arm;
    SpringArm3D _fixed_arm{_arm}; // share retained probes, independent recovery state
    SceneEntity3D _target;
    Pose _previous,_current;
    bool _have=false;
    f32 _yaw=0,_pitch=-15;
};
} // namespace kin
