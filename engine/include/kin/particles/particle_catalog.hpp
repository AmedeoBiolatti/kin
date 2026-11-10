#pragma once

#include <kin/assets/asset_handle.hpp>
#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/render_layer.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

class AssetManager;

enum class ParticlePixelPolicy {
    Free,
    LogicalPixel,
    Subpixel,
    Output,
};

struct ParticleRange {
    f32 min = 0.0f;
    f32 max = 0.0f;
};

struct ParticleRenderStyle {
    std::string sprite_id;
    i32 layer = layer_value(RenderLayer::Effects);
    i32 order = 0;
    bool y_sort = false;
    f32 sort_y_offset = 0.0f;
};

// Where a burst's particles start: at its position, anywhere in an ellipse
// `shape_size` across about it, or in a box `shape_size` big about it.
enum class ParticleShape : u8 { Point, Circle, Box };

// A value over a particle's life: keys `at` a part of it (0 as it starts, 1 as
// it ends), in order, the value straight between them (and the first's before
// the first, the last's after the last).
struct ParticleKey {
    f32 at = 0.0f;
    f32 value = 0.0f;
    bool operator==(const ParticleKey&) const = default;
};
struct ParticleColorKey {
    f32 at = 0.0f;
    Color color{255, 255, 255, 255};
    bool operator==(const ParticleColorKey&) const = default;
};

// The curve's value at `t` of a life; `otherwise` when it has no keys.
f32 particle_curve_at(std::span<const ParticleKey> curve, f32 t, f32 otherwise);
Color particle_gradient_at(std::span<const ParticleColorKey> gradient, f32 t, Color otherwise);

// A burst of `count` particles. Each goes `speed` units a second, `direction`
// degrees (clockwise from right: 90 is down) give or take half of `spread`
// (360: any way), and lives `lifetime` seconds; it is pushed by
// `acceleration` (gravity) and slowed by `drag` (the part of its speed it
// loses each second, about), turned `rotation` degrees and spun `spin`
// degrees a second. Its size and colour go from start to end over its life,
// through mid (at `mid_at` of it) when `use_mid`; or as its curves go, when
// they have keys: `size_curve` its size, `color_curve` its colour, and
// `speed_curve` how fast it goes (times its speed: 1, as it was made).
//
// Sub-effects, other effects of its catalog: `on_death` burst where each
// particle ends, and `trail` burst where it is, `trail_rate` times a second,
// along its way. Theirs burst theirs, three deep at most.
struct ParticleBurst {
    Vec2f position{};
    i32 count = 8;
    ParticleRange speed{20.0f, 80.0f};
    ParticleRange lifetime{0.25f, 0.6f};
    f32 start_size = 2.0f;
    f32 end_size = 0.0f;
    Color start_color{255, 255, 255, 255};
    Color end_color{255, 255, 255, 0};
    Vec2f acceleration{};
    ParticleRenderStyle render{};
    f32 direction = 0.0f;
    f32 spread = 360.0f;
    ParticleShape shape = ParticleShape::Point;
    Vec2f shape_size{};
    f32 drag = 0.0f;
    ParticleRange rotation{};
    ParticleRange spin{};
    bool use_mid = false;
    f32 mid_at = 0.5f;
    f32 mid_size = 2.0f;
    Color mid_color{255, 255, 255, 255};
    std::vector<ParticleKey> size_curve{};
    std::vector<ParticleColorKey> color_curve{};
    std::vector<ParticleKey> speed_curve{};
    std::string on_death{};
    std::string trail{};
    f32 trail_rate = 10.0f;
};

// How an effect streams, started as an emitter (ParticleSystem::start): `rate`
// particles a second, and `per_distance` a unit it moves, each made as its
// burst makes one (but turned with the emitter). For `duration` seconds (0:
// until it is stopped); as if it had been for `prewarm` seconds already as it
// starts. Its particles start with `inherit` of the emitter's own velocity,
// and move with it after when `local`.
struct ParticleEmission {
    f32 rate = 0.0f;
    f32 per_distance = 0.0f;
    f32 duration = 0.0f;
    f32 prewarm = 0.0f;
    f32 inherit = 0.0f;
    bool local = false;
    bool operator==(const ParticleEmission&) const = default;
};

struct ParticleEffect {
    ParticleBurst burst{};
    ParticleEmission emission{};
    // It streams when started (else a start bursts it once).
    bool streams() const { return emission.rate > 0.0f || emission.per_distance > 0.0f; }
};

struct ParticleField {
    i32 count = 0;
    u64 seed = 1;
    Rectf area{};
    ParticleRange velocity_x{};
    ParticleRange velocity_y{};
    ParticleRange size{1.0f, 1.0f};
    ParticleRange brightness{255.0f, 255.0f};
    Color color{255, 255, 255, 255};
    bool use_brightness = true;
    bool wrap = true;
    ParticleRenderStyle render{};
};

class ParticleCatalog {
public:
    void clear();
    void set_effect(std::string name, ParticleEffect effect);
    void set_field(std::string name, ParticleField field);
    bool remove_effect(std::string_view name);
    bool remove_field(std::string_view name);

    const ParticleEffect* effect(std::string_view name) const;
    const ParticleField* field(std::string_view name) const;
    bool has_effect(std::string_view name) const;
    bool has_field(std::string_view name) const;

    const std::unordered_map<std::string, ParticleEffect>& effects() const { return _effects; }
    const std::unordered_map<std::string, ParticleField>& fields() const { return _fields; }

private:
    std::unordered_map<std::string, ParticleEffect> _effects;
    std::unordered_map<std::string, ParticleField> _fields;
};

ParticleCatalog default_particle_catalog();
ParticleCatalog load_particle_catalog(const std::filesystem::path& path);
bool save_particle_catalog(const ParticleCatalog& catalog, const std::filesystem::path& path);
// A catalog's text read (false, and `error` says where and why, when it is not
// one), and written: an `effect` line of each effect and a `field` line of
// each field, by name. An effect's settings past kin's first ones follow its
// line as `name=value` (only those not at their defaults), so older readers
// still read it: a curve as `size_curve=0:2;0.5:6;1:0` (`at:value`; a colour
// `at:r,g,b,a`).
bool parse_particle_catalog(std::string_view text, ParticleCatalog& out, std::string& error);
std::string serialize_particle_catalog(const ParticleCatalog& catalog);

std::string_view particle_shape_name(ParticleShape shape); // "point", "circle", "box"
std::optional<ParticleShape> particle_shape_named(std::string_view name);
void register_particle_catalog_loader(AssetManager& assets);

std::string_view particle_pixel_policy_name(ParticlePixelPolicy policy);
bool parse_particle_pixel_policy(std::string_view value, ParticlePixelPolicy& out);

} // namespace kin
