#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <kin/platform/input.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kin::asset_preview {
inline constexpr std::string_view modes[] = {"lit",      "base",      "normals", "uv",
                                             "metallic", "roughness", "alpha",   "wireframe"};
struct Options {
    std::filesystem::path root, model, materials;
    std::string variant, clip;
    int width = 960, height = 640, mode = 0, part = -1;
    float yaw = 35, pitch = 25, turntable = 0, time = 0;
    bool hud = false, bounds = false, ground = true, shadows = true, ao = false, orthographic = false, help = false,
         inspect = false;
    std::vector<char*> runtime;
};
inline int integer(std::string_view text, int low, int high) {
    int value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < low || value > high)
        throw std::invalid_argument("invalid integer: " + std::string(text));
    return value;
}
inline float number(std::string_view text) {
    float value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !std::isfinite(value) ||
        std::abs(value) > 100000)
        throw std::invalid_argument("invalid finite number: " + std::string(text));
    return value;
}
inline Options parse(int argc, char** argv) {
    Options o;
    o.runtime.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        const auto equals = arg.find('=');
        const auto key = arg.substr(0, equals);
        auto value = [&]() -> std::string_view {
            if (equals != arg.npos)
                return arg.substr(equals + 1);
            if (i + 1 >= argc)
                throw std::invalid_argument("missing value for " + std::string(key));
            return argv[++i];
        };
        if (key == "--root")
            o.root = value();
        else if (key == "--model")
            o.model = value();
        else if (key == "--materials")
            o.materials = value();
        else if (key == "--variant")
            o.variant = value();
        else if (key == "--clip")
            o.clip = value();
        else if (key == "--time")
            o.time = number(value());
        else if (key == "--width")
            o.width = integer(value(), 320, 3840);
        else if (key == "--height")
            o.height = integer(value(), 180, 2160);
        else if (key == "--yaw")
            o.yaw = number(value());
        else if (key == "--pitch")
            o.pitch = std::clamp(number(value()), -85.f, 85.f);
        else if (key == "--turntable")
            o.turntable = number(value());
        else if (key == "--part")
            o.part = integer(value(), 0, 255);
        else if (key == "--mode") {
            const auto name = value();
            const auto it = std::find(std::begin(modes), std::end(modes), name);
            if (it == std::end(modes))
                throw std::invalid_argument("unknown inspection mode: " + std::string(name));
            o.mode = int(it - std::begin(modes));
        } else if (arg == "--hud")
            o.hud = true;
        else if (arg == "--bounds")
            o.bounds = true;
        else if (arg == "--ao")
            o.ao = true;
        else if (arg == "--ortho")
            o.orthographic = true;
        else if (arg == "--no-ground")
            o.ground = false;
        else if (arg == "--no-shadows")
            o.shadows = false;
        else if (arg == "--inspect")
            o.inspect = true;
        else if (arg == "--help")
            o.help = true;
        else
            o.runtime.push_back(argv[i]); // standard Kin headless/server/profile arguments
    }
    if (o.help)
        return o;
    if (!o.help && o.model.empty())
        throw std::invalid_argument("--model FILE is required");
    if (o.root.empty()) {
        o.model = std::filesystem::absolute(o.model);
        o.root = o.model.parent_path();
    }
    o.root = std::filesystem::absolute(o.root).lexically_normal();
    return o;
}
inline InputMap input_map() {
    InputMap map;
    map.bind("preview.quit", Key::Escape);
    map.bind("preview.mode", Key::Tab);
    map.bind("preview.bounds", Key::B);
    map.bind("preview.ground", Key::G);
    map.bind("preview.shadows", Key::S);
    map.bind("preview.ao", Key::A);
    map.bind("preview.projection", Key::O);
    map.bind("preview.freeze", Key::Space);
    map.bind("preview.reload", Key::R);
    map.bind("preview.home", Key::Home);
    map.bind("preview.part", Key::Period);
    map.bind("preview.hud", Key::H);
    return map;
}
} // namespace kin::asset_preview
