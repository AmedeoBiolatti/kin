#include <kin/particles/particle_catalog.hpp>

#include <kin/assets/asset_manager.hpp>
#include <kin/assets/content.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace kin {
namespace {

std::string trim_comment(std::string line) {
    if (const auto comment = line.find('#'); comment != std::string::npos) {
        line.erase(comment);
    }
    return line;
}

ParticleRange read_range(std::istream& in) {
    ParticleRange range;
    in >> range.min >> range.max;
    return range;
}

Color read_color(std::istream& in) {
    i32 r = 255;
    i32 g = 255;
    i32 b = 255;
    i32 a = 255;
    in >> r >> g >> b >> a;
    return {
        static_cast<u8>(std::clamp(r, 0, 255)),
        static_cast<u8>(std::clamp(g, 0, 255)),
        static_cast<u8>(std::clamp(b, 0, 255)),
        static_cast<u8>(std::clamp(a, 0, 255)),
    };
}

std::string read_optional_sprite(std::istream& in) {
    std::string sprite;
    if (in >> sprite && sprite != "-") {
        return sprite;
    }
    return {};
}

void write_color(std::ostream& out, Color color) {
    out << static_cast<i32>(color.r) << ' '
        << static_cast<i32>(color.g) << ' '
        << static_cast<i32>(color.b) << ' '
        << static_cast<i32>(color.a);
}

} // namespace

void ParticleCatalog::clear() {
    _effects.clear();
    _fields.clear();
}

void ParticleCatalog::set_effect(std::string name, ParticleEffect effect) {
    if (!name.empty()) {
        _effects[std::move(name)] = std::move(effect);
    }
}

void ParticleCatalog::set_field(std::string name, ParticleField field) {
    if (!name.empty()) {
        _fields[std::move(name)] = std::move(field);
    }
}

const ParticleEffect* ParticleCatalog::effect(std::string_view name) const {
    const auto found = _effects.find(std::string{name});
    return found == _effects.end() ? nullptr : &found->second;
}

const ParticleField* ParticleCatalog::field(std::string_view name) const {
    const auto found = _fields.find(std::string{name});
    return found == _fields.end() ? nullptr : &found->second;
}

bool ParticleCatalog::remove_effect(std::string_view name) {
    return _effects.erase(std::string{name}) > 0;
}

bool ParticleCatalog::remove_field(std::string_view name) {
    return _fields.erase(std::string{name}) > 0;
}

bool ParticleCatalog::has_effect(std::string_view name) const {
    return effect(name) != nullptr;
}

bool ParticleCatalog::has_field(std::string_view name) const {
    return field(name) != nullptr;
}

ParticleCatalog default_particle_catalog() {
    ParticleCatalog catalog;
    catalog.set_effect("hit_warm", {.burst = {
        .count = 12,
        .speed = {35.0f, 120.0f},
        .lifetime = {0.12f, 0.35f},
        .start_size = 2.0f,
        .end_size = 0.0f,
        .start_color = {255, 220, 120, 255},
        .end_color = {255, 80, 40, 0},
        .acceleration = {0.0f, 80.0f},
    }});
    catalog.set_effect("hit_cool", {.burst = {
        .count = 12,
        .speed = {35.0f, 100.0f},
        .lifetime = {0.12f, 0.35f},
        .start_size = 2.0f,
        .end_size = 0.0f,
        .start_color = {120, 210, 255, 255},
        .end_color = {60, 80, 255, 0},
        .acceleration = {0.0f, 60.0f},
    }});
    catalog.set_effect("vanish_warm", {.burst = {
        .count = 24,
        .speed = {20.0f, 80.0f},
        .lifetime = {0.25f, 0.65f},
        .start_size = 3.0f,
        .end_size = 0.0f,
        .start_color = {255, 200, 140, 220},
        .end_color = {255, 120, 60, 0},
    }});
    catalog.set_effect("step_dust", {.burst = {
        .count = 4,
        .speed = {6.0f, 22.0f},
        .lifetime = {0.16f, 0.34f},
        .start_size = 2.0f,
        .end_size = 0.0f,
        .start_color = {170, 150, 110, 180},
        .end_color = {170, 150, 110, 0},
        .acceleration = {0.0f, 30.0f},
    }});
    catalog.set_field("simple_stars", {
        .count = 48,
        .seed = 777,
        .area = {0.0f, 0.0f, 320.0f, 180.0f},
        .velocity_y = {4.0f, 18.0f},
        .size = {1.0f, 2.0f},
        .brightness = {80.0f, 220.0f},
        .wrap = true,
        .render = {.layer = layer_value(RenderLayer::Background)},
    });
    catalog.set_field("simple_dust", {
        .count = 24,
        .seed = 991,
        .area = {0.0f, 0.0f, 320.0f, 180.0f},
        .velocity_x = {-3.0f, 3.0f},
        .velocity_y = {6.0f, 20.0f},
        .size = {1.0f, 1.0f},
        .color = {150, 170, 190, 80},
        .use_brightness = false,
        .wrap = true,
        .render = {.layer = layer_value(RenderLayer::Background, 5)},
    });
    return catalog;
}

std::string_view particle_pixel_policy_name(ParticlePixelPolicy policy) {
    switch (policy) {
    case ParticlePixelPolicy::Free: return "free";
    case ParticlePixelPolicy::LogicalPixel: return "logical";
    case ParticlePixelPolicy::Subpixel: return "subpixel";
    case ParticlePixelPolicy::Output: return "output";
    }
    return "free";
}

bool parse_particle_pixel_policy(std::string_view value, ParticlePixelPolicy& out) {
    if (value == "free") {
        out = ParticlePixelPolicy::Free;
        return true;
    }
    if (value == "logical" || value == "logical_pixel") {
        out = ParticlePixelPolicy::LogicalPixel;
        return true;
    }
    if (value == "subpixel") {
        out = ParticlePixelPolicy::Subpixel;
        return true;
    }
    if (value == "output") {
        out = ParticlePixelPolicy::Output;
        return true;
    }
    return false;
}

namespace {

// "a,b" or "a" (both the same).
bool read_pair(std::string_view text, f32& a, f32& b) {
    std::istringstream in{std::string(text)};
    char comma = 0;
    if (!(in >> a)) {
        return false;
    }
    b = a;
    if (in >> comma) {
        return comma == ',' && static_cast<bool>(in >> b);
    }
    return true;
}

bool read_color_text(std::string_view text, Color& color) {
    std::string spaced(text);
    std::ranges::replace(spaced, ',', ' ');
    std::istringstream in{spaced};
    i32 c[4]{};
    if (!(in >> c[0] >> c[1] >> c[2] >> c[3])) {
        return false;
    }
    color = {static_cast<u8>(std::clamp(c[0], 0, 255)), static_cast<u8>(std::clamp(c[1], 0, 255)),
             static_cast<u8>(std::clamp(c[2], 0, 255)), static_cast<u8>(std::clamp(c[3], 0, 255))};
    return true;
}

// "0:2;0.5:6;1:0": keys `at:value`, in order of `at` once read.
template <typename Key, typename ReadValue>
bool read_keys(std::string_view text, std::vector<Key>& keys, ReadValue read_value) {
    keys.clear();
    while (!text.empty()) {
        const std::size_t end = std::min(text.find(';'), text.size());
        const std::string_view key = text.substr(0, end);
        text.remove_prefix(std::min(end + 1, text.size()));
        const std::size_t colon = key.find(':');
        if (colon == std::string_view::npos) {
            return false;
        }
        Key read{};
        f32 again = 0.0f;
        if (!read_pair(key.substr(0, colon), read.at, again) || !read_value(key.substr(colon + 1), read)) {
            return false;
        }
        read.at = std::clamp(read.at, 0.0f, 1.0f);
        keys.push_back(read);
    }
    std::ranges::stable_sort(keys, {}, &Key::at);
    return !keys.empty();
}

bool read_curve(std::string_view text, std::vector<ParticleKey>& curve) {
    return read_keys(text, curve, [](std::string_view value, ParticleKey& key) {
        f32 again = 0.0f;
        return read_pair(value, key.value, again);
    });
}

bool read_gradient(std::string_view text, std::vector<ParticleColorKey>& gradient) {
    return read_keys(text, gradient, [](std::string_view value, ParticleColorKey& key) {
        return read_color_text(value, key.color);
    });
}

bool read_flag(std::string_view text, bool& flag) {
    if (text == "1" || text == "true") {
        flag = true;
        return true;
    }
    if (text == "0" || text == "false") {
        flag = false;
        return true;
    }
    return false;
}

// A name of another effect: anything with no space (the line's tokens have none).
bool read_name(std::string_view text, std::string& name) {
    name = std::string(text);
    return !name.empty();
}

// An effect's `name=value` settings after its first ones; false: one is not known, or not read.
bool read_extra(std::string_view token, ParticleEffect& effect, std::string& why) {
    const std::size_t equals = token.find('=');
    if (equals == std::string_view::npos) {
        why = "expected name=value, not '" + std::string(token) + "'";
        return false;
    }
    ParticleBurst& burst = effect.burst;
    ParticleEmission& emission = effect.emission;
    const std::string_view name = token.substr(0, equals);
    const std::string_view value = token.substr(equals + 1);
    f32 a = 0.0f;
    f32 b = 0.0f;
    bool read = false;
    // One number, kept no lower than `least`.
    const auto number = [&](f32& to, f32 least) {
        return read_pair(value, a, b) && ((to = std::max(least, a)), true);
    };
    if (name == "direction") {
        read = read_pair(value, a, b) && ((burst.direction = a), true);
    } else if (name == "spread") {
        read = read_pair(value, a, b) && ((burst.spread = a), true);
    } else if (name == "shape") {
        const std::optional<ParticleShape> shape = particle_shape_named(value);
        read = shape && ((burst.shape = *shape), true);
    } else if (name == "shape_size") {
        read = read_pair(value, a, b) && ((burst.shape_size = {a, b}), true);
    } else if (name == "drag") {
        read = read_pair(value, a, b) && ((burst.drag = a), true);
    } else if (name == "rotation") {
        read = read_pair(value, a, b) && ((burst.rotation = {a, b}), true);
    } else if (name == "spin") {
        read = read_pair(value, a, b) && ((burst.spin = {a, b}), true);
    } else if (name == "mid_at") {
        read = read_pair(value, a, b) && ((burst.mid_at = a), (burst.use_mid = true), true);
    } else if (name == "mid_size") {
        read = read_pair(value, a, b) && ((burst.mid_size = a), (burst.use_mid = true), true);
    } else if (name == "mid_color") {
        read = read_color_text(value, burst.mid_color) && ((burst.use_mid = true), true);
    } else if (name == "size_curve") {
        read = read_curve(value, burst.size_curve);
    } else if (name == "color_curve") {
        read = read_gradient(value, burst.color_curve);
    } else if (name == "speed_curve") {
        read = read_curve(value, burst.speed_curve);
    } else if (name == "on_death") {
        read = read_name(value, burst.on_death);
    } else if (name == "trail") {
        read = read_name(value, burst.trail);
    } else if (name == "trail_rate") {
        read = number(burst.trail_rate, 0.0f);
    } else if (name == "rate") {
        read = number(emission.rate, 0.0f);
    } else if (name == "per_distance") {
        read = number(emission.per_distance, 0.0f);
    } else if (name == "duration") {
        read = number(emission.duration, 0.0f);
    } else if (name == "prewarm") {
        read = number(emission.prewarm, 0.0f);
    } else if (name == "inherit") {
        read = read_pair(value, a, b) && ((emission.inherit = a), true);
    } else if (name == "local") {
        read = read_flag(value, emission.local);
    } else {
        why = "unknown effect setting '" + std::string(name) + "'";
        return false;
    }
    if (!read) {
        why = "can't read " + std::string(name) + " '" + std::string(value) + "'";
    }
    return read;
}

bool read_catalog(std::string_view text, const std::string& where, ParticleCatalog& catalog, std::string& error) {
    std::istringstream file{std::string(text)};
    std::string line;
    i32 line_no = 0;
    const auto fail = [&](std::string_view message) {
        error = where + ":" + std::to_string(line_no) + ": " + std::string{message};
        return false;
    };
    while (std::getline(file, line)) {
        ++line_no;
        std::istringstream in(trim_comment(std::move(line)));
        std::string kind;
        if (!(in >> kind)) {
            continue;
        }
        if (kind == "effect") {
            std::string name;
            ParticleEffect effect;
            if (!(in >> name >> effect.burst.count)) {
                return fail("malformed effect");
            }
            effect.burst.speed = read_range(in);
            effect.burst.lifetime = read_range(in);
            in >> effect.burst.start_size >> effect.burst.end_size;
            effect.burst.start_color = read_color(in);
            effect.burst.end_color = read_color(in);
            in >> effect.burst.acceleration.x >> effect.burst.acceleration.y
               >> effect.burst.render.layer >> effect.burst.render.order;
            if (!in) {
                return fail("effect requires count, speed, lifetime, sizes, colors, acceleration, layer, order");
            }
            effect.burst.render.sprite_id = read_optional_sprite(in);
            std::string token;
            std::string why;
            while (in >> token) {
                if (!read_extra(token, effect, why)) {
                    return fail(why);
                }
            }
            catalog.set_effect(std::move(name), std::move(effect));
            continue;
        }
        if (kind == "field") {
            std::string name;
            u64 seed = 1;
            ParticleField field;
            i32 wrap = 1;
            i32 use_brightness = 1;
            if (!(in >> name >> field.count >> seed
                     >> field.area.x >> field.area.y >> field.area.w >> field.area.h)) {
                return fail("malformed field");
            }
            field.seed = seed;
            field.velocity_x = read_range(in);
            field.velocity_y = read_range(in);
            field.size = read_range(in);
            field.brightness = read_range(in);
            field.color = read_color(in);
            in >> use_brightness >> wrap >> field.render.layer >> field.render.order;
            if (!in) {
                return fail("field requires count, seed, area, ranges, color, flags, layer, order");
            }
            field.use_brightness = use_brightness != 0;
            field.wrap = wrap != 0;
            field.render.sprite_id = read_optional_sprite(in);
            catalog.set_field(std::move(name), std::move(field));
            continue;
        }
        return fail("unknown directive");
    }
    return true;
}

template <typename Map>
std::vector<std::string> sorted_names(const Map& map) {
    std::vector<std::string> names;
    names.reserve(map.size());
    for (const auto& [name, value] : map) {
        names.push_back(name);
    }
    std::ranges::sort(names);
    return names;
}

void write_color_text(std::ostream& out, Color color) {
    out << static_cast<i32>(color.r) << ',' << static_cast<i32>(color.g) << ','
        << static_cast<i32>(color.b) << ',' << static_cast<i32>(color.a);
}

void write_curve(std::ostream& out, std::string_view name, const std::vector<ParticleKey>& curve) {
    if (curve.empty()) {
        return;
    }
    out << ' ' << name << '=';
    for (std::size_t i = 0; i < curve.size(); ++i) {
        out << (i ? ";" : "") << curve[i].at << ':' << curve[i].value;
    }
}

void write_gradient(std::ostream& out, std::string_view name, const std::vector<ParticleColorKey>& gradient) {
    if (gradient.empty()) {
        return;
    }
    out << ' ' << name << '=';
    for (std::size_t i = 0; i < gradient.size(); ++i) {
        out << (i ? ";" : "") << gradient[i].at << ':';
        write_color_text(out, gradient[i].color);
    }
}

void write_range_extra(std::ostream& out, std::string_view name, ParticleRange range) {
    out << ' ' << name << '=' << range.min;
    if (range.max != range.min) {
        out << ',' << range.max;
    }
}

} // namespace

namespace {

// Where `t` falls between a curve's keys: the key before it, and how far to the next.
template <typename Key, typename Mix>
auto key_at(std::span<const Key> keys, f32 t, Mix mix) {
    if (t <= keys.front().at) {
        return mix(keys.front(), keys.front(), 0.0f);
    }
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (t < keys[i].at) {
            const Key& a = keys[i - 1];
            const Key& b = keys[i];
            const f32 span = b.at - a.at;
            return mix(a, b, span > 0.0f ? (t - a.at) / span : 1.0f);
        }
    }
    return mix(keys.back(), keys.back(), 0.0f);
}

} // namespace

f32 particle_curve_at(std::span<const ParticleKey> curve, f32 t, f32 otherwise) {
    if (curve.empty()) {
        return otherwise;
    }
    return key_at(curve, t, [](const ParticleKey& a, const ParticleKey& b, f32 f) {
        return a.value + (b.value - a.value) * f;
    });
}

Color particle_gradient_at(std::span<const ParticleColorKey> gradient, f32 t, Color otherwise) {
    if (gradient.empty()) {
        return otherwise;
    }
    return key_at(gradient, t, [](const ParticleColorKey& a, const ParticleColorKey& b, f32 f) {
        const auto mix = [f](u8 x, u8 y) {
            return static_cast<u8>(std::clamp(std::round(static_cast<f32>(x) + (static_cast<f32>(y) - static_cast<f32>(x)) * f),
                                              0.0f, 255.0f));
        };
        return Color{mix(a.color.r, b.color.r), mix(a.color.g, b.color.g), mix(a.color.b, b.color.b), mix(a.color.a, b.color.a)};
    });
}

std::string_view particle_shape_name(ParticleShape shape) {
    switch (shape) {
    case ParticleShape::Point: return "point";
    case ParticleShape::Circle: return "circle";
    case ParticleShape::Box: return "box";
    }
    return "point";
}

std::optional<ParticleShape> particle_shape_named(std::string_view name) {
    if (name == "point") return ParticleShape::Point;
    if (name == "circle") return ParticleShape::Circle;
    if (name == "box") return ParticleShape::Box;
    return std::nullopt;
}

bool parse_particle_catalog(std::string_view text, ParticleCatalog& out, std::string& error) {
    ParticleCatalog catalog;
    error.clear();
    if (!read_catalog(text, "particles", catalog, error)) {
        return false;
    }
    out = std::move(catalog);
    return true;
}

std::string serialize_particle_catalog(const ParticleCatalog& catalog) {
    std::ostringstream out;
    out << "# kin particle catalog\n";
    const ParticleBurst plain{};
    for (const std::string& name : sorted_names(catalog.effects())) {
        const ParticleBurst& burst = catalog.effect(name)->burst;
        out << "effect " << name << ' ' << burst.count << ' '
            << burst.speed.min << ' ' << burst.speed.max << ' '
            << burst.lifetime.min << ' ' << burst.lifetime.max << ' '
            << burst.start_size << ' ' << burst.end_size << ' ';
        write_color(out, burst.start_color);
        out << ' ';
        write_color(out, burst.end_color);
        out << ' ' << burst.acceleration.x << ' ' << burst.acceleration.y << ' '
            << burst.render.layer << ' ' << burst.render.order << ' '
            << (burst.render.sprite_id.empty() ? "-" : burst.render.sprite_id);
        // The rest, where not as kin makes them.
        if (burst.direction != plain.direction) {
            out << " direction=" << burst.direction;
        }
        if (burst.spread != plain.spread) {
            out << " spread=" << burst.spread;
        }
        if (burst.shape != plain.shape) {
            out << " shape=" << particle_shape_name(burst.shape);
        }
        if (burst.shape_size != plain.shape_size) {
            out << " shape_size=" << burst.shape_size.x << ',' << burst.shape_size.y;
        }
        if (burst.drag != plain.drag) {
            out << " drag=" << burst.drag;
        }
        if (burst.rotation.min != 0.0f || burst.rotation.max != 0.0f) {
            write_range_extra(out, "rotation", burst.rotation);
        }
        if (burst.spin.min != 0.0f || burst.spin.max != 0.0f) {
            write_range_extra(out, "spin", burst.spin);
        }
        if (burst.use_mid) {
            out << " mid_at=" << burst.mid_at << " mid_size=" << burst.mid_size << " mid_color=";
            write_color_text(out, burst.mid_color);
        }
        write_curve(out, "size_curve", burst.size_curve);
        write_gradient(out, "color_curve", burst.color_curve);
        write_curve(out, "speed_curve", burst.speed_curve);
        if (!burst.on_death.empty()) {
            out << " on_death=" << burst.on_death;
        }
        if (!burst.trail.empty()) {
            out << " trail=" << burst.trail;
        }
        if (burst.trail_rate != plain.trail_rate) {
            out << " trail_rate=" << burst.trail_rate;
        }
        const ParticleEmission& emission = catalog.effect(name)->emission;
        const ParticleEmission still{};
        if (emission.rate != still.rate) {
            out << " rate=" << emission.rate;
        }
        if (emission.per_distance != still.per_distance) {
            out << " per_distance=" << emission.per_distance;
        }
        if (emission.duration != still.duration) {
            out << " duration=" << emission.duration;
        }
        if (emission.prewarm != still.prewarm) {
            out << " prewarm=" << emission.prewarm;
        }
        if (emission.inherit != still.inherit) {
            out << " inherit=" << emission.inherit;
        }
        if (emission.local) {
            out << " local=1";
        }
        out << '\n';
    }
    for (const std::string& name : sorted_names(catalog.fields())) {
        const ParticleField& field = *catalog.field(name);
        out << "field " << name << ' ' << field.count << ' ' << field.seed << ' '
            << field.area.x << ' ' << field.area.y << ' ' << field.area.w << ' ' << field.area.h << ' '
            << field.velocity_x.min << ' ' << field.velocity_x.max << ' '
            << field.velocity_y.min << ' ' << field.velocity_y.max << ' '
            << field.size.min << ' ' << field.size.max << ' '
            << field.brightness.min << ' ' << field.brightness.max << ' ';
        write_color(out, field.color);
        out << ' ' << (field.use_brightness ? 1 : 0) << ' ' << (field.wrap ? 1 : 0) << ' '
            << field.render.layer << ' ' << field.render.order << ' '
            << (field.render.sprite_id.empty() ? "-" : field.render.sprite_id) << '\n';
    }
    return out.str();
}

ParticleCatalog load_particle_catalog(const std::filesystem::path& path) {
    const std::optional<std::string> text = read_content_text(path);
    if (!text) {
        KIN_LOG_ERROR_F("asset",
                        "particle catalog open failed",
                        (LogFields{
                            {.name = "path", .value = path.string()},
                            {.name = "type", .value = "Particles"},
                        }));
        throw std::runtime_error("Failed to open particle catalog: " + path.string());
    }
    ParticleCatalog catalog;
    std::string error;
    if (!read_catalog(*text, path.string(), catalog, error)) {
        throw std::runtime_error(error);
    }
    KIN_LOG_INFO_F("asset",
                   "particle catalog loaded",
                   (LogFields{
                       {.name = "path", .value = path.string()},
                       {.name = "type", .value = "Particles"},
                       {.name = "effects", .value = std::to_string(catalog.effects().size())},
                       {.name = "fields", .value = std::to_string(catalog.fields().size())},
                   }));
    return catalog;
}

bool save_particle_catalog(const ParticleCatalog& catalog, const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out << serialize_particle_catalog(catalog);
    return static_cast<bool>(out);
}

void register_particle_catalog_loader(AssetManager& assets) {
    assets.register_loader<ParticleCatalog>([](const std::filesystem::path& path) {
        return load_particle_catalog(path);
    });
}

} // namespace kin
