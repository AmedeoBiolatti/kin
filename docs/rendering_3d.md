# 3D Rendering (Experimental)

> Experimental: built with `-DKIN_EXPERIMENTAL_3D=ON`, and the API may change
> in any release. Why it exists and where it is going: [the plan](3d_plan.md).

Kin draws 3D inside its 2D frame. A **view** is a picture (colour and depth)
that a 3D **scene** is rendered into; Renderer2D lays it into the frame like
any render target, with 2D drawing and ui2 under or over it. A full-screen 3D
game is a view the size of the screen with its HUD drawn on top; a model
turning in a panel is a small view.

3D needs the SDL_GPU backend (`renderer.capabilities().rendering_3d`). Under
SDL_Renderer (and in headless runs without `KIN_RENDER_BACKEND=gpu`) meshes
come back empty and a view shows only its background, so a game still runs
and its state can still be tested.

```cpp
#include <kin/render3d/renderer3d.hpp>

kin::Renderer3D r3d{renderer};
kin::Mesh crate = r3d.create_mesh(kin::meshes::box({1, 1, 1}));
kin::View3D view = r3d.create_view({640, 360}, {.samples = 4});

// each frame
kin::Scene3D scene;
scene.camera = kin::Camera3D::orbit({0, 0, 0}, angle, 30.0f, 4.0f);
scene.draw(crate, kin::Mat4f::rotation_y(angle), {.color = kin::Color::rgb(200, 140, 80)});
r3d.render(view, scene);                                 // after the 2D drawn so far
renderer.draw_texture(view.texture(), {0, 0, 640, 360}); // laid in like a render target
```

## Conventions

- Right-handed, +Y up, -Z forward (glTF's); metres are a good unit. A camera
  looks down its own -Z.
- Angles in degrees, as in 2D; a positive angle turns anticlockwise looking
  down the axis toward the origin.
- `kin::Mat4f` is column-major and maps column vectors: `a * b` applies `b`
  first. `Mat4f::trs(position, rotation, scale)` places a thing.
- Depth is reverse Z: 1 at the near plane, 0 at the far one (or infinity).

The maths is in `kin/core/math3d.hpp`: `Vec3f`, `Vec4f`, `Quatf`
(`axis_angle`, `yaw_pitch`, `look_rotation`, `slerp`), `Mat4f` (`translation`,
`scaling`, `rotation`, `look_at`, `perspective`, `orthographic`, `inverse`),
and `project`, `unproject` and `screen_ray` between the world and 2D draw
coordinates.

## Meshes

`kin::MeshData` is triangles as data: vertices (position, normal, UV, colour)
and indices, three a triangle, counter-clockwise seen from the front.
`validate()` says whether it can be drawn; `append(other, transform)` builds
a shape from shapes. `Renderer3D::create_mesh` keeps it on the GPU as a
`kin::Mesh` (cheap to copy). Meshes may outlive the renderer.

Shapes made in code (`kin::meshes`): `box`, `plane`, `sphere`, `cylinder`,
`cone`, `torus`, `star_prism`, `octahedron`.

## Scenes

A `kin::Scene3D` is filled each frame and holds no GPU state: a camera, the
lights, and draws.

- `draw(mesh, transform, material, tint)`: one copy. Copies of the same mesh
  and material one after another are drawn as one instanced draw.
- `draw_instanced(mesh, instances, material)`: many, each a transform and a
  tint.
- `add_instances(mesh, count, material)`: room to fill in place (valid until
  the next draw is added).

`kin::Material3D`: `color` (times the mesh's vertex colours and each
instance's tint), `emissive` (light of its own, a share of its colour),
`unlit`, `double_sided` (back faces are culled otherwise). A transform that
mirrors turns a mesh inside out: draw it double-sided.

Lights: `scene.lights.sun` (`direction`, toward the light; `color`, linear,
above 1 is brighter) and `scene.lights.ambient`. Light is added in linear
light; the result is stored as the 2D pipeline stores colour (encoded in a
gamma scene, linear in a linear one, above white in an HDR one, where bloom
and tonemapping pick it up).

## Cameras

`kin::Camera3D` is a position, a rotation and a projection
(`Projection3D::Perspective` with `fov`, or `Orthographic` with `height`, in
world units), `z_near` and `z_far` (infinite by default).
`Camera3D::look_at(eye, target)` and `Camera3D::orbit(target, yaw, pitch,
distance)` make one.

Between the view and 2D, in the coordinates of the rectangle the view is laid
into: `camera.project(world, rect)` (where a point shows, for labels and
health bars) and `camera.ray(point, rect)` (what a click points at;
`kin::hit_plane_y` meets it with the ground).

## Views

`Renderer3D::create_view(size, {.samples, .background})`: `samples` is MSAA
(1, 2, 4 or 8, fewer where the device has fewer); `background` shows where
nothing is drawn (transparent lets the 2D under the view show). `render(view,
scene)` draws the scene into it, recorded between the 2D drawn before and
after; a view keeps its picture until rendered again, so a still model needs
no rendering each frame. `resize(view, size)` gives it a new size and texture.

`Renderer3D::capabilities()` says what the device has (depth bits, most
samples); `stats()` what the last render drew (draw calls, instances,
triangles, CPU time).

## Cost

From `kin_draw_bench 200 N meshes` (RTX 4080 Laptop, 1280x720, 4x MSAA): an
instanced draw of 20,000 boxes takes about 0.2 ms of CPU in `render()` and
0.3-0.5 ms of GPU; 20,000 separate draws about 85 ns each. Instances are
written straight into the upload memory, on the renderer's job system above
8,192 of them (`renderer.set_job_system`). Instance what repeats.

Not yet: textures on meshes, shadows, culling, transparency, point lights,
picking, model files (see [the plan](3d_plan.md)).
