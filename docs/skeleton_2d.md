# 2D skeletal animation and IK

`kin/anim/skeleton2d.hpp` supplies rigs, poses, clip sampling, weighted deformation
and two-bone IK. `kin/ecs/skeleton2d.hpp` adds asset-bound renderers, animators and
attachments. This native format requires no external skeleton runtime.

```cpp
actor.set(kin::SkeletonRenderer2D{.asset = {"rigs/hero.kinskel"}});
actor.set(kin::SkeletonAnimator2D{.animation = "walk"});
actor.set(kin::SkeletonIK2D{.root = "upper_arm", .tip = "forearm",
                          .target = hand_target_world});
// After Assets2D::bind, once per simulation step:
kin::update_skeletons2d(world, dt);
```

Assets2D binds the rig/atlas. Each renderer owns its pose; inherited prefab
components acquire their own copy before updating. Render collectors submit the
deformed mesh dynamically with hierarchy transforms, sorting, culling and clip
groups. Renderer2D::draw_textured_geometry draws indexed meshes with normalized
UVs on SDL, SDL_GPU and WebGL2. Check capabilities().textured_geometry on custom
backends; the compact backend does not implement it.

Animators support clip selection, speed, pause, loop/once, time seeking and a
finished flag. Changing animation resets time. Keys interpolate local position
and scale linearly and rotation over the shortest angle. Unkeyed bones use rest
transforms. Reflected animator/IK fields can be driven by existing property
animation/state-machine bindings and entity scripts. Call update_skeletons2d
explicitly, as with sprite animators; render-time binding does not advance clips.

IK runs after sampling and converts the world target into skeleton space. It
solves a direct parent/child chain with positive uniform scales and no shear,
clamps unreachable targets, preserves lengths and supports either bend direction
and a 0..1 weight. Unsupported chains return false. Multiple constraints can be
solved directly on the pose in a chosen order with solve_two_bone_ik2d.

A rig's named attachment is a bone-relative transform. A direct child entity
with SkeletonAttachment2D follows it:

```cpp
weapon.child_of(actor).set(kin::Transform2D{})
    .set(kin::SkeletonAttachment2D{.attachment = "hand", .offset = {2, 0}});
```

skeleton_attachment2d returns the attachment's affine matrix in skeleton space
for custom drawing/effects. The ECS attachment path uses Transform2D; use that
matrix directly for exact shear.

`.kinskel` is JSON, angles clockwise degrees, positions in rig units, normalized
UVs, and texture_asset relative to the content root:

```json
{
  "schema": "kin.skeleton2d/1",
  "texture_asset": "images/arm.png",
  "bones": [
    {"name": "upper", "parent": -1, "length": 10},
    {"name": "lower", "parent": 0, "position": {"x": 10, "y": 0}, "length": 10}
  ],
  "vertices": [
    {"position": {"x": 0, "y": 0}, "uv": {"x": 0, "y": 0},
     "influences": [{"bone": 0, "weight": 1}]},
    {"position": {"x": 10, "y": 0}, "uv": {"x": 1, "y": 0},
     "influences": [{"bone": 0, "weight": 0.5}, {"bone": 1, "weight": 0.5}]},
    {"position": {"x": 10, "y": 4}, "uv": {"x": 1, "y": 1},
     "influences": [{"bone": 1, "weight": 1}]}
  ],
  "indices": [0, 1, 2],
  "attachments": [{"name": "hand", "bone": 1, "position": {"x": 10, "y": 0}}],
  "clips": {
    "wave": {"duration": 1, "tracks": [{"bone": 0, "keys": [
      {"time": 0, "rotation": 0}, {"time": 1, "rotation": 90}
    ]}]}
  }
}
```

At most 256 bones, 65536 vertices and four nonnegative influences per vertex.
Parents precede children (-1 is a root). Skin vertices are rest positions in
skeleton space; the loader normalizes weights and validates indices, key ordering,
unique names and nonsingular bind transforms. C++ assets should pass
validate_skeleton2d and remain immutable while poses refer to them. Keys contain
absolute local transforms; omitted fields use the bone's rest transform. One rig
uses one atlas. External Spine/DragonBones import and editor rigging are not included.

Asset inventory recognizes Skeleton2D; shipping discovery and async preparation
follow texture_asset. Broken reloads retain an entity's usable rig; a valid
replacement resets its pose before the next animation update.

## Blending and layers

Set `SkeletonAnimator2D::fade_seconds` to crossfade when `animation` or
`blend_space` changes (zero preserves immediate switching). Transitions capture
the displayed base pose, so interrupting a fade starts from the current result.
Pausing the animator also pauses its fade. The cached base excludes layers and
IK; those are applied once after blending. A rig replacement resets this state.

`SkeletonLayer2D` applies one additional clip before IK. `bone` selects a named
bone; `include_children` includes its subtree. An empty bone selects the whole
rig. `weight` is clamped to 0..1. Override layers interpolate local transforms;
`additive=true` applies the clip's translation/rotation/scale difference from the
rest pose. Layer time, speed, playing and loop are independent of the base.
For more than one layer, call `layer_skeleton2d` on poses in the game's chosen order.

Native `blend_skeleton2d` mixes weighted clip samples; `blend_skeleton_poses2d`
mixes two poses. Rotation follows the shortest angular difference. Negative or
nonfinite weights and invalid samples fail without changing the output. Empty
or all-zero samples produce the rest pose.

`.kinskel` accepts an optional `blend_spaces` object:

```json
"blend_spaces": {
  "movement": {
    "points": [
      {"position": {"x": 0, "y": 0}, "clip": "idle"},
      {"position": {"x": 1, "y": 0}, "clip": "run_right"},
      {"position": {"x": 0, "y": 1}, "clip": "run_down"}
    ],
    "triangles": [[0, 1, 2]]
  }
}
```

Choose `blend_space="movement"` and update `blend_position` from movement input.
`blend_cycle_seconds` controls a normalized phase shared across clips of different
durations. One point is constant; two points interpolate along their segment.
Three or more require explicit nondegenerate triangles, using barycentric weights
inside and the nearest triangle edge outside. Limits are 64 points/128 triangles
per space. The loader validates clips, duplicate positions and triangle indices.
Use `sample_skeleton_blend_space2d` directly for custom animation controllers.
