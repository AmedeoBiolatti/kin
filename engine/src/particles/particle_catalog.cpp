#include <kin/particles/particle_catalog.hpp>

#include <kin/assets/asset_manager.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

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

ParticleCatalog load_particle_catalog(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        KIN_LOG_ERROR_F("asset",
                        "particle catalog open failed",
                        (LogFields{
                            {.name = "path", .value = path.string()},
                            {.name = "type", .value = "Particles"},
                        }));
        throw std::runtime_error("Failed to open particle catalog: " + path.string());
    }

    ParticleCatalog catalog;
    std::string line;
    i32 line_no = 0;
    const auto fail = [&](std::string_view message) {
        throw std::runtime_error(path.string() + ":" + std::to_string(line_no) + ": " + std::string{message});
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
                fail("malformed effect");
            }
            effect.burst.speed = read_range(in);
            effect.burst.lifetime = read_range(in);
            in >> effect.burst.start_size >> effect.burst.end_size;
            effect.burst.start_color = read_color(in);
            effect.burst.end_color = read_color(in);
            in >> effect.burst.acceleration.x >> effect.burst.acceleration.y
               >> effect.burst.render.layer >> effect.burst.render.order;
            if (!in) {
                fail("effect requires count, speed, lifetime, sizes, colors, acceleration, layer, order");
            }
            effect.burst.render.sprite_id = read_optional_sprite(in);
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
                fail("malformed field");
            }
            field.seed = seed;
            field.velocity_x = read_range(in);
            field.velocity_y = read_range(in);
            field.size = read_range(in);
            field.brightness = read_range(in);
            field.color = read_color(in);
            in >> use_brightness >> wrap >> field.render.layer >> field.render.order;
            if (!in) {
                fail("field requires count, seed, area, ranges, color, flags, layer, order");
            }
            field.use_brightness = use_brightness != 0;
            field.wrap = wrap != 0;
            field.render.sprite_id = read_optional_sprite(in);
            catalog.set_field(std::move(name), std::move(field));
            continue;
        }
        fail("unknown directive");
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
    std::ofstream out(path);
    if (!out) {
        return false;
    }

    out << "# kin particle catalog\n";
    std::vector<std::string> names;
    names.reserve(catalog.effects().size());
    for (const auto& [name, effect] : catalog.effects()) {
        names.push_back(name);
    }
    std::ranges::sort(names);
    for (const std::string& name : names) {
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
            << (burst.render.sprite_id.empty() ? "-" : burst.render.sprite_id) << '\n';
    }

    names.clear();
    names.reserve(catalog.fields().size());
    for (const auto& [name, field] : catalog.fields()) {
        names.push_back(name);
    }
    std::ranges::sort(names);
    for (const std::string& name : names) {
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
    return static_cast<bool>(out);
}

void register_particle_catalog_loader(AssetManager& assets) {
    assets.register_loader<ParticleCatalog>([](const std::filesystem::path& path) {
        return load_particle_catalog(path);
    });
}

} // namespace kin
