#include <kin/ecs/shader_effect.hpp>

#include <kin/renderer/renderer2d.hpp>

#include <charconv>
#include <cstdio>
#include <sstream>

namespace kin {
namespace {

std::optional<f32> number(std::string_view text) {
    f32 value = 0.0f;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::vector<ShaderParamValue> parse_shader_params(std::string_view text) {
    std::vector<ShaderParamValue> out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t start = text.find_first_not_of(" \t\n", at);
        if (start == std::string_view::npos) {
            break;
        }
        const std::size_t end = std::min(text.find_first_of(" \t\n", start), text.size());
        const std::string_view word = text.substr(start, end - start);
        at = end;
        const std::size_t equals = word.find('=');
        if (equals == 0 || equals == std::string_view::npos) {
            continue;
        }
        ShaderParamValue value{.name = std::string(word.substr(0, equals)), .values = {}};
        std::string_view rest = word.substr(equals + 1);
        bool read = !rest.empty();
        while (read && !rest.empty()) {
            const std::size_t comma = rest.find(',');
            const std::optional<f32> v = number(rest.substr(0, comma));
            if (!v) {
                read = false;
                break;
            }
            value.values.push_back(*v);
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        }
        if (read) {
            out.push_back(std::move(value));
        }
    }
    return out;
}

std::string format_shader_params(const std::vector<ShaderParamValue>& values) {
    std::string out;
    for (const ShaderParamValue& value : values) {
        if (!out.empty()) {
            out += ' ';
        }
        out += value.name + '=';
        for (std::size_t i = 0; i < value.values.size(); ++i) {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", static_cast<double>(value.values[i]));
            out += (i ? "," : "") + std::string(text);
        }
    }
    return out;
}

ShaderParams make_shader_params(const Renderer2D& renderer, ShaderHandle shader, std::string_view params) {
    ShaderParams made = renderer.shader_params(shader);
    for (const ShaderParamValue& value : parse_shader_params(params)) {
        made.set(value.name, value.values);
    }
    return made;
}

void update_shader_effects(EcsWorld& world, f32 dt) {
    world.raw().each([&](ShaderEffect& effect) { effect.time += dt; });
}

} // namespace kin
