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
