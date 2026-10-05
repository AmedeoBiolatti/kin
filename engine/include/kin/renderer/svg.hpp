#pragma once

// Shapes as SVG files ("SVG-lite"): what a vector editor (Inkscape, Figma,
// Illustrator's "SVG 1.1") exports for flat-coloured artwork reads into a
// kin::Shape, and a Shape writes as SVG any browser opens.
//
// Read: <svg> (viewBox, width, height), <g>, <path>, <rect> (rx, ry), <circle>,
// <ellipse>, <line>, <polyline>, <polygon>, <defs>, <symbol> and <use> (an
// element or group by id, at x, y); the transform attribute (matrix,
// translate, scale, rotate, skewX, skewY); fill, fill-rule, fill-opacity,
// stroke, stroke-width, stroke-linejoin, stroke-linecap, stroke-miterlimit,
// stroke-opacity, opacity and color, as attributes, in style="", inherited
// from groups. Colours: #rgb, #rgba, #rrggbb, #rrggbbaa, rgb(), rgba(), the
// basic names, currentColor.
//
// Not read (each noted in `warnings`, the rest still read): gradients and
// patterns (drawn unfilled), text, images, masks, clip paths, filters, CSS
// <style> sheets, units other than px. Group opacity multiplies into each
// element's colours instead of compositing the group.

#include <kin/renderer/shape.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

// Null when `svg` is not an SVG document; `error` says why.
std::optional<Shape> read_svg(std::string_view svg, std::string* error = nullptr,
                              std::vector<std::string>* warnings = nullptr);
std::optional<Shape> load_svg(const std::filesystem::path& path, std::string* error = nullptr,
                              std::vector<std::string>* warnings = nullptr);

// Circles, ellipses and rectangles as themselves, every other element as a
// <path>; transforms as matrices.
std::string write_svg(const Shape& shape);
bool save_svg(const Shape& shape, const std::filesystem::path& path);

} // namespace kin
