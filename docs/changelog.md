# Changelog

All notable changes to kin are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and kin uses
[Semantic Versioning](https://semver.org/) — while the version is 0.x, minor
releases may change APIs.

## [Unreleased]

- Bind assets that components name by path in 2D scenes (`Assets2D`, run by
  `EcsScene`): textures, sprite catalogs, sounds and audio catalogs, and
  particle catalogs, read again when their files change. Particle emitters
  burst an authored effect at a rate where their entity is.
- Describe 2D physics and particle components to scene files and tools, with
  `choices` for string fields and nested-member fields. Add
  `PhysicsWorldDef::units_per_meter` for worlds in points. Give prefab
  instances physics bodies of their own, and remove `EcsPhysicsBridge`'s
  observers with it (they could reach a destroyed `PhysicsWorld`).
- Add hex tilemaps (`TileMap::grid`: pointy- or flat-top hexagons in offset
  cells), drawn, picked, collided, filled and pathed by their hexagons; tileset
  files (`.kintileset`) that maps name to share, their tiles cut from an image
  (`TileDefinition::frame`); `TilemapComponent`, bound by `Assets2D` and drawn
  by `WorldRenderState` where its entity is (`TilemapRenderOptions::origin`).
- Add sprite animations to sprite catalogs (`animation` lines: frames, frames
  a second, loop, once or ping-pong, frames held longer), played on an entity
  by `SpriteAnimator` (`update_sprite_animators`), described to scene files.
  Add `parse_sprite_catalog`, `serialize_sprite_catalog` and removing from a
  catalog. `entry_of` may name another component's asset (`SpriteRenderer.asset`).
- Add scene cameras (`CameraComponent`: zoom, limits, smoothing), drawn
  through by `EcsScene` (`scene_cameras`, `view_size`; `scene_camera`,
  `update_cameras`). Describe `ShapeRenderer` (shapes made from a `ShapeSpec`
  or an SVG file by `Assets2D`), `ClipGroup` (its clip made from a shape) and
  the new `TextRenderer` (text in the world, in a content font or the system's)
  to scene files. Add `AssetType::Shape` (`.svg`).
- Add blend spaces (`blendspace` blocks: animations mixed by a float
  parameter, kept in step), played by states and `set_base`, and transition
  blends (`blend` seconds: the state left fades into the next;
  `AnimationPlayer::fading`).
- Let an `Animator` run a state machine of its file (`machine`, its `params`
  set by the game, its `state` told). Add the `finished` condition (a state's
  animation has played through) and `place` lines (where editors draw states).
  An any-state transition into the state already running no longer restarts
  it every step. `.kinanim` files keep a `step` easing (it was written as
  linear).
- Add `Animator`, which plays an animation of a `.kinanim` file on an entity
  from scene files (`update_animators`; `Assets2D` reads the file), its tracks
  setting any described field (`register_described_properties`,
  `EcsComponentRegistry::field_access`); instances of prefabs animate their own
  copies.
- Give particle effects a direction and spread, an emission shape (point,
  circle, box), drag, rotation and spin, and a size and colour midway through
  their lives, written as optional `name=value` settings so older catalogs
  read unchanged. Add `parse_particle_catalog`, `serialize_particle_catalog`,
  and `ParticleCatalog::remove_effect`/`remove_field`.
- Write f32 field values in their shortest form (`0.2`, not `0.20000000298`).
- Add `input.drop` to the scene server and `App::hold_close` for editors.
- Add game controller bindings (`pad` lines, `PadControl`: buttons, the d-pad,
  sticks and triggers pushed past halfway) and axes (`InputAxis`, `axis`
  lines: bindings for -1 and 1, analog sticks, a dead zone;
  `Input::axis`) to input maps, read by `parse_input_map` and written by
  `serialize_input_map`. `App::open_gamepads` polls a controller with the
  window's events. `binding_held` and `axis_value` let editors try bindings
  that are not in the map.
- Rich text (`parse_advanced_text`, the dialogue element) reads `[b]` and
  `[i]`: in `AdvancedTextMarkupOptions::bold_font` and `italic_font` when
  given. Before, they showed as text, and a dialogue's line was cut short by
  their length while it was revealed.
- Add game icons and project info to games' builds:
  - `GameInfo::icon` (`icon <png>` in a game info file) and
    `WindowedAppConfig::icon` set the window's icon; `window_config(info,
    content)` fills it in. `Window::set_icon` takes an `Image`.
  - `kin_game(... INFO project.kin)` takes the title, version and icon from a
    game info file, and uses `<icon>.ico` in the content as the Windows icon.
  - `kin_game` defines `KIN_GAME_TITLE`, `KIN_GAME_VERSION` and
    `KIN_GAME_ICON` for the game's code.
- Translate UI screens and scene text without code:
  - A `.kinui` label, button or toggle with an id, and no `key` of its own, is
    translated under `ui.<screen>.<id>` (a text field's hint under
    `ui.<screen>.<id>.hint`) when the active localization has that key.
    `screen_name`, `screen_text_key` and `screen_language_file` give the
    names and a file to start from.
  - A scene's `TextRenderer` is translated under `scene.<scene>.<entity>`, or
    its own `key`. `shown_text` and `scene_text_key` give the text and key.
  - Lua's `l10n` table gets `set_locale()` in scene scripts too.
- Add dialogue in scenes:
  - `SceneDialogue` runs one dialogue at a time. It reveals text letter by
    letter, keeps the game's variables between dialogues, reports events, and
    saves with snapshots.
  - `TalkComponent` and `Talker`, with `update_talk`, start a dialogue when
    the talk action is pressed nearby.
  - A `dialogue` element in UI screens shows the running dialogue and passes
    on clicks.
  - Lua scripts get a `dialogue` table and `on_dialogue_event`.
  - `.kindialogue` files keep node places, speaker colours and line tags,
    and list the nodes a game also starts at (`entries`).
  - Add `dialogue_problems` and `dialogue_targets`.
- Add UI screens as data: `.kinui` files are trees of boxes, panels, labels,
  buttons, images, bars, toggles, sliders, text fields and spacers, with sizes,
  anchors and a theme.
  - `ui2::UiScreen` draws one, sets elements by id and reports clicks and
    values.
  - A `UiScreenComponent` shows a screen in a scene.
  - `SceneScreens` reads screens and themes and reloads them when they change;
    entity scripts reach them through `ui`.
  - Add `AssetType::UiScreen` and `AssetType::UiTheme`.
- Let `.kintheme` files name a font file (`font`, `bold_font`, `font_size`)
  and image skins (`image <path> [margin] [x y w h]`), read by
  `ThemeFile::build`. Add the theme name lists for tools.
- Add lights to scenes: `LightComponent` lights from its entity, and
  `SceneLighting` (the ambient colour) makes `EcsScene` light the world, while
  the UI layer and above stay unlit. Add `scene_lights` and
  `apply_scene_lighting`.
- Add `ColorGrade`: a scene's exposure, tonemapping, look (a `.cube` file or a
  LUT image) and dithering, set as the colour output.
- Add `Renderer2D::draw_color_output` and `LayerOptions::grade` for graded
  previews.
- Add a `blend` field to sprites, textures, rects, shapes and particles
  (`blend_material`; the renderer applies a material's blend mode).
- Add `ShaderEffect`: a fragment shader of the content drawn over a box at its
  entity, with its uniforms set by name from `params`.
  - `time` and `size` are set by kin.
  - `Assets2D::shader` compiles the shader with glslc, or loads the `.spv`
    beside it, and reloads it in place when it changes.
  - Add `AssetType::Shader` and `AssetType::ColorLut`.
- Add scripts on entities: `ScriptComponent` names a Lua file, which
  `EntityScripts` runs with `on_start`, `on_update` and `on_contact`. `self`
  reaches the entity's components, transform, `Animator`, input actions and
  axes; scripts can find, spawn and destroy entities. Changed script files are
  read again while the game runs. Add `check_lua_syntax`.

- Add experimental Emscripten/WebGL2 exports for Hex and Summit, with itch.io
  ZIP packaging, bundled assets/fonts/licenses, browser audio, pointer lock,
  fullscreen and persistent settings with a storage-denied fallback.
- Share owned scene/window sessions between native loops and browser frames;
  add cooperative jobs and select serial Jolt when workers are unavailable.
  Permit rendering-only 3D builds without Jolt or Lua.
- Add WebGL2 render targets, instancing, stock PBR materials, skinning,
  procedural sky/fog and cascaded sun shadows. Cache material state and instance
  buffers, preserve indexed 2D geometry and draw common HUD shapes analytically
  with shared native coverage math. Keep caches bounded with streaming fallback.
- Add browser rendering/gameplay CI, simulation exports, pixel references and
  matched native/browser benchmark tools. Record CPU, upload and pacing data
  separately from optional profiling/GPU diagnostics; retain exact symbols.

- Added tag-triggered release publishing gated on the full CI matrix and fresh
  desktop runtime acceptance. Publish the validated player archives, matching
  symbols, reports and checksums without rebuilding.
- Validate original ZIP entry names before Python's Windows normalization;
  fix scene-test callback lifetimes exposed by release LTO.
- Preserve seeded 2D particle fields with GCC 13.3 size optimization and LTO;
  retain Sponza's unavailable-renderer status after scene shutdown, and enforce
  read-only Windows acceptance despite writable runner group permissions.
- Honor explicit SDL render-driver selection when validating the software path
  on a real display; retain the accelerated SDL renderer as the default.

- Added optional engine component targets, renderer/SDL profiles, release LTO,
  section garbage collection, separate debug symbols, optional compiler PGO,
  asset dependency reports and lossless asset cooking. `kin_game` keeps the
  existing content-pack layout and package names while sharing the audited
  desktop exporter. Signal Siege now carries its multilingual font subsets.
- Added versioned release identity, package size/provenance reports and desktop
  acceptance CI alongside the existing Steam Runtime builds.


### Fixed

- ui2 text: a DEL byte (0x7F) in otherwise plain text read and wrote past the
  ASCII kerning table (a heap overflow); it is a control character now, as the
  others. Shaped runs drawn with distance-field fonts kept pointers into a
  cache that drawing the next run could move (a use after free).
- ui2: a double click, a tooltip's delay and a caret's blinking are timed in
  seconds, not frames (a window that waits for events draws few frames: two
  clicks seconds apart counted as a double click). A widget hidden by the clips
  it draws under (a field scrolled out of its panel) is no longer hit there.
  Tab goes through boxes and text fields in layout order (fields came after
  every box). The wheel scrolls the box under what is on top, not one under a
  dialog or a menu. A virtualized list builds the rows for this frame's scroll
  offset (a thumb dragged, `set_scroll_offset`, `scroll_to_row`), not last
  frame's (rows were blank for a frame). Enter or Esc leaves a text field and
  its focus (it took the keys back the next frame). A widget's `state` the app
  changes after declaring the widget that frame is kept, not undone by the
  widget's run.
- ui2: a box that was hovered last frame under a box no longer there was not
  hovered (a drop target, right after the pointer moved). `MenuBar` no longer
  highlights a title while idle, and switches an open menu only when the
  pointer moves onto another title (one resting there took over the menu the
  keys opened). A virtualized `ListView` scrolls to keep its selection in view
  only when the keys moved it.
- ui2: a `TextInput`, `NumberInput` or `TextEdit` being typed in keeps its
  focused frame under the pointer (it showed the hover colour instead); other
  controls show the focused frame for the keys' focus only.
- `rasterize_svg` refuses text without an `<svg>` element before SDL_image
  parses it, as SDL_image does for files: SDL_image does not free a parse that
  comes out empty (16 bytes lost per call).

- Preserve transparent TAA motion on software GPUs by routing colour-only particle
  and custom material shaders through the velocity pre-pass. Keep particle-shadow
  history stable across views, and restore Windows compilation of Summit, the
  renderer tests and the reference tool.

- Bound navmesh boundary-rejection diagnostics to eight face examples plus one
  count/bounds summary per build, including when refinement rejects many faces.

- Report each rejected navmesh source face once per build, withdraw only conflicting
  samples, and avoid a pass-limit warning when the final pass completes the work.
  Cache ear blockers to bound boundary triangulation to quadratic time.
- Measure opaque motion at visible fragments for transparent TAA near the camera;
  keep local-shadow content identities stable across interleaved views.
- Document `restore(saved, ParticleRestore3D::Rollback)` for particle rollback presentation continuity.

- Restore the eight-radius transparent motion fallback after spatial misses;
  preserve character position edits between fixed steps in legacy interpolation.
- Complete the last navmesh refinement pass before warning, report its location,
  and replace unchecked ears/per-face drops with validated boundary triangulation
  and shared-edge cut withdrawal. Cover area, overlap and precision limits.

- Resolve rounded stair seam cuts and carry samples across long coarse flights;
  retain valid geometry at the numerical pass limit and cover 64/200-tread paths.
  Recheck support-dependent clearance when seam topology changes.
- Try anonymous transparent copies in collection order before spatial matching,
  preserving fast moving rows and avoiding the common per-frame place grid.
- Preserve authored character turns on render-only frames and interpolate position
  independently of mouse-look; materialize inherited transforms before updates.
- Preserve particle presentation on identical restores and provide explicit
  `restore(bytes, ParticleRestore3D::Rollback)` continuity for rollback/resimulation.


- Preserve transparent TAA over stationary triangles extending behind the camera,
  for plain/skinned meshes and both velocity paths. Evaluate motion derivatives
  before cutout discards. Match shared-material transparent reorders spatially;
  retain opaque-history coverage tests and allocate appearance arrays only for
  transparent copies. Skip shadow-content fingerprints without temporal receivers.
- Keep authored character rotations stable through legacy interpolation, reset
  rotation history on snaps, and guard missing physics transform components.
- Retain compact particle motion history instead of full sprite snapshots. Give
  clear/restore a fresh presentation epoch without changing simulation randomness,
  state hashes or saved bytes.
- Reject stale/foreign deferred entity placeholders by command batch in C++ and
  Lua. Report native exception positions in both Lua hosts. Destroyed reflected
  2D entities reject later access (including `has()` and repeated `destroy()`) with
  a Lua error; liveness predicates remain available.
- Expose rigid-body `scene3d.teleport(entity, position, rotation?)`, preserving
  orientation when omitted and resetting velocity/interpolation for kinematics.

- Reuse navmesh seam topology and selectively recheck changed faces instead of
  rebuilding the level per pass. Preserve long slab cuts and complete coarse
  flights; cover rerouted stair guides and descending nosings. Document the
  effective `max(agent_height, max_step_height)` headroom.
- Track transparent appearance per matched copy and material partition, including
  independent anonymous draws sharing a mesh. Frame the vendored Sponza smoke
  fixture with ample coverage above the blank-picture threshold.
- Use one stale-entity Lua error across gameplay arguments, reflected access and
  commands; preserve explicit liveness queries and historical action tokens.
- Centralize model profile diagnostic prefixes and upload preflighted primitives
  without repeating mesh validation. Public mesh creation remains checked.

- Interpolate character rotation and particle sprites with the fixed-step frame;
  clamp finite presentation alpha and skip unchanged sleeping-body roots. Add
  an explicit bridge teleport for kinematics and reuse rope readback buffers.
- Preserve Lua caller locations through `pcall`, reject expired command targets,
  and validate on-load errors in receiver regressions.
- Restore recoverable Jolt capacity reporting in Debug; keep other Jolt asserts
  active. Validate session descendants outside Flecs iteration locks.
- Separate shadow content revisions from redraws, keep anonymous glass identities
  independent of opaque culling, and express opaque motion tolerance in pixels.
  Select bounded particle history with compact linear-time candidates.
- Preserve rotated stair seams and test cylinder headroom under stair soffits;
  keep query allocator storage off small thread stacks.
- Retain glTF buffer owners without a second loose-file copy, report profile
  violations consistently during inspection, and enforce smoke-test exit codes.

- Repair short-edge stair seam triangulation, exact-position welding, mixed-grid
  nosings and stairs on continuous floors. Recheck new joins against blocking walls.
- Retain lit-particle TAA under 1% fire flicker; preserve opaque motion through
  particles and glass, and show completed colour frames if coverage replay fails.
- Avoid Lua registry references per 2D method/property call, preserve script locations
  on receiver errors, and preserve prefab leases across `scene3d.resolve`.
- Keep rope-hand IK continuous at alpha 1; drain/publish constraint break events.
- Exercise Sponza smoke with vendored geometry under an empty reference cache.

- Restore safe 2D Lua method/property receivers in Release without changing 2D
  numeric conversions. Missing/wrong `self` raises a Lua error.
- Avoid terrain-wide ramp-margin floods and per-candidate clipping allocations;
  preserve interior-wall corner links, exact-radius doorway clearance, radius-only
  ramp joins, nosed stairs with mismatched tessellation, and mandatory step points.
- Preserve transparent TAA under camera parallax without an opaque-motion copy;
  validate moving local lights across the complete particle image.
- Pack/upload particle casters only for maps being redrawn, prevent draw-time
  creation of cast views, and release caster-order and particle-history scratch.
- Preserve model rejection reasons even with no usable primitives; validate the
  profile before GPU uploads and support omitted glTF inverse bind matrices.
  Remove `Model3D::asset_diagnostics`; failed load results own the diagnostics.
- Preserve renderer failures through session picking. Make required GPU example
  tests fail on missing devices/assets, and use a vendored Sponza-host smoke model.
- Correct headless interpolation and Lua-safety documentation.

- Restore Clang aggregate animator construction; make headless `App::run_for`
  render the completed step and exercise that callback in runtime tests.
- Preserve traversable stair joins for nonzero agent radii, distinguish risers
  from portal walls, and compute slope clearance locally. Rebuild support
  topology before eroding ramps on continuous floors.
- Check Lua 3D usertype receivers in Release and use the same exact integer
  conversion rules in every build, including integral arithmetic results.
- Retain lit-particle history under moving lights; remove the unused history
  sort. Reject divergent opaque/transparent motion locally under static glass.
  Skip presentation preparation at alpha 1 and reject entity-zero targets safely.
- Wake loads after dynamic support reshaping, narrow support wake queries to
  actual contacts, and avoid redundant kinematic/layer wake queries. Bound rope
  and constraint events, restore identity one-child compound bakes, remove stale
  ECS physics handle components, reuse rope readback storage, and provide valid
  internal motion properties for kinematic surface shapes.
- Keep the required lavapipe CI check name, enable 3D branch pushes, install glslc
  across the compiler matrix, cover example GPU workflows, and compare Summit
  replay hashes and exact step counts within each build rather than against one
  machine's result.

- Sample traversal hand IK with moving holds and rendered rope polylines,
  including rotation and clearance. Preserve manual targets and reset grip
  history after release, replacement and rollback.
- Guard root/invalid Flecs entities during interpolation; sample body-parented
  cameras, ragdoll bones, ropes and IK targets from the same presentation frame.
  Cache moving subtrees once per collection. Headless SceneApp now supplies alpha 1.
- Connect stairs with optional `max_step_height` and preserve vertical step points
  in paths. Propagate navmesh radius clearance from local boundaries.
- Keep lit-particle TAA history under light colour/intensity flicker. Use hashed
  particle history matching and compact selected shadow sprites into one draw per
  source/map, retaining per-map budgets, culling and cache keys.
- Reject wrong-typed optional Lua arguments. Scope argument checks to 3D bindings,
  removing the engine-wide sol2 safety define and restoring legacy 2D conversions.
- Cache failed model-file loads until their source changes. SDL_Renderer 3D
  fallback rendering/warm-up now succeed. Fix unsafe animation snippets and stale
  result/error examples; run GPU CI in Debug as well as Release.

- Correct local navmesh erosion on vertically overlapping floors connected by a
  ramp, and boundary-only stair risers in headroom clipping.
- Restore sleeping-body wakeups after collision-layer/group/filter changes,
  keep disabled contacts readable, and avoid full body reconciliation for an
  unchanged refused body. Preserve native particle emission controls.
- Check Lua 3D option, argument and userdata types locally, and bridge
  reflected entities/session handles with `scene3d.handle` and `scene3d.resolve`.
- Add read-only fixed-step presentation through session/frame collection alpha,
  including body/character descendants and joint-attachment roots.
- Use local transparent temporal coverage and particle/blended-mesh reprojection;
  retain opaque custom-material velocities. Bound particle shadow work per map,
  cull individual casters, and scope local cache invalidation to selected sprites.
- Require GPU session/frame and authored-asset regressions in the lavapipe job,
  including collected interpolation and view/binding behavior.
- Document units, authored skinned component spelling and the accumulated 3D
  breaking changes in [the migration guide](3d_api_migration.md).

- Experimental 3D review: ordinary and shipping builds leave `KIN_EXPERIMENTAL_3D` off;
  the `3d` preset and CI opt in explicitly. Jolt follows kin's MSVC runtime selection.
  Renderer tests have a software-GPU timeout and reserve timing comparisons for hardware GPUs.
- Experimental 3D physics: deterministic contact payload ordering, compound child IDs through
  offset wrappers, waking neighbours after static-body edits, teleport interpolation resets,
  rollback motion types and temporary masses, and arbitrary convex-face debug drawing.
  Contact recording now defaults off; games consuming contact events must opt in.
- Experimental 3D rendering and scenes: transparent additive output is encoded, contact shadows
  respect sun strength, indirect cluster draws use the CPU fallback on D3D12, failed mesh-pool
  fence waits retain the fence, and authored environments preserve caller-owned picking.
  LOD and additional shadow settings can be authored. Collected scenes reuse two retained
  snapshots with threshold hysteresis; particles use the existing fog-colour table.
- Experimental 3D assets and simulation: equivalent skins share a palette and animation channels
  reach every skin that uses a joint; supported texture transforms are applied and incompatible
  transforms produce diagnostics. Root motion retains multiple loops, IK handles larger skeletons,
  particle catch-up is bounded, and the time-of-day clock accumulates in double precision.
- Experimental 3D renderer: `pick()` before the first render or after a resize read an
  uncleared target; TAA gave a re-made mesh the history of the mesh freed at the same address
  (keyed by serial now); readback fence waits were released unchecked (a failed wait keeps the
  fence and returns false); the occlusion cut warned every frame when its history was too small,
  and at all for a scene with nothing clustered.
- Experimental 3D lighting: spot, point and moon shadows biased by the normal-mapped normal
  (acne on bumpy surfaces; the sun used the geometric one, now all do); sky fog ignored
  `start`; fog over an Image sky takes the fixed colour as documented; the view output
  tonemapped premultiplied colour over partial alpha (dark fringes on HDR edges); TAA blended
  black where a history sample was NaN; the environment filter spread a NaN texel.
- Experimental 3D animation: root motion wrapped the wrong way at negative speed; a model with
  several skins drew only the first skin's parts (distinct unposed skins are diagnosed); an
  `Animator3D` kept pointing at a model that `ModelFile3D::poll` had replaced.
- Experimental 3D physics: Jolt's capacity flags (manifold cache, body pairs, contact
  constraints) were discarded: `World3D::step` returns a `StepReport3D`, logged once; the ECS
  bridge re-cooked a refused body every step (once now, until its components change) and had
  no way back from `restore_state` (`WorldPhysicsState3D::resync_from_world`); rope and ragdoll
  bodies could be destroyed or reshaped from outside (`owner_of`; refused); a kinematic body
  could not become dynamic; a rotated collider on an entity with a non-uniform scale is refused
  (as the asset cooker refuses it) instead of being made the wrong size; capped event lists
  kept the last events instead of the first; the ECS bridge, scene sessions and
  particles compile without FMA contraction so a step is the same on GCC and Clang.
- Experimental 3D: `SceneSession3D`'s error for a lifecycle request made while a replacement
  scene is prepared says the request is cancelled, not held.

- Rebind changed vertex storage buffers when consecutive 3D draws share a graphics pipeline. Multiple retained instance
  batches previously kept the first table bound, hiding or misplacing later geometry despite correct draw counters. Add
  streamed/retained RGBA and picking regression coverage for depth, shadows, AO/contact, MSAA and TAA, plus a mutation check.

- SDL_GPU: mip chains of textures that are not square had levels nobody wrote. SDL's mip generation stops at the level where the shorter side
  is 1, but kin counted levels by the longer side, so a mipmapped 1024 x 512 texture had a last 1 x 1 level holding whatever its memory held,
  read when the texture is drawn very small. Mipmapped textures (`ScaleMode::Mipmapped`) and 3D environments now have only the levels that
  are written (`GpuDevice::complete_mip_levels`); in 3D this was the intermittent green metal spheres with image-based light (the NaN in that level
  went into the environment's filtered copies), and the cause of 1 to 2 level differences between runs of the same picture.
- 3D: draws were ordered by their mesh's heap address, so two surfaces tied in depth (the kiln, the crowd under TAA) came out in a different order from
  one process to the next; the order is now the order the meshes were made. The reference scenes' "flaky" pictures are identical in every run.
- Debug: `KIN_POISON_TEXTURES=1|2` and `KIN_POISON_BUFFERS=1` fill new targets, environment textures, the levels of a mip chain to be generated and cluster
  buffers with NaN (or 3e4, or 0xFF bytes) before first use, so a read of unwritten memory shows every time; unset, they cost nothing per frame.

- SDL_GPU: a hidden or minimized window let the CPU run frames ahead of
  the GPU without bound (no swapchain texture, so nothing waited): GPU
  memory grew until the device was lost. Such frames now wait for the
  GPU when more than two are pending.

### Changed

- ui2 timing in seconds: `Context::now()` (the steady clock, or one given to
  `set_clock`, read at `begin()`), `TooltipOptions::delay`/`warm` (were
  `delay_frames`/`warm_frames`), `UiTextEditState::last_click_time` and
  `caret_time`, `State::register_click(id, pos, now)`.
  `Context::request_redraw(seconds)` keeps the soonest asked and `apply()`
  passes it on (`redraw_in()`): a focused editor asks for the frame its caret
  blinks, not every frame.
- ui2 allocates far less per frame (a studio frame: 6,600 heap allocations to
  1,500): `SurfaceStyle::shader_params` is a `shared_ptr<const ShaderParams>`
  (every surface allocated its 16 floats, and a `BoxStyle` holds one a state), the
  layout nodes' child lists are kept from frame to frame, and what
  `end_layout()` runs for each node is kept in reused blocks, not a
  `std::function` each.
- ui2 keys go only to the focused widget: `ListView`, `TreeView`, `IconGrid`,
  `TabBar`, `MenuBar` and `NodeGraph` take them while they have focus (a click
  or Tab gives it; `keys_without_focus` for the old way), `MenuList` and
  `DialogueView` also while nothing else has focus. The focus ring shows for
  the keys' focus only, not a click's (`Interaction::quiet_focus`,
  `State::focus_visible()`).
- `NumberInput` shows its value until typed in, commits on Enter or leaving,
  goes back on Esc, steps with Up and Down, and outlines what is not a number
  in range (`invalid`). Its range defaults to every float (was 0 to 1).
  `PropertyInspector`'s text rows commit on leaving and go back on Esc.
- `SvgOptions::oversample` defaults to 0: the renderer's content scale (was 1,
  blurred icons on high-density displays unless given).
- `TextStyle::inherit` and `WidgetStyle::inherit` (default true): false keeps a
  style as given, its default values too, instead of filling them from the
  theme. Widgets are measured in their themed styles, as they are drawn.
- `TooltipOptions` tooltips are drawn after layout, over everything.

- Separate mesh temporal `history_id` from picking `id`; assign generation- and
  pooled-lease-aware identities during ECS collection and distinct model-part IDs.
  Zero history IDs preserve direct callers' existing matching behavior.
- Deprecate the transform-mutating physics `interpolate()` API in favor of
  read-only session/frame presentation.
- Replace particle capture/restore booleans with `ParticleMotion3D` and
  `ParticleRestore3D` modes, preserving defaults and simulation bytes.
- Replace unused session bridge timestep settings with `physics_debug`; group
  standalone bridge debug controls in `config.debug`. See the API migration guide.

- Remove the unreachable `SceneSession3DReload::Failed` enumerator; failures use
  the existing `Result3D` diagnostics. `scene3d.entity` returns nil for missing IDs.

- Native physics result predicates/mutations and `StepReport3D` are
  `[[nodiscard]]`. Physics state hashing now always includes temporary mass,
  including zero; hashes must be regenerated with this engine build.
- Backend picking returns `Result3D<PickResult3D>` so readback failures remain
  failures through `Renderer3D::pick`; an empty successful result is a miss.

- `Status3D` is `[[nodiscard]]`; explicitly handle or deliberately discard results.
  `Result3D<bool>` has no bool conversion: check `.ok()` for success and inspect
  its payload for change/readback availability.

- **Breaking 3D:** Fallible rendering/loading/binding/session/frame operations use `Result3D<T>`
  and `Status3D`, carrying diagnostics by value. CPU model/navmesh builders
  return their payload; mesh/skeleton validators no longer take error pointers.
- Construct retained batches with `InstanceBatch3D::create` /
  `ModelBatch3D::create`. Model-file polling returns `Result3D<bool>` and performs
  the initial load explicitly; watcher construction does no I/O.
- Local lights share flat `shadow_*` fields in native code, Lua and authored
  data. Replace `shadows` with `shadow_enabled`, `shadow_near` with
  `shadow_near_plane`, and C++ `light.shadow.*` with `light.shadow_*`.
  Immediate point submission is named `add_point_light`, matching
  `add_spot_light`. Reference fixture JSON follows the same fields.


- 3D is disabled by default and explicitly enabled by the `3d` preset and 3D CI.
  Contact collection defaults off. RGB lighting/sky/fog/weather/particle APIs use
  `LinearRgb`; ground ambient uses an enabled/color record. Resource load failures
  travel in returned results instead of output strings. See the migration guide for
  C++ type/header renames, shader bindings, supported-model acceptance and the
  navigation clearance behavior change.

- Give experimental 3D public types and headers consistent 3D suffixes, including Mesh3D/Model3D, ViewOptions3D and RenderStats3D. Light inputs use explicit LinearRgb channels with the existing sRGB conversion helpers; scene/prefab RGB triples keep their linear/HDR meaning. Add a 2D build preset that excludes 3D shaders, and a shared clang-format policy with a check for rendering/authoring sources. See docs/3d_api_migration.md.

- Make normal ECS collection retain large opaque/masked mesh/model buckets automatically, with changed-run updates,
  conservative range bounds, snapshot isolation and bounded CPU records. Reuse renderer layouts after exact structural
  validation of clear/resubmission into the same scene. Preserve feature quality and GPU/workload/budget fallbacks.

- Retire the renderer's implementation A/B environment controls and remove their
  obsolete paths, including point-light tile-mask and cutout-depth experiments.
  Preserve automatic capability/workload fallbacks and bounded caches. GPU pass
  labels now use `Scene3D::debug.gpu_pass_markers`; profiling samples expose
  `--gpu-pass-markers`. Remove `kin_reference --ab` and stale experiment counters.


- Experimental 3D physics: `World3D::save_state` blobs carry their length and a checksum, and
  `restore_state` checks the whole blob before touching the world (a short or corrupt blob
  changes nothing and returns false); blobs saved before this are refused. Kinematic bodies
  report their mass, so they can become dynamic later with `set_motion`, and `state_hash` of a
  world with kinematic bodies changes.
- Experimental 3D models: the glTF reader refuses what it cannot read correctly instead of
  loading it silently wrong: Draco/meshopt-compressed and sparse accessors (they read as zeros),
  absolute paths and `..` that leaves the content root or the mounted pack, nodes nested over
  512 deep. `data:` images are decoded. A plain node between two of a skin's joints is kept as
  a joint instead of breaking the chain.
- ECS: `MeshRenderer3D` entities have a pick id (the entity id, or `MeshRenderer3D::id`), so
  `Renderer3D::pick` names them; it returned 0 for every one.

- Experimental 3D performance round (measured before/after, interleaved, medians):
  - Retained instance tables are keyed by batch and material colour (two views drawing one
    batch in two colours no longer re-upload the table every frame), release expired tables
    without a device-wide wait, evict least-recently-used tables when the 128-table / 32 MiB
    budget is full (one upgrade per frame, so the largest batches win and nothing thrashes),
    and upload only the edited records of a batch (grove with 2,000 moving instances: 256 KB
    instead of 1.28 MB a frame, frame time -11%). New `RenderDebugStats3D` counters
    `retained_evictions`, `retained_refused`, `device_waits`.
  - The adaptive upload staging buffer grows after a batch it had to split, up to 64 MiB
    (streaming 17 MiB a frame: one submission instead of 1.8, fence time -25%).
  - The local shadow array is sized by the lights that get a map this render, not by every
    light with shadows enabled, and shrinks after 120 renders.
  - Fog coloured by the procedural sky reads a 48-row elevation table built on the CPU when
    the sky changes instead of integrating the atmosphere per fragment (fogged cameras
    10-37% less GPU time at 1280x720; pictures move by at most one level; fixed-colour fog
    is byte-identical).
  - `InstanceBatch3D::update` grows group bounds incrementally and re-measures a group only
    when a bounding copy moved inward (2,000 scattered single-copy edits: 354 ms to 0.24 ms a
    frame); past 16 edits `changed_since` is rounded to 16-copy blocks instead of the whole
    batch, and `changed_ranges_since` lists the edited ranges.
  - Particle wind is evaluated per 1/120 s step with the sines' time phases precomputed and
    a vectorised polynomial sine (20k particles with gusts and turbulence: 16 ms to 0.57 ms
    per update; the wind field agrees with `Wind3D::velocity` to 1.4e-5 m/s).
  - The ECS physics bridge re-reads its bound set only when flecs reports a structural change
    (or a rebuild, refusal, motion change or lost body asks for it), stamps static and
    kinematic transforms, and reuses event vectors when publishing singletons (500 resting bodies:
    0.085 ms to 0.038 ms a step of bridge overhead). `SceneSession3D::step` skips the editor
    hook's transform propagation and particle-budget check when no later-phase system ran.
  - Animation blends in place into the player's scratch poses and caches the animator's
    state/layer keys (five times fewer allocations in the skeleton tests). `World3D::shape`
    returns the stored handle without allocating; Summit turns `contact_events` off.
  - Clustered meshes release their welded CPU vertices after upload (`MeshClusterBuild3D`
    holds them only while a mesh is made; `MeshClusters3D` is the hierarchy alone), and flat
    unindexed meshes' LOD levels index shared zero-normal vertices instead of three per
    triangle per level (a 2 M-triangle soup with 4 levels: -168 MB resident, -325 MB peak;
    the levels then ignore a normal map, as flat clusters do). Benchmarks
    `kin_render3d_bench` (batch edits, wind) and `kin_physics3d_bench ecs` are added.

### Added

- ui2 boxes with tooltips, drags and drops, drawn in an overlay pass after
  layout (`Context::overlay`): `BoxStyle::tooltip(text)`,
  `draggable(type, text, value)` and `accepts_drop(type)` (`BoxResult::drag_started`,
  `dragging`, `drop_hovered`, `dropped`, the payload in `drag_payload()`).
  `Context::popup(id, style, PopupPlace)` is a box that opens under an anchor
  or at a point (`open_popup_at`), closes on a press outside it and its opener,
  or Esc while on top (`popup_on_top`). `set_tooltip_style` sets every
  tooltip's look.
- ui2 value fields: `Context::value_field(id, shown, options, valid)` shows a
  value until it is typed in, commits on Enter or leaving
  (`ValueFieldResult::committed`), goes back to the value on Esc (a `live`
  field commits the value from before the typing again), and outlines what
  does not validate; `number_field` for numbers in a range (`number_text`,
  `parse_number`), `edit_value(id)` to start typing in one (a rename),
  `stop_value_fields()`.
- ui2 focus traps: `BoxStyle::focus_trap()` keeps Tab and the focus inside a
  dialog while it is open.
- ui2 scrolling: `Context::scroll_into_view(id)` (a box, or a widget by the box
  around it), `ScrollBoxOptions::stick_to_end` (a log follows its last lines
  while it is at its end), PageUp, PageDown, Home and End in the scroll box
  the focus is in, else the one under the pointer.
- ui2 ids from text and paths: `make_id(label, part)` for a part named by text
  or a number, `make_id(parent, path)` (`make_path_id`) for a file's.
- `kin/ui2/composed.hpp`: widgets built from boxes, in the look of
  `ComposedLook` (the theme's colours by default): `menu_bar` (Alt and a
  title's letter, the arrows, submenus, keys shown), `pick_list` (a searchable
  list under an anchor; with `free_text`, Enter takes what is typed when
  nothing matches) and
  `breadcrumbs`.
- `Context::measured(widget)`, a widget's size in its themed styles;
  `theme_styles(theme, widget)` gives a widget its theme's styles where it
  inherits them; `layout_root()`, `typing()`.
- ui2 text: Left, Right, Backspace and Delete step over whole graphemes
  (`utf8_next_grapheme`, `utf8_prev_grapheme`: combining marks, emoji with
  modifiers and joiners, flags, CR LF). Segoe UI Emoji is a fallback font on
  Windows. A full bitmap glyph atlas starts over with the text being drawn
  instead of leaving its new glyphs out; `TtfFontOptions::atlas_pages` caps
  its pages.

- Tested `kin-gltf/1` export profile with CPU/GPU asset acceptance regressions.
  Reject distinct skin palettes, oversized/unsupported skins, morphs, mixed poses
  and incompatible UV placements as whole assets; preserve last-good reloads.
  Navmesh construction now applies agent height and radius with conservative
  whole-triangle headroom and boundary clearance, including slope margins.

- Lua gameplay in `SceneSession3D`: transactional script loading/module reloads,
  shared ECS queries/systems, input/app services, checked 3D raycasts/impulses and
  deferred prefab spawn/despawn. Lua shares native phases and command cancellation.
  Add `lua_range`, CPU/GPU/content-pack regressions and lavapipe CI coverage.
  Existing 2D `ScriptScene` keeps its callback order.

- Separate the compact 20-field `RenderStats3D` game summary from explicit
  `RenderDebugStats3D` implementation instrumentation via `debug_stats()`.
  Move forced LOD, CPU/GPU cluster comparison, blocking readback and frozen TAA
  jitter into `Scene3D::debug`, outside visual quality settings. Mark partial
  triangle counts without GPU readback; keep default diagnostics lightweight
  and provide a separate detailed debug sink. Migrate benchmark/report callers.

- Common 3D runtime diagnostics on renderer, asset loading, scene submission and batches. Invalid draw/pose/material
  submissions and batch edits report failure without throwing; scenes retain only their first submission error and
  reject rendering before GPU work. Additive 3D binding reports through its scene runtime. Returned results own their diagnostics; owner accessors retain inspection snapshots.
  Keep the throwing material-binding builder internal; public material loading uses `Assets3D::load_materials`.

- Non-owning `SceneFrame3D` over an existing ECS/render state and optional physics bridge or raw simulation callback;
  the owning scene session reuses its native phase stages and collection. Additive 3D instantiation reuses
  `SceneAssetRuntime`, binds only the new instance and rolls it back on binding failure. Runtime scene contexts
  optionally expose one shared `Renderer3D` in normal, headless and command-server runs; Camp, Projectile Range
  and Summit reuse it. 2D-only builds reject the explicit 3D opt-in.

- Rigid node animation for glTF models: animations of plain nodes (doors, fans, crane arms) play through a
  per-model node skeleton (`Model3D::node_skeleton`, `ModelPart3D::node`/`node_offset`), with
  `Scene3D::draw(model, transform, node_matrices)`, `Model3D::part_transform`, a `draw_skinned` overload that
  poses a skin and animated props together, and an `Animator3D` beside a `ModelRenderer3D` driving clips,
  blends, layers, actions, events and IK on those nodes. Models without node animation load and draw unchanged.
- `SceneSession3D` runs headless over a renderer without 3D: loading, spawning, pools, edits, physics,
  animation, particles and queries work on CPU-only `Assets3D` models (parts, skeletons, clips, nodes and
  bounds kept, empty mesh handles); `render()` and `pick()` report "no renderer". `kin_scene_session3d_tests`
  now runs its headless checks without a GPU.
- `SceneSession3D::pick(pixel)` maps the last rendered frame's pick to the live entity (by entity index or a
  renderer's own `id`), and `options.view.pick` survives an authored `SceneEnvironment3D`.
- 3D audio in the ECS: `AudioEmitter` and `AudioOneShot` are placed by `WorldTransform3D` (with `offset_3d`),
  the listener comes from the `AudioListener` entity or the active `SceneCamera3D`, and `SceneSession3D` runs
  the audio systems in its Audio phase when `SceneSession3DOptions::audio` is set.
- `World3D::add_character_impulse` and `set_character_velocity`: wind, knock-backs and launch pads push the
  capsule, braked by the controller on the ground, carried in the air, launching it when sent upward.
- Live body settings: `World3D::set_mass`, `set_gravity_factor`, `set_linear_damping`, `set_angular_damping`,
  `set_friction` and `set_restitution` with getters, refusing owned (character, ragdoll, rope) bodies and waking
  what a change can unsettle. They are setup, not part of `state_hash`/`save_state`.
- Grab and climb finds a ledge whose top is a neighbouring body when the held face ends there (not across a gap,
  under a parapet or below a roof). Added `World3D::shape_cast_all` (vector and bounded forms) and opt-in
  `World3DConfig::activation_events` (bodies falling asleep and waking, as events).
- Navigation meshes: `kin/pathfinding/navmesh3d.hpp` builds a triangle navmesh from a soup or a `MeshData3D`
  (welding, slope limit, edge adjacency, regions) with `nearest`, `find_path` (A* corridor plus funnel string
  pulling, within about 1% of the shortest path), `raycast`, `region_of` and `random_point`; deterministic,
  CPU only, in the 2D-only build too. `kin_navmesh3d_tests` checks optimality against a visibility graph.
- `kin::BakeCache` (kin/assets/bake_cache.hpp): an opt-in folder of baked results keyed by a hash of their
  inputs, with atomic writes, checksummed entries and least-recently-used eviction. `Renderer3D::set_bake_cache`
  restores cluster hierarchies and level-of-detail chains byte-identical instead of rebuilding them;
  `PhysicsAssets3D::set_bake_cache` restores cooked hull and triangle-mesh colliders through the new
  `Shape3D::save_binary`/`restore_binary`. `kin_reference --bake-cache DIR` uses it.
- GPU-compressed textures: KTX2 and DDS readers (kin/assets/texture_file.hpp) for BC1/3/4/5/7 and ASTC 4x4,
  refusing supercompressed, cube, array, 3D and malformed files with a reason; `Renderer2D::create_texture_compressed`
  uploads the file's own mip chain (SDL_GPU), `Assets3D::load_texture` reads `.ktx2`/`.dds`, and glTF images in
  those formats are uploaded compressed and shared like any other texture.
- Native 3D interaction queries with caller-bounded discovery, checked target/verb
  locks, current contact revalidation and shared reach/visibility rules. Camp
  pickups and crate pushing reuse the API with native keyboard/gamepad prompts.

- One-shot 3D character actions with reusable masks, playback tokens, cancellation/completion, scoped markers and delayed checked Camp pickups.
- Timed 3D animation markers in existing animator assets, bounded per-step events, shared event handlers, and Camp footsteps/dust.
- Experimental 3D: checked-target joint attachments on prefab roots, with cached
  joint lookup, final animation/IK/render placement, child mesh/light placement,
  explicit failure states and bounded preparation. Clear bindings on pooled reuse;
  demonstrate a held lantern in Camp using shared bundled assets.

- Experimental 3D: native checked-target follow/orbit `CameraRig3D`, shared
  `SpringArm3D` collision/recovery, independent gameplay/render views and
  `RenderTransform3D` presentation placement. Camp follows its player; Summit
  reuses the arm solver. No editor or simulation-pose mutation is required.
  Add opt-in outward initial-overlap reporting for camera sweeps, preserving
  legacy gameplay sweep behavior.

- Add weak scene-generation physics queries with checked entity/native-body resolution, entity/layer/sensor filters,
  closest ray/sweep and caller-bounded multi-ray/overlap results. Keep unmapped native obstacles as hits, reject stale
  leases/rebuilt bodies, and collect directly into caller storage with explicit overflow. Use visible highlighted
  supplies and occlusion-aware collection/pushing in Camp; add allocation, identity, filtering and gameplay tests.

- Move Camp player movement, facing, camera orbit, interaction and collection into ordinary C++ ECS systems.
  Feed a fixed-size per-step command from existing named input actions, gamepad snapshots or scripts; reuse
  controller physics and deferred prefab teardown, acknowledge successful collection, and retain explicit
  host scene replacement. Add mapper and shared-route gameplay/reset/failure tests.

- Add full-frame ECS host phase hooks with whole-graph preflight, empty-phase callbacks,
  unchanged rate/interval accounting and fail-stop boundaries. Run SceneSession3D native physics
  before PostPhysics and animation/particles after Animation, each once per step. Commit
  contact-driven removals before animation; move projectile_range contacts to PostPhysics.

- Add explicit SceneSession3D prefab preloading and fixed-capacity ECS/asset pools over the shared loaders and bindings.
  Reset authored animation/particle/render/physics state, remap internal refs, renew checked lease identities and expose
  capacity/reuse counters. Keep idle slots out of simulation while counting retained storage against budgets. Use the
  pools in projectile_range and add bounded lifecycle/latency/memory checks, including packed content and source removal.
- Add generation-scoped SceneCommands3D endpoints over the existing ECS command buffer, bounded deferred prefab
  spawn/despawn requests and checked result tickets. Execute at scene phase boundaries, cancel pending work on failed
  systems/discard/replacement, and demonstrate a complete C++ projectile/contact-damage/destruction/respawn loop.
- Add C++ SceneSession3D runtime prefab spawning/despawning with generation-checked session handles, bounded ownership,
  selected asset/physics initialization, constructor rollback and typed configuration over existing live validation.
  Native gameplay components need no inspector schema. Demonstrate supplies, collection, pushing and level reset in camp.
  Release unused mesh/material collection buckets so despawned objects do not pin assets.
- Add bounded transactional SceneSession3D field batches through the existing component registry/inspector path.
  Rebind affected assets, validate particles/settings and replace ordinary rigid bodies before releasing old bodies;
  preserve compatible runtime state, reject failed edits and extend dependency watches. Add live camp editing controls.
- Compose prefab files with the shared scene reference/override format, bounded recursive resolution and namespaced
  local IDs. Add structural instance overrides and linked save diffs for child/component additions/removals, names and
  internal parents; add explicit child adoption/component-removal helpers. Preserve referenced compiler generations
  with a bounded world-local cache, propagate nested source watches and reuse the default asset loader/content packs.
  Split the campfire effect into a reusable nested prefab and exercise a smaller-smoke structural variant in camp_walk.
- Author existing 3D particle emitters through shared scene/prefab fields, including referenced textures, seeded ranges,
  forces, lifetime curves and sprite settings. Reuse Assets3D texture sharing and transactional dependency reloads;
  isolate live state per prefab instance and validate per-emitter/scene capacity budgets before stock simulation/drawing.
  Add a reusable campfire prefab with flames, smoke, sparks and a point light to both authored camp scenes.
- Add CPU-only bounded `.kincollider` recipes for shared compound, convex hull and mesh collision through the existing
  scene/prefab registry and native shape builders. Expose character-controller/animation-link settings and body layers;
  isolate prefab character simulation state. Integrate collision dependency watching and transactional cooking into
  physics-enabled SceneSession3D. Add `camp_walk`, a playable authored camp using existing free assets.
- Add authored 3D material patches, named variants and per-part selectors through `ModelMaterials3D`, the existing
  material/texture APIs and shared scene/prefab fields. Share weakly cached bindings and role/filter-aware textures without
  cloning geometry; validate explicit rebinding and coordinate material/texture hot reload with SceneSession3D.
  Preserve canonical authored IDs when exporting runtime entities with linked prefab instances.
- Add renderer-thread `SceneSession3D` ownership over the shared scene/prefab loaders, assets, ECS rendering and optional
  physics, with staged activation, transactional replacement, explicit dependency reloads, configurable authoring budgets
  and weak asset invalidation. Include a complete authored camp using Summit's existing free assets. Fix dynamic-body
  writeback for inherited prefab transforms and check animator source sizes before reading.
- Preserve scene-loaded prefab links and minimal field overrides in checked world saves, with shared CPU baselines,
  header/reference/parent round trips, explicit unpacking and diagnostics for unsupported structural edits. Roll back failed
  scene instantiation. Add authored camera/environment/time-of-day components and explicit validated scene application.
  Add referenced 3D animation configurations using the existing `.kinanim` graph parser, with blend spaces, layers, fades,
  named IK, model compatibility checks and failure-safe explicit rebinding. Move Summit's locomotion and IK setup into content.
- Extend the shared ECS component registry and scene/prefab utilities with finite `Vec3f`, unit `Quatf`, native asset refs
  and reflected property adapters. Register authored 3D transforms, static/skinned model references, base animator settings,
  local lights and primitive physics. Add explicit renderer-thread asset binding with contextual errors, safe prefab
  inheritance and weak resource ownership. Summit's survivor now uses an ordinary `.kinprefab`.
- Add renderer-scoped `Assets3D` model handles with weak, bounded lookup caches and automatic sharing of external/embedded
  texture dependencies, preserving colour space and filtering. Add `Model3D::placement`, whole-object model IDs and persistent
  `ModelBatch3D` part expansion/spatial grouping/material replacement. Migrate Summit's prop and survivor loading and scenery
  submission onto these APIs, removing its manual atlas replacement and mesh-part batching. Cover ownership, invalidation,
  content packs, dependency identity and GPU image parity.
- Add a denser Summit forest approach using shared meshes, cell-grouped immutable instance batches and matching rock/trunk
  collision. Keep the sparse layout selectable with `--scenery compact`, identify layouts in snapshots and command logs, and
  cover both routes across three seeds. Add a disk-streamed full-frame traversal benchmark and a serial, RAM-bounded comparison
  harness with replay validation and contention checks. Handle disappearing processes during benchmark RSS monitoring.
- Dress Summit with selected CC0 Kenney props and a fitted survivor carrying authored idle/run/jump animation, plus two
  1K ambientCG PBR materials. Share meshes, the prop atlas and packed AO/roughness textures; include pinned preparation tools,
  licenses and manifests. Add 31 expedition prop colliders from shared placements, retaining hollow tents/fire pits and simple
  trunk proxies; preserve classic-course physics and reject older expedition snapshots. Validate skin/rig/collider compatibility.
- Initialise Jolt before allocating mesh/hull input arrays when those shape factories are the first physics call, fixing a
  crash exposed by CPU asset validation. Add fresh-process mesh-first and hull-first regression checks.

- Remeasure the Medium industrial yard against Godot 4.7.2 Forward+ with Kin point-light masks enabled: four guarded
  engine pairs at 720p/1440p, checked shared recipe/features/motion/skin/shadow work and continuous contention telemetry.
  Record frame p50/p95, RAM peaks and native shading/script limitations; no renderer or quality changes.

- Add opt-in conservative point-light tile masks (`KIN_POINT_TILES=1`) for stock 3D materials. Pack 1,024
  16-light masks into an extra 2,064 bytes of frame uniforms, with no new GPU buffer or persistent cache.
  Preserve light order, fog/local-shadow/velocity variants and fallback paths; add coverage/cost counters,
  randomized bounds checks, full-resolution image parity and guarded yard measurements.
  Clean paired yard runs reduce median 1440p p95 from 7.90 to 7.38 ms; retain opt-in status because the 720p control
  shows no clear gain. Record process isolation, GPU pass attribution, clock variation and bounded-memory evidence.

- Add fixed-range `Scene3D::update_joint_matrices` for animating submitted poses without rebuilding mesh commands.
  Allow bounded prepared-layout reuse for skinned commands while refreshing poses, bounds, shadow membership and
  current/previous GPU joint tables. Industrial yard retains commands by default, with `--rebuild-scene` as a reference.

- Skip point-light BRDF work when the existing windowed attenuation is zero, before normalization and shadow lookup.
  Preserve every positive contribution without an epsilon cutoff; add ordinary/local-light radius-boundary and inactive
  light ordering coverage. Add no buffers or caches, and record image/work parity plus repeated industrial-yard p95 timings.

- Add native industrial-yard profiling controls for short/visible GPU captures, point-light limits and shadow reception.
  Attribute the wide-view p95 region to main-pass lighting and GPU backpressure, with bounded captures, default-image
  checks and runtime/pass timing evidence; leave renderer/shader behavior unchanged.

- Add a bounded industrial-yard rendering benchmark shared with Godot Forward+: independent PBR props, distributed
  instance edits, real six-joint robots, glass/smoke and overlapping perspective spot shadows. Include deterministic
  binary recipes, serial RAM guards, motion/skin validation and independent feature controls; retain the Grove benchmark.

- Add opt-in `KIN_RENDERER3D_O3` for an isolated GCC/Clang Release compiler experiment on the GPU 3D renderer. Keep O2
  as the default after image/work parity checks and small, mixed O2/O3 timing results; archive the comparison and flags.

- Reduce 3D culling CPU work with cached-sphere camera classification and active LOD/cascade counter clearing, without
  new per-copy storage or GPU resources. Keep original scalar decisions, source order and shadow selection, with reference
  controls and image/picking tests across motion, worker rendering, cascades, TAA, HDR and MSAA transitions.

- Reuse prepared 3D draw/task layouts across fixed-size instance-batch edits, with an eight-byte batch layout generation
  separate from content revisions. Group validity changes invalidate ranges; camera/light visibility, bounds and uploads
  still refresh. `KIN_PREPARED_BATCH_EDITS=0` keeps the prior invalidation rule, with shared-edit/validity/picking coverage.

- Retained 3D batches refresh only edited CPU sphere ranges using a bounded 16-edit history (128 bytes/batch), with full
  refresh for new/old readers. Large instance tables pack directly into staging on existing workers while retaining safe
  full-table GPU cycling. Add reference switches and a native moving-instance benchmark; preserve image/picking behavior.

- Reuse bounded 3D renderer draw/piece/task layout for unchanged scene commands, with scene identity/revision and exact
  instance-batch layout revision checks. Refresh visibility, lighting, shadow membership and table bindings every render; keep
  the reference path for LOD, command edits and oversized layouts. Add diagnostics and `KIN_PREPARED_SCENES=0` for A/B runs,
  with image/picking tests for edits, moves, overlays, TAA, worker rendering and cache-budget fallback.

- Fix Blockfield's Godot positional-shadow atlas allocation: omni shadows need adjacent slots and were silently omitted
  by single-slot quadrants. Export actual shadow draw/triangle counts and atlas dimensions, and validate a point-shadow-only
  lighting case. Record a fresh Kin/Forward+ CPU/GPU profile and mark earlier local-shadow comparisons as incomplete.

- Opt-in `KIN_MASKED_DEPTH_PREPASS=1` experiments with stock lit cutouts in the existing main depth target. Shading loads
  that depth and uses read-only depth tests for prepassed draws. No new render target/per-instance cache; separate counters,
  profiler labels and warm-up cover the additional pass. Unsupported views/draws retain their paths; the default stays off.

- Optional `KIN_GPU_PASS_MARKERS=1` SDL debug groups name 3D GPU passes for external hardware profilers without fence
  scopes, submission splits or new GPU allocations. Blockfield comparison `--visible` provides an uncapped, exactly
  bounded visible capture with its normal warm-up/route, allowing Nsight Systems to observe frame boundaries.

- Blockfield's render-comparison harness retains static `Scene3D` commands between frames, updating camera and lights
  through the existing API. `--rebuild-scene` restores the former per-frame submission workload for paired measurements.

- Camera-inside instance cells classify eligible single-level ranges together and pack fully visible pieces without
  per-copy visibility branches. Cascade/LOD/skin and local-bound refinement, shadow selection and source order remain
  intact. `KIN_CAMERA_RANGES=0` restores the prior loops; no per-instance storage is added.

- Local-shadow gathering tests four cached/scratch spheres together on supported SSE2 builds, prepares plane/radius
  constants on the worker stack and classifies single-bucket survivors in batches. Scalar small ranges/tails, moon reach,
  LOD/cluster buckets and cache ordering remain intact. `KIN_LOCAL_SHADOW_SIMD=0` restores scalar tests; no per-instance
  cache or larger task records are added, with scalar fallback on other build targets.

- Directional shadow reach prepares sun/moon exit faces once, delays swept endpoints until needed, reuses them across sun
  cascades and skips camera/slice planes proven inside for a spatial cell. Offscreen casters and boundary arithmetic are
  preserved; masks add eight bytes per cell while compact local tasks remain 32 bytes. `KIN_SHADOW_REACH=0` restores eager tests.

- GPU shared upload staging adapts to recent aggregate batch occupancy, shrinking below the previous 16 MiB floor after
  quiet windows and regrowing for bursts. Capacity/allocation counters and `KIN_ADAPTIVE_STAGING=0` support comparisons.
  This reduces steady resident upload memory for the Grove workload without changing draw/caster selection or shaders.

- Local-shadow preparation compacts single-bucket task counters, reuses accepted indices for the upload list after cache/cluster
  decisions, and keeps caster-union slot storage only for streamed instances. Multi-level LOD, clustered and cached maps retain
  deterministic copy ordering. `KIN_LOCAL_SHADOW_PACKING=0` restores prior packing; diagnostics expose CPU payload capacities.

- Local-shadow cell culling carries unresolved plane masks and strict light-radius containment proofs into gathering.
  Fully accepted single-level ranges fill accepted indices without per-copy sphere tests or classification-byte storage;
  partial ranges test only unresolved planes. Moon reach tests, LOD/cluster refinement and offscreen casters are preserved.
  `KIN_LOCAL_SHADOW_CULLING=0` selects the previous refinement; new counters distinguish candidates, refinement and scratch.

- Resident 3D instance batches cache transformed CPU culling spheres within a 4 MiB limit tied to the retained GPU-table
  budget. Main and shadow views reuse them; edits refresh the cache independently of GPU upload fallbacks. Uncached local
  casters use compact transient storage, with oversized old storage released when caching starts. Diagnostics expose cache,
  scratch and rebuild counts; `KIN_CACHED_INSTANCE_BOUNDS=0` restores sphere transforms for comparison.

- `InstanceBatch3D` keeps fixed-size instance records behind shared scene handles and supports explicit range edits with
  refreshed cell bounds. A bounded GPU cache reuses opaque/masked records across camera and shadow views, uploading compact
  visibility lists instead of repacking unchanged copies. Capacity/feature conflicts use the existing path. Grove retains
  foliage; upload-byte counters and switches support paired measurements with the original CPU/GPU paths.

- Plain 3D instances can retain spatial cells through draw merging with `draw_instanced_group` and cached `instance_bounds`.
  Camera and shadow views reject whole cells or refine intersecting ones, preserving caster selection, draw counts, alpha
  sorting and picking. Stats report avoided tests; `KIN_SPATIAL_CULLING=0` selects the reference path. The grove uses its
  existing 64 foliage / eight mist cells without new geometry, textures or per-instance caches.
- Kin's local-shadow jobs now batch candidate ranges across small draws, reuse current-render world bounds from the main
  cull, and pack caster records directly into transfer memory. This removes the full CPU caster-record mirror while retaining
  deterministic caster lists and existing shaders. GPU tests cover 9,000 separate draws, mirrored/nonuniform transforms,
  local shadows without sun/camera culling, and serial/parallel image equivalence; paired grove timings are in the 3D plan.
- Blockfield's native rendering benchmark has optional Linux thread/process CPU clocks and a bounded `--workers 0..4`
  experiment control. Grove profiling records shadow preparation, static-instance upload traffic and worker overhead;
  the renderer plan includes measured priorities and memory limits. Core rendering behavior and default worker policy are unchanged.
- Blockfield's shared Kin/Godot benchmark has a `grove` preset with a ground-level camera tour, independently scaled
  alpha-cutout foliage and layered transparency, a bounded shared scene recipe, interactive Kin preview, density sweeps,
  per-frame recipe/work counters and p99 summaries. Geometry and small textures stay shared; existing lighting outputs are preserved.
- Kin's local-shadow preparation uses flat caster lists with disjoint worker ranges, removing per-view/per-draw copy-vector
  allocations. Indexed draws select shared meshes through page offsets, so the main, shadow, depth, velocity and picking passes
  retain unchanged buffer bindings. `KIN_MESH_PAGE_BINDINGS=0` provides a diagnostic comparison. GPU tests cover page reuse and
  serial/parallel shadow gathering across instance-piece boundaries; paired rich-lighting benchmarks retain identical pixels.
- Blockfield has playable `--quality pbr|high` presets with shared PBR maps, additional point/spot lights, HDR, MSAA/TAA, local
  shadows and AO/contact shadows. The comparison runner's `--lighting` sweep measures these features separately in Kin and
  supported Godot renderers, with small procedural maps/IBL, shared geometry, strict work counters, close-up views and a 2×
  render-resolution sampling test. Input-driven smoke tests exercise each playable preset and verify its GPU features.
- Kin's unlit main pass now selects colour-only or base-textured shaders, with separate cut-out variants, removing unused vertex
  work and lighting bindings. Colour conversion, emission, blending, HDR and MSAA are preserved; fog, skinning, TAA and custom
  shaders retain the general path. Warm-up selects these pipelines too. `RenderStats3D` reports specialized draws, and
  `KIN_UNLIT_SHADERS=0` permits same-binary comparisons. Full-frame GPU equivalence tests cover the variants and resource switches.
- Blockfield's Kin/Godot rendering comparison now includes a RAM-bounded 1×/5×/10×/20×/100× triangle sweep. Disjoint terrain
  tiles share mesh buffers and use instanced draws; a full-field camera and exact per-frame counters ensure both engines submit
  the same triangle workload. Separate stress results retain RAM limits and distinguish geometry throughput from draw-call,
  unique-geometry and shader/pixel complexity.
- A bounded Kin/Godot release rendering comparison for Blockfield: a shared chunk-mesh snapshot, matching camera/material/view
  settings, Mobile and Forward+ Vulkan runs, sequential repeats, CSV metrics, screenshots and RAM limits. Geometry and Godot
  assets live on the Linux disk; no voxel addon, duplicate terrain implementation or source build is required. Reports distinguish
  whole render-loop intervals from the engines' different CPU/GPU timer boundaries.
- Kin's SDL_GPU renderer pools small static meshes in shared vertex/index buffer pages, preserving independent mesh bounds,
  draw order and edits. Freed ranges wait for GPU fences before reuse; empty pages are released, retired geometry has backpressure,
  and oversized, skinned and clustered meshes retain dedicated buffers. `KIN_SHARED_MESH_BUFFERS=0` selects the original path
  for comparisons. Render stats expose pool reservation, live/retired bytes, page counts and range reuse; Blockfield reports them.
- Blockfield wider-view controls: vertical camera FOV 45–150 degrees (`--fov`, PgUp/PgDn), view distances up to 32 columns,
  and a new default of distance 8 / FOV 90. Benchmarks without explicit frame limits include at least 900 frames after warm-up;
  scene reports and the HUD expose the view settings. Sequential distance/FOV sweeps keep the mesh budget bounded.
- Blockfield (`examples/blockfield`), a creative voxel sandbox on the experimental 3D backend: seeded hills, trees and caves,
  first-person walking/flight, block picking/mining/placement, a hotbar and saved edits. Columns use greedy meshing and matching
  Jolt colliders near the player, one bounded background meshing job, distance-based eviction and a resident mesh budget. Its
  benchmark renders every frame while streaming and modifying terrain, exports bounded timing summaries and optional CSV metrics,
  and exposes counts and memory bounds through scene reports. CPU tests cover meshing/seams, editing, saves, picking and collision.
- Summit's interactive default is now **The Beacon**, a compact expedition with an orange direct climbing route and a blue cave
  passage that reconnect at camp, a collidable shelter with health/stamina recovery and quieter wind, objective/trail guidance,
  a sustained beacon interaction, extraction, briefing and completion screens, and retry. `--classic` and the original climb/fall/rope
  scripts retain the procedural course. Expedition progression participates in snapshots, hashing and map-aware replay; new CPU
  tests cover both routes, interaction interruption and save/restore, with a full GPU screenshot run.
- Summit, a one-player climbing game on the experimental 3D renderer and physics (`examples/summit`; see the plan's "P4" and the
  examples' README): a procedural mountain from a seed (pieces that make both the colliders and the render meshes, a route a bot
  follows and a stamina planner checks), a player model (stamina, grab and climb, falls, a knock-out with a ragdoll and a get-up,
  respawn at camps, injury), input as commands (`PlayerCommand`, an input mapper with rebinding, dead zones and a gamepad), a
  simulation that advances only by a fixed step of commands with `state_hash()`, `save()` and `restore()` (replay, save and restore,
  frame-rate independence and step purity are tested), a spring-arm camera, a scripted run (`summit --script climb`), and a fixed look
  with the sky, fog and weather of P3. Engine: `kin/platform/gamepad.hpp` (`Gamepads`, `GamepadState`: a polled controller, opt-in;
  `Input` is unchanged). A character's own movement no longer collides with a body set to collide with nothing (`set_no_collisions`,
  `Body3DDesc::ignore_all`: a parked ragdoll's bodies, a disabled character), which they did before.
- Summit's presentation (`examples/summit`, the plan's "P4b"): a ui2 HUD (stamina bar that changes colour, the two hands, health and
  injury, altitude and best, a fall warning, camp and summit messages) built from a plain `HudModel` and laid out by ui2's solver,
  a pause menu with keyboard, pad and mouse and a controls screen, sound synthesised at start-up through `kin::AudioEngine` (wind by
  altitude and gusts, footsteps by ground, grabs, breath, falls, rope, fires; a testable `SoundCue` list from the game's events),
  an environment that changes with altitude and the clock (fog, haze, a snow line, camp fires and lights at dusk, dust and breath
  puffs), a look pass (chamfered, roughened rock with strata, moss and snow and a tiling normal map, buttresses and scree, ground and
  ranges, a climber with clothes and a pack, landing crouch, arms straight to the holds, a camera that frames a lying body), and a
  settings file (look, camera, volumes, bindings). No engine change; the simulation's hashes are unchanged.
- Summit: the person and the camera no longer shake when frames come faster than the 60 Hz step (90 or 144 Hz, worst looking
  side on and turning the mouse). The person is drawn at the camera's instant between steps (`CameraRig::person_position`,
  `person_facing`), the camera's look is the hand's (`CameraRig::set_look_ahead`; not interpolated between uneven steps) and the
  mouse's turn goes into steps in even shares (`LookFeed`). Presentation and input path only; the scripted hashes are unchanged.
- Summit: the climber's torso, limbs and boots were wound inside out (their inside showed from outside). Fixed, and a presentation test
  (`winding`) now checks every CPU-built mesh Summit draws (the climber's parts in all four styles, rock, decor and ground of four
  seeds, the view's primitives) for triangles facing against their normals. The climber's mesh data moved to `climber.cpp`, with no
  renderer needed. Pictures and scripted hashes are otherwise unchanged. With the winding fixed, the open ends of the limb tubes
  showed holes (neck, shoulders, limb ends): every tube now has flat end caps with the end ring's joint weights, the beanie's
  dome has a disc under its rim, and a test requires every climber part to be a closed surface (no boundary edges).
- Summit: every visible rock is a collider (buttresses at the cliffs' ends, scree, boulders). They were render-only meshes; they are
  now static convex hulls of the same mesh vertices (`decor_chunks`, `decor_chunk_points`, `decor_shape`; one list makes both the
  triangles and the bodies, tolerance 0.04 m both ways, tested), with the Cliff's surface on buttresses and the ground's on stones.
  The decor's noise is smaller so a hull follows it. The level's bytes are unchanged; the scripted hashes change (climb seed 1
  `65e783b36e5dea50`, 9769 steps), no fall on seeds 1 to 60. `summit --debug-colliders` and `--on-decor boulder:N` for looking.
- 3D particles and weather (experimental; see [3D rendering](rendering_3d.md) "Particles and weather" and the plan's "P3c"):
  `kin/render3d/particles3d.hpp` (`ParticleSystem3D`: point, box, sphere, cone and disc emitters, ranges, gravity, drag, a shared `Wind3D`
  with gusts and turbulence, size, colour and alpha over life, a flipbook, a ground; a fixed-step deterministic simulation with `state_hash`,
  `save` and `restore`; `Weather3D`: rain with splashes, snow, dust and mist as camera-following volumes in world space, a pure function of a
  clock; `Weather3D::apply` dims the light and thickens the fog; the CPU's twins `particle_quad`, `soft_particle_fade`, `particle_light_tint`,
  `weather_particle`), `Scene3D::draw_particles` and `Scene3D::weather`, ECS `ParticleEmitter3D` and a `Weather3D` component
  (`simulate_particles`, `advance_weather`, `apply_weather`), `RendererCapabilities3D::{particles, soft_particles}` and
  `RenderStats3D::{particles_submitted, particles_drawn, particle_batches, particle_soft, weather_particles, particles_dropped,
  particle_cpu_ms}`. One instanced draw a batch from a CPU-written storage buffer: camera-facing, stretched, axis-locked and flat quads;
  texture, atlas or procedural disc; alpha, additive and premultiplied; depth-tested and not written; sorted back to front by a radix sort;
  soft particles reading the pre-pass's depth; fogged by `kin_fog.inc.glsl` in the vertex shader and lit by a one-colour tint of the sun,
  fill and ambient light. Opt-in: with none of it used every picture is unchanged byte for byte, the 81 earlier shaders compile to the same
  code, and three shaders are added. Harness: `kin_reference` JSON `weather` and `particles`, options `--rain`, `--snow`, `--wind`,
  `--no-weather`, `--no-particles`, `--soft`, `--particles`; scenes `rain_street`, `snow_valley`, `particles_soft`, `particles_blend`,
  `particles_budget`; Sponza Walk "Particles and weather" section (F drops a fire) and `--preset rain|snow|storm|fire`.
- Spot lights and the shadows of lights that are not the sun (experimental; see [3D rendering](rendering_3d.md) "Spot lights and shadows
  from lights that are not the sun" and the plan's "P3b"): `SpotLightSource3D` (position, direction, colour, radius, size, inner and outer cone
  angle with glTF's squared step in the cosine), `LightShadow3D` (a per-light shadow map's resolution, bias, normal offset, near plane,
  distance, softness; also on `PointLightSource3D`, whose map is a cube of six faces), `Lights3D::{spots, max_shadowed_spots, max_shadowed_points,
  local_shadow_resolution, fill_shadow, sun_shadow_strength}`, `DirectionalShadow3D` (the fill light, the moon, casting through cascades of its
  own), `Scene3D::add_spot_light`, `TimeOfDay3D::shadows` (`apply()` fades the sun's and the moon's shadow strength with their height),
  `kin/render3d/local_lights3d.hpp` (the CPU twin of the falloff, the maps' views, the importance and the slot assignment with its
  hysteresis), ECS `PointLight3D` and `SpotLight3D`, `RendererCapabilities3D::local_shadows` and `RenderStats3D::{spots, spot_slot,
  point_slot, local_shadow_*, moon_cascades, lights_variant}`. Up to 16 spots a frame, up to 8 spot maps and 2 point cubes (the most
  important ones, with a hysteresis so the choice does not flip), shared in one depth array of the largest side asked for; casters are
  culled by the cone (a face of a cube only if a visible surface can fall in), include cut-out, skinned, instanced and clustered meshes (cut
  from the light), and each map is kept while its exact inputs are unchanged (`cache_shadows`). Opt-in: with none of it used every picture
  is unchanged byte for byte, the 88 earlier shaders compile to the same code, and four variants of the stock fragment shader are added.
  Harness: `kin_reference` JSON `lights.spots`, `shadow`, `fill_shadow`, `attach` (a light carried with the camera),
  `time_of_day.shadows`, `procedural: "stage"`, options `--spots`, `--spot-shadows`, `--point-lights`, `--point-shadows`, `--moon-shadow`,
  `--moon-cascades`, `--local-res`; scenes `spot_shadows`, `spot_night`, `spot_budget`, `moon_shadow`, `point_shadow`; Sponza Walk "Spot lights
  and shadows" section (J drops a spot light) and `--preset spots|flashlight|moon|lamps`.
- Sky, fog and time of day (experimental; see [3D rendering](rendering_3d.md) "Sky, fog and time of day" and the plan's "P3a"):
  `kin/render3d/sky3d.hpp` (`Sky3D`, `Fog3D`, `Scene3D::sky`, `Scene3D::fog`, and the CPU's `sky_radiance`, `sky_color`, `sky_sun_light`,
  `sky_irradiance`, `fog_optical_depth`, `fog_opacity`, `fog_apply`), `kin/render3d/timeofday3d.hpp` (`TimeOfDay3D`, `evaluate`, `apply`,
  `sun_times`, `advance`; an ECS component with `WorldRenderState3D::advance_time_of_day` and `apply_time_of_day`),
  `Lights3D::sky_lights_scene`, `RenderStats3D::{sky_drawn, fog_applied, sky_env_regenerated, sky_env_generations, sky_env_cpu_ms}`.
  A procedural single-scattering sky (Rayleigh, Mie, ozone; sun disc, stars, moon, ground below the horizon, or the environment's
  panorama) drawn in the main pass after the opaque meshes, jitter- and velocity-correct under TAA; exponential distance and height fog
  integrated exactly along the view ray, coloured by the sky with the sun's lobe, applied to opaque, cut-out, skinned, instanced,
  clustered, blended and additive meshes (not custom shaders, not the overlay); the sky as the scene's image-based light, remade only
  when the sun moves; a one-step dither for views with a sky. Opt-in: with it off every picture is unchanged byte for byte and the 70 earlier
  shaders compile to the same code. Harness: `kin_reference` scenes `sky_day`, `sky_dusk`, `sky_night`, `fog_valley`, `tod_sweep`
  (`procedural: "landscape"`); Sponza Walk "Sky and fog" section and `--preset sky|dusk|fog|night|day-night`.
- 3D physics, ragdolls and ropes (experimental; see [3D physics](physics_3d.md) "Ragdolls" and "Ropes" and the plan's "P2e"):
  `kin/physics3d/ragdoll3d.hpp`, `rope3d.hpp`; `World3D::create_ragdoll`, `start_ragdoll`, `blend_ragdoll`, `ragdoll_get_up`,
  `set_ragdoll_subtree`, `ragdoll_pose`, `ragdoll_get_up_place`, `create_rope`, `rope_points`, `rope_closest_point`, `rope_tension`,
  `reel_rope`, `set_character_enabled`, `set_motion`, `set_added_mass`, `set_group`, `Body3DDesc::group`. A PASSIVE ragdoll built from a
  skeleton (`auto_ragdoll_desc`: capsules per bone from the bind pose, swing-twist joints, hinges by name) with states Animated
  (kinematic bodies on the clip, hittable), Ragdoll, Blend (with per-bone weights, a partial ragdoll for an arm or a tail) and GetUp
  (the pose blends from the lying snapshot to the clip; a place and a facing to stand at); the pose is written into
  `Animator3D::pose_override` and applied by `WorldRenderState3D` after the clips and IK and before the joint matrices, so the draw, the
  TAA velocity and the shadows use it unchanged. Hand-over from a character controller and recovery with a clearance check. Ropes: chains
  of capsules with ball joints between world or body anchors (or free), sagging, with a rest length that reels and pays out, a
  tension reading, breaking, a polyline and a closest point, drawn as a tube; a rope grip provider so that a hand takes hold anywhere
  on one, the character's weight is on the rope (it swings with the pendulum's period), climbs, jumps off. ECS: `Ragdoll3D`,
  `Rope3D`, `RagdollEvents3D`, `RopeEvents3D`. A pure function of state, commands and dt, in the hash and the saved state, the same bits on
  every worker count. Harness: `kin_reference` scenes `physics_ragdoll` and `physics_rope`; Sponza Walk "Ragdoll and rope" section,
  `--preset ragdoll`, `--preset rope`; `kin_physics3d_bench ragdolls` and `ropes`. Off unless used: with no ragdoll or rope in a world
  nothing of them is in its hash, and every existing picture is unchanged.
- 3D physics, grab and climb (experimental; see [3D physics](physics_3d.md) "Grab and climb" and the plan's "P2d"):
  `kin/physics3d/traversal3d.hpp`, `CharacterDesc3D::traversal`, `World3D::probe_grips`, `set_traversal_input`, `traversal`,
  `set_traversal_resource`, `set_surface_material`, `set_hold`: a general capability on top of the controller, two hands by default
  (the limbs are an array). A probe (a fan of sphere sweeps and rays against a layer mask, through the hold, ledge and surface
  providers) finds what a hand can take; a grip per limb keeps its anchor in the grabbed body's own space (platforms and crates carry it)
  with strength, slip and exhaustion; the character hangs (kinematic, driven from the anchors with a damped swing), shimmies and climbs
  along the surface, pulls up over a ledge in a swept deterministic move that fails cleanly under a low roof, jumps off and drops; a held
  dynamic body is loaded with its weight; surface materials (grippable, grip, slip, drain, tag) and authored holds are data on bodies;
  `Resource3D` (drain by mode, a delay before it regenerates, exhaustion with hysteresis) is the stamina and what sprinting spends. A pure
  function of state, commands and dt, in `state_hash()`, `save_state()` and `restore_state()`, the same bits on every worker count, and
  any up vector. Events: gripped, slipped, released, exhausted, mantle started, finished, blocked, fell. ECS: `Hold3D`,
  `SurfaceMaterial3D`, `CharacterController3D::traversal` and `traversal_state`, the hands' IK and the facing from
  `CharacterAnimationLink3D::hands`, `locomotion_animator`'s `hang` clip, `DebugDraw3D::grips` and `candidates`. Level
  `tests/climb_level.hpp`; `kin_reference` scene `physics_climb` (a scripted timeline and its checkpoints, also run with no GPU by
  `kin-physics3d-traversal`); Sponza Walk "Climb" section and `--preset climb`; `kin_physics3d_bench climbing`. Off unless
  `traversal.enabled`; a character without it, the no-3D build and every existing picture are unchanged. Not in this step: ropes, feet
  on the wall, ragdoll.
- 3D physics, the character controller (experimental; see [3D physics](physics_3d.md) "Character controller" and the plan's "P2c"):
  `kin/physics3d/character3d.hpp` and `World3D::create_character`, `set_character_input`, `character`, `character_contacts`,
  `teleport_character`, `set_character_up`, `set_character_crouch`, `set_character_desc`, `character_events`: a capsule controller on
  Jolt's `CharacterVirtual` (slopes up to a set angle and a slide down steeper ones, stairs up to a step height, stick to the ground
  over humps and down ramps, jumps with optional coyote time and buffering, moving platforms and dynamic bodies carry it, it pushes
  dynamic bodies by its strength, layers and a mask, sensors, crouching with a clearance check, wall and ground friction, an up vector
  per character that turns smoothly so that walls and ceilings can be walked on); the command is a struct (move, forward, jump,
  crouch, sprint), the step is a pure function of state, command and dt, the same bits on every worker count, in `state_hash()`,
  `save_state()` and `restore_state()`; characters update before the world's step in number order. ECS: `CharacterController3D`
  (made in entity id order, written back to Transform3D, events with entity ids), `CharacterAnimationLink3D` and
  `locomotion_animator` (speed, airborne, crouching and the foot placer's ground from the physics world). Level
  `tests/character_level.hpp`; `kin_reference` scene `physics_character` (a scripted route and its checkpoints, also run with no GPU
  by `kin-physics3d-character`); Sponza Walk "Character controller" section and `--preset controller`; `kin_physics3d_bench
  characters`. `DebugDraw3D` draws a capsule wrapped for an offset. Off unless used; the no-3D build and every existing picture are
  unchanged. Not in this step: grab and climb, ragdoll, ropes.
- 3D physics, queries, events, constraints, debug draw and entities (experimental; see [3D physics](physics_3d.md) and the plan's "P2b"):
  `World3D::raycast`, `raycast_all`, `shape_cast`, `overlap`, `point_inside`, `closest_point` (a layer mask, an ignored body, sensors
  in or out; values in a fixed order, no callbacks); contact events per body pair (Begin, Persist, End; sensors; user ids, point,
  normal, closing speed), sorted and the same on every worker count, read with `events()` / `drain_events()`, a pair that sleeps
  together is not reported as ended; constraints (fixed, distance with a range and a spring, point, hinge with limits and a motor,
  cone, slider; breakable; connected bodies do not collide by default; in the state hash and the saved state); `teleport`,
  `follow_transform`, `set_layer`, `set_sensor`, `set_shape`; `DebugDraw3D` (shapes, sleeping and awake, contacts, constraints, query
  results as line lists, no Jolt debug renderer); ECS `Collider3D`, `RigidBody3D` and `WorldPhysicsState3D` (bodies made in entity id
  order, kinematic bodies from Transform3D, dynamic ones written back, events on the world, opt-in debug lines and interpolation),
  `ground_query_from_world` for the foot placement of P1b, `collider_from_mesh`. `kin_reference` scene `physics_pile`; Sponza Walk
  "Physics" section, `--preset physics`, key B; `kin_physics3d_bench queries`; new tests `kin-physics3d-ecs` and
  `test_gpu_physics_debug_lines`. Off unless used; the no-3D build and every existing picture are unchanged. Not in this step: the
  character controller, grab and climb, ragdoll, ropes.
- 3D physics, the Jolt core (experimental; see [3D physics](physics_3d.md) and the plan's "P2a"): `kin/physics3d`: `World3D` (gravity as a
  vector, a fixed `step(dt)` with a set number of collision steps, bodies by descriptor: static, kinematic and dynamic, mass or density,
  friction, restitution, damping, layer, sensor flag, user id; transforms, velocities, forces, impulses, activation) and `Shape3D`
  (box, sphere, capsule, cylinder, convex hull, triangle mesh, height field, compound), 32 collision layers with a pair table and
  32-bit masks, `state_hash()` and `save_state()` / `restore_state()`. Jolt Physics 5.5.0 (MIT) is fetched only with
  `KIN_EXPERIMENTAL_3D` and built for cross-platform determinism; steps run on `kin::JobSystem` with the same bits for any number of
  workers. No header of Jolt is included by kin's. New: `kin-physics3d` test, `kin_physics3d_bench`. Not in this step: queries, events,
  debug draw, ECS components, character, ragdoll, ropes. The no-3D build and all existing pictures are unchanged.
- 3D, inverse kinematics, the known limits fixed (experimental; see [3D rendering](rendering_3d.md) "Inverse kinematics" and the plan's "P1b follow-up"):
  joint limits for `solve_two_bone` (`max_bend_degrees`, `hinge` / `hinge_axis`, `max_twist_degrees`: a knee that bends one way about its own
  axis and whose hip turns no more than a limit, with no flip as the pole goes round behind); FABRIK as an ECS constraint kind
  (`IKKind3D::Fabrik`) with a pole; foot locking (a planted foot held in the world while the body moves, released and eased back), toes
  (`FootLeg3D::toe`, `IKConstraint3D::toe_joints`) that lie on the ground under them, and the ground query called once a frame per leg however
  often the constraints are solved; a non-uniform scale on the root joint or between the mid and end joints is now exact (it was approximate);
  a straight chain with no pole bends the way the rest pose is bent (else the hinge's side, else -Y as before); `TwoBoneState3D` and
  `bend_swing_degrees_per_second` for the elbow's side to swing at a bounded speed where a pole passes through the target's axis.
  Everything is off by default and the pictures of before are byte for byte. `kin_reference`: `fabrik`, `max_bend`, `hinge_axis`,
  `max_twist`, `bend_swing`, `toes`, `lock`, ground `ramps`; the scene `skinned_ik_limits`; Sponza Walk's person has toes and a tail, and
  `--preset ik-tail`, `ik-walk`. Fixed: `TwoBoneResult3D::reached` was true for a target inside the hole round the root. Docs: the stale
  "Not yet: skinned meshes" line of the 3D guide.
- 3D, inverse kinematics (experimental; see [3D rendering](rendering_3d.md) "Inverse kinematics" and the plan's "P1b"): `kin/render3d/ik3d.hpp`:
  `solve_two_bone` (an arm to a hold, a leg to the ground: pole by direction or point, soft stretch, minimum bend, an end orientation
  target, a weight; exact under scaled and turned models), `solve_look_at` (a head or a spine-neck-head chain with shared turn, yaw and
  pitch limits, released toward forward past a limit so a target behind does not flip it, smoothing in a state object),
  `solve_chain_fabrik`, and `plan_foot_placement` / `apply_foot_placement` (a `GroundQuery3D` the game supplies, the foot lifted to the hit
  and tilted to the clamped normal, the pelvis lowered for the lowest foot, eased). CPU only: no shader changes. ECS: `Animator3D::ik_constraints`
  (`IKConstraint3D`, `animator.ik("name").set_target(...).set_weight(...)`, targets as a world point or another entity, weight fades),
  solved after the clips and before the joint matrices in `WorldRenderState3D::collect()` (or `apply_ik()`); an animator with none, and every
  existing picture, is byte for byte as before. `kin_reference` `animation.ik` and the scene `skinned_ik` (a procedural person,
  `skinned_biped.glb`); Sponza Walk's "Character IK" section (`--preset ik`).
- 3D, skinned meshes and clips, part 2 (experimental; see [3D rendering](rendering_3d.md) "Skinned meshes" and the plan's "P1a part 2"): a skinned
  copy is now in every pass: the depth pre-pass (ambient occlusion, contact shadows, occlusion culling's occluders), the temporal velocity of a
  moving pose (last frame's joint matrices matched to the copy by `MeshInstance3D::id` or order), cut-outs, and pick; the shadow cache keeps the
  map for a still pose and redraws for a moved one; `draw_skinned(model, ...)` shares one range of matrices among a model's parts and takes an
  `id`. `AnimationPlayer3D::play_blend` / `crossfade_to_blend` / `set_blend_parameter` (1D blends by phase). ECS: `SkinnedMeshRenderer3D`,
  `Animator3D` (clip by name, a `kin/anim` state machine with blend spaces, fades, masked and additive layers as data, root motion) and
  `WorldRenderState3D::animate`. `kin_reference` scenes `skinned_blend` and `skinned_ecs` (`models[].animation.blend` / `.ecs`); Sponza
  Walk has a "Character (skinned)" section (`--preset character`). Scenes without skinned copies are byte for byte, and cost the same, as before.
- 3D, skinned meshes and clips, part 1 (experimental; see [3D rendering](rendering_3d.md) "Skinned meshes" and the plan's "P1a"):
  `Skeleton3D`, `Pose3D`, `PoseMask3D`, `AnimClip3D` (step, linear and cubic-spline keyframes as glTF has them), `AnimationPlayer3D`
  (crossfades, masked and additive layers, root motion), `compute_joint_matrices`; the glTF reader takes skins, `JOINTS_0` and
  `WEIGHTS_0`, and animations (`Model3D::skeletons`, `Model3D::clips`); `MeshData3D::skin`; `Scene3D::draw_skinned`; GPU skinning in the main and
  shadow passes (new vertex shader variants; a mesh with no skin and a scene with no skinned copy are byte for byte, and cost the same, as before).
  Not yet: ambient occlusion, contact shadows, the temporal velocity of a moving pose, and pick for skinned copies. `kin_reference` scenes
  `skinned_bend`, `skinned_cesium`, `skinned_fox` (the last two fetch CesiumMan and Fox) and `skinned_crowd`; `fetch.py --lock NAME` locks new
  assets alone.
- 3D, speed of the opt-in features, phase 2 (small picture changes within stated bounds; see the plan's "Opt-in speedups, phase 2"): in views of 1.5 million pixels
  or more, ambient occlusion reads a linear depth chain (one dispatch, five R32F levels) and costs 15 to 30% less (1080p: 0.2 to 0.5 ms, 4K: 2 to 4.6 ms);
  contact shadows blurred on their own get one blur pass (-0.03 ms at 720p). Everything else (and every view under 1.5 million pixels) is as before.
  `KIN_AO_CLASSIC=1` / `KIN_AO_CHAIN=1` / `KIN_CONTACT_CLASSIC=1` pick the old or new path (read on each render), `RenderStats3D::ao_chain_built` says which ran;
  `kin_reference` has `--ab NAME[:A:B]` (A and B timed in blocks of frames of one run, drift-proof) and `--flicker N` (frame-to-frame change under TAA). The
  plan also records what was tried and dropped (a checkerboard, one slice, a cheaper contact march, half-resolution contact shadows).
- 3D, speed of the opt-in features (byte-identical pictures; see the plan's "Opt-in speedups, phase 1"): temporal anti-aliasing no longer draws a
  velocity pre-pass when nothing else needs one (the main pass writes the velocity: the kiln's TAA cost fell by 2.2 ms), ambient occlusion and contact
  shadows share one blur dispatch, and the shadow map is kept while nothing it depends on changes (`Lights3D::cache_shadows`,
  `RenderStats3D::shadow_cached`). Targets of ambient occlusion, contact shadows, the pre-pass, temporal anti-aliasing and occlusion culling are given
  back after 120 renders unused (`RenderStats3D::feature_bytes`, `targets_released`). `Renderer3D::warm_up(view, scene)` makes the pipelines and
  targets a scene's first render would, so that turning a feature on does not hitch (`RenderStats3D::pipelines_made`). `kin_reference` has
  `--shadow-cache`, `--crowd-move`, `--hitch <feature>`, `--warm-up` and `--vram-cycle`; Sponza Walk has a "Keep the map while nothing moves" switch,
  warms up at start and shows the new numbers in its stats.
- Experimental 3D drawn inside the 2D frame (the `KIN_EXPERIMENTAL_3D` CMake
  option, on in the `exp/3d-rendering` branch; see
  [3D rendering](rendering_3d.md) and [the plan](3d_plan.md)):
  `kin/core/math3d.hpp` (`Vec3f`, `Vec4f`, `Quatf`, `Mat4f`, reverse-Z
  projections, `project`, `unproject`, `screen_ray`); meshes from data or made
  in code (`kin::meshes`); views with depth and MSAA, laid in with
  `draw_texture` and rendered between the 2D draws around them; scenes of
  instanced draws, lit by a sun and ambient light, emissive, unlit or
  double-sided; perspective and orthographic cameras that project to and
  pick from 2D coordinates. SDL_GPU only: under SDL_Renderer a view shows its
  background.
- Signal Siege `--view 3d`: the same run drawn in 3D from behind the player,
  with the same checksum.
- 3D: the scene's overlay (a first-person gun, always in front), frustum
  culling of every copy (on the job system for big scenes), ramps
  (`meshes::wedge`), and entities (`Transform3D`, `WorldTransform3D` through
  the flecs hierarchy, `MeshRenderer3D`, `WorldRenderState3D`).
- Mouse look: `Input::mouse_motion()` and `frame_mouse_motion()` (relative
  motion, whether or not the pointer moves), `Input::set_mouse_motion()`,
  `Input::last_mouse_motion_event_time_ns()`, and
  `Window::set_relative_mouse()`. The agent server's `input.mouse` takes
  `dx`/`dy`. `Key::LeftShift`.
- Signal Breach (`examples/signal_breach`): Signal Siege first person, on the
  experimental 3D: a deck with ramps and ledges, enemies flying in 3D, an
  autopilot for benchmarks and agents, and `--latency-probe` to time mouse
  motion to the screen.
- 3D: point lights (16 a frame, chosen by reach and strength), the sun's
  shadow over an area (one shadow map, filtered), alpha-blended and additive
  materials, textures on meshes, HDR views tonemapped on their way to 2D with
  bloom of their own, and glTF 2.0 models (`Renderer3D::load_model`,
  `ModelFile3D` for hot reload) read with cgltf (a new dependency, with
  `KIN_EXPERIMENTAL_3D`).
- 3D (experimental): levels of detail for meshes (`create_mesh(data, {.levels = n})`, `Scene3D::lod`), built with
  meshoptimizer (a new dependency, with `KIN_EXPERIMENTAL_3D`); off by default.
- 3D (experimental): cluster hierarchies for huge meshes (`create_mesh(data, {.clusters = true})`), cut per frame by
  `Scene3D::lod` so that near parts stay fine and far parts are coarse inside one mesh; off by default. The cut, the
  frustum culling and the gathering of the chosen triangles run in compute where the device can
  (`Lod3D::gpu_culling`), with the picture the CPU cut makes and indirect draws: `render()` of the kiln costs 0.05 ms
  instead of 0.4 to 0.9.
- 3D (experimental): occlusion culling for clustered meshes (`Lod3D::occlusion`), a two-pass test against a depth pyramid built from this frame's
  own depth, so the picture is the one without it; off by default, since it pays only where much more is hidden than its fixed work costs (a wall hiding
  hundreds of heavy copies); a `hidden` reference scene and Sponza Walk's `--preset occlusion`.
- `decode_image()`: an image from a file's bytes in memory.
- 3D sound: `AudioEngine::set_listener_3d()`, `set_position_3d()` and
  `AudioPlayRequest::position_3d`: pan by direction from where the listener
  faces, fade by 3D distance; `calculate_spatial_audio_3d()`.
- Signal Breach: shadows, muzzle-flash and explosion lights, glowing shots,
  additive sparks and tracers, a glTF gun and crates, sound in 3D;
  `--no-shadows`, `--no-bloom`, `--no-lights` to measure each.
- 3D picking (`ViewOptions3D::pick`, `Renderer3D::pick`: the id given with a
  draw and the world point under a pixel), wireframe materials (also lines
  from degenerate triangles), a fill light, ambient light from below
  (`Lights3D::ground_ambient`) and `Material3D::receives_shadow`. Large meshes
  are uploaded in pieces, written straight into the upload memory.
- Reference scenes for 3D (`tests/reference/scenes`, `tools/reference`):
  the Cornell box, material spheres, a white furnace, a shadow test,
  Khronos' Flight Helmet and Sponza (fetched, pinned, checked), a crowd of
  20,000 and the studio's kiln (local); `kin_reference` renders and times
  them, Blender's Cycles renders the same cameras as the truth, and
  `compare.py` reports the pictures' distances and the times.
- 3D shadows cost less: casters whose shadow can't fall in view are left
  out of the shadow map, single-sided meshes cast with their front faces
  only (a lone quad facing away from the sun casts nothing: make it
  double-sided), the shadow pass has its own vertex shader, and the map is
  no longer cycled. At 20,000 enemies Signal Breach's shadows take about a
  fifth of the GPU time they did. `RenderStats3D::shadow_casters`.
- 3D materials are glTF's metallic-roughness (G1a of the plan):
  `Material3D::metallic`, `roughness`, `metallic_roughness`, `normal` (and
  `normal_scale`), `occlusion` (and `occlusion_strength`) and
  `emissive_texture`, lit with a Cook-Torrance specular (GGX, Smith, Schlick)
  by the sun, fill and point lights, and reflecting the ambient light; normal
  maps need no tangents. `Blend3D::Mask` with `alpha_cutoff`: cut-outs that
  are drawn, picked and shadowed cut out. `Renderer3D::load_model` reads all
  of it (data maps as linear data, not sRGB colour), and alpha `MASK`.
  Point lights now fall off with the square of the distance, windowed to
  nothing at `radius`: `PointLightSource3D::color` is the light a unit away. Signal
  Breach's flashes and the Cornell reference scene's lamp are rescaled.
  Surfaces now have the specular of a rough dielectric by default
  (`roughness` 1, `metallic` 0): a little more light than before.
- 3D, G1b: glTF sampler wrap modes (`Material3D::wrap_u`/`wrap_v`, read from
  models: textures that tile no longer clamp); image-based light
  (`Lights3D::environment`, `Renderer3D::load_environment` for Radiance `.hdr`,
  `create_environment` for floats): diffuse irradiance and GGX-filtered
  specular made once by compute shaders, in place of the hemisphere ambient;
  custom material shaders (`Material3D::shader`, `shader_params`,
  `Renderer3D::create_material_shader` and `compile_material_shader`) written
  against `kin_material.inc.glsl`, which has kin's lighting as functions.
  Shader building passes `-I` for the engine's includes. Reference scenes
  `ibl_spheres` and `ibl_helmet` with Poly Haven's Kloofendal sky (CC0).

## [0.2.6] — 2026-10-07

A release for sound, other languages and shipping. Audio mixes on the
device's own thread, plays Ogg Vorbis, MP3 and FLAC as well as WAV, streams
music, crossfades it and changes it on the beat, and runs effects per bus
(see [audio](audio.md)). Text comes by key in the player's language, in any
script and right to left, with input methods, fonts per language and text that
fits its box (see [localization](localization.md)). A game builds into a
folder players can run anywhere, with its executable, its content in one
`.kinpak` archive, and the license texts it carries (see
[shipping](shipping.md)).

### Upgrading from 0.2.5

- `Input::mouse_wheel_y()` is now for `update()`: what the wheel turned since
  a fixed step last read it. Code that reads the wheel at render time should
  read `frame_mouse_wheel_y()` instead (ui2 already does).
- `Input::advance_keyboard_edges()` is now `advance_step_edges()`: it also uses
  up the step wheel.
- `Input::set_mouse_wheel_y(v)` adds `v`, as a wheel event would, instead of
  replacing the value.
- `IAudioBackend` is pulled instead of pushed: `queued_frames()` and
  `queue_interleaved()` are replaced by `start(render)` and `stop()`, and
  `AudioEngineConfig::queue_target_frames` is gone. Custom backends call the
  render function from their audio thread.
- `AudioEngine::stats()` returns a copy, and `mixed_frames` is 64-bit.
- `set_bus_volume("master", v)` now scales every bus under `master`, as the
  hierarchy says; before, it only reached voices playing on `master` itself.
- Every engine loader reads through the content layer (`kin/assets/content.hpp`).
  Paths are unchanged, and a `.kinpak` mounted at a folder now answers for it.
- `kin_configure_game_assets` also sets the game up with `kin_game`. That adds
  install rules, the targets `<game>_content` and `<game>_package`, and
  `KIN_GAME_CONTENT`.
- The engine's GPU shaders are built into it; `KIN_GPU_SHADER_DIR` is gone.
- The agent server's HTTP transport listens on `127.0.0.1` only (it listened
  on every interface).
- `ScriptEngine::load_file` reports a missing script as `cannot open <path>`.
- Signal Siege's language files moved to `examples/content/lang/`.
- `TextDirection` moved to `kin/core/bidi.hpp` (it is still `kin::TextDirection`).
- Games with text fields should call `ui.apply_text_input(window)` after
  `ui.end()` each frame (it replaces `window.set_text_input_enabled(ui.wants_text_input())`).
- `TtfFontOptions::fallbacks` and `system_fallback_fonts()` are `FontSource`s
  (a path converts to one).
- `LanguageFile::direction` is optional: unset follows the locale tag.
- ui2 TTF text beyond ASCII is laid out glyph by glyph from the face's metrics
  instead of shaped as one string, so its width can change by a pixel or so.
  Text that needs a shaper (right to left, Arabic, Indic, combining marks) is
  still shaped, run by run.
- `wrap_text` now breaks CJK text between characters, and long words between
  characters rather than bytes.
- The 5x7 bitmap font measures characters, not bytes, so UTF-8 text is
  narrower than before.

### Added

- `Input::frame_mouse_wheel_y()`: the wheel since the last rendered frame, for
  render-time UI, as `frame_pressed()` is to `pressed()`.
- Audio: `AudioEngine::render()` mixes on the calling thread, for offline
  rendering and tests. `set_bus_volume` takes a fade, `set_bus_muted` and
  `set_bus_paused` mute or pause a bus and everything under it (pause
  gameplay sound while UI sound plays on), and `AudioPlayRequest::fade_in`
  fades a voice in.
- Audio: `preload(catalog)` decodes a catalog's clips up front, and
  `preload_async(catalog, jobs)` does it on worker threads; a `play()` that
  needs a clip still loading waits for it instead of decoding it again.
- Audio: a cue with several clips plays a random one (never the same twice
  running), and `pitch_var` varies its pitch, both drawn from
  `AudioEngineConfig::seed`.
- Audio formats: Ogg Vorbis, MP3, FLAC and AIFF play alongside WAV, decoded
  by stb_vorbis and dr_libs (new dependencies, fetched by CMake like the
  others). `load_audio_clip` recognises a file by its contents.
- Streamed clips: `clip theme music/theme.ogg stream=true` keeps the file
  compressed in memory and decodes it as it plays, for music and long
  ambience. `load_audio_stream` loads one directly, and `AudioDecoder` /
  `open_audio_decoder` decode a file held in memory.
- Music: `play_music(catalog, cue, crossfade)` keeps one music cue playing
  and crossfades to the next; asking for the cue already playing keeps it
  going. `stop_music`, `music()`.
- Voice control: `set_volume(handle, volume, fade)`, `set_pitch(handle,
  pitch)` and `playback_position(handle)`.
- Ducking: `duck music when=dialogue volume=0.3 attack=0.15 release=0.8` in a
  catalog turns a bus down while another plays (`AudioCatalog::add_duck`).
- `write_audio_settings` / `apply_audio_settings` keep the output device, bus
  volumes and mutes in a settings file, `AudioEngine::bus_states()` lists them, and
  `AudioEngine::write_report` writes stats, buses and voices for a scene's
  report.
- Lua: `bind_lua_audio` gives scripts an `audio` table, and
  `ScriptSceneConfig::bind` (with the matching `ScriptEngine` constructor)
  lets a host add its own bindings to a script scene's Lua state.
- Adaptive music: `play_music(catalog, cue, AudioMusicTransition{crossfade,
  sync, match_position})` changes music on the next beat, bar or clip end
  (cue `bpm=`, `beats_per_bar=`, `beat_offset=`), optionally from the same
  position. `layers=true` cues play their clips in step as stems
  (`set_music_layer`), and `playlist=true` cues play their clips back to back
  with no gap (`shuffle=true` for random order). `music_position()` gives the
  beat and bar, and `play_synced` plays a cue on the next beat or bar. Voices
  can start and stop on an exact output frame. Lua has `play_music` with
  options, `set_music_layer`, `music_position` and `play_synced`.
- More audio effects: `bandpass` and `notch` filters, an EQ `peak` band,
  `lowshelf` and `highshelf` (with `gain` in dB; `freq` names the frequency),
  and a `delay` (`time`, `feedback`, `wet`, `dry`) whose time glides when
  changed.
- Audio levels: `bus_level(bus)` and `output_level()` give each bus's and the
  output's peak and RMS (always on, in `write_report` too), and
  `enable_analysis(bus)` with `spectrum(bus, bands)` / `magnitude(bus, from,
  to)` give a frequency spectrum for visualizers. Lua's `audio` table has
  `bus_level`, `output_level`, `enable_analysis` and `spectrum`.
- Audio reloading: `AudioEngine::watch(files)` reloads clip files when they
  change (new plays get the new version), `watch_catalog(files, path,
  catalog)` reloads a catalog file and applies it, and `apply_catalog`
  applies a catalog edited in code. A broken edit keeps the last good
  version.
- ECS audio: `AudioEmitter` has `volume`, `pitch`, `paused`, `fade_in` and
  `when_done` (`Restart`, `Keep`, `Remove`, `Despawn`); removing an emitter
  or destroying its entity fades its sound out. `AudioOneShot` has `volume`
  and `pitch`.
- Audio voices: `seek(handle, seconds)` (declicked, streams too) and
  `set_paused(handle, paused)` / `paused(handle)` for one voice.
- Audio memory: `unload_unused()`, `unload(catalog)` and `remove_clip(id)`
  free cached clips, and `loaded_clip_count()` / `loaded_clip_bytes()` say
  what the cache holds. Finished voices hand their clips back to the game
  thread to be freed.
- Distance curves: a cue's `rolloff=smooth|linear|inverse`,
  `rolloff_power=` and `pan=` strength (`AudioRolloff`, `SpatialAudio`).
- Output devices: `list_audio_output_devices()`,
  `AudioEngine::set_output_device(name)` and `output_device()`; a chosen
  device that is unplugged falls back to the default and back again.
  `create_sdl_audio_backend` takes a device name.
- Lua's `audio` table adds `seek`, `position`, `set_paused`, `paused`,
  `output_devices`, `set_output_device` and `output_device`.
- Audio effects per bus: low-pass and high-pass filters, a reverb
  (Freeverb) and a compressor, in a bus's chain from `effect` lines in its
  catalog or `set_bus_effects`; `set_bus_effect` changes one while it plays
  (filters glide to a new cutoff, reverb tails carry on). Lua scripts get
  `audio.set_bus_effects` and `audio.set_bus_effect`.
- Loop points: `loop_start=` and `loop_end=` on a catalog clip (in frames)
  make a looping cue play its intro once and then repeat the loop, streamed or
  not.
- Shipping ([shipping](shipping.md)):
  - `kin_game(target ...)` in CMake. It sets up the game's content, Windows
    version info and icon, install rules, `<game>_content` (the pack) and
    `<game>_package` (a `.zip` or `.tar.gz` under `packages/`).
  - `KIN_SHIPPING` and the `ship` / `ship-vs` presets: no agent server, render
    probe, determinism check or release profiling. Windows games are windowed
    apps on the static runtime; Linux games link libstdc++ in. The log goes
    with the game's saves.
  - `.kinpak` content packs: `ContentPack`, `write_content_pack`,
    `mount_content_pack`, and the `kin_pack` tool (`create`, `list`, `verify`,
    `extract`).
  - Content reads from a pack or disk: `read_content_file`,
    `read_content_text`, `view_content_file`, `content_file_exists`,
    `content_directory_exists`, `content_file_size`, `list_content_files`.
  - `find_content_root` (`--content`, `KIN_CONTENT`, the source folder in
    development, then beside the executable), `content_root`,
    `executable_dir`.
  - `KIN_ENABLE_AGENT_SERVER`, `KIN_PACK_CONTENT`, `KIN_REQUIRE_GPU_SHADERS`.
  - `kin_compile_glsl` and `kin_embed_files`, to build shaders and small files
    into a binary; `cmake/kin_spirv.cmake` and `KIN_SPIRV_DIR`, for build
    machines without glslc.
  - License texts of kin's dependencies, shipped in `licenses/` with each game.
  - CI packages Signal Siege on Linux, built in the Steam Runtime (glibc
    2.31), and on Windows with the GPU shaders, and runs it from another
    folder (and, on Linux, in the runtime).
- `kin::Localization` (`kin/l10n/localization.hpp`): `.kinlang` (JSON) and CSV
  language files, several per language, reloaded as they change; a fallback
  chain (`fr-CA`, `fr`, the base); `text(key)` and `tr(key, args)`; missing
  keys logged once and listed; `validate()` against the base; the
  pseudo-locale `en-XA`. `system_locales()`, `match_locale()`,
  `normalize_locale()` and `locale_direction()` for tags.
- Messages (`kin/l10n/message_format.hpp`): arguments, `plural` (CLDR rules for
  about 45 languages, `=N`, `#`), `select` and ICU quoting; numbers grouped for
  the locale.
- `--locale`, `--pseudo-locale` and `--fail-on-missing-text` on every
  `run_scene_app` game; `SceneAppConfig::localization`; the run report's
  `localization` section. Headless and server runs show the base language
  unless told otherwise.
- `Ui2Text` and `Ui2EntityBuilder::text_key`: retained widgets' text by key,
  updated when the language changes.
- Dialogue shown from the localization (`dialogue.<doc>.<node>`, choices,
  speakers, history), with the dialogue's variables as values;
  `dialogue_language_file` extracts a document's text.
- `tr()` and an `l10n` table in `ScriptEngine` and `LuaScript` scripts.
- `kin/core/bidi.hpp`: the Unicode bidirectional algorithm for a line's runs.
- ui2 fonts: `TtfFontOptions` with `fallbacks`; `system_fallback_fonts()`,
  which `system_ui_font` uses; `set_text_base_direction`.
- `ui2::set_ui_direction`: right-to-left interfaces (layouts reflected,
  `align_rect` Start and End swapped). `run_scene_app` follows the language.
- `utf8_append`. `FileWatcher::load_and_watch` can return its watch id.
- Signal Siege in English, French, Japanese and Arabic, with a language item
  on its title. `kin_text_bench`.
- Input methods: `Input::text_composition()`; `TextInput` and `TextEdit`
  show the composition at the caret; `Window::set_text_input_area` and
  `ui2::Context::apply_text_input` place the candidate list.
- Right-to-left editing: `ui2::caret_x`, `caret_at`, `selection_spans` and
  `paragraph_direction`; `TextInput` and `TextEdit` use them.
- `ui2::Context::push_mirror` / `mirror_if_right_to_left`: widgets mirror their
  insides in right-to-left interfaces (Toggle, Slider, ProgressBar,
  LabeledBar, IconButton, ResourceRow, IconMeter, MenuList, TabBar, ComboBox,
  ListView; ScrollView's scrollbar moves left).
- `ui2::fit_text`, `TextOverflow::Ellipsis` and `Shrink` (Label, and the new
  `Button::overflow`); `ui2::collect_overflows`; the run report's
  `ui_overflow` and `--fail-on-text-overflow`.
- Fonts per language: `TtfFontOptions::language_fallbacks`, `FontSource`
  (a face of a collection), `system_language_fonts()`,
  `ui2::set_text_language`.
- Ordinals: `{n, selectordinal, ...}`, `ordinal_category`, `ordinal_categories`.
- gettext: `parse_gettext` and `write_gettext`; `.po` files load from language
  directories.
- Assets by language: `Localization::localized_path` (`l10n/<locale>/<path>`),
  used by `AssetManager` and `AudioCatalog`.
- `ui2::caret_move`: the arrows move the caret on screen through
  mixed-direction text in `TextInput` and `TextEdit`.
- Popups in right-to-left interfaces: placed reflected about their anchor;
  `MenuBar` mirrors; `Context::to_screen` and `screen_pointer`.

### Changed

- `AudioEngineConfig::music_voices` defaults to 8 (from 2), as music layers
  and crossfades each take a voice.
- WAV files are decoded by dr_wav instead of `SDL_LoadWAV`, and loading a
  clip no longer initialises SDL's audio subsystem.
- Audio mixes on the device's audio thread, so a long frame no longer makes
  it crackle, and output latency no longer depends on the frame rate.
- `stop()` and `stop_bus()` honour their fade; even an immediate stop, or a
  voice stolen for a new one, fades over 5 ms instead of clicking. A stopped
  voice no longer counts as playing.
- Clips keep their file's sample rate and channel count (mono stays mono)
  and are resampled with linear interpolation as they play, which also
  interpolates pitch changes. Engines run at other rates than 48 kHz now play
  clips at the right pitch.
- Spatial voices pan with an equal-power law, so a sound crossing the
  listener no longer dips in the middle, and gain and pan changes ramp over a
  block instead of stepping.
- Buses are mixed on their own and added into their parents (submixes), in
  blocks of at most 1024 frames; with no effects the output is the same as
  before. `write_report`'s buses list their effects.
- A master limiter turns the mix down when voices add up past full scale,
  instead of hard-clipping it (`stats().limited_frames` counts how often).
- ui2 TTF atlases (Bitmap and Sdf) grow as characters appear instead of
  holding printable ASCII only, and kern beyond ASCII. Text beyond ASCII no
  longer becomes a texture per string: 70 accented labels a frame measure and
  draw 3.2x faster, Cyrillic 2.3x; ASCII is as before.
- Right-to-left and shaped runs are ordered by the bidirectional algorithm
  and drawn white and tinted, so one texture serves every colour.
- Atlas pages keep their pixels, so backends without `update_texture` (the
  software renderer) remake the page instead.

### Fixed

- A wheel notch read in `update()` reaches exactly one fixed step. It was
  cleared every rendered frame, so above the sim rate a notch on a frame that
  ran no step was lost (about half of them at 120 Hz), and a frame that ran
  several steps applied it in each.
- `AppFrameStats::update_steps` counts the step being run inside `update()`
  under `App::run`, as it already did under `run_for`: 1 is a frame's first
  step (it was 0).

## [0.2.5] — 2026-10-06

A smaller release, for large saves and cheaper UI text. Saves can be written
compact, carry a one-line summary and list without reading their payloads,
and a parsed JSON value is a quarter of the size it was. ui2 text is kerned
as the font says, draws exactly as wide as it measures and measures about 50
times faster; rounded rectangles are built without trigonometry.

### Upgrading from 0.2.4

- `JsonValue`'s layout changed (strings, arrays and objects are boxed):
  rebuild everything that includes `kin/core/json_value.hpp`. A value moved
  from is now null.
- Fonts kerned in GPOS now draw kerned, so their lines are narrower than
  before, and `measure()` changes by about a pixel for most fonts (heights
  follow the line's ink, as SDL_ttf's do). Layouts that hard-code text widths
  may want a look.

### Added

- Saves: `SaveStoreConfig::compact` writes settings and slots without
  indentation or line breaks, a fraction of the size and the time for large
  payloads; either form reads back. `SaveSlotInfo::summary` is a short line of
  the game's own, kept beside the slot's info so `list_slots` shows it.
- `parse_json_skipping(text, key)` parses a document passing over the root
  object's member `key` unbuilt, and `JsonValue::take_member` moves a member
  out of a parsed document instead of copying it.

### Changed

- `JsonValue` keeps strings, arrays and objects in a box of their own: a value
  is at most 32 bytes (was over a hundred), so a parsed save of millions of
  numbers costs a fraction of the memory. `items()` and `members()` return an
  empty container for a value of another type, as before; a value moved from
  is left null.
- `SaveStore::list_slots` reads each slot's info without parsing its payload,
  and `read_slot` / `read_settings` move the payload out of the parsed file
  instead of copying it.
- ui2 TTF text: drawing (Bitmap and Sdf) and `measure()` lay printable ASCII
  out by one walk over the face's metrics, read once a face, so text draws
  exactly as wide as it measures. Measuring no longer shapes the string with
  SDL_ttf each call: 210 measures of HUD labels went from 2.7 ms to 0.05 ms a
  frame. Drawing no longer asks SDL_ttf for each glyph pair's kerning. Other
  text (beyond ASCII) is still shaped by SDL_ttf.
- Rounded rectangles: both backends build the outline from one shared
  `rounded_rect_loop`, whose corner arcs come from a table of cos and sin a
  segment count, not from `cos`/`sin` for every point of every fill and
  outline: the same points, about 3.4x faster to build.

### Fixed

- Frame pacing under `max_fps`: when frames fell more than a period behind,
  the deadlines started over a full period from now, so a slow frame was
  followed by a wait. They now start over from now: a late frame is never
  made later by a wait.
- ui2 TTF text is kerned as the font says: kerning now comes from HarfBuzz
  (GPOS as well as a legacy `kern` table), solved per pair from SDL_ttf's
  shaped widths and shared by every size of a font. Drawing used
  `TTF_GetGlyphKerning`, which reads only the legacy table, so fonts kerned in
  GPOS (Lato, EB Garamond, Noto Sans) drew unkerned and wider than they
  measured, by up to 41 px on a line at 37 px.

## [0.2.4] — 2026-10-06

A release for 2D drawing. Everything can be drawn through a transform, and
cameras zoom and turn; vector shapes are composed in code or read from SVG;
text stays sharp at any size; sprites mirror; anything can be clipped to a
path, a texture or anything drawn; and an opt-in linear colour pipeline blends
as light does, with HDR, tonemapping and LUT grading. Under them: layers,
cached targets, compute shaders, mipmaps and a faster SDL_GPU backend, plus
the render probe and the determinism check for finding what draws where and
what differs between runs.

### Upgrading from 0.2.3

- Children follow their parents' rotation and scale, not only their position,
  and renderer offsets, sizes and line ends go through the entity's world
  transform. Code that turned or scaled children by hand to make up for it
  should stop.
- The ECS physics sync converts angles (`Transform2D::rotation` is degrees,
  Box2D's radians) and works in world space for bodies under a parent. Games
  that converted angles themselves should drop the conversion.
- flecs is 4.0.5 (was 4.0.3): worlds in one process keep their own component
  ids. A project that pins flecs itself should move to 4.0.5.
- SDL's renderer now has layers: `begin_layer` draws into a target and applies
  its opacity, where it drew straight through before.
- `pop_clip()` pops whatever clip is on top, rectangle, path or mask; code that
  only pushes rectangles is unaffected.
- `IRenderer2DBackend` gains virtuals for shapes, distance fields, masks,
  stencil clips, layers and colour, each with a default, so custom backends
  build unchanged. `RenderCommandType` gains `Shape` and `Group`: a `switch`
  over it may warn until they are handled.

### Added

- Render probe: `--probe-render[=PATH]` on any `run_scene_app` game renders each
  headless frame, compares it with the frames before it tile by tile, and
  writes a `kin.render_probe/1` report of flicker (jitter, frame popping, single
  wrong frames) and spikes, with when and where each happened. `--probe-fail`
  fails the run when it finds anything; `--probe-tile=N` sets the tile size.
  `kin::RenderProbe` runs the same analysis on frames from anywhere. The
  `KIN_ENABLE_RENDER_PROBE` CMake option (on by default) compiles it out.
- Render probe events name their culprits: the entities and render components
  (or named scopes, or scenes) whose draws changed where the event happened,
  and how (`moved`, `frame`, `color`, `appeared`, ...). `KIN_DRAW_SCOPE(label)`
  and `KIN_DRAW_ENTITY(entity, component)` name draws made outside the ECS
  render components.
- Determinism check: `--check-determinism[=PATH]` on any `run_scene_app` game
  runs it three times in lockstep (twice alike, once with one job worker),
  hashing each frame's state, and reports the first frame where a run differs
  with the entities, components and report fields that differ
  (`kin.determinism/1`). `kin::hash_state` / `kin::describe_state` hash and list
  a scene stack's state. The `KIN_ENABLE_DETERMINISM_CHECK` CMake option (on by
  default) compiles it out.
- `KIN_JOB_WORKERS` sets the default job system's worker count.
- `ProcessOptions::environment` sets variables for a child process.
- `kin::CachedTarget` and `kin::cache_key(...)`: a render target drawn again
  only when its key or size changes (`kin/renderer/cached_target.hpp`). A
  288 x 288 target of 44 layers: 4.3 Mpixels and 0.5 ms of CPU a frame redrawn,
  0.08 Mpixels and 0.002 ms cached.
- `Renderer2D::begin_layer({.opacity, .resolution, .blend})`: draws land in a
  pooled render target in the same coordinates and are laid over once, at one
  opacity, over only the box they covered; `resolution` below 1 for soft
  content. Four sparse shadow layers on a 2560 x 1440 screen: 15.9 Mpixels a
  frame by hand with whole-screen targets, 2.3 with layers, 1.4 at half
  resolution. SDL_GPU.
- Overdraw by part and by eye: a `gpu_scope`'s pixels and overdraw
  (`GpuScopeTiming::pixels` / `overdraw`, `overdraw.<name>` in `--profile`), and
  the overdraw view (`Renderer2D::set_overdraw_view`, the debug overlay's toggle,
  `--overdraw-view`), which shows how many times each pixel is shaded as
  colours. `--screenshot=PATH` saves a run's last frame.
- Overdraw: `RendererBackendStats::last_pixels_drawn` and `last_overdraw`
  (SDL_GPU), reported as `render.overdraw` by `--profile` and the debug
  overlay: how many times each screen pixel was shaded in the frame.
- `ScaleMode::Mipmapped`: textures drawn smaller than they are get mipmaps,
  made on the GPU (and remade on update), sampled trilinearly: no shimmer, and
  faster to read (4000 sprites of a 2048 x 2048 texture at 24 x 24: about 30%
  less frame time). SDL_GPU, RGBA8 textures.
- Shader hot reload for development: `kin::ShaderFile` keeps a GLSL file
  compiled (with `glslc`, about 0.1 s) and reloaded in place when it is saved,
  keeping the last good version when an edit does not compile;
  `kin::compile_glsl()` and `Renderer2D::reload_shader()` underneath.
- Compute shaders: `Renderer2D::create_compute_shader` (layout and workgroup
  size read from the SPIR-V), `create_storage_texture` and `dispatch_compute`
  (`ComputeBindings`: sampled sources, read-only buffers, written textures and
  buffers, params), recorded in order with the frame's draws. SDL_GPU only
  (`capabilities().compute`).
- `Renderer2D::draw_shader_surface_scaled(resolution, ...)`: a costly, smooth
  effect computed at lower resolution into a pooled render target and
  stretched back (half resolution: about a third of the GPU time).
- Storage buffers for shaders: `Renderer2D::create_data_buffer`,
  `update_data_buffer`, `write_data_buffer`, and `draw_shader_surface` /
  `draw_shader_geometry` taking `DataBuffer`s, bound after the textures
  (`kin/renderer/data_buffer.hpp`). SDL_GPU only (`capabilities().data_buffers`).
- `Renderer2D::pipeline_record()` and `prewarm_pipelines(record)`: the GPU
  pipelines a run made, made again while the next run loads instead of at
  their first draw (up to ~20 ms each on a cold driver cache). `run_scene_app`
  keeps the record for windowed runs in the user data folder
  (`SceneAppConfig::pipeline_record_path`).
- Shader reflection: `create_shader` reads the SPIR-V's sampler, storage buffer
  and uniform block counts (a `ShaderDesc` that disagrees is logged, the
  shader's used), and `Renderer2D::shader_params(shader)` gives params that
  `set("name", ...)` by uniform block member. `kin::reflect_spirv` is public.
- `Renderer2D::write_texture(texture, at, size, fill)`: `fill` writes the
  texels straight into the upload memory on SDL_GPU, skipping the copy
  `update_texture` makes, for texels made each frame.
- `Renderer2D::gpu_scope("name")`: the GPU time of a part of a frame, reported
  as `gpu.<name>` by `--profile` and the debug overlay (and by
  `take_gpu_scope_timings()`). Measured with fences on each side, splitting the
  frame's submission there, while GPU timing is on; otherwise it does nothing.
- `Renderer2D::draw_shader_geometry()`: triangles drawn with a material shader,
  so only the pixels a shape covers run it, instead of a rectangle over its
  bounds. Each `kin::ShaderVertex` carries four free floats the fragment shader
  reads at `location = 2`, so one draw can hold many shapes with their own
  parameters. SDL_GPU only (`capabilities().shader_geometry`).
- `BlendMode::Max` and `BlendMode::Min`: per channel (alpha too), the larger or
  smaller of what is drawn and what is there. Overlapping shadows, fog of war,
  coverage and heat maps can be drawn shape by shape. The SDL_GPU backend has
  them; SDL's software renderer draws them as `Alpha` with a warning
  (`RendererBackendCapabilities::min_max_blend`).
- Frame times within `AppConfig::snap_tolerance` (1 ms) of a whole number of
  fixed steps count as exactly that many (`kin::FrameTimeSnapper`), so ordinary
  display jitter no longer runs some frames 0 updates and the next 2. The time
  snapped away is paid back a whole step at a time, so game time keeps up.
- `RendererBackendStats::gpu_frames_sampled` and `last_gpu_frame_span`: when
  new GPU timing arrived, and how many frames it covers.
- Transforms: `Renderer2D::push_transform` / `pop_transform` / `scoped_transform`
  draw through a `kin::Affine2` (`kin/core/affine.hpp`), on both backends.
  Mapped on the CPU, so draws still batch across transform changes, and sprite
  batches stay instanced under rotation and even scale. Through a zoomed,
  turned camera 20,000 quads take about 5% more CPU to record
  (`kin_draw_bench 300 20000 camera`); untransformed drawing costs what it did.
- `Camera2D::zoom` and `rotation`, about the viewport's centre, with
  `view_transform()`, `center()` and `look_at()`; `world_to_screen`,
  `screen_to_world` and `visible_rect` follow them, and a `RenderQueue` flushed
  with such a camera draws through it.
- `Transform2D::scale` and `WorldTransform::scale`; `kin::world_transform(entity)`
  and `kin::compose(parent, child)`.
- Vector shapes (`kin/renderer/path.hpp`, `shape.hpp`, `svg.hpp`):
  - `kin::Path` (lines, curves, SVG arcs; rectangles, circles, polygons, arcs,
    pies, stars; SVG path data read and written), filled under non-zero or
    even-odd with holes, stroked with joins, caps and miter limits;
  - anti-aliased at any scale: one pixel of soft edge from screen-space
    derivatives on SDL_GPU, pulled in on the CPU on SDL_Renderer;
  - circles, ellipses, rounded and sharp rectangles and capsules drawn whole
    on SDL_GPU (one quad, the outline from its distance per pixel), as you go
    or kept in meshes; 2000 cached tokens record in 0.88 ms (3.5 ms as
    triangles);
  - `kin::Shape`: elements that compose (`add(shape, transform)`), tessellated
    once into a `ShapeMesh` drawn by `Renderer2D::draw_shape` through any
    transform and tint;
  - SVG-lite: `read_svg` / `load_svg` read the flat-colour subset vector
    editors export (paths, basic shapes, groups, `<use>`, transforms, styles,
    inheritance), listing what they skip; `write_svg` / `save_svg` write it;
  - immediate drawing: `fill_circle`, `draw_circle`, `fill_ellipse`,
    `draw_ellipse`, `fill_polygon`, `draw_polygon`, `draw_polyline`,
    `draw_line` with a width and cap, `draw_arc`, `fill_pie`, `fill_path`,
    `stroke_path`;
  - `kin::ShapeRenderer` and `RenderQueue::draw_shape`; `games/shapes_demo`.
  - New dependency: mapbox earcut.hpp 3.2.4 (ISC), header-only, for fills.
- Text that scales: TTF fonts made with `ui2::TextRendering::Sdf`
  (`load_ttf_font`, `system_ui_font`, `system_ui_font_bold`) draw from one
  atlas of signed distance fields, sharp at any size, zoom or turn, under any
  transform or camera; their metrics scale linearly. The atlas is made once per
  renderer from glyphs drawn at twice the size, distance-transformed on the job
  system (about 15 ms). `ui2::draw_text_outlined` draws an outline from the
  field in one pass (four stamped copies on other fonts); widgets'
  `TextStyle::outline_width` uses it. `Renderer2D::draw_distance_field` draws
  any distance-field texture. SDL_GPU (`capabilities().distance_fields`);
  drawn as Bitmap elsewhere.
- Clips and masks: `Renderer2D::push_clip(path, rule)` clips to any path,
  anti-aliased, under the transform; `push_mask(draw, options)` masks with
  whatever `draw` draws, and `push_mask(texture, dest, options)` with a texture.
  `kin::MaskOptions` (`kin/renderer/mask.hpp`) reads a mask's alpha or
  luminance, as a soft `Alpha` mask or an all-or-nothing `Stencil` at a
  threshold, optionally inverted, at a fraction of the resolution. Rectangles,
  paths and masks share one stack, nest, and pop with `pop_clip()`;
  `scoped_clip` and `scoped_mask` return guards. SDL_GPU composites in a shader
  (`capabilities().masks`); SDL's renderer by blending, or on the CPU for
  luminance and stencil masks and its software renderer. The shapes demo gives
  its planets night sides and a telescope following the rocket.
- Hard clips: `push_clip(path, rule, kin::ClipEdge::Hard)` keeps whole pixels;
  on SDL_GPU (`capabilities().stencil_clips`) it draws into a pooled stencil
  buffer instead of layers, about five times cheaper than a smooth clip, and
  stencil-mode masks read their mask into it, drawing what they mask straight
  on.
- `kin::ClipRegion` (`kin/renderer/clip.hpp`): a clip as a value (a path, a
  mask, a texture), pushed with `Renderer2D::push_clip(region)`.
- `RenderQueue::draw_group(key, content, clip)`: a queue drawn as one command
  at `key` through a clip, so clipped content keeps together in a sorted queue.
- `kin::ClipGroup`: clips an entity's renderers and its descendants' to a
  region in its own space, drawn as one group at its layer and order.
- `Path::rounded_rect(rect, radii)` with a radius per corner;
  `ui2::Context::push_clip(bounds, corner_radii)`, used by lists, tables and
  trees to keep scrolled rows inside their panel's rounded corners.
- Layers on SDL's renderer: `begin_layer` draws into a target and lays it over
  at its opacity, as on SDL_GPU, instead of drawing straight through.
- Colour: `Renderer2D::set_color_space(ColorSpace::Linear, hdr)` blends in
  linear light (sRGB colour textures decoded as sampled, colours decoded in the
  vertex shaders, sRGB or float targets) and encodes on the way out; `hdr`
  keeps light above white in a 16-bit float scene. Opt-in, also as
  `GameWindowInfo::color_space` / `hdr`; the gamma pipeline stays the default.
  `set_color_output(ColorOutput)`: exposure, tonemapping (`Reinhard`, `Aces`),
  grading through a 3D LUT cross-faded to a second, and dithering, in one pass
  before the swapchain (about 0.13 ms at 1080p). SDL_GPU
  (`capabilities().linear_color`, `color_output`).
- `kin::ColorLut` (`kin/renderer/color_grading.hpp`): LUTs from `.cube` files
  and PNG strips or grids, a neutral one to grade in an image editor, applied
  on the CPU too.
- Colour maths: `srgb_to_linear`, `linear_to_srgb`, `LinearColor`, OKLab
  (`to_oklab`, `from_oklab`), and `mix(a, b, t, ColorMix)` in sRGB, linear
  light or OKLab; `Gradient::mix`.
- The lighting demo draws in linear HDR, tonemapped (ACES) and graded per
  time of day; C switches to the gamma pipeline, G toggles grading.
- Mirrored sprites: `SpriteRenderer::flip_x` / `flip_y` and
  `TextureRenderer::flip_x` / `flip_y`, mirrored about the pivot; a negative
  world scale mirrors too (and cancels a flag). `kin::Flip` on
  `Renderer2D::draw_texture` / `draw_sprite`, the `RenderQueue` texture and
  sprite calls, and `SpriteInstance`; flipped sprites still batch, on both
  backends. Animatable as `SpriteRenderer.flip_x` / `flip_y`.

### Changed

- SDL_GPU shader draws with the same params (and shader, sources, state) in a
  row are one draw call, not one each: 2000 small shader surfaces a frame went
  from 0.47 to 0.24 ms back to back (`kin_draw_bench 60 1 surfaces`).
- SDL_GPU `create_shader` builds the shader's usual pipeline (alpha blend, an
  RGBA8 target) at once instead of at its first draw: on a cold driver cache
  that moves a ~19 ms hitch from the first frame drawing it to load time.
  Pipelines made later are logged at debug level with their cost.
- SDL_GPU quads with the default shader append straight to the last draw when
  its state matches, and the clip rectangle is computed once per change, not
  per draw: `fill_rect` about 12% less CPU, `draw_texture` 4%. `kin_draw_bench`
  measures `draw_texture`, `fill_rect` and `draw_sprites`.
- SDL_GPU sends a third less geometry: quads (textures, rects, lines) go as
  four corners drawn through a static index buffer, not six vertices (hex_demo
  189 to 126 KB a frame, same draw calls). An empty texture
  (`create_texture` with no pixels) is cleared on the GPU instead of being sent
  zeros, where its format can be a render target.
- SDL_GPU texture uploads (`create_texture`, `update_texture`) no longer submit
  a command buffer each: a frame's uploads are recorded into one, sent ahead of
  the frame, from a shared staging buffer that grows to fit big uploads and
  shrinks back. Texels are copied with streaming stores, and on the renderer's
  job system for uploads of 1 MB or more. Replacing a whole texture the frame
  hasn't drawn yet cycles its storage. Released textures are pooled (a few
  seconds, up to 256 MB) for `create_texture` to reuse. 150 uploads of 256 KB
  in a frame went from about 25 ms of calls plus a 70 ms stall at present to
  2.1 ms. `RendererBackendStats::texture_uploads` and `texture_upload_submits`
  count them; `kin_upload_bench` measures them.
- `--profile` (and `--profile-json`, `--profile-text`, `--profile-lines`) no
  longer makes a run headless: it profiles the window, until the game quits or
  `--frames` N. For the old 600-frame headless pass add `--headless`.
  (`--profile-render` is still a headless pass.)
- `gpu.frame` is recorded only on frames with a new GPU sample, instead of
  repeating the last one; a sample taken after untimed frames (the GPU far
  behind the CPU) is their average rather than their sum.
- Children follow their parents' rotation and scale: `propagate_transforms()`
  (and `world_position()`) turn and scale a child's position by its parents'.
  Before, only positions and angles were added, so a child of a turned parent
  stayed put while the parent turned.
- Renderer offsets, sizes and line ends go through the entity's world
  transform; `TextureRenderer` and `RectRenderer` now turn with their entity
  (they ignored `rotation`), and queued `FillRect` / `DrawRect` commands honour
  `rotation` and `pivot`.
- ui2 lists, tables and trees keep their scrolled rows inside their panel's
  rounded corners (`Context::push_clip(bounds, corner_radii)`).

### Fixed

- ui2 text moves the pen by each glyph's advance, not the width of its bitmap:
  italic text is no longer letter-spaced, glyphs that overhang (an f or a j in
  many faces) no longer push the next one away, and drawn text matches
  `measure_text` more closely.
- SDL_GPU: a texture released right after it was drawn, before the frame was
  flushed, left the queued draw a dangling handle: a crash, or on Vulkan a lost
  device once SDL destroyed the image. The backend now keeps the textures the
  frame's draws use alive until the frame is submitted.
- SDL_GPU: GPU frame timing (`--profile`, `KIN_LOG_FRAME_STATS`, the debug
  overlay) waited on frame fences with `SDL_WaitForGPUFences` on its own thread,
  which also ran SDL's resource cleanup there; under load on Vulkan that lost the
  device or stalled frames for seconds. It now polls `SDL_QueryGPUFence`, a plain
  status read (resolution about 0.1 ms).
- SDL_GPU: with GPU frame timing on and the GPU more than 8 frames behind (a
  heavy scene, no vsync), each further frame's fence was released while the
  frame still ran. SDL put it back in its pool and reset it for the next
  submission, whose resources it then freed when the old frame finished: on
  Vulkan, a lost device a few seconds into `--profile`. Those frames are now
  submitted without a fence (and go untimed).
- Worlds in one process can register components in any order: flecs 4.0.5
  (from 4.0.3) keeps C++ component ids per world. Before, a component first
  registered in a second world could take the id another type had in the
  first, and setting that type wrote past the component's storage (the
  prefab tests crashed once `Transform2D` grew). The animation preview and the
  tests run several worlds.
- Physics bodies under a parent: the ECS sync works in the world and converts
  to and from the parent's space. A simulated body (`sync_from_physics`) stays
  put in the world when its parent moves; one the entity leads
  (`sync_to_physics`) follows its parent. Bodies under bodies are written
  parents first. Before, world positions were written into the
  parent-relative `Transform2D`. `kin::to_local(parent, world)` (the reverse of
  `compose`) and `kin::current_world_transform(entity)` support it.
- The ECS physics sync converts angles: `Transform2D::rotation` is degrees, as
  everywhere in kin, and `PhysicsWorld` radians, as Box2D. Before, radians were
  written into `rotation`, so physics bodies' sprites barely turned.
- `fill_rounded_rect` and `draw_rounded_rect` on SDL_Renderer: a border drew
  as a faint one-pixel line instead of its width. Both now draw as shape
  primitives on every backend.
- The render probe traces draws where the renderer's transform puts them,
  not where they would be without it.
- Bitmap TTF fonts drawn at a scale that changes every frame opened a font face
  and built a glyph atlas for every value, kept for good: now the most recent 12
  faces and 8 atlases are kept.
- SDL_Renderer: a texture kept past its renderer (as the system fonts' static
  cache keeps glyph atlases) no longer frees a texture of the next renderer
  when it goes; the examples crashed on Windows.

## [0.2.3] — 2026-10-01

Tools for building a game around its data: a game's own files hot-reload as
they are edited, Lua runs rules and formulas outside the ECS in a deterministic
sandbox, and ui2 themes load from `.kintheme` files. Games also get files
dropped on the window, the system's file dialogs and child processes without
calling SDL, an optional frame rate cap, and GPU frame timing in the profiler.

### Upgrading from 0.2.2

- A windowed run given `--frames N` now quits after N rendered frames. Scripts
  that passed `--frames` to a windowed run and expected it to keep going should
  drop the flag.
- `KIN_LOG_FRAME_STATS` reports fps from the time between frames, so it reads
  lower than before when the GPU or a frame rate cap holds frames back.
- Voice-limit culling no longer logs a warning per request; read
  `AudioEngine::stats().culled_requests`, or log at debug level, to see it.
- `IRenderer2DBackend` gains `set_gpu_timing_enabled()`, with a default that
  does nothing, so custom backends keep working unchanged.

### Added

- `kin::FileWatcher` hot-reloads a game's own data files: `load_and_watch(path,
  load)` loads a file and reloads it when it changes, keeping the last good data
  when an edit is rejected; `watch(path, callback)` reports changes. Changes are
  found by polling (at most every 250 ms) and reported once a file has settled,
  so partial writes and save-by-rename are seen once. `SceneAppConfig::file_watcher`
  has `run_scene_app` poll it between frames, except in headless and server runs.
- `kin::LuaScript` (`kin/scripting/lua_script.hpp`) runs Lua for code outside
  the ECS (rules, AI, formulas): the host binds an API in `setup`, then
  `call(name, args...)` / `call_for<R>(...)` run the script's functions. A
  deterministic sandbox (no files, time, random numbers or loading code;
  `require` only from `module_root`; `print` to the log), an instruction budget
  per load and call, loads that keep the last good script when a new one fails,
  errors with script:line, and hot reload of the script and its modules through
  `FileWatcher`. New `docs/scripting.md`.
- Desktop integration without calling SDL (`docs/platform.md`): files dropped
  on a window (`Input::take_dropped_files()`, `drop_position()` while a drag
  hovers, `add_dropped_file()` for tests); the system's file dialogs
  (`kin::FileDialogs`: open, save, folder; answers taken on the game's thread,
  never shown in headless runs, `answer_next()` for tests and agents); and child
  processes with non-blocking pipes (`kin::Process`: `write`, `read_line`,
  `close_input`, `running`/`exit_code`, `wait`, `kill`; a child still running
  when its `Process` goes away is ended).
- An optional frame rate cap: `AppConfig::max_fps` / `WindowedAppConfig::max_fps`
  (0, the default, is uncapped), `App::set_max_fps()` to change it while running
  (e.g. from a settings menu), and `--max-fps N` on any kin game. Frames are
  paced to deadlines with `SDL_DelayPrecise`, so the rate holds without drift,
  and a frame that runs late resets the pace instead of rushing the next ones.
  Headless runs are never paced. `AppFrameStats::pacing_wait` and the profile's
  `app.pacing_wait` show the time slept.
- ui2 themes from data files (`kin/ui2/theme_file.hpp`, `.kintheme`): start
  from a built-in theme, name the game's own colours (usable anywhere a colour
  goes, with `@aa` alpha), set kin's colour tokens and sizes (the rest of the
  theme follows the tokens), style any surface (fill, gradient, border, radius,
  shadow, `like` another), and ask for procedural nine-slice skin frames.
  Unknown sections, keys and names are errors with their line, and a file with
  errors changes nothing; `parse_theme_file` fits `FileWatcher::load_and_watch`.
  `make_theme_from_tokens` is now public.
- GPU frame timing on the SDL_GPU backend: `gpu.wait` (time `present()` blocked
  on a swapchain image, i.e. waiting for the GPU) and `gpu.frame` (each frame's
  GPU time, measured with fences since SDL_GPU has no timestamp queries) in
  profiles, the debug overlay and `KIN_LOG_FRAME_STATS`, and as
  `RendererBackendStats::last_gpu_wait_ms` / `last_gpu_frame_ms`. Timing is on
  while something reads it, or via `Renderer2D::set_gpu_timing_enabled()`. The
  backend now also reports `present.flush` and `present.backend`.

### Changed

- `KIN_LOG_FRAME_STATS` reports fps from the time between frames, which counts
  waiting on the GPU and the frame rate cap, instead of from the frame's work
  alone (which overstated it); `frame_ms` is still the work.
- A windowed run given `--frames N` now quits after N rendered frames, running
  in real time with the frame rate cap; it used to ignore `--frames` unless the
  run was also made headless (`--report`, `--profile`).
- Audio requests culled by the voice limits are logged at debug level (still
  counted in `stats().culled_requests`) instead of a warning per request, which
  flooded the log of busy games.

## [0.2.2] — 2026-09-30

Large sprite counts get far cheaper: sprites that share a texture are drawn as
instanced batches on the GPU, the render queue stores plain sprites compactly,
and ECS sprite collection and batch building can use worker threads, with the
same results as one thread. Signal Siege, reworked with a title screen, sound,
new art and a 100,000-enemy stress mode, goes from 48 ms to about 12 ms a frame
at 100,000 enemies on the GPU.

### Upgrading from 0.2.1

- `RenderQueue` flushes hand runs of consecutive same-texture sprites to the new
  `IRenderer2DBackend::draw_sprites()`. Its default draws them one by one through
  `draw_texture()`, so custom backends keep working unchanged.
- `RenderQueue` stores plain Texture and Sprite commands compactly;
  `commands()`, `sort_commands()` and the presorted flushes convert them back on
  first use, so readers see the same commands, in submission order. As before,
  a span from `commands()` is invalidated by further submissions.
- `WorldRenderState::propagate_transforms()` updates parents before children in
  one pass, so a child no longer sees its parent's previous-frame transform when
  the child was created first.
- `WorldRenderState::collect_*` cull rotated sprites as they are submitted, by
  the circle they can turn within; a queue collected with a view may keep a few
  more rotated sprites near its edges than before (flushes still cull exactly).

### Added

- `Renderer2D::draw_sprites(texture, sprites)` draws many quads from one texture
  in one call; the SDL_GPU backend draws them as one instanced batch
  (`SpriteInstance`, `sprite_instanced.vert`), other backends quad by quad.
  `RenderQueue` flushes hand it every run of consecutive same-texture sprites, so
  ECS sprites batch automatically. `Texture` handles compare with `==`.
- `AudioEngine::add_clip` registers an in-memory clip (synthesized, or from
  `make_memory_audio_clip`) under a clip id that cues name like a file-backed one.
- Render queue and threading APIs: `RenderQueue::draw_texture_region()`,
  `append_sprites()` (with `PreparedSprite`), `reserve_sprites()` /
  `texture_index()` / `write_sprite()` for producers on several threads,
  `submitted()` and a ranged `cull(view, first, last)`;
  `SpriteRenderOptions::jobs`; `Renderer2D::set_job_system()` and
  `IRenderer2DBackend::set_job_system()`.

### Changed

- The parallel ECS sprite collect writes sprites straight into the queue
  (`RenderQueue::reserve_sprites()`, `texture_index()`, `write_sprite()`)
  instead of preparing them on workers and appending them on one thread: about
  3–4× faster than a single thread for 100,000 sprites, where the previous
  version was slower than one. Chunks are smaller (1,024 rows) so fast cores take
  over from slow ones. `RenderQueue` sorts in 11-bit radix passes and shifts
  queued sprites into the view with the camera offset looked up once. Signal
  Siege at 100,000 enemies: about 8.4 ms a frame on the GPU against 9.5 ms for
  main (same session); its sprite collect 1.1 ms against 1.9.
- Large sprite workloads use worker threads when given a `JobSystem`:
  `SpriteRenderOptions::jobs` prepares big `TextureRenderer` tables in parallel
  chunks (queued in row order, via the new `RenderQueue::append_sprites()`), and
  `Renderer2D::set_job_system()` lets the SDL_GPU backend fill large
  `draw_sprites()` batches in parallel. Results are identical to a
  single-threaded run. Signal Siege passes its arena's workers, and its art finds
  the enemies near the view, their shadows and the few showing effects in one
  (parallel) pass. At 100,000 enemies a GPU frame took about 13.6 ms against
  19.7 ms for main in the same session; at 10,000, 1.85 ms against 2.39.
- `RenderQueue` keeps plain sprites compactly (no per-sprite `RenderCommand` or
  texture reference) and flushes them straight into sprite batches; its radix
  sort skips key words that never vary. Collecting and flushing 100,000 ECS
  sprites takes about 9.5 ms on the CPU, down from 16.6, and Signal Siege at
  100,000 enemies about 15.9 ms a frame on the GPU, down from 20.2. `commands()`
  and `sort_commands()` still show every command (converting queued sprites on
  first use); `cull(view, first, last)` now counts submissions (`submitted()`),
  not positions. New `draw_texture_region()` queues part of a texture.
- Signal Siege draws its drop shadows with one `draw_sprites()` call. With the
  enemy atlas, a frame at 100,000 enemies takes about 19.7 ms on the GPU, down
  from 24.7, and 2.4 ms at 10,000, down from 2.9.
- `WorldRenderState::propagate_transforms` visits parents before their children
  (a cascaded query) and does no per-entity parent lookup, so a flat world of
  100,000 sprites propagates in about 0.13 ms; nested hierarchies are now updated
  correctly whatever order their entities were created in.
- `WorldRenderState::collect_*` cull each sprite as it is submitted, rotated ones
  by a bounding circle, and cull only the particles (and commands already in the
  queue) afterwards, instead of re-checking the whole queue. `RenderQueue::cull`
  takes an optional command range.
- Signal Siege's three enemy sprites share one texture (an atlas), so the
  y-sorted enemies batch into a few GPU draws instead of one per texture change
  (about 28,000 a frame at 100,000 enemies, now 21): a frame there takes about
  25 ms, down from 29, and 2.9 ms at 10,000 enemies, down from 3.3.
- Signal Siege scales to 100,000 enemies (`--enemies` up to 100,000, and a
  `kin_bench` case `arena_100000_encounter`): the art gathers the enemies near the
  view once per frame for shadows, lights and effects; collisions, respawns and
  the autopilot read slot-indexed positions instead of looking each enemy up; the
  movement pass runs on worker threads for large arenas, with identical results;
  enemies are created and retextured in deferred batches. Simulation checksums are
  unchanged. At 100,000 enemies on the GPU a frame takes about 29 ms, down from
  48; at 10,000, about 3.4 ms, down from 5.3. New profiler zones
  (`--profile-lines`) split the frame.
- Signal Siege's sprites and floor are more detailed: sprites are rasterized at
  twice their drawn size and shaded as bevelled metal with seams, rivets and
  glowing eyes; ships cast soft drop shadows; the deck mixes four tile variants
  (plain, grate, hazard paint, patched) and has hazard strips along the arena's
  edge; reactors are bolted housings with turning fans. The colour roles are
  unchanged.
- Signal Siege reads more clearly: cyan is only the player (drawn over every
  effect, with a ground ring), enemies are solid warm shapes drawn after the
  lighting and without glow, enemy bullets are round white-cored pink orbs, an
  orbiter about to fire shows a closing ring, cores are small and blink before
  expiring, and the floor, lights, particles and bloom are toned down. The HUD's
  countdown gets a panel.
- Signal Siege has a title screen over an autoplaying arena, a results screen
  whose counters roll up, and scene transitions between them; sound effects and an
  ambient drone synthesized at startup and played with `AudioEngine`; a HUD built
  from ui2 widgets with a wave banner animated by property tracks; and the best
  run kept with `SaveStore`. Headless, benchmark and screenshot runs still start
  straight in the arena. `--mute` silences it.
- The Signal Siege example has a new look that shows off more of kin, still
  without asset files: procedurally rasterized sprites (enemies drawn by the ECS
  through `TextureRenderer`), a lit arena with `LightLayer` (a flashlight cone,
  reactor lamps, lights on shots and impacts), `ParticleSystem` effects driven by
  the arena's events, and a bloom, vignette and grade chain on the GPU backend.
  The simulation and the `kin_bench` arena cases are unchanged.

### Fixed

- An `AudioEngine` on the silent (null) backend advances its voices by elapsed
  time. It used to mix only until its queue target was reached, so one-shot sounds
  never finished and, once every voice and instance limit was taken, new ones were
  refused.

## [0.2.1] — 2026-09-30

A multi-line text editor for ui2, faster render queues, tilemaps, ECS inspection
and headless rendering, and a fix for GPU games crashing on exit.

### Upgrading from 0.2.0

- `RenderCommand` fields changed: a Sprite command keeps its texture in
  `texture` and its region in `source` (the `sprite` member is gone), and
  `material` is a `const Material2D*`. Only code that builds or reads commands
  directly is affected; `RenderQueue`'s methods are unchanged.
- After `RenderQueue::flush`, `commands()` keeps submission order; call
  `sort_commands()` first if you need the commands themselves sorted.
- `Input::consume_frame_edges` also clears typed text and the wheel delta.
- TTF text drawn from the glyph atlas is a little narrower (true advances and
  kerning), matching `measure_text`; layouts placed by eye may shift a pixel or
  two.

### Added

- `Renderer2D::id()`: unique for the life of the process, never reused.
- `kin_bench` cases `render.submit_flush_sprites_{2k,8k,32k}`: submit and
  flush, the everyday path.
- `RenderQueue::submit(std::span<const RenderCommand>)` appends a batch of
  commands, such as a cached one, in order.
- `ui2::TextEdit`: a multi-line text editor with wrapping, scrolling, mouse and
  keyboard selection, clipboard, grouped undo, a submit key and auto-grow; with
  `read_only` it is selectable, copyable text. Its state can live in the widget
  or in the `Context` (`Context::text_edit_state`). See docs/ui.md.
- `kin/core/utf8.hpp`: character-safe stepping (`utf8_next`, `utf8_prev`,
  `utf8_floor`), word stepping and `normalize_newlines`.
- `ui2::wrap_text_ranges`: wraps text into byte ranges, keeping every byte.
- `Key::PageUp` and `Key::PageDown`; `Input::frame_repeated` and
  `Context::key_typed` for the OS's key repeat; `Input::set_key_pressed`,
  `set_key_released` and `set_key_repeated` for tests and scripting.
- Scene server `input.key {name, mode?, modifiers?}` presses keys by name with
  modifiers held, e.g. `{"name":"Enter","modifiers":["shift"]}`.
- `games/text_edit_demo`: a chat box, a read-only history and a notes pane.

### Changed

- `RenderQueue::flush` draws in sorted order without moving the commands, so
  `commands()` keeps submission order after a flush. Call `sort_commands()`
  first if you need the commands themselves sorted.
- Text drawn from a TTF glyph atlas (printable ASCII) uses the font's advances
  and kerning. Glyphs narrower than a space no longer take a space's width, so
  such text is a little narrower and now as wide as `measure_text` reports.
- `Input::consume_frame_edges` also clears typed text and the wheel delta.
- `RenderCommand` is 152 bytes instead of 216. Code that builds or reads
  commands directly needs updating:
  - Sprite commands keep their texture in `texture` and their region in
    `source`; the `sprite` member is gone.
  - `material` is a `const Material2D*`. `RenderQueue`'s `MaterialRef`
    parameters are unchanged; only their `material` pointer is kept.
  - `RenderCommandType` is a `u8` enum.

### Performance

- Render queue sorting is 2–2.5× faster: keys are radix sorted, a queue that
  has not changed since its last sort is not sorted again, and `flush` skips
  reordering the commands. Submitting and flushing 32k sprites takes 4.2 ms
  instead of 9.7 ms.
- Smaller render commands make submitting 10–28% faster. Tilemaps also reserve
  queue space for the visible cells and hand their cached chunks over in one
  bulk submit: a partial-view submit takes 0.56 ms instead of 1.7, a full
  uncached one 6.3 ms instead of 10.2, and both allocate half as much.
- The software renderer, used for headless runs and screenshots, draws
  line-heavy scenes about twice as fast: while the logical size equals the
  window it leaves SDL's logical presentation off (with it on, SDL draws every
  line as triangles and blends each through a scratch surface), turning it back
  on when a resize makes them differ. Untextured draws whose colors are all
  opaque also skip blending. The hex demo renders a frame in 5.3 ms instead of
  12.6, `signal_siege` in 5.7 instead of 7.6. Thin translucent lines may
  rasterize a pixel differently.
- ECS inspection is 5× faster: `query_entities` and `EcsWorld::snapshot()`
  read each entity's parent and name once instead of on every comparison while
  sorting. A query matching 4.5k of 10k entities takes 1.7 ms instead of 9 ms.
- `TileMap::collision_rects` finds the rect to merge each run into with a
  lookup instead of scanning every rect found so far: 56× faster on a dense
  layer (101 ms to 1.8 ms), with identical output.

### Fixed

- `TextInput` moves and deletes by character, so multi-byte characters such as
  "é" or "°" no longer break. Ctrl+Left/Right move by word, Ctrl+Backspace and
  Ctrl+Delete delete one, held keys repeat, and pasted line breaks become spaces.
- The scene server's `view.screenshot` no longer replays the last step's input
  into a scene that reads input in `render()`: typed text was entered twice.
- GPU games no longer crash or hang on exit. Textures that outlive their
  renderer, such as the glyph atlases in the static system-font cache, skip
  their release once the GPU device is destroyed instead of calling into it.
- Font caches key on `Renderer2D::id()` rather than the renderer's address, so
  a new renderer created where an old one lived no longer draws text with the
  old renderer's textures.

## [0.2.0] — 2026-09-30

Features the first game built on kin needed: hex grids, 2D lighting, a job
system shared by the whole engine, and shaders with more textures, larger
uniform blocks and data textures. CI now also runs the GPU backend's tests.

### Upgrading from 0.1

- `ShaderParams::uniforms` is a `std::vector<f32>`: code that indexes it is
  unchanged; code that relied on it being a `std::array` needs updating.
- The ECS scheduler, `AssetServer` and `PathServer` share
  `default_job_system()`. `worker_count` in `AssetServerConfig` and
  `PathServerConfig` now limits concurrent work rather than starting threads.
- Destroying an `AssetServer` or `PathServer` drops work that has not started.
- A scripted `press` is released at the start of the next tick, so games see
  its release edge one tick later than before.

### Added

- `draw_shader_surface()` takes a list of source textures, bound at fragment
  sampler slots 0, 1, 2, ... up to `MaxShaderSamplers` (16); previously at most
  two.
- `JobSystem` (`kin/core/jobs.hpp`): background jobs with `Job<T>` handles,
  results applied in submission order by `pump()` / `drain()`, cancellation
  through `std::stop_token`, and `parallel_for`. Background jobs use at most
  part of the workers, so parallel loops always have threads.
- Hex grids: axial coordinates, directions, distance, rotation, rings, ranges
  and lines (`kin/core/hex.hpp`); `HexLayout` for pixel and offset-cell
  conversion in pointy-top and flat-top, odd and even offset layouts.
- Hex pathfinding: `HexGridNav`, `find_path` (A*) and `reachable_cells`
  (movement range).
- `games/hex_demo`, and a hex grids guide (`docs/hex_grids.md`).
- 2D lighting: `LightLayer` multiplies a drawn scene by an ambient colour plus
  `Light2D` point lights, optionally shaped by a texture (cones, spotlights).
  Works on the software and GPU backends without shaders.
- `games/lighting_demo`: lamps, a campfire, a flashlight and a day/night cycle.
- Data textures for shaders: `create_texture(size, format, pixels)` with
  `TextureFormat::R16Uint`, `Rg16Uint` and `R32Float` (on backends with
  `capabilities().data_textures`); `update_texture()` writes in the texture's
  format.
- Shader uniform blocks up to `MaxShaderUniformFloats` (4096 floats, 16 KiB).
- `frame.timing` server command: update and render time over the last 600
  ticks.
- ui2 tooltips warm up: once one has shown, the next shows at once
  (`TooltipOptions::warm_frames`).

### Changed

- `ShaderParams::uniforms` is a `std::vector<f32>` (16 floats by default)
  instead of a `std::array<f32, 16>`. Indexing it is unchanged.
- On the SDL_GPU backend, sampler slots a material shader declares but a draw
  leaves empty are bound to a white texture instead of being left unbound.
- The ECS scheduler, `AssetServer` and `PathServer` run on one shared job
  system, `default_job_system()`, instead of each starting its own threads.
  `set_default_job_system()` replaces it; each also accepts its own pool. The
  `worker_count` settings now limit how much work each runs at once.
- `AssetServer` and `PathServer` drains do queued work on the calling thread
  while they wait. Destroying either drops work that has not started instead
  of finishing it first.
- Every `GameInfo` member has a default, so designated initializers can omit
  any without `-Wmissing-field-initializers` warnings.

### Fixed

- A scripted `press` (`input.mouse`, `input.action`) is released at the start
  of the next tick instead of at the end of its own, so the release is seen and
  ui2 clicks complete. Before, a pressed button never produced a click.

## [0.1.0] — 2026-09-28

First public release.

### Added

- ECS on flecs: native and data components, prefabs, events, inspection, and a
  system scheduler with dependency graphs and parallel batches.
- SDL3 2D rendering: sprites, tilemaps (orthogonal and isometric), particles,
  post-processing, and an animation system with state machines.
- `ui2` immediate-mode UI with widgets, layout, and themes.
- Box2D physics, Lua scripting (sol2), audio, pathfinding (including the
  threaded `PathServer`), dialogue, save data, and hot-reloadable assets.
- Agent interface shared by every `run_scene_app` game: deterministic headless
  runs with JSON reports, a line-JSON/HTTP control server, screenshots, and
  profiling.
- Demos (`games/`), the Signal Siege and Run Observatory examples, `kin_bench`,
  and the engine test suite.

[Unreleased]: https://github.com/AmedeoBiolatti/kin/compare/v0.2.6...HEAD
[0.2.6]: https://github.com/AmedeoBiolatti/kin/compare/v0.2.5...v0.2.6
[0.2.5]: https://github.com/AmedeoBiolatti/kin/compare/v0.2.4...v0.2.5
[0.2.4]: https://github.com/AmedeoBiolatti/kin/compare/v0.2.3...v0.2.4
[0.2.3]: https://github.com/AmedeoBiolatti/kin/compare/v0.2.2...v0.2.3
[0.2.2]: https://github.com/AmedeoBiolatti/kin/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/AmedeoBiolatti/kin/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/AmedeoBiolatti/kin/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/AmedeoBiolatti/kin/releases/tag/v0.1.0
