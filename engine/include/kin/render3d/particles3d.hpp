#pragma once

// 3D particles and weather for kin's experimental 3D (docs/3d_plan.md, P3c). Plain data, pure functions and a deterministic
// simulation on the CPU; Renderer3D::render() draws the result (one instanced draw a system) and the same arithmetic is here
// for the tests to compare the pictures with.
//
//     kin::ParticleSystem3D fire{7};                         // seeded: the same seed, the same sparks
//     fire.desc.rate = 400.0f;
//     fire.desc.shape = kin::ParticleShape3D::Cone;
//     fire.transform = kin::Mat4f::translation({0.0f, 0.2f, -4.0f});
//     fire.update(dt, &scene.weather.wind);                  // fixed steps inside: frame-rate independent
//     scene.draw_particles(fire);                            // each frame, like a mesh
//
//     scene.weather.rain.intensity = 0.7f;                   // a camera-following volume of rain, a function of
//     scene.weather.time += dt;                              // (time, camera): no state, nothing to save
//
// Two things here. `ParticleSystem3D` is a simulated emitter (sparks, fire, smoke, a burst): state you step, save and hash.
// `Weather3D` is rain, snow, dust and mist as a pure function of a clock and the camera: every particle's place is a formula
// of its number and the time, wrapped into a box that slides with the camera in world space, so that nothing swims.

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/render3d/camera3d.hpp>
#include <kin/renderer/texture.hpp>

#include <functional>
#include <span>
#include <vector>

namespace kin {

struct Lights3D;
struct Sky3D;
struct Fog3D;

// A number drawn between `min` and `max` (the same idea as the 2D particles' ParticleRange).
struct ParticleRange3D {
    f32 min = 0.0f;
    f32 max = 0.0f;
};

// Where an emitter's particles are born, in the emitter's own space (its `transform` places it; the emission axis is +Y).
enum class ParticleShape3D : u8 {
    Point,  // one place; directions spread by `spread`
    Box,    // inside a box of half-sizes `extent`
    Sphere, // inside a ball of radius `extent.x` (or its surface); moving away from its centre
    Cone,   // from a disc of radius `extent.x`, directions within `cone_angle` of +Y
    Disc,   // inside a disc of radius `extent.x` in the XZ plane; directions spread by `spread`
};

// How a particle's colour goes over what is behind it.
enum class ParticleBlend3D : u8 {
    Alpha,         // over, by its alpha (kin's premultiplied targets: colour * alpha added to what is left under 1 - alpha)
    Additive,      // adds light: sparks, fire, glows. Needs no sorting.
    Premultiplied, // a texture that already holds colour * alpha: alpha 0 with colour is additive, in the same sprite
};

// How a particle's quad is turned.
enum class ParticleFacing3D : u8 {
    Camera,    // faces the camera, turned by `rotation` about the view direction
    Stretched, // along its velocity (a streak: rain, sparks); its head at the particle, its tail behind it
    Axis,      // turns about `ParticleLook3D::axis` to face the camera (a column of smoke, a tree-like sprite)
    Flat,      // lies in the plane whose normal is `axis`, turned by `rotation` about it (a ring on the ground)
};

// A wind, shared by the particle systems and the weather: a mean velocity along `direction`, gusts (the speed varies with
// time and, as they travel with the wind, with place) and a small swirling turbulence. The gusts and the turbulence are
// sums of sines (three each), so that a point's path through them has a closed form (`displacement`) and the weather needs
// no state; `velocity` and `displacement` agree to the integration's error.
struct Wind3D {
    Vec3f direction{1.0f, 0.0f, 0.0f}; // any length but zero (zero: no wind)
    f32 speed = 0.0f;                  // m/s, the mean along `direction`
    f32 gust_strength = 0.0f;          // the speed varies by this share of itself (0.5: from half to one and a half times)
    f32 gust_period = 6.0f;            // seconds of the slowest gust (the faster ones are 0.43 and 0.19 of it)
    f32 gust_length = 60.0f;           // metres from one gust to the next along the wind (gusts travel with it)
    f32 turbulence = 0.0f;             // m/s of swirl, any direction
    f32 turbulence_length = 5.0f;      // metres: how big the swirls are

    // The air's velocity at `position` and `time`.
    Vec3f velocity(Vec3f position, f64 time) const;
    // The gust's factor on the speed, 1 + gust_strength * g with g in [-1, 1] (mean 0, deviation 0.48).
    f32 gust_factor(Vec3f position, f64 time) const;
    // What the air at a fixed `position` carries a thing over, from `t0` to `t1`: the integral of velocity() in time.
    Vec3f displacement(Vec3f position, f64 t0, f64 t1) const;
};

// What a particle looks like (the same for all of a system's particles), and how the renderer draws the batch.
struct ParticleLook3D {
    ParticleBlend3D blend = ParticleBlend3D::Alpha;
    ParticleFacing3D facing = ParticleFacing3D::Camera;
    Vec3f axis{0.0f, 1.0f, 0.0f}; // Axis and Flat facing (any length but zero)
    // Stretched: a streak is as long as the particle's speed times `stretch` seconds, at least `stretch_min` units.
    f32 stretch = 0.04f;
    f32 stretch_min = 0.0f;
    // The picture: one texture, an atlas of `atlas_columns` x `atlas_rows` frames (row by row; `atlas_frames` of them, 0: all)
    // chosen by each sprite's `frame` (a flipbook over a life: ParticleDesc3D::frames_per_life). None: a procedural soft disc
    // whose edge is `softness` of its radius wide (1: a smooth ball, 0: a hard disc).
    Texture texture{};
    u32 atlas_columns = 1;
    u32 atlas_rows = 1;
    u32 atlas_frames = 0;
    bool blend_frames = false; // a fractional `frame` mixes the two frames it lies between (two texture reads)
    f32 softness = 1.0f;
    // Untextured: a ring, not a disc, of this share of the radius (0: filled): rain's splash on the ground.
    f32 ring = 0.0f;
    // Soft particles: fade near opaque geometry by the depth the view already has (the pre-pass's: costs it where nothing
    // else needs one). The fade is a smoothstep over `soft_distance` (view-space units) of the particle in front of the
    // surface; at the surface it is 0.
    bool soft = false;
    f32 soft_distance = 0.5f;
    // The share of the scene's light that tints the particle (0: its colour as it is, glowing; 1: times ambient plus half the sun
    // and the fill, see particle_light_tint): so that snow is not white at night.
    f32 lit = 0.0f;
    bool fogged = true; // Scene3D::fog applies (as it does to meshes; an additive particle is dimmed by it, not mixed)
    // Back to front within the batch (alpha and premultiplied; additive needs no order). Batches are ordered by their depth.
    bool sort = true;
};

// One particle as the renderer takes it.
struct ParticleSprite3D {
    Vec3f position{};
    f32 size = 1.0f;       // the quad's width (its height too, but for Stretched, where the length is the streak's)
    f32 rotation = 0.0f;   // radians
    Vec3f colour{1.0f, 1.0f, 1.0f}; // linear, times the look's tint
    f32 alpha = 1.0f;
    Vec3f velocity{};      // world units per second (Stretched only)
    f32 frame = 0.0f;      // flipbook frame, fractional between two
};

// ---- A simulated particle system ----

struct ParticleDesc3D {
    // Emission.
    ParticleShape3D shape = ParticleShape3D::Point;
    Vec3f extent{0.5f, 0.5f, 0.5f}; // Box: half-sizes; Sphere, Cone and Disc: x is the radius
    bool surface_only = false;      // Sphere and Box: on the surface, not inside
    f32 cone_angle = 25.0f;         // Cone: degrees from +Y
    f32 spread = 0.0f;              // Point, Box, Disc: degrees round +Y the directions fall in
    f32 rate = 0.0f;                // particles a second (0: only bursts)
    u32 max_particles = 20000;      // live at once; births past it are dropped
    // At birth.
    ParticleRange3D lifetime{1.0f, 2.0f};  // seconds
    ParticleRange3D speed{1.0f, 2.0f};     // along the emission direction
    Vec3f velocity_min{};                  // plus a vector drawn between these two, in the emitter's space
    Vec3f velocity_max{};
    ParticleRange3D size{0.1f, 0.1f};
    ParticleRange3D rotation{0.0f, 0.0f};  // degrees
    ParticleRange3D spin{0.0f, 0.0f};      // degrees per second
    // Forces.
    Vec3f gravity{};     // m/s^2 (world)
    f32 drag = 0.0f;     // 1/s: velocity relaxes toward the air's (wind * wind_influence) and, with gravity, the terminal one
    f32 wind_influence = 1.0f; // how much of the wind's velocity the air carries here (0: still air)
    // Over a life (t = age / lifetime).
    f32 size_start = 1.0f, size_mid = 1.0f, size_end = 1.0f; // scale of `size` at t = 0, 0.5, 1 (piecewise linear)
    Vec3f colour_start{1.0f, 1.0f, 1.0f};
    Vec3f colour_end{1.0f, 1.0f, 1.0f};
    f32 alpha_start = 1.0f, alpha_end = 0.0f;
    f32 fade_in = 0.0f;             // share of the life over which alpha rises from 0 (no pop at birth)
    f32 colour_variation = 0.0f;    // each particle's brightness is 1 - this * its random number
    // Flipbook: frames run over a life `frames_per_life` times (0: the first frame only); `frame_random` starts each at its own.
    f32 frames_per_life = 0.0f;
    bool frame_random = false;
    // A ground (visual only, no physics): particles reaching it die, or bounce (0: die; 1: elastic).
    bool ground = false;
    f32 ground_height = 0.0f;
    f32 bounce = 0.0f;
    // How it is drawn.
    ParticleLook3D look{};
};

// Live state of one particle (public for tests and tools).
struct Particle3D {
    Vec3f position{};
    Vec3f velocity{};
    f32 age = 0.0f;
    f32 lifetime = 1.0f;
    f32 size = 0.1f;
    f32 rotation = 0.0f; // radians
    f32 spin = 0.0f;     // radians per second
    f32 random = 0.0f;   // 0..1, this particle's own
    u32 index = 0;       // its birth number
};

// A simulation in fixed steps (`step` seconds, 1/120): update(dt) takes whole steps out of the time it has been given, so
// the state after T seconds does not depend on how T was cut into frames, and a saved state restores to the same future.
// Particles are drawn from `sprites()`. Deterministic: a particle's randoms are a hash of the seed, its birth number and
// the purpose, so the same seed and calls give the same particles on every machine.
class ParticleSystem3D {
public:
    explicit ParticleSystem3D(u64 seed = 1) : _seed(seed) {}

    ParticleDesc3D desc{};
    // Where the emitter is (its space to the world): shape, directions and `velocity_min/max` are in it.
    Mat4f transform{};
    bool emitting = true; // false: no births from `rate` (bursts still go)
    f32 rate_scale = 1.0f; // times `desc.rate`: a smooth intensity

    static constexpr f64 step = 1.0 / 120.0;

    void set_seed(u64 seed) { _seed = seed; }
    u64 seed() const { return _seed; }
    void clear();
    // `count` particles now, at the emitter (a burst), or at a world point.
    void burst(u32 count);
    void burst_at(Vec3f position, u32 count);
    // Takes `dt` seconds (any size; whole steps of `step` are run, the rest kept). `wind` is the air's (null: none).
    void update(f64 dt, const Wind3D* wind = nullptr);
    // The state a viewer shows now: the particles as sprites (with the unfinished step's motion carried on).
    void sprites(std::vector<ParticleSprite3D>& out) const;
    std::span<const Particle3D> particles() const { return _particles; }
    std::size_t count() const { return _particles.size(); }
    f64 time() const { return _time; }

    // The state, for rollback: a hash of everything the simulation is a function of (FNV-1a 64 of the bits), and bytes that
    // `restore` takes back (false, changing nothing, for bytes that are not from a system of this kind). The desc, the
    // transform and the seed are the game's: put them back yourself (the seed is saved and restored).
    u64 state_hash() const;
    std::vector<u8> save() const;
    bool restore(std::span<const u8> bytes);

    // A particle's value over its life, as sprites() uses them (public for the tests).
    static f32 size_over_life(const ParticleDesc3D& desc, f32 t);
    static f32 alpha_over_life(const ParticleDesc3D& desc, f32 t);

private:
    void spawn(u32 count, bool at_point, Vec3f point, const Wind3D* wind, f64 sub_step);
    void advance(Particle3D& p, f64 dt, const Wind3D* wind) const;
    f32 random(u32 index, u32 purpose) const;

    u64 _seed = 1;
    std::vector<Particle3D> _particles;
    u32 _next_index = 0;
    f64 _carry = 0.0;  // births not yet whole
    f64 _accumulated = 0.0; // time given and not yet stepped
    f64 _time = 0.0;
    u32 _dropped = 0;
};

// What `lit` tints a particle with: ambient (the sky's and the ground's, averaged) plus half of the sun and half of the fill.
Vec3f particle_light_tint(const Lights3D& lights);
// The soft-particle fade the shader applies: 0 at the surface, 1 `distance` or more in front of it (a smoothstep).
f32 soft_particle_fade(f32 gap, f32 distance);
// The procedural disc's alpha at radius `r` (0 at the centre, 1 at the edge) for a `softness` and a `ring` (see ParticleLook3D).
f32 particle_disc_alpha(f32 r, f32 softness, f32 ring = 0.0f);

// The quad's four corners (counter-clockwise from bottom-left as the texture's uv has them: (-1,-1), (1,-1), (1,1), (-1,1))
// in the world: what the vertex shader makes. `camera` supplies the position and basis.
struct ParticleQuad3D {
    Vec3f corner[4]{};
};
ParticleQuad3D particle_quad(const ParticleSprite3D& sprite, const ParticleLook3D& look, const Camera3D& camera);
// The streak's vector a Stretched sprite is drawn with: velocity * stretch, at least stretch_min long (zero speed: along +Y).
Vec3f particle_streak(const ParticleSprite3D& sprite, const ParticleLook3D& look);

// ---- Weather ----

// One layer of weather: a camera-following volume of `max_particles` particles of which the first `intensity` share are
// shown. A particle's place is a formula of its number and the clock (fall, wind's drift, flutter), wrapped into the
// volume round the camera, so it does not move when the camera does, and any number of them costs no state.
struct WeatherLayer3D {
    f32 intensity = 0.0f;      // 0 to 1: how many are shown (they appear and go one by one: smooth)
    u32 max_particles = 8000;  // at intensity 1
    Vec3f box{36.0f, 24.0f, 36.0f}; // the volume's size round the camera (x, y, z)
    f32 above = 0.75f;         // the share of its height above the camera
    f32 fall_speed = 9.0f;     // m/s (negative: rises)
    f32 fall_variation = 0.2f; // share of it each particle differs by
    f32 wind_influence = 0.9f; // the share of the wind's drift each is carried by (a random 0.8 to 1 of it)
    f32 flutter = 0.0f;        // m: each wanders by this much in the horizontal, slowly
    f32 flutter_rate = 0.5f;   // Hz
    f32 size_min = 0.03f, size_max = 0.05f; // widths, m
    f32 edge_fade = 0.2f;      // share of the box's sides over which a particle fades in and out as it wraps
    Vec3f colour{0.75f, 0.8f, 0.9f};
    f32 alpha = 0.35f;
    f32 splash = 0.0f;         // with a ground: the share of the drops that splash where they meet it (0: none)
    f32 splash_size = 0.12f;   // m across at the end
    f32 splash_time = 0.18f;   // s
    ParticleLook3D look{};
};

WeatherLayer3D weather_rain_layer();
WeatherLayer3D weather_snow_layer();
WeatherLayer3D weather_dust_layer();
WeatherLayer3D weather_mist_layer();

// What apply() does at full rain (rain; snow counts 0.6 as much, mist 0.5, dust none).
struct WeatherEffects3D {
    f32 sun_dim = 0.6f;         // the sun's and the fill's light is scaled by 1 - this * wetness
    f32 ambient_dim = 0.3f;     // the ambient light's (both hemispheres) likewise
    f32 fog_density = 0.004f;   // added to Fog3D::density at wetness 1 (fog turned on)
    f32 fog_height_density = 0.0f;
    f32 sun_scatter_dim = 1.0f; // the fog's sun glare is scaled by 1 - this * wetness
    f32 sun_disc_dim = 0.9f;    // Sky3D::sun_disc likewise (the sun hidden by cloud)
    f32 fog_grey = 0.5f;        // Fog3D::colour goes this far toward its own grey at wetness 1
};

struct Weather3D {
    WeatherLayer3D rain = weather_rain_layer();
    WeatherLayer3D snow = weather_snow_layer();
    WeatherLayer3D dust = weather_dust_layer();
    WeatherLayer3D mist = weather_mist_layer();
    Wind3D wind{};
    // A ground height drops and flakes are hidden below (and rain splashes on): world y.
    bool has_ground = false;
    f32 ground_height = 0.0f;
    // The weather's clock, seconds; the picture is a function of it. The game advances it (a rewound clock rewinds the rain).
    f64 time = 0.0;
    u32 seed = 1;
    WeatherEffects3D effects{};

    // Any layer on.
    bool active() const { return rain.intensity > 0.0f || snow.intensity > 0.0f || dust.intensity > 0.0f || mist.intensity > 0.0f; }
    // How wet the air is for apply(): the strongest of rain, 0.6 snow, 0.5 mist (0 to 1).
    f32 wetness() const;
    // Dims the sun, the fill and the ambient light (what TimeOfDay3D's apply() has just written: call this after it, every
    // frame, or the dimming compounds) and sets `fog` to `base_fog` thickened by the weather (turned on, its sun glare and
    // colour pulled down). At wetness 0 nothing is dimmed and `fog` is `base_fog`. The sky's own settings are not touched:
    // scale your base `Sky3D::sun_disc` by `sun_disc_scale()` yourself (it is a setting, not rewritten each frame).
    void apply(Lights3D& lights, Fog3D& fog, const Fog3D& base_fog) const;
    f32 sun_disc_scale() const;
};

// A weather batch: a look and the sprites [first, first + count) of the shared vector.
struct WeatherBatch3D {
    ParticleLook3D look;
    u32 first = 0;
    u32 count = 0;
    enum class Kind : u8 { Rain, Snow, Dust, Mist, Splash } kind = Kind::Rain;
};
// Appends each shown layer's sprites (rain, snow, dust, mist, and the splashes of the rain), for the camera at `camera`. Pure:
// the same weather and camera position give the same sprites. Particles outside the camera's view are still made (the
// renderer culls); those below the ground are not.
// `parallel`, when given, runs `n` independent pieces of work (a parallel_for): the particles are made in chunks of 2048 and joined in
// order, so the result is the same with or without it.
using ParallelFor = std::function<void(i32 n, const std::function<void(i32)>& body)>;
// `in_view`, when given, says whether a sphere (a sprite's centre and the radius that holds its quad, a streak included) is in the view:
// a particle that is not is not made at all (saves the writing and the cull of the renderer; the picture is the same).
using SphereTest = std::function<bool(Vec3f centre, f32 radius)>;
void weather_sprites(const Weather3D& weather, const Camera3D& camera, std::vector<ParticleSprite3D>& sprites,
                     std::vector<WeatherBatch3D>& batches, const ParallelFor& parallel = {}, const SphereTest& in_view = {});
// One layer's particle number `i` at the weather's clock, wrapped for a camera at `eye`: the sprite, and whether it is shown
// (it can be hidden: below the ground, past the layer's intensity). `kind` is WeatherBatch3D::Kind's number of the layer (0 rain,
// 1 snow, 2 dust, 3 mist: its own random stream). The unit tests compare the pictures with it. `unwrapped`, when given, gets
// the particle's place before it is wrapped into the volume (what stays the same when the camera moves).
bool weather_particle(const Weather3D& weather, const WeatherLayer3D& layer, u32 kind, u32 i, Vec3f eye, ParticleSprite3D& out,
                      Vec3f* unwrapped = nullptr);
// The splash of drop `i` of a layer with `splash` > 0 and a ground: shown while the drop has been under the ground for
// less than `splash_time` (it is hidden under it then): a flat ring on the ground, growing and fading.
bool weather_splash(const Weather3D& weather, const WeatherLayer3D& layer, u32 kind, u32 i, Vec3f eye, ParticleSprite3D& out);

} // namespace kin
