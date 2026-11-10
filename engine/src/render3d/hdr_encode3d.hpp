#pragma once

#include <kin/core/types.hpp>
#include <span>
#include <string>

namespace kin {

// A Radiance .hdr picture's bytes (RGBE, run-length coded): `rgb` three floats a texel, rows from the top
// (environment.cpp; save_reflection_capture and write_scene_snapshot write them).
std::string encode_hdr3d(Vec2i size, std::span<const f32> rgb);

} // namespace kin
